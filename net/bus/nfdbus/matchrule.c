/*
 * matchrule.c  D-Bus match rule implementation
 *
 * Based on signals.c from dbus
 *
 * Copyright (C) 2010  Collabora, Ltd.
 * Copyright (C) 2003, 2005  Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include "matchrule.h"

#include <linux/rbtree.h>
#include <linux/list.h>
#include <linux/slab.h>

#include "message.h"

enum bus_match_flags {
	BUS_MATCH_MESSAGE_TYPE            = 1 << 0,
	BUS_MATCH_INTERFACE               = 1 << 1,
	BUS_MATCH_MEMBER                  = 1 << 2,
	BUS_MATCH_SENDER                  = 1 << 3,
	BUS_MATCH_DESTINATION             = 1 << 4,
	BUS_MATCH_PATH                    = 1 << 5,
	BUS_MATCH_ARGS                    = 1 << 6,
	BUS_MATCH_PATH_NAMESPACE          = 1 << 7,
	BUS_MATCH_CLIENT_IS_EAVESDROPPING = 1 << 8
};

struct bus_match_rule {
	/* For debugging only*/
	char *rule_text;

	unsigned int flags; /**< BusMatchFlags */

	int   message_type;
	char *interface;
	char *member;
	char *sender;
	char *destination;
	char *path;

	unsigned int *arg_lens;
	char **args;
	int args_len;

	/* bus_match_rule is attached to rule_pool, either in a simple
	 * double-linked list if the rule does not have any interface, or in a
	 * red-black tree sorted by interface. If several rules can have the
	 * same interface, the first one is attached with struct rb_node and the
	 * next ones are in the list
	 */

	struct rb_node node;
	/* Doubly-linked non-circular list. If the rule has an interface, it is
	 * in the rb tree and the single head is right here. Otherwise, the
	 * single head is in rule_pool->rules_without_iface. With this data
	 * structure, we don't need any allocation to insert or remove the rule.
	 */
	struct hlist_head first;
	struct hlist_node list;

	/* used to delete all names from the tree */
	struct list_head del_list;
};

struct dbus_name {
	struct rb_node node;
	char *name;

	/* used to delete all names from the tree */
	struct list_head del_list;
};

#define BUS_MATCH_ARG_IS_PATH  0x8000000u

#define DBUS_STRING_MAX_LENGTH 1024

/** Max length of a match rule string; to keep people from hosing the
 * daemon with some huge rule
 */
#define DBUS_MAXIMUM_MATCH_RULE_LENGTH 1024

struct bus_match_rule *bus_match_rule_new(gfp_t gfp_flags)
{
	struct bus_match_rule *rule;

	rule = kzalloc(sizeof(struct bus_match_rule), gfp_flags);
	if (rule == NULL)
		return NULL;

	return rule;
}

void bus_match_rule_free(struct bus_match_rule *rule)
{
	kfree(rule->rule_text);
	kfree(rule->interface);
	kfree(rule->member);
	kfree(rule->sender);
	kfree(rule->destination);
	kfree(rule->path);
	kfree(rule->arg_lens);

	/* can't use dbus_free_string_array() since there
	 * are embedded NULL
	 */
	if (rule->args) {
		int i;

		i = 0;
		while (i < rule->args_len) {
			kfree(rule->args[i]);
			++i;
		}

		kfree(rule->args);
	}

	kfree(rule);
}

static int
bus_match_rule_set_message_type(struct bus_match_rule *rule,
				int type,
				gfp_t gfp_flags)
{
	rule->flags |= BUS_MATCH_MESSAGE_TYPE;

	rule->message_type = type;

	return 1;
}

static int
bus_match_rule_set_interface(struct bus_match_rule *rule,
			     const char *interface,
			     gfp_t gfp_flags)
{
	char *new;

	WARN_ON(!interface);

	new = kstrdup(interface, gfp_flags);
	if (new == NULL)
		return 0;

	rule->flags |= BUS_MATCH_INTERFACE;
	kfree(rule->interface);
	rule->interface = new;

	return 1;
}

static int
bus_match_rule_set_member(struct bus_match_rule *rule,
			  const char *member,
			  gfp_t gfp_flags)
{
	char *new;

	WARN_ON(!member);

	new = kstrdup(member, gfp_flags);
	if (new == NULL)
		return 0;

	rule->flags |= BUS_MATCH_MEMBER;
	kfree(rule->member);
	rule->member = new;

	return 1;
}

static int
bus_match_rule_set_path(struct bus_match_rule *rule,
			const char *path,
			gfp_t gfp_flags)
{
	char *new;

	WARN_ON(!path);

	new = kstrdup(path, gfp_flags);
	if (new == NULL)
		return 0;

	rule->flags |= BUS_MATCH_PATH;
	kfree(rule->path);
	rule->path = new;

	return 1;
}

static int
bus_match_rule_set_sender(struct bus_match_rule *rule,
			  const char *sender,
			  gfp_t gfp_flags)
{
	char *new;

	WARN_ON(!sender);

	new = kstrdup(sender, gfp_flags);
	if (new == NULL)
		return 0;

	rule->flags |= BUS_MATCH_SENDER;
	kfree(rule->sender);
	rule->sender = new;

	return 1;
}

static int
bus_match_rule_set_destination(struct bus_match_rule *rule,
			       const char   *destination,
			       gfp_t gfp_flags)
{
	char *new;

	WARN_ON(!destination);

	new = kstrdup(destination, gfp_flags);
	if (new == NULL)
		return 0;

	rule->flags |= BUS_MATCH_DESTINATION;
	kfree(rule->destination);
	rule->destination = new;

	return 1;
}

#define ISWHITE(c) (((c) == ' ') || ((c) == '\t') || ((c) == '\n') || \
		    ((c) == '\r'))

static int find_key(const char *str, int start, char *key, int *value_pos)
{
	const char *p;
	const char *s;
	const char *key_start;
	const char *key_end;

	s = str;

	p = s + start;

	while (*p && ISWHITE(*p))
		++p;

	key_start = p;

	while (*p && *p != '=' && !ISWHITE(*p))
		++p;

	key_end = p;

	while (*p && ISWHITE(*p))
		++p;

	if (key_start == key_end) {
		/* Empty match rules or trailing whitespace are OK */
		*value_pos = p - s;
		return 1;
	}

	if (*p != '=') {
		pr_warn("Match rule has a key with no subsequent '=' character");
		return 0;
	}
	++p;

	strncat(key, key_start, key_end - key_start);

	*value_pos = p - s;

	return 1;
}

static int find_value(const char *str, int start, const char *key, char *value,
		      int *value_end)
{
	const char *p;
	const char *s;
	char quote_char;
	int orig_len;

	orig_len = strlen(value);

	s = str;

	p = s + start;

	quote_char = '\0';

	while (*p) {
		if (quote_char == '\0') {
			switch (*p) {
			case '\0':
				goto done;

			case '\'':
				quote_char = '\'';
				goto next;

			case ',':
				++p;
				goto done;

			case '\\':
				quote_char = '\\';
				goto next;

			default:
				strncat(value, p, 1);
			}
		} else if (quote_char == '\\') {
			/*\ only counts as an escape if escaping a quote mark */
			if (*p != '\'')
				strncat(value, "\\", 1);

			strncat(value, p, 1);

			quote_char = '\0';
		} else {
			if (*p == '\'')
				quote_char = '\0';
			else
				strncat(value, p, 1);
		}

next:
		++p;
	}

done:

	if (quote_char == '\\')
		strncat(value, "\\", 1);
	else if (quote_char == '\'') {
		pr_warn("Unbalanced quotation marks in match rule");
		return 0;
	}

	/* Zero-length values are allowed */

	*value_end = p - s;

	return 1;
}

/* duplicates aren't allowed so the real legitimate max is only 6 or
 * so. Leaving extra so we don't have to bother to update it.
 * FIXME this is sort of busted now with arg matching, but we let
 * you match on up to 10 args for now
 */
#define MAX_RULE_TOKENS 16

/* this is slightly too high level to be termed a "token"
 * but let's not be pedantic.
 */
struct rule_token {
	char *key;
	char *value;
};

static int tokenize_rule(const char *rule_text,
			 struct rule_token tokens[MAX_RULE_TOKENS],
			 gfp_t gfp_flags)
{
	int i;
	int pos;
	int retval;

	retval = 0;

	i = 0;
	pos = 0;
	while (i < MAX_RULE_TOKENS &&
	       pos < strlen(rule_text)) {
		char *key;
		char *value;

		key = kzalloc(DBUS_STRING_MAX_LENGTH, gfp_flags);
		if (!key) {
			pr_err("Out of memory");
			return 0;
		}

		value = kzalloc(DBUS_STRING_MAX_LENGTH, gfp_flags);
		if (!value) {
			kfree(key);
			pr_err("Out of memory");
			return 0;
		}

		if (!find_key(rule_text, pos, key, &pos))
			goto out;

		if (strlen(key) == 0)
			goto next;

		tokens[i].key = key;

		if (!find_value(rule_text, pos, tokens[i].key, value, &pos))
			goto out;

		tokens[i].value = value;

next:
		++i;
	}

	retval = 1;

out:
	if (!retval) {
		i = 0;
		while (tokens[i].key || tokens[i].value) {
			kfree(tokens[i].key);
			kfree(tokens[i].value);
			tokens[i].key = NULL;
			tokens[i].value = NULL;
			++i;
		}
	}

	return retval;
}

/*
 * The format is comma-separated with strings quoted with single quotes
 * as for the shell (to escape a literal single quote, use '\'').
 *
 * type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',
 * member='Foo', path='/bar/foo',destination=':452345.34'
 *
 */
struct bus_match_rule *bus_match_rule_parse(const char *rule_text,
					    gfp_t gfp_flags)
{
	struct bus_match_rule *rule;
	struct rule_token tokens[MAX_RULE_TOKENS+1]; /* NULL termination + 1 */
	int i;

	if (strlen(rule_text) > DBUS_MAXIMUM_MATCH_RULE_LENGTH) {
		pr_warn("Match rule text is %ld bytes, maximum is %d",
			    strlen(rule_text),
			    DBUS_MAXIMUM_MATCH_RULE_LENGTH);
		return NULL;
	}

	memset(tokens, '\0', sizeof(tokens));

	rule = bus_match_rule_new(gfp_flags);
	if (rule == NULL) {
		pr_err("Out of memory");
		goto failed;
	}

	rule->rule_text = kstrdup(rule_text, gfp_flags);
	if (rule->rule_text == NULL) {
		pr_err("Out of memory");
		goto failed;
	}

	if (!tokenize_rule(rule_text, tokens, gfp_flags))
		goto failed;

	i = 0;
	while (tokens[i].key != NULL) {
		const char *key = tokens[i].key;
		const char *value = tokens[i].value;

		if (strcmp(key, "type") == 0) {
			int t;

			if (rule->flags & BUS_MATCH_MESSAGE_TYPE) {
				pr_warn("Key %s specified twice in match rule\n",
					key);
				goto failed;
			}

			t = dbus_message_type_from_string(value);

			if (t == DBUS_MESSAGE_TYPE_INVALID) {
				pr_warn("Invalid message type (%s) in match rule\n",
					value);
				goto failed;
			}

			if (!bus_match_rule_set_message_type(rule, t,
							     gfp_flags)) {
				pr_err("Out of memeory");
				goto failed;
			}
		} else if (strcmp(key, "sender") == 0) {
			if (rule->flags & BUS_MATCH_SENDER) {
				pr_warn("Key %s specified twice in match rule\n",
					key);
				goto failed;
			}

			if (!bus_match_rule_set_sender(rule, value,
						       gfp_flags)) {
				pr_err("Out of memeory");
				goto failed;
			}
		} else if (strcmp(key, "interface") == 0) {
			if (rule->flags & BUS_MATCH_INTERFACE) {
				pr_warn("Key %s specified twice in match rule\n",
					key);
				goto failed;
			}

			if (!bus_match_rule_set_interface(rule, value,
							  gfp_flags)) {
				pr_err("Out of memeory");
				goto failed;
			}
		} else if (strcmp(key, "member") == 0) {
			if (rule->flags & BUS_MATCH_MEMBER) {
				pr_warn("Key %s specified twice in match rule\n",
					key);
				goto failed;
			}

			if (!bus_match_rule_set_member(rule, value,
						       gfp_flags)) {
				pr_err("Out of memeory");
				goto failed;
			}
		} else if (strcmp(key, "path") == 0) {
			if (rule->flags & BUS_MATCH_PATH) {
				pr_warn("Key %s specified twice in match rule\n",
					key);
				goto failed;
			}

			if (!bus_match_rule_set_path(rule, value,
						     gfp_flags)) {
				pr_err("Out of memory");
				goto failed;
			}
		} else if (strcmp(key, "destination") == 0) {
			if (rule->flags & BUS_MATCH_DESTINATION) {
				pr_warn("Key %s specified twice in match rule\n",
					key);
				goto failed;
			}

			if (!bus_match_rule_set_destination(rule, value,
							    gfp_flags)) {
				pr_err("Out of memeory");
				goto failed;
			}
		} else if (strcmp(key, "eavesdrop") == 0) {
			if (strcmp(value, "true") == 0) {
				rule->flags |= BUS_MATCH_CLIENT_IS_EAVESDROPPING;
			} else if (strcmp(value, "false") == 0) {
				rule->flags &= ~(BUS_MATCH_CLIENT_IS_EAVESDROPPING);
			} else {
				pr_warn("eavesdrop='%s' is invalid, " \
					"it should be 'true' or 'false'\n",
					value);
				goto failed;
			}
		} else if (strncmp(key, "arg", 3) != 0) {
			pr_warn("Unknown key \"%s\" in match rule\n",
				   key);
			goto failed;
		}

		++i;
	}

	goto out;

failed:
	if (rule) {
		bus_match_rule_free(rule);
		rule = NULL;
	}

out:

	i = 0;
	while (tokens[i].key || tokens[i].value) {
		WARN_ON(i >= MAX_RULE_TOKENS);
		kfree(tokens[i].key);
		kfree(tokens[i].value);
		++i;
	}

	return rule;
}

/* return the match rule containing the hlist_head. It may not be the first
 * match rule in the list. */
struct bus_match_rule *match_rule_search(struct rb_root *root,
					 const char *interface)
{
	struct rb_node *node = root->rb_node;

	while (node) {
		struct bus_match_rule *data =
			container_of(node, struct bus_match_rule, node);
		int result;

		result = strcmp(interface, data->interface);

		if (result < 0)
			node = node->rb_left;
		else if (result > 0)
			node = node->rb_right;
		else
			return data;
	}
	return NULL;
}

void match_rule_insert(struct rb_root *root, struct bus_match_rule *data)
{
	struct rb_node **new = &(root->rb_node), *parent = NULL;

	/* Figure out where to put new node */
	while (*new) {
		struct bus_match_rule *this =
			container_of(*new, struct bus_match_rule, node);
		int result = strcmp(data->interface, this->interface);

		parent = *new;
		if (result < 0)
			new = &((*new)->rb_left);
		else if (result > 0)
			new = &((*new)->rb_right);
		else {
			/* the head is not used */
			INIT_HLIST_HEAD(&data->first);
			/* Add it at the beginning of the list */
			hlist_add_head(&data->list, &this->first);
			return;
		}
	}

	/* this rule is single in its list */
	INIT_HLIST_HEAD(&data->first);
	hlist_add_head(&data->list, &data->first);

	/* Add new node and rebalance tree. */
	rb_link_node(&data->node, parent, new);
	rb_insert_color(&data->node, root);
}

struct bus_match_maker *bus_matchmaker_new(gfp_t gfp_flags)
{
	struct bus_match_maker *matchmaker;
	int i;

	matchmaker = kzalloc(sizeof(struct bus_match_maker), gfp_flags);
	if (matchmaker == NULL)
		return NULL;

	for (i = DBUS_MESSAGE_TYPE_INVALID; i < DBUS_NUM_MESSAGE_TYPES; i++) {
		struct rule_pool *p = matchmaker->rules_by_type + i;

		p->rules_by_iface = RB_ROOT;
	}

	kref_init(&matchmaker->kref);

	return matchmaker;
}

void bus_matchmaker_free(struct kref *kref)
{
	struct bus_match_maker *matchmaker;
	struct list_head del_list;
	struct rb_node *n;
	int i;

	matchmaker = container_of(kref, struct bus_match_maker, kref);

	/* free names */
	INIT_LIST_HEAD(&del_list);
	n = matchmaker->names.rb_node;
	if (n) {
		struct dbus_name *dbus_name, *cur, *tmp;

		dbus_name = rb_entry(n, struct dbus_name, node);
		list_add_tail(&dbus_name->del_list, &del_list);

		list_for_each_entry(cur, &del_list, del_list) {
			struct dbus_name *right, *left;
			if (cur->node.rb_right) {
				right = rb_entry(cur->node.rb_right,
						 struct dbus_name, node);
				list_add_tail(&right->del_list, &del_list);
			}
			if (cur->node.rb_left) {
				left = rb_entry(cur->node.rb_left,
						struct dbus_name, node);
				list_add_tail(&left->del_list, &del_list);
			}
		}
		list_for_each_entry_safe(dbus_name, tmp, &del_list, del_list) {
			kfree(dbus_name->name);
			list_del(&dbus_name->del_list);
			kfree(dbus_name);
		}
	}
	WARN_ON(!list_empty_careful(&del_list));

	/* free match rules */
	for (i = 0 ; i < DBUS_NUM_MESSAGE_TYPES ; i++) {
		struct rule_pool *pool = matchmaker->rules_by_type + i;
		struct bus_match_rule *match_rule, *cur, *tmp;
		struct hlist_node *list_tmp, *list_tmp2;

		/* free match rules from the list */
		hlist_for_each_entry_safe(cur, list_tmp, list_tmp2,
					  &pool->rules_without_iface, list) {
			bus_match_rule_free(cur);
		}

		/* free match rules from the tree */
		if (!pool->rules_by_iface.rb_node)
			continue;
		match_rule = rb_entry(pool->rules_by_iface.rb_node,
				      struct bus_match_rule, node);
		list_add_tail(&match_rule->del_list, &del_list);

		list_for_each_entry(cur, &del_list, del_list) {
			struct bus_match_rule *right, *left;
			if (cur->node.rb_right) {
				right = rb_entry(cur->node.rb_right,
						 struct bus_match_rule, node);
				list_add_tail(&right->del_list, &del_list);
			}
			if (cur->node.rb_left) {
				left = rb_entry(cur->node.rb_left,
						struct bus_match_rule, node);
				list_add_tail(&left->del_list, &del_list);
			}
		}
		list_for_each_entry_safe(match_rule, tmp, &del_list, del_list) {
			/* keep a ref during the loop to ensure the first
			 * iteration of the loop does not delete it */
			hlist_for_each_entry_safe(cur, list_tmp, list_tmp2,
						  &match_rule->first, list) {
				if (cur != match_rule)
					bus_match_rule_free(cur);
			}
			list_del(&match_rule->del_list);
			bus_match_rule_free(match_rule);
		}
		WARN_ON(!list_empty_careful(&del_list));
	}

	kfree(matchmaker);
}

/* The rule can't be modified after it's added. */
int bus_matchmaker_add_rule(struct bus_match_maker *matchmaker,
			    struct bus_match_rule *rule)
{
	struct rule_pool *pool;

	WARN_ON(rule->message_type < 0);
	WARN_ON(rule->message_type >= DBUS_NUM_MESSAGE_TYPES);

	pool = matchmaker->rules_by_type + rule->message_type;

	if (rule->interface)
		match_rule_insert(&pool->rules_by_iface, rule);
	else
		hlist_add_head(&rule->list, &pool->rules_without_iface);

	return 1;
}

static int match_rule_equal(struct bus_match_rule *a,
			    struct bus_match_rule *b)
{
	if (a->flags != b->flags)
		return 0;

	if ((a->flags & BUS_MATCH_MESSAGE_TYPE) &&
	    a->message_type != b->message_type)
		return 0;

	if ((a->flags & BUS_MATCH_MEMBER) &&
	    strcmp(a->member, b->member) != 0)
		return 0;

	if ((a->flags & BUS_MATCH_PATH) &&
	    strcmp(a->path, b->path) != 0)
		return 0;

	if ((a->flags & BUS_MATCH_INTERFACE) &&
	    strcmp(a->interface, b->interface) != 0)
		return 0;

	if ((a->flags & BUS_MATCH_SENDER) &&
	    strcmp(a->sender, b->sender) != 0)
		return 0;

	if ((a->flags & BUS_MATCH_DESTINATION) &&
	    strcmp(a->destination, b->destination) != 0)
		return 0;

	if (a->flags & BUS_MATCH_ARGS) {
		int i;

		if (a->args_len != b->args_len)
			return 0;

		i = 0;
		while (i < a->args_len) {
			int length;

			if ((a->args[i] != NULL) != (b->args[i] != NULL))
				return 0;

			if (a->arg_lens[i] != b->arg_lens[i])
				return 0;

			length = a->arg_lens[i] & ~BUS_MATCH_ARG_IS_PATH;

			if (a->args[i] != NULL) {
				WARN_ON(!b->args[i]);
				if (memcmp(a->args[i], b->args[i], length) != 0)
					return 0;
			}

			++i;
		}
	}

	return 1;
}

/* Remove a single rule which is equal to the given rule by value */
void bus_matchmaker_remove_rule_by_value(struct bus_match_maker *matchmaker,
					 struct bus_match_rule *rule)
{
	struct rule_pool *pool;

	WARN_ON(rule->message_type < 0);
	WARN_ON(rule->message_type >= DBUS_NUM_MESSAGE_TYPES);

	pool = matchmaker->rules_by_type + rule->message_type;

	if (rule->interface) {
		struct bus_match_rule *head =
			match_rule_search(&pool->rules_by_iface,
					  rule->interface);

		if (head) {
			struct hlist_node *cur;
			struct bus_match_rule *cur_rule;
			hlist_for_each_entry(cur_rule, cur, &head->first, list) {
				if (match_rule_equal(cur_rule, rule)) {
					hlist_del(cur);
					if (hlist_empty(&head->first))
						rb_erase(&head->node,
							 &pool->rules_by_iface);
					bus_match_rule_free(cur_rule);
					break;
				}
			}
		}
	} else {
		struct hlist_head *head = &pool->rules_without_iface;

		struct hlist_node *cur;
		struct bus_match_rule *cur_rule;
		hlist_for_each_entry(cur_rule, cur, head, list) {
			if (match_rule_equal(cur_rule, rule)) {
				hlist_del(cur);
				bus_match_rule_free(cur_rule);
				break;
			}
		}
	}

}

static int connection_is_primary_owner(struct bus_match_maker *connection,
				       const char *service_name)
{
	struct rb_node *node = connection->names.rb_node;

	if (!service_name)
		return 0;

	while (node) {
		struct dbus_name *data = container_of(node, struct dbus_name,
						      node);
		int result;

		result = strcmp(service_name, data->name);

		if (result < 0)
			node = node->rb_left;
		else if (result > 0)
			node = node->rb_right;
		else
			return 1;
	}
	return 0;
}

static int match_rule_matches(struct bus_match_maker *matchmaker,
			      struct bus_match_maker *sender,
			      int eavesdrop,
			      struct bus_match_rule *rule,
			      const struct dbus_message *message)
{
	/* Don't consider the rule if this is a eavesdropping match rule
	 * and eavesdropping is not allowed on that peer */
	if ((rule->flags & BUS_MATCH_CLIENT_IS_EAVESDROPPING) && !eavesdrop)
		return 0;

	/* Since D-Bus 1.5.6, match rules do not match messages which have a
	 * DESTINATION field unless the match rule specifically requests this
	 * by specifying eavesdrop='true' in the match rule. */
	if (message->destination &&
	    !(rule->flags & BUS_MATCH_CLIENT_IS_EAVESDROPPING))
		return 0;

	if (rule->flags & BUS_MATCH_MEMBER) {
		const char *member;

		WARN_ON(!rule->member);

		member = message->member;
		if (member == NULL)
			return 0;

		if (strcmp(member, rule->member) != 0)
			return 0;
	}

	if (rule->flags & BUS_MATCH_SENDER) {
		WARN_ON(!rule->sender);

		if (sender == NULL) {
			if (strcmp(rule->sender,
				   "org.freedesktop.DBus") != 0)
				return 0;
		} else
			if (!connection_is_primary_owner(sender, rule->sender))
				return 0;
	}

	if (rule->flags & BUS_MATCH_DESTINATION) {
		const char *destination;

		WARN_ON(!rule->destination);

		destination = message->destination;
		if (destination == NULL)
			return 0;

		/* This will not just work out of the box because it this is
		 * an eavesdropping match rule. */
		if (matchmaker == NULL) {
			if (strcmp(rule->destination,
				   "org.freedesktop.DBus") != 0)
				return 0;
		} else
			if (!connection_is_primary_owner(matchmaker,
							 rule->destination))
				return 0;
	}

	if (rule->flags & BUS_MATCH_PATH) {
		const char *path;

		WARN_ON(!rule->path);

		path = message->path;
		if (path == NULL)
			return 0;

		if (strcmp(path, rule->path) != 0)
			return 0;
	}

	return 1;
}

static bool get_recipients_from_list(struct bus_match_maker *matchmaker,
				     struct bus_match_maker *sender,
				     int eavesdrop,
				     struct hlist_head *rules,
				     const struct dbus_message *message)
{
	struct hlist_node *cur;
	struct bus_match_rule *rule;

	if (rules == NULL) {
		pr_debug("no rules of this type\n");
		return 0;
	}

	hlist_for_each_entry(rule, cur, rules, list) {
		if (match_rule_matches(matchmaker, sender, eavesdrop, rule,
					message)) {
			pr_debug("[YES] deliver with match rule \"%s\"\n",
				 rule->rule_text);
			return 1;
		} else {
			pr_debug("[NO]  deliver with match rule \"%s\"\n",
				 rule->rule_text);
		}
	}
	pr_debug("[NO]  no match rules\n");
	return 0;
}

static struct hlist_head
*bus_matchmaker_get_rules(struct bus_match_maker *matchmaker,
			  int message_type, const char *interface)
{
	static struct hlist_head empty = {0,};
	struct rule_pool *p;

	WARN_ON(message_type < 0);
	WARN_ON(message_type >= DBUS_NUM_MESSAGE_TYPES);

	p = matchmaker->rules_by_type + message_type;

	if (interface == NULL)
		return &p->rules_without_iface;
	else {
		struct bus_match_rule *rule =
			match_rule_search(&p->rules_by_iface, interface);
		if (rule)
			return &rule->first;
		else
			return &empty;
	}
}

bool bus_matchmaker_filter(struct bus_match_maker *matchmaker,
			   struct bus_match_maker *sender,
			   int eavesdrop,
			   const struct dbus_message *message)
{
	int type;
	const char *interface;
	struct hlist_head *neither, *just_type, *just_iface, *both;

	type = message->type;
	interface = message->interface;

	neither = bus_matchmaker_get_rules(matchmaker,
					   DBUS_MESSAGE_TYPE_INVALID, NULL);
	just_type = just_iface = both = NULL;

	if (interface != NULL)
		just_iface = bus_matchmaker_get_rules(matchmaker,
						      DBUS_MESSAGE_TYPE_INVALID,
						      interface);

	if (type > DBUS_MESSAGE_TYPE_INVALID && type < DBUS_NUM_MESSAGE_TYPES) {
		just_type = bus_matchmaker_get_rules(matchmaker, type, NULL);

		if (interface != NULL)
			both = bus_matchmaker_get_rules(matchmaker, type,
							interface);
	}

	if (get_recipients_from_list(matchmaker, sender, eavesdrop, neither,
				     message))
		return 1;
	if (get_recipients_from_list(matchmaker, sender, eavesdrop, just_iface,
				     message))
		return 1;
	if (get_recipients_from_list(matchmaker, sender, eavesdrop, just_type,
				     message))
		return 1;
	if (get_recipients_from_list(matchmaker, sender, eavesdrop, both,
				     message))
		return 1;

	return connection_is_primary_owner(matchmaker, message->destination);
}

void bus_matchmaker_add_name(struct bus_match_maker *matchmaker,
			     const char *name,
			     gfp_t gfp_flags)
{
	struct dbus_name *dbus_name;
	struct rb_node **new = &(matchmaker->names.rb_node), *parent = NULL;

	dbus_name = kmalloc(sizeof(struct dbus_name), gfp_flags);
	if (!dbus_name)
		return;
	dbus_name->name = kstrdup(name, gfp_flags);
	if (!dbus_name->name)
		return;

	/* Figure out where to put new node */
	while (*new) {
		struct dbus_name *this = container_of(*new, struct dbus_name,
						      node);
		int result = strcmp(dbus_name->name, this->name);

		parent = *new;
		if (result < 0)
			new = &((*new)->rb_left);
		else if (result > 0)
			new = &((*new)->rb_right);
		else
			return;
	}

	/* Add new node and rebalance tree. */
	rb_link_node(&dbus_name->node, parent, new);
	rb_insert_color(&dbus_name->node, &matchmaker->names);
}

void bus_matchmaker_remove_name(struct bus_match_maker *matchmaker,
				const char *name)
{
	struct rb_node *node = matchmaker->names.rb_node;

	while (node) {
		struct dbus_name *data = container_of(node, struct dbus_name,
						      node);
		int result;

		result = strcmp(name, data->name);

		if (result < 0)
			node = node->rb_left;
		else if (result > 0)
			node = node->rb_right;
		else {
			rb_erase(&data->node, &matchmaker->names);
			kfree(data->name);
			kfree(data);
			node = NULL;
		}
	}

}
