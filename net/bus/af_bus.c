/*
 * Implementation of Bus domain sockets.
 *
 * Copyright (c) 2012, GENIVI Alliance
 *
 * Authors:	Javier Martinez Canillas <javier.martinez@collabora.co.uk>
 *              Alban Crequy <alban.crequy@collabora.co.uk>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Based on BSD Unix domain sockets (net/unix).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/dcache.h>
#include <linux/namei.h>
#include <linux/socket.h>
#include <linux/bus.h>
#include <linux/fcntl.h>
#include <linux/termios.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <net/net_namespace.h>
#include <net/sock.h>
#include <net/af_bus.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <net/scm.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/rtnetlink.h>
#include <linux/mount.h>
#include <net/checksum.h>
#include <linux/security.h>

struct hlist_head bus_socket_table[BUS_HASH_SIZE + 1];
EXPORT_SYMBOL_GPL(bus_socket_table);
struct hlist_head bus_address_table[BUS_HASH_SIZE];
EXPORT_SYMBOL_GPL(bus_address_table);
DEFINE_SPINLOCK(bus_table_lock);
DEFINE_SPINLOCK(bus_address_lock);
EXPORT_SYMBOL_GPL(bus_address_lock);
static atomic_long_t bus_nr_socks;

#define bus_sockets_unbound	(&bus_socket_table[BUS_HASH_SIZE])

#define BUS_ABSTRACT(sk)	(bus_sk(sk)->addr->hash != BUS_HASH_SIZE)

#ifdef CONFIG_SECURITY_NETWORK
static void bus_get_secdata(struct scm_cookie *scm, struct sk_buff *skb)
{
	memcpy(BUSSID(skb), &scm->secid, sizeof(u32));
}

static inline void bus_set_secdata(struct scm_cookie *scm, struct sk_buff *skb)
{
	scm->secid = *BUSSID(skb);
}
#else
static inline void bus_get_secdata(struct scm_cookie *scm, struct sk_buff *skb)
{ }

static inline void bus_set_secdata(struct scm_cookie *scm, struct sk_buff *skb)
{ }
#endif /* CONFIG_SECURITY_NETWORK */

/*
 *  SMP locking strategy:
 *    bus_socket_table hash table is protected with spinlock bus_table_lock
 *    bus_address_table hash table is protected with spinlock bus_address_lock
 *    each bus is protected by a separate spin lock.
 *    multicast atomic sending is protected by a separate spin lock.
 *    each socket state is protected by a separate spin lock.
 *    each socket address is protected by a separate spin lock.
 *
 *  When holding more than one lock, use the following hierarchy:
 *  - bus_table_lock.
 *  - bus_address_lock.
 *  - socket lock.
 *  - bus lock.
 *  - bus send_lock.
 *  - sock address lock.
 */

#define bus_peer(sk) (bus_sk(sk)->peer)

static inline int bus_our_peer(struct sock *sk, struct sock *osk)
{
	return bus_peer(osk) == sk;
}

static inline int bus_recvq_full(struct sock const *sk)
{
	return skb_queue_len(&sk->sk_receive_queue) > sk->sk_max_ack_backlog;
}

static inline u16 bus_addr_prefix(struct sockaddr_bus *busaddr)
{
	return (busaddr->sbus_addr.s_addr & BUS_PREFIX_MASK) >> BUS_CLIENT_BITS;
}

static inline u64 bus_addr_client(struct sockaddr_bus *sbusaddr)
{
	return sbusaddr->sbus_addr.s_addr & BUS_CLIENT_MASK;
}

static inline bool bus_mc_addr(struct sockaddr_bus *sbusaddr)
{
	return bus_addr_client(sbusaddr) == BUS_CLIENT_MASK;
}

struct sock *bus_peer_get(struct sock *s)
{
	struct sock *peer;

	bus_state_lock(s);
	peer = bus_peer(s);
	if (peer)
		sock_hold(peer);
	bus_state_unlock(s);
	return peer;
}
EXPORT_SYMBOL_GPL(bus_peer_get);

static inline void bus_release_addr(struct bus_address *addr)
{
	if (atomic_dec_and_test(&addr->refcnt))
		kfree(addr);
}

/*
 *	Check bus socket name:
 *		- should be not zero length.
 *	        - if started by not zero, should be NULL terminated (FS object)
 *		- if started by zero, it is abstract name.
 */

static int bus_mkname(struct sockaddr_bus *sbusaddr, int len,
		      unsigned int *hashp)
{
	int offset = (sbusaddr->sbus_path[0] == '\0');

	if (len <= sizeof(short) || len > sizeof(*sbusaddr))
		return -EINVAL;
	if (!sbusaddr || sbusaddr->sbus_family != AF_BUS)
		return -EINVAL;

	len = strnlen(sbusaddr->sbus_path + offset, BUS_PATH_MAX) + 1 +
		sizeof(__kernel_sa_family_t) +
		sizeof(struct bus_addr);

	*hashp = bus_compute_hash(sbusaddr->sbus_addr);
	return len;
}

static void __bus_remove_address(struct bus_address *addr)
{
	hlist_del(&addr->table_node);
}

static void __bus_insert_address(struct hlist_head *list,
				 struct bus_address *addr)
{
	hlist_add_head(&addr->table_node, list);
}

static inline void bus_remove_address(struct bus_address *addr)
{
	spin_lock(&bus_address_lock);
	__bus_remove_address(addr);
	spin_unlock(&bus_address_lock);
}

static inline void bus_insert_address(struct hlist_head *list,
				      struct bus_address *addr)
{
	spin_lock(&bus_address_lock);
	__bus_insert_address(list, addr);
	spin_unlock(&bus_address_lock);
}

static void __bus_remove_socket(struct sock *sk)
{
	sk_del_node_init(sk);
}

static void __bus_insert_socket(struct hlist_head *list, struct sock *sk)
{
	WARN_ON(!sk_unhashed(sk));
	sk_add_node(sk, list);
}

static inline void bus_remove_socket(struct sock *sk)
{
	spin_lock(&bus_table_lock);
	__bus_remove_socket(sk);
	spin_unlock(&bus_table_lock);
}

static inline void bus_insert_socket(struct hlist_head *list, struct sock *sk)
{
	spin_lock(&bus_table_lock);
	__bus_insert_socket(list, sk);
	spin_unlock(&bus_table_lock);
}

static inline bool __bus_has_prefix(struct sock *sk, u16 prefix)
{
	struct bus_sock *u = bus_sk(sk);
	struct bus_address *addr;
	struct hlist_node *node;
	bool ret = false;

	hlist_for_each_entry(addr, node, &u->addr_list, addr_node) {
		if (bus_addr_prefix(addr->name) == prefix)
			ret = true;
	}

	return ret;
}

static inline bool bus_has_prefix(struct sock *sk, u16 prefix)
{
	bool ret;

	bus_state_lock(sk);
	ret = __bus_has_prefix(sk, prefix);
	bus_state_unlock(sk);

	return ret;
}

static inline bool __bus_eavesdropper(struct sock *sk, u16 condition)
{
	struct bus_sock *u = bus_sk(sk);

	return u->eavesdropper;
}

static inline bool bus_eavesdropper(struct sock *sk, u16 condition)
{
	bool ret;

	bus_state_lock(sk);
	ret = __bus_eavesdropper(sk, condition);
	bus_state_unlock(sk);

	return ret;
}

static inline bool bus_has_prefix_eavesdropper(struct sock *sk, u16 prefix)
{
	bool ret;

	bus_state_lock(sk);
	ret = __bus_has_prefix(sk, prefix) || __bus_eavesdropper(sk, 0);
	bus_state_unlock(sk);

	return ret;
}

static inline struct bus_address *__bus_get_address(struct sock *sk,
						    struct bus_addr *sbus_addr)
{
	struct bus_sock *u = bus_sk(sk);
	struct bus_address *addr = NULL;
	struct hlist_node *node;

	hlist_for_each_entry(addr, node, &u->addr_list, addr_node) {
		if (addr->name->sbus_addr.s_addr == sbus_addr->s_addr)
			return addr;
	}

	return NULL;
}

static inline struct bus_address *bus_get_address(struct sock *sk,
						  struct bus_addr *sbus_addr)
{
	struct bus_address *addr;

	bus_state_lock(sk);
	addr = __bus_get_address(sk, sbus_addr);
	bus_state_unlock(sk);

	return addr;
}

static struct sock *__bus_find_socket_byname(struct net *net,
					     struct sockaddr_bus *sbusname,
					     int len, unsigned int hash)
{
	struct sock *s;
	struct hlist_node *node;

	sk_for_each(s, node, &bus_socket_table[hash]) {
		struct bus_sock *u = bus_sk(s);

		if (!net_eq(sock_net(s), net))
			continue;

		if (u->addr->len == len &&
		    !memcmp(u->addr->name, sbusname, len))
			return s;
	}

	return NULL;
}

static inline struct sock *bus_find_socket_byname(struct net *net,
						  struct sockaddr_bus *sbusname,
						  int len, unsigned int hash)
{
	struct sock *s;

	spin_lock(&bus_table_lock);
	s = __bus_find_socket_byname(net, sbusname, len, hash);
	if (s)
		sock_hold(s);
	spin_unlock(&bus_table_lock);
	return s;
}

static struct sock *__bus_find_socket_byaddress(struct net *net,
						struct sockaddr_bus *sbusname,
						int len, int protocol,
						unsigned int hash)
{
	struct sock *s;
	struct bus_address *addr;
	struct hlist_node *node;
	struct bus_sock *u;
	int offset = (sbusname->sbus_path[0] == '\0');
	int path_len = strnlen(sbusname->sbus_path + offset, BUS_PATH_MAX);

	len = path_len + 1 + sizeof(__kernel_sa_family_t) +
	      sizeof(struct bus_addr);

	hlist_for_each_entry(addr, node, &bus_address_table[hash],
			     table_node) {
		s = addr->sock;
		u = bus_sk(s);

		if (s->sk_protocol != protocol)
			continue;

		if (!net_eq(sock_net(s), net))
			continue;

		if (addr->len == len &&
		    addr->name->sbus_family == sbusname->sbus_family &&
		    addr->name->sbus_addr.s_addr == sbusname->sbus_addr.s_addr
		    && bus_same_bus(addr->name, sbusname))
			goto found;
	}
	s = NULL;
found:
	return s;
}

static inline struct sock *bus_find_socket_byaddress(struct net *net,
						     struct sockaddr_bus *name,
						     int len, int protocol,
						     unsigned int hash)
{
	struct sock *s;

	spin_lock(&bus_address_lock);
	s = __bus_find_socket_byaddress(net, name, len, protocol, hash);
	if (s)
		sock_hold(s);
	spin_unlock(&bus_address_lock);
	return s;
}

static inline int bus_writable(struct sock *sk)
{
	return (atomic_read(&sk->sk_wmem_alloc) << 2) <= sk->sk_sndbuf;
}

static void bus_write_space(struct sock *sk)
{
	struct bus_sock *u = bus_sk(sk);
	struct bus_sock *p;
	struct hlist_node *node;
	struct socket_wq *wq;

	if (bus_writable(sk)) {
		rcu_read_lock();
		wq = rcu_dereference(sk->sk_wq);
		if (wq_has_sleeper(wq))
			wake_up_interruptible_sync_poll(&wq->wait,
				POLLOUT | POLLWRNORM | POLLWRBAND);
		sk_wake_async(sk, SOCK_WAKE_SPACE, POLL_OUT);
		rcu_read_unlock();

		if (u && u->bus) {
			spin_lock(&u->bus->lock);
			hlist_for_each_entry(p, node, &u->bus->peers,
					     bus_node) {
				wake_up_interruptible_sync_poll(sk_sleep(&p->sk),
								POLLOUT |
								POLLWRNORM |
								POLLWRBAND);
				sk_wake_async(&p->sk, SOCK_WAKE_SPACE,
					      POLL_OUT);
			}
			spin_unlock(&u->bus->lock);
		}
	}
}

static void bus_bus_release(struct kref *kref)
{
	struct bus *bus;

	bus = container_of(kref, struct bus, kref);

	kfree(bus);
}

static void bus_sock_destructor(struct sock *sk)
{
	struct bus_sock *u = bus_sk(sk);

	skb_queue_purge(&sk->sk_receive_queue);

	WARN_ON(atomic_read(&sk->sk_wmem_alloc));
	WARN_ON(!sk_unhashed(sk));
	WARN_ON(sk->sk_socket);
	if (!sock_flag(sk, SOCK_DEAD)) {
		pr_info("Attempt to release alive bus socket: %p\n", sk);
		return;
	}

	if (u->bus) {
		kref_put(&u->bus->kref, bus_bus_release);
		u->bus = NULL;
	}

	atomic_long_dec(&bus_nr_socks);
	local_bh_disable();
	sock_prot_inuse_add(sock_net(sk), sk->sk_prot, -1);
	local_bh_enable();
#ifdef BUS_REFCNT_DEBUG
	pr_debug("BUS %p is destroyed, %ld are still alive.\n", sk,
		 atomic_long_read(&bus_nr_socks));
#endif
}

static int bus_release_sock(struct sock *sk, int embrion)
{
	struct bus_sock *u = bus_sk(sk);
	struct path path;
	struct sock *skpair;
	struct sk_buff *skb;
	int state;
	struct bus_address *addr;
	struct hlist_node *node, *tmp;

	bus_remove_socket(sk);

	if (u->bus && u->authenticated &&
	    !u->bus_master && !u->bus_master_side) {
		spin_lock(&u->bus->lock);
		hlist_del(&u->bus_node);
		if (u->eavesdropper)
			atomic64_dec(&u->bus->eavesdropper_cnt);
		spin_unlock(&u->bus->lock);
	}

	/* Clear state */
	bus_state_lock(sk);
	sock_orphan(sk);
	sk->sk_shutdown = SHUTDOWN_MASK;
	path	     = u->path;
	u->path.dentry = NULL;
	u->path.mnt = NULL;
	state = sk->sk_state;
	sk->sk_state = BUS_CLOSE;

	if (u->bus_master)
			u->bus->master = NULL;

	if (u->bus_master_side) {
		bus_release_addr(u->addr);
		u->addr = NULL;
	} else {
		u->addr = NULL;

		spin_lock(&bus_address_lock);
		hlist_for_each_entry_safe(addr, node, tmp, &u->addr_list,
					  addr_node) {
			hlist_del(&addr->addr_node);
			__bus_remove_address(addr);
			bus_release_addr(addr);
		}
		spin_unlock(&bus_address_lock);
	}

	bus_state_unlock(sk);

	wake_up_interruptible_all(&u->peer_wait);

	skpair = bus_peer(sk);

	if (skpair != NULL) {
		bus_state_lock(skpair);
		/* No more writes */
		skpair->sk_shutdown = SHUTDOWN_MASK;
		if (!skb_queue_empty(&sk->sk_receive_queue) || embrion)
			skpair->sk_err = ECONNRESET;
		bus_state_unlock(skpair);
		skpair->sk_state_change(skpair);
		sk_wake_async(skpair, SOCK_WAKE_WAITD, POLL_HUP);
		sock_put(skpair); /* It may now die */
		bus_peer(sk) = NULL;
	}

	/* Try to flush out this socket. Throw out buffers at least */

	while ((skb = skb_dequeue(&sk->sk_receive_queue)) != NULL) {
		if (state == BUS_LISTEN)
			bus_release_sock(skb->sk, 1);
		/* passed fds are erased in the kfree_skb hook	      */
		kfree_skb(skb);
	}

	if (path.dentry)
		path_put(&path);

	sock_put(sk);

	/* ---- Socket is dead now and most probably destroyed ---- */

	if (bus_tot_inflight)
		bus_gc();		/* Garbage collect fds */

	return 0;
}

static void init_peercred(struct sock *sk)
{
	put_pid(sk->sk_peer_pid);
	if (sk->sk_peer_cred)
		put_cred(sk->sk_peer_cred);
	sk->sk_peer_pid  = get_pid(task_tgid(current));
	sk->sk_peer_cred = get_current_cred();
}

static void copy_peercred(struct sock *sk, struct sock *peersk)
{
	put_pid(sk->sk_peer_pid);
	if (sk->sk_peer_cred)
		put_cred(sk->sk_peer_cred);
	sk->sk_peer_pid  = get_pid(peersk->sk_peer_pid);
	sk->sk_peer_cred = get_cred(peersk->sk_peer_cred);
}

static int bus_listen(struct socket *sock, int backlog)
{
	int err;
	struct sock *sk = sock->sk;
	struct bus_sock *u = bus_sk(sk);
	struct pid *old_pid = NULL;
	const struct cred *old_cred = NULL;

	err = -EINVAL;
	if (!u->addr || !u->bus_master)
		goto out;	/* Only listens on an bound an master socket */
	bus_state_lock(sk);
	if (sk->sk_state != BUS_CLOSE && sk->sk_state != BUS_LISTEN)
		goto out_unlock;
	if (backlog > sk->sk_max_ack_backlog)
		wake_up_interruptible_all(&u->peer_wait);
	sk->sk_max_ack_backlog	= backlog;
	sk->sk_state		= BUS_LISTEN;
	/* set credentials so connect can copy them */
	init_peercred(sk);
	err = 0;

out_unlock:
	bus_state_unlock(sk);
	put_pid(old_pid);
	if (old_cred)
		put_cred(old_cred);
out:
	return err;
}

static int bus_release(struct socket *);
static int bus_bind(struct socket *, struct sockaddr *, int);
static int bus_connect(struct socket *, struct sockaddr *,
			       int addr_len, int flags);
static int bus_accept(struct socket *, struct socket *, int);
static int bus_getname(struct socket *, struct sockaddr *, int *, int);
static unsigned int bus_poll(struct file *, struct socket *,
				    poll_table *);
static int bus_ioctl(struct socket *, unsigned int, unsigned long);
static int bus_shutdown(struct socket *, int);
static int bus_setsockopt(struct socket *, int, int, char __user *,
			   unsigned int);
static int bus_sendmsg(struct kiocb *, struct socket *,
		       struct msghdr *, size_t);
static int bus_recvmsg(struct kiocb *, struct socket *,
		       struct msghdr *, size_t, int);

static void bus_set_peek_off(struct sock *sk, int val)
{
	struct bus_sock *u = bus_sk(sk);

	mutex_lock(&u->readlock);
	sk->sk_peek_off = val;
	mutex_unlock(&u->readlock);
}

static const struct proto_ops bus_seqpacket_ops = {
	.family =	PF_BUS,
	.owner =	THIS_MODULE,
	.release =	bus_release,
	.bind =		bus_bind,
	.connect =	bus_connect,
	.socketpair =	sock_no_socketpair,
	.accept =	bus_accept,
	.getname =	bus_getname,
	.poll =		bus_poll,
	.ioctl =	bus_ioctl,
	.listen =	bus_listen,
	.shutdown =	bus_shutdown,
	.setsockopt =	bus_setsockopt,
	.getsockopt =	sock_no_getsockopt,
	.sendmsg =	bus_sendmsg,
	.recvmsg =	bus_recvmsg,
	.mmap =		sock_no_mmap,
	.sendpage =	sock_no_sendpage,
	.set_peek_off =	bus_set_peek_off,
};

static struct proto bus_proto = {
	.name			= "BUS",
	.owner			= THIS_MODULE,
	.obj_size		= sizeof(struct bus_sock),
};

/*
 * AF_BUS sockets do not interact with hardware, hence they
 * dont trigger interrupts - so it's safe for them to have
 * bh-unsafe locking for their sk_receive_queue.lock. Split off
 * this special lock-class by reinitializing the spinlock key:
 */
static struct lock_class_key af_bus_sk_receive_queue_lock_key;

static struct sock *bus_create1(struct net *net, struct socket *sock)
{
	struct sock *sk = NULL;
	struct bus_sock *u;

	atomic_long_inc(&bus_nr_socks);
	if (atomic_long_read(&bus_nr_socks) > 2 * get_max_files())
		goto out;

	sk = sk_alloc(net, PF_BUS, GFP_KERNEL, &bus_proto);
	if (!sk)
		goto out;

	sock_init_data(sock, sk);
	lockdep_set_class(&sk->sk_receive_queue.lock,
				&af_bus_sk_receive_queue_lock_key);

	sk->sk_write_space	= bus_write_space;
	sk->sk_max_ack_backlog	= BUS_MAX_QLEN;
	sk->sk_destruct		= bus_sock_destructor;
	u	  = bus_sk(sk);
	u->path.dentry = NULL;
	u->path.mnt = NULL;
	u->bus = NULL;
	u->bus_master = false;
	u->authenticated = false;
	u->eavesdropper = false;
	spin_lock_init(&u->lock);
	atomic_long_set(&u->inflight, 0);
	INIT_LIST_HEAD(&u->link);
	INIT_HLIST_HEAD(&u->addr_list);
	INIT_HLIST_NODE(&u->bus_node);
	mutex_init(&u->readlock); /* single task reading lock */
	init_waitqueue_head(&u->peer_wait);
	bus_insert_socket(bus_sockets_unbound, sk);
out:
	if (sk == NULL)
		atomic_long_dec(&bus_nr_socks);
	else {
		local_bh_disable();
		sock_prot_inuse_add(sock_net(sk), sk->sk_prot, 1);
		local_bh_enable();
	}
	return sk;
}

static int bus_create(struct net *net, struct socket *sock, int protocol,
		       int kern)
{
	struct sock *sk;

	if (protocol < BUS_PROTO_NONE || protocol > BUS_PROTO_DBUS)
		return -EPROTONOSUPPORT;

	if (protocol != BUS_PROTO_NONE)
		request_module("net-pf-%d-proto-%d", PF_BUS, protocol);

	sock->state = SS_UNCONNECTED;

	if (sock->type == SOCK_SEQPACKET)
		sock->ops = &bus_seqpacket_ops;
	else
		return -ESOCKTNOSUPPORT;

	sk = bus_create1(net, sock);
	if (!sk)
		return -ENOMEM;

	sk->sk_protocol = protocol;

	return 0;
}

static int bus_release(struct socket *sock)
{
	struct sock *sk = sock->sk;

	if (!sk)
		return 0;

	sock->sk = NULL;

	return bus_release_sock(sk, 0);
}

static struct sock *bus_find_other(struct net *net,
				   struct sockaddr_bus *sbusname, int len,
				   int protocol, unsigned int hash, int *error)
{
	struct sock *u;
	struct path path;
	int err = 0;

	if (sbusname->sbus_path[0]) {
		struct inode *inode;
		err = kern_path(sbusname->sbus_path, LOOKUP_FOLLOW, &path);
		if (err)
			goto fail;
		inode = path.dentry->d_inode;
		err = inode_permission(inode, MAY_WRITE);
		if (err)
			goto put_fail;

		err = -ECONNREFUSED;
		if (!S_ISSOCK(inode->i_mode))
			goto put_fail;
		u = bus_find_socket_byaddress(net, sbusname, len, protocol,
					      hash);
		if (!u)
			goto put_fail;

		touch_atime(&path);
		path_put(&path);

	} else {
		err = -ECONNREFUSED;
		u = bus_find_socket_byaddress(net, sbusname, len, protocol, hash);
		if (u) {
			struct dentry *dentry;
			dentry = bus_sk(u)->path.dentry;
			if (dentry)
				touch_atime(&bus_sk(u)->path);
		} else
			goto fail;
	}

	return u;

put_fail:
	path_put(&path);
fail:
	*error = err;
	return NULL;
}


static int bus_bind(struct socket *sock, struct sockaddr *uaddr, int addr_len)
{
	struct sock *sk = sock->sk;
	struct net *net = sock_net(sk);
	struct bus_sock *u = bus_sk(sk);
	struct sockaddr_bus *sbusaddr = (struct sockaddr_bus *)uaddr;
	char *sbus_path = sbusaddr->sbus_path;
	struct dentry *dentry = NULL;
	struct path path;
	int err;
	unsigned int hash;
	struct bus_address *addr;
	struct hlist_head *list;
	struct bus *bus;

	err = -EINVAL;
	if (sbusaddr->sbus_family != AF_BUS)
		goto out;

	/* If the address is available, the socket is the bus master */
	sbusaddr->sbus_addr.s_addr = BUS_MASTER_ADDR;

	err = bus_mkname(sbusaddr, addr_len, &hash);
	if (err < 0)
		goto out;
	addr_len = err;

	mutex_lock(&u->readlock);

	err = -EINVAL;
	if (u->addr)
		goto out_up;

	err = -ENOMEM;
	addr = kzalloc(sizeof(*addr) + sizeof(struct sockaddr_bus), GFP_KERNEL);
	if (!addr)
		goto out_up;

	memcpy(addr->name, sbusaddr, sizeof(struct sockaddr_bus));
	addr->len = addr_len;
	addr->hash = hash;
	atomic_set(&addr->refcnt, 1);
	addr->sock = sk;
	INIT_HLIST_NODE(&addr->addr_node);
	INIT_HLIST_NODE(&addr->table_node);

	if (sbus_path[0]) {
		umode_t mode;
		err = 0;
		/*
		 * Get the parent directory, calculate the hash for last
		 * component.
		 */
		dentry = kern_path_create(AT_FDCWD, sbus_path, &path, 0);
		err = PTR_ERR(dentry);
		if (IS_ERR(dentry))
			goto out_mknod_parent;

		/*
		 * All right, let's create it.
		 */
		mode = S_IFSOCK |
		       (SOCK_INODE(sock)->i_mode & ~current_umask());
		err = mnt_want_write(path.mnt);
		if (err)
			goto out_mknod_dput;
		err = security_path_mknod(&path, dentry, mode, 0);
		if (err)
			goto out_mknod_drop_write;
		err = vfs_mknod(path.dentry->d_inode, dentry, mode, 0);
out_mknod_drop_write:
		mnt_drop_write(path.mnt);
		if (err)
			goto out_mknod_dput;
		mutex_unlock(&path.dentry->d_inode->i_mutex);
		dput(path.dentry);
		path.dentry = dentry;
	}

	err = -ENOMEM;
	bus = kzalloc(sizeof(*bus), GFP_KERNEL);
	if (!bus)
		goto out_unlock;

	spin_lock(&bus_table_lock);

	if (!sbus_path[0]) {
		err = -EADDRINUSE;
		if (__bus_find_socket_byname(net, sbusaddr, addr_len, hash)) {
			bus_release_addr(addr);
			kfree(bus);
			goto out_unlock;
		}

		list = &bus_socket_table[addr->hash];
	} else {
		list = &bus_socket_table[dentry->d_inode->i_ino &
					 (BUS_HASH_SIZE-1)];
		u->path = path;
	}

	kref_init(&bus->kref);
	bus->master = sk;
	INIT_HLIST_HEAD(&bus->peers);
	spin_lock_init(&bus->lock);
	spin_lock_init(&bus->send_lock);
	atomic64_set(&bus->addr_cnt, 0);
	atomic64_set(&bus->eavesdropper_cnt, 0);

	hlist_add_head(&addr->addr_node, &u->addr_list);

	err = 0;
	__bus_remove_socket(sk);
	u->addr = addr;
	u->bus_master = true;
	u->bus = bus;
	__bus_insert_socket(list, sk);
	bus_insert_address(&bus_address_table[addr->hash], addr);

out_unlock:
	spin_unlock(&bus_table_lock);
out_up:
	mutex_unlock(&u->readlock);
out:
	return err;

out_mknod_dput:
	dput(dentry);
	mutex_unlock(&path.dentry->d_inode->i_mutex);
	path_put(&path);
out_mknod_parent:
	if (err == -EEXIST)
		err = -EADDRINUSE;
	bus_release_addr(addr);
	goto out_up;
}

static long bus_wait_for_peer(struct sock *other, long timeo)
{
	struct bus_sock *u = bus_sk(other);
	int sched;
	DEFINE_WAIT(wait);

	prepare_to_wait_exclusive(&u->peer_wait, &wait, TASK_INTERRUPTIBLE);

	sched = !sock_flag(other, SOCK_DEAD) &&
		!(other->sk_shutdown & RCV_SHUTDOWN) &&
		bus_recvq_full(other);

	bus_state_unlock(other);

	if (sched)
		timeo = schedule_timeout(timeo);

	finish_wait(&u->peer_wait, &wait);
	return timeo;
}

static int bus_connect(struct socket *sock, struct sockaddr *uaddr,
			       int addr_len, int flags)
{
	struct sockaddr_bus *sbusaddr = (struct sockaddr_bus *)uaddr;
	struct sock *sk = sock->sk;
	struct net *net = sock_net(sk);
	struct bus_sock *u = bus_sk(sk), *newu, *otheru;
	struct sock *newsk = NULL;
	struct sock *other = NULL;
	struct sk_buff *skb = NULL;
	struct bus_address *addr = NULL;
	unsigned int hash;
	int st;
	int err;
	long timeo;

	/* Only connections to the bus master is allowed */
	sbusaddr->sbus_addr.s_addr = BUS_MASTER_ADDR;

	err = bus_mkname(sbusaddr, addr_len, &hash);
	if (err < 0)
		goto out;
	addr_len = err;

	err = -ENOMEM;
	addr = kzalloc(sizeof(*addr) + sizeof(struct sockaddr_bus), GFP_KERNEL);
	if (!addr)
		goto out;

	atomic_set(&addr->refcnt, 1);
	INIT_HLIST_NODE(&addr->addr_node);
	INIT_HLIST_NODE(&addr->table_node);

	timeo = sock_sndtimeo(sk, flags & O_NONBLOCK);

	/* First of all allocate resources.
	   If we will make it after state is locked,
	   we will have to recheck all again in any case.
	 */

	err = -ENOMEM;

	/* create new sock for complete connection */
	newsk = bus_create1(sock_net(sk), NULL);
	if (newsk == NULL)
		goto out;

	/* Allocate skb for sending to listening sock */
	skb = sock_wmalloc(newsk, 1, 0, GFP_KERNEL);
	if (skb == NULL)
		goto out;

restart:
	/*  Find listening sock. */
	other = bus_find_other(net, sbusaddr, addr_len, sk->sk_protocol, hash,
			       &err);
	if (!other)
		goto out;

	/* Latch state of peer */
	bus_state_lock(other);

	/* Apparently VFS overslept socket death. Retry. */
	if (sock_flag(other, SOCK_DEAD)) {
		bus_state_unlock(other);
		sock_put(other);
		goto restart;
	}

	err = -ECONNREFUSED;
	if (other->sk_state != BUS_LISTEN)
		goto out_unlock;
	if (other->sk_shutdown & RCV_SHUTDOWN)
		goto out_unlock;

	if (bus_recvq_full(other)) {
		err = -EAGAIN;
		if (!timeo)
			goto out_unlock;

		timeo = bus_wait_for_peer(other, timeo);

		err = sock_intr_errno(timeo);
		if (signal_pending(current))
			goto out;
		sock_put(other);
		goto restart;
	}

	/* Latch our state.

	   It is tricky place. We need to grab our state lock and cannot
	   drop lock on peer. It is dangerous because deadlock is
	   possible. Connect to self case and simultaneous
	   attempt to connect are eliminated by checking socket
	   state. other is BUS_LISTEN, if sk is BUS_LISTEN we
	   check this before attempt to grab lock.

	   Well, and we have to recheck the state after socket locked.
	 */
	st = sk->sk_state;

	switch (st) {
	case BUS_CLOSE:
		/* This is ok... continue with connect */
		break;
	case BUS_ESTABLISHED:
		/* Socket is already connected */
		err = -EISCONN;
		goto out_unlock;
	default:
		err = -EINVAL;
		goto out_unlock;
	}

	bus_state_lock_nested(sk);

	if (sk->sk_state != st) {
		bus_state_unlock(sk);
		bus_state_unlock(other);
		sock_put(other);
		goto restart;
	}

	err = security_bus_connect(sk, other, newsk);
	if (err) {
		bus_state_unlock(sk);
		goto out_unlock;
	}

	/* The way is open! Fastly set all the necessary fields... */

	sock_hold(sk);
	bus_peer(newsk)	= sk;
	newsk->sk_state		= BUS_ESTABLISHED;
	newsk->sk_type		= sk->sk_type;
	newsk->sk_protocol	= sk->sk_protocol;
	init_peercred(newsk);
	newu = bus_sk(newsk);
	RCU_INIT_POINTER(newsk->sk_wq, &newu->peer_wq);
	otheru = bus_sk(other);

	/* copy address information from listening to new sock*/
	if (otheru->addr && otheru->bus_master) {
		atomic_inc(&otheru->addr->refcnt);
		newu->addr = otheru->addr;
		memcpy(addr->name, otheru->addr->name,
		       sizeof(struct sockaddr_bus));
		addr->len = otheru->addr->len;
		addr->name->sbus_addr.s_addr =
			(atomic64_inc_return(&otheru->bus->addr_cnt) &
			 BUS_CLIENT_MASK);
		addr->hash = bus_compute_hash(addr->name->sbus_addr);
		addr->sock = sk;
		u->addr = addr;
		kref_get(&otheru->bus->kref);
		u->bus = otheru->bus;
		u->bus_master_side = false;
		kref_get(&otheru->bus->kref);
		newu->bus = otheru->bus;
		newu->bus_master_side = true;
		hlist_add_head(&addr->addr_node, &u->addr_list);

		bus_insert_address(&bus_address_table[addr->hash], addr);
	}
	if (otheru->path.dentry) {
		path_get(&otheru->path);
		newu->path = otheru->path;
	}

	/* Set credentials */
	copy_peercred(sk, other);
	sk->sk_sndbuf = other->sk_sndbuf;
	sk->sk_max_ack_backlog	= other->sk_max_ack_backlog;
	newsk->sk_sndbuf = other->sk_sndbuf;

	sock->state	= SS_CONNECTED;
	sk->sk_state	= BUS_ESTABLISHED;
	sock_hold(newsk);

	smp_mb__after_atomic_inc();	/* sock_hold() does an atomic_inc() */
	bus_peer(sk)	= newsk;

	bus_state_unlock(sk);

	/* take ten and and send info to listening sock */
	spin_lock(&other->sk_receive_queue.lock);
	__skb_queue_tail(&other->sk_receive_queue, skb);
	spin_unlock(&other->sk_receive_queue.lock);
	bus_state_unlock(other);
	other->sk_data_ready(other, 0);
	sock_put(other);
	return 0;

out_unlock:
	if (other)
		bus_state_unlock(other);

out:
	kfree_skb(skb);
	if (addr)
		bus_release_addr(addr);
	if (newsk)
		bus_release_sock(newsk, 0);
	if (other)
		sock_put(other);
	return err;
}

static int bus_accept(struct socket *sock, struct socket *newsock, int flags)
{
	struct sock *sk = sock->sk;
	struct sock *tsk;
	struct sk_buff *skb;
	int err;

	err = -EINVAL;
	if (sk->sk_state != BUS_LISTEN)
		goto out;

	/* If socket state is BUS_LISTEN it cannot change (for now...),
	 * so that no locks are necessary.
	 */

	skb = skb_recv_datagram(sk, 0, flags&O_NONBLOCK, &err);
	if (!skb) {
		/* This means receive shutdown. */
		if (err == 0)
			err = -EINVAL;
		goto out;
	}

	tsk = skb->sk;
	skb_free_datagram(sk, skb);
	wake_up_interruptible(&bus_sk(sk)->peer_wait);

	/* attach accepted sock to socket */
	bus_state_lock(tsk);
	newsock->state = SS_CONNECTED;
	sock_graft(tsk, newsock);
	bus_state_unlock(tsk);
	return 0;

out:
	return err;
}


static int bus_getname(struct socket *sock, struct sockaddr *uaddr,
		       int *uaddr_len, int peer)
{
	struct sock *sk = sock->sk;
	struct bus_sock *u;
	DECLARE_SOCKADDR(struct sockaddr_bus *, sbusaddr, uaddr);
	int err = 0;

	if (peer) {
		sk = bus_peer_get(sk);

		err = -ENOTCONN;
		if (!sk)
			goto out;
		err = 0;
	} else {
		sock_hold(sk);
	}

	u = bus_sk(sk);

	bus_state_lock(sk);
	if (!u->addr) {
		sbusaddr->sbus_family = AF_BUS;
		sbusaddr->sbus_path[0] = 0;
		*uaddr_len = sizeof(short);
	} else {
		struct bus_address *addr = u->addr;

		*uaddr_len = sizeof(struct sockaddr_bus);
		memcpy(sbusaddr, addr->name, *uaddr_len);
	}
	bus_state_unlock(sk);
	sock_put(sk);
out:
	return err;
}

static void bus_detach_fds(struct scm_cookie *scm, struct sk_buff *skb)
{
	int i;

	scm->fp = BUSCB(skb).fp;
	BUSCB(skb).fp = NULL;

	for (i = scm->fp->count-1; i >= 0; i--)
		bus_notinflight(scm->fp->fp[i]);
}

static void bus_destruct_scm(struct sk_buff *skb)
{
	struct scm_cookie scm;
	memset(&scm, 0, sizeof(scm));
	scm.pid  = BUSCB(skb).pid;
	scm.cred = BUSCB(skb).cred;
	if (BUSCB(skb).fp)
		bus_detach_fds(&scm, skb);

	scm_destroy(&scm);
	if (skb->sk)
		sock_wfree(skb);
}

#define MAX_RECURSION_LEVEL 4

static int bus_attach_fds(struct scm_cookie *scm, struct sk_buff *skb)
{
	int i;
	unsigned char max_level = 0;
	int bus_sock_count = 0;

	for (i = scm->fp->count - 1; i >= 0; i--) {
		struct sock *sk = bus_get_socket(scm->fp->fp[i]);

		if (sk) {
			bus_sock_count++;
			max_level = max(max_level,
					bus_sk(sk)->recursion_level);
		}
	}
	if (unlikely(max_level > MAX_RECURSION_LEVEL))
		return -ETOOMANYREFS;

	/*
	 * Need to duplicate file references for the sake of garbage
	 * collection.  Otherwise a socket in the fps might become a
	 * candidate for GC while the skb is not yet queued.
	 */
	BUSCB(skb).fp = scm_fp_dup(scm->fp);
	if (!BUSCB(skb).fp)
		return -ENOMEM;

	if (bus_sock_count) {
		for (i = scm->fp->count - 1; i >= 0; i--)
			bus_inflight(scm->fp->fp[i]);
	}
	return max_level;
}

static int bus_scm_to_skb(struct scm_cookie *scm, struct sk_buff *skb,
			  bool send_fds)
{
	int err = 0;

	BUSCB(skb).pid  = get_pid(scm->pid);
	if (scm->cred)
		BUSCB(skb).cred = get_cred(scm->cred);
	BUSCB(skb).fp = NULL;
	if (scm->fp && send_fds)
		err = bus_attach_fds(scm, skb);

	skb->destructor = bus_destruct_scm;
	return err;
}

/*
 * Some apps rely on write() giving SCM_CREDENTIALS
 * We include credentials if source or destination socket
 * asserted SOCK_PASSCRED.
 */
static void maybe_add_creds(struct sk_buff *skb, const struct socket *sock,
			    const struct sock *other)
{
	if (BUSCB(skb).cred)
		return;
	if (test_bit(SOCK_PASSCRED, &sock->flags) ||
	    !other->sk_socket ||
	    test_bit(SOCK_PASSCRED, &other->sk_socket->flags)) {
		BUSCB(skb).pid  = get_pid(task_tgid(current));
		BUSCB(skb).cred = get_current_cred();
	}
}

/*
 *	Send AF_BUS data.
 */

static void bus_deliver_skb(struct sk_buff *skb)
{
	struct bus_send_context *sendctx = BUSCB(skb).sendctx;
	struct socket *sock = sendctx->sender_socket;

	if (sock_flag(sendctx->other, SOCK_RCVTSTAMP))
		__net_timestamp(skb);
	maybe_add_creds(skb, sock, sendctx->other);
	skb_queue_tail(&sendctx->other->sk_receive_queue, skb);
	if (sendctx->max_level > bus_sk(sendctx->other)->recursion_level)
		bus_sk(sendctx->other)->recursion_level = sendctx->max_level;
}

/**
 * bus_sendmsg_finish - delivery an skb to a destination
 * @skb: sk_buff to deliver
 *
 * Delivers a packet to a destination. The skb control buffer has
 * all the information about the destination contained on sending
 * context. If the sending is unicast, then the skb is delivered
 * and the receiver notified but if the sending is multicast, the
 * skb is just marked as delivered and the actual delivery is made
 * outside the function with the bus->send_lock held to ensure that
 * the multicast sending is atomic.
 */
static int bus_sendmsg_finish(struct sk_buff *skb)
{
	int err;
	struct bus_send_context *sendctx;
	struct socket *sock;
	struct sock *sk;
	struct net *net;
	size_t len = skb->len;

	sendctx = BUSCB(skb).sendctx;
	sock = sendctx->sender_socket;
	sk = sock->sk;
	net = sock_net(sk);

restart:
	if (!sendctx->other) {
		err = -ECONNRESET;
		if (sendctx->recipient == NULL)
			goto out_free;

		sendctx->other = bus_find_other(net, sendctx->recipient,
						sendctx->namelen,
						sk->sk_protocol,
						sendctx->hash, &err);

		if (sendctx->other == NULL ||
		    !bus_sk(sendctx->other)->authenticated) {

			if (sendctx->other)
				sock_put(sendctx->other);

			if (!bus_sk(sk)->bus_master_side) {
				err = -ENOTCONN;
				sendctx->other = bus_peer_get(sk);
				if (!sendctx->other)
					goto out_free;
			} else {
				sendctx->other = sk;
				sock_hold(sendctx->other);
			}
		}
	}

	if (sk_filter(sendctx->other, skb) < 0) {
		/* Toss the packet but do not return any error to the sender */
		err = len;
		goto out_free;
	}

	bus_state_lock(sendctx->other);

	if (sock_flag(sendctx->other, SOCK_DEAD)) {
		/*
		 *	Check with 1003.1g - what should
		 *	datagram error
		 */
		bus_state_unlock(sendctx->other);
		sock_put(sendctx->other);

		err = 0;
		bus_state_lock(sk);
		if (bus_peer(sk) == sendctx->other) {
			bus_peer(sk) = NULL;
			bus_state_unlock(sk);
			sock_put(sendctx->other);
			err = -ECONNREFUSED;
		} else {
			bus_state_unlock(sk);
		}

		sendctx->other = NULL;
		if (err)
			goto out_free;
		goto restart;
	}

	err = -EPIPE;
	if (sendctx->other->sk_shutdown & RCV_SHUTDOWN)
		goto out_unlock;

	if (bus_recvq_full(sendctx->other)) {
		if (!sendctx->timeo) {
			err = -EAGAIN;
			goto out_unlock;
		}

		sendctx->timeo = bus_wait_for_peer(sendctx->other,
						   sendctx->timeo);

		err = sock_intr_errno(sendctx->timeo);
		if (signal_pending(current))
			goto out_free;

		goto restart;
	}

	if (!sendctx->multicast && !sendctx->eavesdropper) {
		bus_deliver_skb(skb);
		bus_state_unlock(sendctx->other);
		sendctx->other->sk_data_ready(sendctx->other, 0);
		sock_put(sendctx->other);
	} else {
		sendctx->deliver = 1;
		bus_state_unlock(sendctx->other);
	}

	return len;

out_unlock:
	bus_state_unlock(sendctx->other);
out_free:
	kfree_skb(skb);
	if (sendctx->other)
		sock_put(sendctx->other);

	return err;
}

/**
 * bus_sendmsg_mcast - do a multicast sending
 * @skb: sk_buff to deliver
 *
 * Send a packet to a multicast destination.
 * The function is also called for unicast sending when eavesdropping
 * is enabled. Since the unicast destination and the eavesdroppers
 * have to receive the packet atomically.
 */
static int bus_sendmsg_mcast(struct sk_buff *skb)
{
	struct bus_send_context *sendctx;
	struct bus_send_context *tmpctx;
	struct socket *sock;
	struct sock *sk;
	struct net *net;
	struct bus_sock *u, *s;
	struct hlist_node *node;
	u16 prefix = 0;
	struct sk_buff **skb_set = NULL;
	struct bus_send_context **sendctx_set = NULL;
	int  rcp_cnt, send_cnt;
	int i;
	int err;
	int len = skb->len;
	bool (*is_receiver) (struct sock *, u16);
	bool main_rcp_found = false;

	sendctx = BUSCB(skb).sendctx;
	sendctx->deliver = 0;
	sock = sendctx->sender_socket;
	sk = sock->sk;
	u = bus_sk(sk);
	net = sock_net(sk);

	if (sendctx->multicast) {
		prefix = bus_addr_prefix(sendctx->recipient);
		if (sendctx->eavesdropper)
			is_receiver = &bus_has_prefix_eavesdropper;
		else
			is_receiver = &bus_has_prefix;
	} else {
		is_receiver = &bus_eavesdropper;

		/*
		 * If the destination is not the peer accepted socket
		 * we have to get the correct destination.
		 */
		if (!sendctx->to_master && sendctx->recipient) {
			sendctx->other = bus_find_other(net, sendctx->recipient,
							sendctx->namelen,
							sk->sk_protocol,
							sendctx->hash, &err);


			if (sendctx->other == NULL ||
			    !bus_sk(sendctx->other)->authenticated) {

				if (sendctx->other)
					sock_put(sendctx->other);

				if (sendctx->other == NULL) {
					if (!bus_sk(sk)->bus_master_side) {
						err = -ENOTCONN;
						sendctx->other = bus_peer_get(sk);
						if (!sendctx->other)
							goto out;
					} else {
						sendctx->other = sk;
						sock_hold(sendctx->other);
					}
				}
				sendctx->to_master = 1;
			}
		}
	}


try_again:
	rcp_cnt = 0;
	main_rcp_found = false;

	spin_lock(&u->bus->lock);

	hlist_for_each_entry(s, node, &u->bus->peers, bus_node) {

		if (!net_eq(sock_net(&s->sk), net))
			continue;

		if (is_receiver(&s->sk, prefix) ||
		    (!sendctx->multicast &&
		     !sendctx->to_master &&
		     &s->sk == sendctx->other))
			rcp_cnt++;
	}

	spin_unlock(&u->bus->lock);

	/*
	 * Memory can't be allocated while holding a spinlock so
	 * we have to release the lock, do the allocation for the
	 * array to store each destination peer sk_buff and grab
	 * the bus peer lock again. Peers could have joined the
	 * bus while we relesed the lock so we allocate 5 more
	 * recipients hoping that this will be enough to not having
	 * to try again in case only a few peers joined the bus.
	 */
	rcp_cnt += 5;
	skb_set = kzalloc(sizeof(struct sk_buff *) * rcp_cnt, GFP_KERNEL);

	if (!skb_set) {
		err = -ENOMEM;
		goto out;
	}

	sendctx_set = kzalloc(sizeof(struct bus_send_context *) * rcp_cnt,
			      GFP_KERNEL);
	if (!sendctx_set) {
		err = -ENOMEM;
		goto out;
	}

	for (i = 0; i < rcp_cnt; i++) {
		skb_set[i] = skb_clone(skb, GFP_KERNEL);
		if (!skb_set[i]) {
			err = -ENOMEM;
			goto out_free;
		}
		sendctx_set[i] = BUSCB(skb_set[i]).sendctx
			= kmalloc(sizeof(*sendctx) * rcp_cnt, GFP_KERNEL);
		if (!sendctx_set[i]) {
			err = -ENOMEM;
			goto out_free;
		}
		memcpy(sendctx_set[i], sendctx, sizeof(*sendctx));
		err = bus_scm_to_skb(sendctx_set[i]->siocb->scm,
				     skb_set[i], true);
		if (err < 0)
			goto out_free;
		bus_get_secdata(sendctx_set[i]->siocb->scm,
				skb_set[i]);

		sendctx_set[i]->other = NULL;
	}

	send_cnt = 0;

	spin_lock(&u->bus->lock);

	hlist_for_each_entry(s, node, &u->bus->peers, bus_node) {

		if (!net_eq(sock_net(&s->sk), net))
			continue;

		if (send_cnt >= rcp_cnt) {
			spin_unlock(&u->bus->lock);

			for (i = 0; i < rcp_cnt; i++) {
				sock_put(sendctx_set[i]->other);
				kfree_skb(skb_set[i]);
				kfree(sendctx_set[i]);
			}
			kfree(skb_set);
			kfree(sendctx_set);
			sendctx_set = NULL;
			skb_set = NULL;
			goto try_again;
		}

		if (is_receiver(&s->sk, prefix) ||
		    (!sendctx->multicast &&
		     !sendctx->to_master &&
		     &s->sk == sendctx->other)) {
			skb_set_owner_w(skb_set[send_cnt], &s->sk);
			tmpctx = BUSCB(skb_set[send_cnt]).sendctx;
			sock_hold(&s->sk);
			if (&s->sk == sendctx->other) {
				tmpctx->main_recipient = 1;
				main_rcp_found = true;
			}
			tmpctx->other = &s->sk;
			tmpctx->recipient = s->addr->name;
			tmpctx->eavesdropper = bus_eavesdropper(&s->sk, 0);

			send_cnt++;
		}
	}

	spin_unlock(&u->bus->lock);

	/*
	 * Peers have left the bus so we have to free
	 * their pre-allocated bus_send_context and
	 * socket buffers.
	 */
	if (send_cnt < rcp_cnt) {
		for (i = send_cnt; i < rcp_cnt; i++) {
			kfree_skb(skb_set[i]);
			kfree(sendctx_set[i]);
		}
		rcp_cnt = send_cnt;
	}

	for (i = 0; i < send_cnt; i++) {
		tmpctx = BUSCB(skb_set[i]).sendctx;
		tmpctx->deliver = 0;
		err = NF_HOOK(NFPROTO_BUS, NF_BUS_SENDING, skb_set[i],
			      NULL, NULL, bus_sendmsg_finish);
		if (err == -EPERM)
			sock_put(tmpctx->other);
	}

	/*
	 * If the send context is not multicast, the destination
	 * coud be either the peer accepted socket descriptor or
	 * a peer that is not an eavesdropper. If the peer is not
	 * the accepted socket descriptor and has been authenticated,
	 * it is a member of the bus peer list so it has already been
	 * marked for delivery.
	 * But if the destination is the accepted socket descriptor
	 * or is a non-authenticated peer it is not a member of the
	 * bus peer list so the packet has to be explicitly deliver
	 * to it.
	 */

	if (!sendctx->multicast &&
	    (sendctx->to_master ||
	     (sendctx->bus_master_side && !main_rcp_found))) {
		sendctx->main_recipient = 1;
		err = NF_HOOK(NFPROTO_BUS, NF_BUS_SENDING, skb, NULL, NULL,
			bus_sendmsg_finish);
		if (err == -EPERM)
			sock_put(sendctx->other);
	}

	spin_lock(&u->bus->send_lock);

	for (i = 0; i < send_cnt; i++) {
		tmpctx = sendctx_set[i];
		if (tmpctx->deliver != 1)
			continue;

		bus_state_lock(tmpctx->other);
		bus_deliver_skb(skb_set[i]);
		bus_state_unlock(tmpctx->other);
	}

	if (!sendctx->multicast &&
	    sendctx->deliver == 1 &&
	    !bus_sk(sendctx->other)->eavesdropper) {
		bus_state_lock(sendctx->other);
		bus_deliver_skb(skb);
		bus_state_unlock(sendctx->other);
	}

	spin_unlock(&u->bus->send_lock);

	for (i = 0; i < send_cnt; i++) {
		tmpctx = sendctx_set[i];
		if (tmpctx->deliver != 1)
			continue;

		tmpctx->other->sk_data_ready(tmpctx->other, 0);
		sock_put(tmpctx->other);
	}

	if (!sendctx->multicast &&
	    sendctx->deliver == 1 &&
	    !bus_sk(sendctx->other)->eavesdropper) {
		sendctx->other->sk_data_ready(sendctx->other, 0);
		sock_put(sendctx->other);
	}

	err = len;
	goto out;

out_free:
	for (i = 0; i < rcp_cnt; i++) {
		if (skb_set[i])
			kfree_skb(skb_set[i]);
	}

out:
	kfree(skb_set);
	if (sendctx_set) {
		for (i = 0; i < rcp_cnt; i++)
			kfree(sendctx_set[i]);
		kfree(sendctx_set);
	}

	if (sendctx->deliver == 0) {
		if (!sendctx->to_master &&
		    !(sendctx->bus_master_side && !main_rcp_found))
			kfree_skb(skb);
		if (!sendctx->to_master &&
		    !(sendctx->bus_master_side && !main_rcp_found))
			if (sendctx->other)
				sock_put(sendctx->other);
	}
	scm_destroy(sendctx->siocb->scm);

	return err;
}

static inline void bus_copy_path(struct sockaddr_bus *dest,
				 struct sockaddr_bus *src)
{
	int offset;

	/*
	 * abstract path names start with a null byte character,
	 * so they have to be compared starting at the second char.
	 */
	offset = (src->sbus_path[0] == '\0');

	strncpy(dest->sbus_path + offset,
		src->sbus_path + offset,
		BUS_PATH_MAX);
}

/**
 * bus_sendmsg - send an skb to a destination
 * @kiocb: I/O control block info
 * @sock: sender socket
 * @msg: message header
 * @len: message length
 *
 * Send an socket buffer to a destination. The destination could be
 * either an unicast or a multicast address. In any case, a copy of
 * the packet has to be send to all the sockets that are allowed to
 * eavesdrop the communication bus.
 *
 * If the destination address is not associated with any socket, the
 * packet is default routed to the bus master (the sender accepted
 * socket).
 *
 * The af_bus sending path is hooked to the netfilter subsystem so
 * netfilter hooks can filter or modify the packet before delivery.
 */
static int bus_sendmsg(struct kiocb *kiocb, struct socket *sock,
				struct msghdr *msg, size_t len)
{
	struct sock *sk = sock->sk;
	struct bus_sock *u = bus_sk(sk);
	struct sockaddr_bus *sbusaddr = msg->msg_name;
	int err;
	struct sk_buff *skb;
	struct scm_cookie tmp_scm;
	bool to_master = false;
	bool multicast = false;
	struct bus_send_context sendctx;

	err = sock_error(sk);
	if (err)
		return err;

	if (sk->sk_state != BUS_ESTABLISHED)
		return -ENOTCONN;

	if (!msg->msg_namelen)
		sbusaddr = NULL;

	if (sbusaddr)
		bus_copy_path(sbusaddr, u->addr->name);

	if ((!sbusaddr && !u->bus_master_side) ||
	    (sbusaddr && sbusaddr->sbus_addr.s_addr == BUS_MASTER_ADDR))
		to_master = true;
	else if (sbusaddr && !u->bus_master_side && !u->authenticated)
		return -EHOSTUNREACH;

	sendctx.namelen = 0; /* fake GCC */
	sendctx.siocb = kiocb_to_siocb(kiocb);
	sendctx.other = NULL;

	if (NULL == sendctx.siocb->scm)
		sendctx.siocb->scm = &tmp_scm;
	wait_for_bus_gc();
	err = scm_send(sock, msg, sendctx.siocb->scm, false);
	if (err < 0)
		return err;

	err = -EOPNOTSUPP;
	if (msg->msg_flags&MSG_OOB)
		goto out;

	if (sbusaddr && !to_master) {
		err = bus_mkname(sbusaddr, msg->msg_namelen, &sendctx.hash);
		if (err < 0)
			goto out;
		sendctx.namelen = err;
		multicast = bus_mc_addr(sbusaddr);
	} else {
		err = -ENOTCONN;
		sendctx.other = bus_peer_get(sk);
		if (!sendctx.other)
			goto out;
	}

	err = -EMSGSIZE;
	if (len > sk->sk_sndbuf - 32)
		goto out;

	sendctx.timeo = sock_sndtimeo(sk, msg->msg_flags & MSG_DONTWAIT);

restart:
	bus_state_lock(sk);
	if (bus_recvq_full(sk)) {
		err = -EAGAIN;
		if (!sendctx.timeo) {
			bus_state_unlock(sk);
			goto out;
		}

		sendctx.timeo = bus_wait_for_peer(sk, sendctx.timeo);

		err = sock_intr_errno(sendctx.timeo);
		if (signal_pending(current))
			goto out;

		goto restart;
	} else {
		bus_state_unlock(sk);
	}

	skb = sock_alloc_send_skb(sk, len, msg->msg_flags&MSG_DONTWAIT, &err);
	if (skb == NULL)
		goto out;

	err = bus_scm_to_skb(sendctx.siocb->scm, skb, true);
	if (err < 0)
		goto out_free;
	sendctx.max_level = err + 1;
	bus_get_secdata(sendctx.siocb->scm, skb);

	skb_reset_transport_header(skb);
	err = memcpy_fromiovec(skb_put(skb, len), msg->msg_iov, len);
	if (err)
		goto out_free;

	sendctx.sender_socket = sock;
	if (u->bus_master_side && sendctx.other) {
		/* if the bus master sent an unicast message to a peer, we
		 * need the address of that peer
		 */
		sendctx.sender = bus_sk(sendctx.other)->addr->name;
	} else {
		sendctx.sender = u->addr->name;
	}
	sendctx.recipient = sbusaddr;
	sendctx.authenticated = u->authenticated;
	sendctx.bus_master_side = u->bus_master_side;
	sendctx.to_master = to_master;
	sendctx.multicast = multicast;
	sendctx.eavesdropper = atomic64_read(&u->bus->eavesdropper_cnt) ? 1 : 0;
	BUSCB(skb).sendctx = &sendctx;

	if (sendctx.multicast || sendctx.eavesdropper) {
		sendctx.main_recipient = 0;
		err = bus_sendmsg_mcast(skb);
		return sendctx.multicast ? len : err;
	} else {
		sendctx.main_recipient = 1;
		len = NF_HOOK(NFPROTO_BUS, NF_BUS_SENDING, skb, NULL, NULL,
			      bus_sendmsg_finish);

		if (len == -EPERM) {
			err = len;
			goto out;
		} else {
			scm_destroy(sendctx.siocb->scm);
			return len;
		}
	}

out_free:
	kfree_skb(skb);
out:
	if (sendctx.other)
		sock_put(sendctx.other);
	scm_destroy(sendctx.siocb->scm);
	return err;
}

static void bus_copy_addr(struct msghdr *msg, struct sock *sk)
{
	struct bus_sock *u = bus_sk(sk);

	msg->msg_namelen = 0;
	if (u->addr) {
		msg->msg_namelen = u->addr->len;
		memcpy(msg->msg_name, u->addr->name,
		       sizeof(struct sockaddr_bus));
	}
}

static int bus_recvmsg(struct kiocb *iocb, struct socket *sock,
			  struct msghdr *msg, size_t size, int flags)
{
	struct sock_iocb *siocb = kiocb_to_siocb(iocb);
	struct scm_cookie tmp_scm;
	struct sock *sk = sock->sk;
	struct bus_sock *u = bus_sk(sk);
	int noblock = flags & MSG_DONTWAIT;
	struct sk_buff *skb;
	int err;
	int peeked, skip;

	if (sk->sk_state != BUS_ESTABLISHED)
		return -ENOTCONN;

	err = -EOPNOTSUPP;
	if (flags&MSG_OOB)
		goto out;

	msg->msg_namelen = 0;

	err = mutex_lock_interruptible(&u->readlock);
	if (err) {
		err = sock_intr_errno(sock_rcvtimeo(sk, noblock));
		goto out;
	}

	skip = sk_peek_offset(sk, flags);

	skb = __skb_recv_datagram(sk, flags, &peeked, &skip, &err);
	if (!skb) {
		bus_state_lock(sk);
		/* Signal EOF on disconnected non-blocking SEQPACKET socket. */
		if (err == -EAGAIN && (sk->sk_shutdown & RCV_SHUTDOWN))
			err = 0;
		bus_state_unlock(sk);
		goto out_unlock;
	}

	wake_up_interruptible_sync_poll(&u->peer_wait,
					POLLOUT | POLLWRNORM | POLLWRBAND);

	if (msg->msg_name)
		bus_copy_addr(msg, skb->sk);

	if (size > skb->len - skip)
		size = skb->len - skip;
	else if (size < skb->len - skip)
		msg->msg_flags |= MSG_TRUNC;

	err = skb_copy_datagram_iovec(skb, skip, msg->msg_iov, size);
	if (err)
		goto out_free;

	if (sock_flag(sk, SOCK_RCVTSTAMP))
		__sock_recv_timestamp(msg, sk, skb);

	if (!siocb->scm) {
		siocb->scm = &tmp_scm;
		memset(&tmp_scm, 0, sizeof(tmp_scm));
	}
	scm_set_cred(siocb->scm, BUSCB(skb).pid, BUSCB(skb).cred);
	bus_set_secdata(siocb->scm, skb);

	if (!(flags & MSG_PEEK)) {
		if (BUSCB(skb).fp)
			bus_detach_fds(siocb->scm, skb);

		sk_peek_offset_bwd(sk, skb->len);
	} else {
		/* It is questionable: on PEEK we could:
		   - do not return fds - good, but too simple 8)
		   - return fds, and do not return them on read (old strategy,
		     apparently wrong)
		   - clone fds (I chose it for now, it is the most universal
		     solution)

		   POSIX 1003.1g does not actually define this clearly
		   at all. POSIX 1003.1g doesn't define a lot of things
		   clearly however!

		*/

		sk_peek_offset_fwd(sk, size);

		if (BUSCB(skb).fp)
			siocb->scm->fp = scm_fp_dup(BUSCB(skb).fp);
	}
	err = (flags & MSG_TRUNC) ? skb->len - skip : size;

	scm_recv(sock, msg, siocb->scm, flags);

out_free:
	skb_free_datagram(sk, skb);
out_unlock:
	mutex_unlock(&u->readlock);
out:
	return err;
}

static int bus_shutdown(struct socket *sock, int mode)
{
	struct sock *sk = sock->sk;
	struct sock *other;

	mode = (mode+1)&(RCV_SHUTDOWN|SEND_SHUTDOWN);

	if (!mode)
		return 0;

	bus_state_lock(sk);
	sk->sk_shutdown |= mode;
	other = bus_peer(sk);
	if (other)
		sock_hold(other);
	bus_state_unlock(sk);
	sk->sk_state_change(sk);

	if (other) {

		int peer_mode = 0;

		if (mode&RCV_SHUTDOWN)
			peer_mode |= SEND_SHUTDOWN;
		if (mode&SEND_SHUTDOWN)
			peer_mode |= RCV_SHUTDOWN;
		bus_state_lock(other);
		other->sk_shutdown |= peer_mode;
		bus_state_unlock(other);
		other->sk_state_change(other);
		if (peer_mode == SHUTDOWN_MASK)
			sk_wake_async(other, SOCK_WAKE_WAITD, POLL_HUP);
		else if (peer_mode & RCV_SHUTDOWN)
			sk_wake_async(other, SOCK_WAKE_WAITD, POLL_IN);
		sock_put(other);
	}

	return 0;
}

static int bus_add_addr(struct sock *sk, struct bus_addr *sbus_addr)
{
	struct bus_address *addr;
	struct sock *other;
	struct bus_sock *u = bus_sk(sk);
	struct net *net = sock_net(sk);
	int ret = 0;

	addr = kzalloc(sizeof(*addr) + sizeof(struct sockaddr_bus), GFP_KERNEL);
	if (!addr) {
		ret = -ENOMEM;
		goto out;
	}

	memcpy(addr->name, u->addr->name, sizeof(struct sockaddr_bus));
	addr->len = u->addr->len;

	addr->name->sbus_addr.s_addr = sbus_addr->s_addr;
	addr->hash = bus_compute_hash(addr->name->sbus_addr);
	other = bus_find_socket_byaddress(net, addr->name, addr->len,
					  sk->sk_protocol, addr->hash);

	if (other) {
		sock_put(other);
		kfree(addr);
		ret = -EADDRINUSE;
		goto out;
	}

	atomic_set(&addr->refcnt, 1);
	INIT_HLIST_NODE(&addr->addr_node);
	INIT_HLIST_NODE(&addr->table_node);

	addr->sock = sk;

	hlist_add_head(&addr->addr_node, &u->addr_list);
	bus_insert_address(&bus_address_table[addr->hash], addr);

out:
	sock_put(sk);

	return ret;
}

static int bus_del_addr(struct sock *sk, struct bus_addr *sbus_addr)
{
	struct bus_address *addr;
	int ret = 0;

	bus_state_lock(sk);
	addr = __bus_get_address(sk, sbus_addr);
	if (!addr) {
		ret = -EINVAL;
		bus_state_unlock(sk);
		goto out;
	}
	hlist_del(&addr->addr_node);
	bus_state_unlock(sk);

	bus_remove_address(addr);
	bus_release_addr(addr);
out:
	sock_put(sk);

	return ret;
}

static int bus_join_bus(struct sock *sk)
{
	struct sock *peer;
	struct bus_sock *u = bus_sk(sk), *peeru;
	int err = 0;

	peer = bus_peer_get(sk);
	if (!peer)
		return -ENOTCONN;
	peeru = bus_sk(peer);

	if (!u->bus_master_side || peeru->authenticated) {
		err = -EINVAL;
		goto sock_put_out;
	}

	if (sk->sk_state != BUS_ESTABLISHED) {
		err = -ENOTCONN;
		goto sock_put_out;
	}

	if (peer->sk_shutdown != 0) {
		err = -ENOTCONN;
		goto sock_put_out;
	}

	bus_state_lock(peer);
	peeru->authenticated = true;
	bus_state_unlock(peer);

	spin_lock(&u->bus->lock);
	hlist_add_head(&peeru->bus_node, &u->bus->peers);
	spin_unlock(&u->bus->lock);

sock_put_out:
	sock_put(peer);
	return err;
}

static int __bus_set_eavesdrop(struct sock *sk, bool eavesdrop)
{
	struct sock *peer = bus_peer_get(sk);
	struct bus_sock *u = bus_sk(sk), *peeru;
	int err = 0;

	if (!peer)
		return -ENOTCONN;

	if (sk->sk_state != BUS_ESTABLISHED) {
		err = -ENOTCONN;
		goto sock_put_out;
	}

	peeru = bus_sk(peer);

	if (!u->bus_master_side || !peeru->authenticated) {
		err = -EINVAL;
		goto sock_put_out;
	}

	if (peer->sk_shutdown != 0) {
		err = -ENOTCONN;
		goto sock_put_out;
	}

	bus_state_lock(peeru);
	if (peeru->eavesdropper != eavesdrop) {
		peeru->eavesdropper = eavesdrop;
		if (eavesdrop)
			atomic64_inc(&u->bus->eavesdropper_cnt);
		else
			atomic64_dec(&u->bus->eavesdropper_cnt);
	}
	bus_state_unlock(peeru);

sock_put_out:
	sock_put(peer);
	return err;
}

static int bus_set_eavesdrop(struct sock *sk)
{
	return __bus_set_eavesdrop(sk, true);
}

static int bus_unset_eavesdrop(struct sock *sk)
{
	return __bus_set_eavesdrop(sk, false);
}

static inline void sk_sendbuf_set(struct sock *sk, int sndbuf)
{
	bus_state_lock(sk);
	sk->sk_sndbuf = sndbuf;
	bus_state_unlock(sk);
}

static inline void sk_maxqlen_set(struct sock *sk, int qlen)
{
	bus_state_lock(sk);
	sk->sk_max_ack_backlog = qlen;
	bus_state_unlock(sk);
}

static int bus_get_qlenfull(struct sock *sk)
{
	struct sock *peer;
	struct bus_sock *u = bus_sk(sk), *peeru;
	int ret = 0;

	peer = bus_peer_get(sk);
	if (!peer)
		return -ENOTCONN;

	peeru = bus_sk(peer);

	if (!u->bus_master_side || peeru->authenticated) {
		ret = -EINVAL;
		goto sock_put_out;
	}

	if (sk->sk_state != BUS_ESTABLISHED) {
		ret = -ENOTCONN;
		goto sock_put_out;
	}

	if (peer->sk_shutdown != 0) {
		ret = -ENOTCONN;
		goto sock_put_out;
	}

	ret = bus_recvq_full(peer);

sock_put_out:
	sock_put(peer);
	return ret;
}

static int bus_setsockopt(struct socket *sock, int level, int optname,
			   char __user *optval, unsigned int optlen)
{
	struct sockaddr_bus addr;
	int res;
	int val;

	if (level != SOL_BUS)
		return -ENOPROTOOPT;

	switch (optname) {
	case BUS_ADD_ADDR:
	case BUS_DEL_ADDR:
		if (optlen < sizeof(struct sockaddr_bus))
			return -EINVAL;

		if (!bus_sk(sock->sk)->bus_master_side)
			return -EINVAL;

		if (copy_from_user(&addr, optval, sizeof(struct sockaddr_bus)))
			return -EFAULT;

		if (addr.sbus_family != AF_BUS)
			return -EINVAL;

		if (optname == BUS_ADD_ADDR)
			res = bus_add_addr(bus_peer_get(sock->sk),
					   &addr.sbus_addr);
		else
			res = bus_del_addr(bus_peer_get(sock->sk),
					   &addr.sbus_addr);
		break;
	case BUS_JOIN_BUS:
		res = bus_join_bus(sock->sk);
		break;
	case BUS_SET_EAVESDROP:
		res = bus_set_eavesdrop(sock->sk);
		break;
	case BUS_UNSET_EAVESDROP:
		res = bus_unset_eavesdrop(sock->sk);
		break;
	case BUS_SET_SENDBUF:
	case BUS_SET_MAXQLEN:
		if (sock->sk->sk_state != BUS_LISTEN) {
			res = -EINVAL;
		} else {
			res = -EFAULT;

			if (copy_from_user(&val, optval, optlen))
				break;

			res = 0;

			if (optname == BUS_SET_SENDBUF)
				sk_sendbuf_set(sock->sk, val);
			else
				sk_maxqlen_set(sock->sk, val);
		}
		break;
	case BUS_GET_QLENFULL:
		res = bus_get_qlenfull(sock->sk);

		if (copy_to_user(&res, optval, optlen)) {
			res = -EFAULT;
			break;
		}
		res = 0;
		break;
	default:
		res = -EINVAL;
		break;
	}

	return res;
}

long bus_inq_len(struct sock *sk)
{
	struct sk_buff *skb;
	long amount = 0;

	if (sk->sk_state == BUS_LISTEN)
		return -EINVAL;

	spin_lock(&sk->sk_receive_queue.lock);
	skb_queue_walk(&sk->sk_receive_queue, skb)
		amount += skb->len;
	spin_unlock(&sk->sk_receive_queue.lock);

	return amount;
}
EXPORT_SYMBOL_GPL(bus_inq_len);

long bus_outq_len(struct sock *sk)
{
	return sk_wmem_alloc_get(sk);
}
EXPORT_SYMBOL_GPL(bus_outq_len);

static int bus_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	struct sock *sk = sock->sk;
	long amount = 0;
	int err;

	switch (cmd) {
	case SIOCOUTQ:
		amount = bus_outq_len(sk);
		err = put_user(amount, (int __user *)arg);
		break;
	case SIOCINQ:
		amount = bus_inq_len(sk);
		if (amount < 0)
			err = amount;
		else
			err = put_user(amount, (int __user *)arg);
		break;
	default:
		err = -ENOIOCTLCMD;
		break;
	}
	return err;
}

static unsigned int bus_poll(struct file *file, struct socket *sock,
				    poll_table *wait)
{
	struct sock *sk = sock->sk, *other;
	unsigned int mask, writable;
	struct bus_sock *u = bus_sk(sk), *p;
	struct hlist_node *node;

	sock_poll_wait(file, sk_sleep(sk), wait);
	mask = 0;

	/* exceptional events? */
	if (sk->sk_err || !skb_queue_empty(&sk->sk_error_queue))
		mask |= POLLERR;
	if (sk->sk_shutdown & RCV_SHUTDOWN)
		mask |= POLLRDHUP | POLLIN | POLLRDNORM;
	if (sk->sk_shutdown == SHUTDOWN_MASK)
		mask |= POLLHUP;

	/* readable? */
	if (!skb_queue_empty(&sk->sk_receive_queue))
		mask |= POLLIN | POLLRDNORM;

	/* Connection-based need to check for termination and startup */
	if (sk->sk_state == BUS_CLOSE)
		mask |= POLLHUP;

	/* No write status requested, avoid expensive OUT tests. */
	if (!(poll_requested_events(wait) & (POLLWRBAND|POLLWRNORM|POLLOUT)))
		return mask;

	writable = bus_writable(sk);
	other = bus_peer_get(sk);
	if (other) {
		if (bus_recvq_full(other))
			writable = 0;
		sock_put(other);
	}

	/*
	 * If the socket has already joined the bus we have to check
	 * that each peer receiver queue on the bus is not full.
	 */
	if (!u->bus_master_side && u->authenticated) {
		spin_lock(&u->bus->lock);
		hlist_for_each_entry(p, node, &u->bus->peers, bus_node) {
			if (bus_recvq_full(&p->sk)) {
				writable = 0;
				break;
			}
		}
		spin_unlock(&u->bus->lock);
	}

	if (writable)
		mask |= POLLOUT | POLLWRNORM | POLLWRBAND;
	else
		set_bit(SOCK_ASYNC_NOSPACE, &sk->sk_socket->flags);

	return mask;
}

#ifdef CONFIG_PROC_FS
static struct sock *first_bus_socket(int *i)
{
	for (*i = 0; *i <= BUS_HASH_SIZE; (*i)++) {
		if (!hlist_empty(&bus_socket_table[*i]))
			return __sk_head(&bus_socket_table[*i]);
	}
	return NULL;
}

static struct sock *next_bus_socket(int *i, struct sock *s)
{
	struct sock *next = sk_next(s);
	/* More in this chain? */
	if (next)
		return next;
	/* Look for next non-empty chain. */
	for ((*i)++; *i <= BUS_HASH_SIZE; (*i)++) {
		if (!hlist_empty(&bus_socket_table[*i]))
			return __sk_head(&bus_socket_table[*i]);
	}
	return NULL;
}

struct bus_iter_state {
	struct seq_net_private p;
	int i;
};

static struct sock *bus_seq_idx(struct seq_file *seq, loff_t pos)
{
	struct bus_iter_state *iter = seq->private;
	loff_t off = 0;
	struct sock *s;

	for (s = first_bus_socket(&iter->i); s;
	     s = next_bus_socket(&iter->i, s)) {
		if (sock_net(s) != seq_file_net(seq))
			continue;
		if (off == pos)
			return s;
		++off;
	}
	return NULL;
}

static void *bus_seq_start(struct seq_file *seq, loff_t *pos)
	__acquires(bus_table_lock)
{
	spin_lock(&bus_table_lock);
	return *pos ? bus_seq_idx(seq, *pos - 1) : SEQ_START_TOKEN;
}

static void *bus_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct bus_iter_state *iter = seq->private;
	struct sock *sk = v;
	++*pos;

	if (v == SEQ_START_TOKEN)
		sk = first_bus_socket(&iter->i);
	else
		sk = next_bus_socket(&iter->i, sk);
	while (sk && (sock_net(sk) != seq_file_net(seq)))
		sk = next_bus_socket(&iter->i, sk);
	return sk;
}

static void bus_seq_stop(struct seq_file *seq, void *v)
	__releases(bus_table_lock)
{
	spin_unlock(&bus_table_lock);
}

static int bus_seq_show(struct seq_file *seq, void *v)
{

	if (v == SEQ_START_TOKEN)
		seq_puts(seq, "Num       RefCount Protocol Flags    Type St " \
			 "Inode Path\n");
	else {
		struct sock *s = v;
		struct bus_sock *u = bus_sk(s);
		bus_state_lock(s);

		seq_printf(seq, "%pK: %08X %08X %08X %04X %02X %5lu",
			s,
			atomic_read(&s->sk_refcnt),
			0,
			s->sk_state == BUS_LISTEN ? __SO_ACCEPTCON : 0,
			s->sk_type,
			s->sk_socket ?
			(s->sk_state == BUS_ESTABLISHED ? SS_CONNECTED : SS_UNCONNECTED) :
			(s->sk_state == BUS_ESTABLISHED ? SS_CONNECTING : SS_DISCONNECTING),
			sock_i_ino(s));

		if (u->addr) {
			int i, len;
			seq_putc(seq, ' ');

			i = 0;
			len = u->addr->len - sizeof(short);
			if (!BUS_ABSTRACT(s))
				len--;
			else {
				seq_putc(seq, '@');
				i++;
			}
			for ( ; i < len; i++)
				seq_putc(seq, u->addr->name->sbus_path[i]);
		}
		bus_state_unlock(s);
		seq_putc(seq, '\n');
	}

	return 0;
}

static const struct seq_operations bus_seq_ops = {
	.start  = bus_seq_start,
	.next   = bus_seq_next,
	.stop   = bus_seq_stop,
	.show   = bus_seq_show,
};

static int bus_seq_open(struct inode *inode, struct file *file)
{
	return seq_open_net(inode, file, &bus_seq_ops,
			    sizeof(struct bus_iter_state));
}

static const struct file_operations bus_seq_fops = {
	.owner		= THIS_MODULE,
	.open		= bus_seq_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release_net,
};

#endif

static const struct net_proto_family bus_family_ops = {
	.family = PF_BUS,
	.create = bus_create,
	.owner	= THIS_MODULE,
};

static int __init af_bus_init(void)
{
	int rc = -1;
	struct sk_buff *dummy_skb;

	BUILD_BUG_ON(sizeof(struct bus_skb_parms) > sizeof(dummy_skb->cb));

	rc = proto_register(&bus_proto, 1);
	if (rc != 0) {
		pr_crit("%s: Cannot create bus_sock SLAB cache!\n", __func__);
		return rc;
	}

	sock_register(&bus_family_ops);
	return rc;
}

static void __exit af_bus_exit(void)
{
	sock_unregister(PF_BUS);
	proto_unregister(&bus_proto);
}

module_init(af_bus_init);
module_exit(af_bus_exit);

MODULE_AUTHOR("Alban Crequy, Javier Martinez Canillas");
MODULE_DESCRIPTION("Linux Bus domain sockets");
MODULE_LICENSE("GPL");
MODULE_ALIAS_NETPROTO(PF_BUS);
