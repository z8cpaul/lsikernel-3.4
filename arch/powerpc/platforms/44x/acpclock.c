/*
 * Copyright (C) 2010-2012 Wind River Systems
 *
 * Some drivers like pl022 need clk interface. But now these drivers
 * can work without it. So define a dummy clk driver for compile issue
 * and future extend.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/export.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/kref.h>
#include <linux/slab.h>
#include <linux/of.h>

#include <asm/dcr-native.h>
#include <asm/clk_interface.h>

#define CLK_DEBUG

#define CLK_REF0 125000000

static unsigned char ps[] = {1, 3, 2, 4};

static unsigned char idiv[] = {
	1, 16, 17, 30, 13, 18, 7, 31, 14, 11, 19, 21, 27, 8, 23, 32,
	15, 29, 12, 6, 10, 20, 26, 22, 28, 5, 9, 25, 4, 24, 3, 2};

static unsigned char odiv[] = {
	1, 2, 28, 27, 22, 21, 30, 29, 24, 23, 12, 11, 16, 15, 32, 31,
	26, 25, 20, 19, 10, 9, 14, 13, 18, 17, 8, 7, 6, 5, 4, 3};

static unsigned char mult[] = {
	1, 123, 117, 251, 245, 69, 111, 125, 119, 95, 105, 197, 239,
	163, 63, 253, 247, 187, 57, 223, 233, 207, 157, 71, 113, 15,
	89, 37, 191, 19, 99, 127, 121, 109, 93, 61, 185, 155, 13, 97,
	107, 11, 9, 81, 31, 49, 83, 199, 241, 33, 181, 143, 217, 173,
	51, 165, 65, 85, 151, 147, 227, 41, 201, 255, 249, 243, 195,
	237, 221, 231, 35, 189, 59, 183, 79, 29, 141, 215, 145, 225,
	235, 219, 27, 139, 137, 135, 175, 209, 159, 53, 45, 177, 211,
	23, 167, 73, 115, 67, 103, 161, 55, 205, 87, 17, 91, 153, 7,
	47, 179, 171, 149, 39, 193, 229, 77, 213, 25, 133, 43, 21,
	101, 203, 5, 169, 75, 131, 3, 129, 1, 250, 244, 124, 118, 196,
	238, 252, 246, 222, 232, 70, 112, 36, 190, 126, 120, 60, 184,
	96, 106, 80, 30, 198, 240, 142, 216, 164, 64, 146, 226, 254,
	248, 236, 220, 188, 58, 28, 140, 224, 234, 138, 136, 208, 158,
	176, 210, 72, 114, 160, 54, 16, 90, 46, 178, 38, 192, 212, 24,
	20, 100, 168, 74, 128, 122, 116, 68, 110, 94, 104, 162, 62,
	186, 56, 206, 156, 14, 88, 18, 98, 108, 92, 154, 12, 10, 8,
	48, 82, 32, 180, 172, 50, 84, 150, 40, 200, 242, 194, 230, 34,
	182, 78, 214, 144, 218, 26, 134, 174, 52, 44, 22, 166, 66,
	102, 204, 86, 152, 6, 170, 148, 228, 76, 132, 42, 202, 4, 130,
	2
};

#define PREDIV(value)       (idiv[(((value) & 0xfc000000) >> 26)])
#define MULTINT(value)      (((value) & 0x00000ffe) >> 1)
#define MULTINT_PRE(value)  (ps[((MULTINT((value)) & 0x300) >> 8)])
#define MULTINT_MAIN(value) (mult[(MULTINT((value)) & 0xff)])
#define RANGEA(value)       (((value) & 0x03f80000) >> 19)
#define RANGEA_PRE(value)   (ps[((RANGEA((value)) & 0x60) >> 5)])
#define RANGEA_MAIN(value)  (odiv[(RANGEA((value)) & 0x1f)])
#define RANGEB(value)       (((value) & 0x0007f000) >> 12)
#define RANGEB_PRE(value)   (ps[((RANGEB((value)) & 0x60) >> 5)])
#define RANGEB_MAIN(value)  (odiv[(RANGEB((value)) & 0x1f)])


#define CLK_HAS_RATE	0x1	/* has rate in MHz */
#define CLK_HAS_CTRL	0x2	/* has control reg and bit */

struct clk {
	struct list_head node;
	char name[32];
	int flags;
	struct device *dev;
	unsigned long rate;
	void (*calc) (struct clk *);
	struct clk *parent;
	int reg, bit;		/* CLK_HAS_CTRL */
	int div_shift;		/* only used by generic_div_clk_calc */
	struct kref kref;
};

static LIST_HEAD(clocks);
static DEFINE_MUTEX(clocks_mutex);

struct clk *acp_clk_get(struct device *dev, const char *id)
{
	struct clk *clk;

	if (!dev)
		return NULL;

	mutex_lock(&clocks_mutex);
	list_for_each_entry(clk, &clocks, node) {
		if (clk->dev == dev) {
			kref_get(&clk->kref);
			mutex_unlock(&clocks_mutex);
			return clk;
		}
	}

	clk = kzalloc(sizeof(*clk), GFP_KERNEL);
	if (!clk) {
		dev_err(dev, "Create clock error\n");
		return ERR_PTR(-ENOMEM);
	}

	INIT_LIST_HEAD(&clk->node);
	kref_init(&clk->kref);
	clk->dev = dev;
	list_add(&clk->node, &clocks);
	mutex_unlock(&clocks_mutex);

	return clk;
}

static void acp_clk_release(struct kref *kref)
{
	struct clk *clk = container_of(kref, struct clk, kref);

	mutex_lock(&clocks_mutex);
	list_del(&clk->node);
	mutex_lock(&clocks_mutex);

	kfree(clk);
}

void acp_clk_put(struct clk *clk)
{
	kref_put(&clk->kref, acp_clk_release);
	return;
}

int acp_clk_enable(struct clk *clk)
{
	return 0;
}

void acp_clk_disable(struct clk *clk)
{
	return;
}

unsigned long acp_clk_get_rate(struct clk *clk)
{
	struct device_node *np = clk->dev->of_node;
	const int *prop;
	int len;

	if (!np)
		return 0;

	prop = of_get_property(np, "clock-frequency", &len);
	if (!prop || len != sizeof(*prop)) {
		dev_err(clk->dev, "Get the clock frequency error\n");
		return -1;
	}

	return *prop;
}

long acp_clk_round_rate(struct clk *clk, unsigned long rate)
{
	return 0;
}

int acp_clk_set_rate(struct clk *clk, unsigned long rate)
{
	return 0;
}

static int clk_register(struct clk *clk)
{
	mutex_lock(&clocks_mutex);
	list_add(&clk->node, &clocks);
	mutex_unlock(&clocks_mutex);
	return 0;
}

/*
  ------------------------------------------------------------------------------
  get_ppc_pll

  Assumes the the PPC PLL is locked...
*/

static void get_ppc_pll(unsigned long *dco, unsigned long *pllouta, unsigned long *plloutb)
{
	unsigned long ctrl;

	ctrl = mfdcr(0xd02);

	*dco = (CLK_REF0 / 1000 / PREDIV(ctrl));
	*dco *= (MULTINT_PRE(ctrl) * MULTINT_MAIN(ctrl));
	*pllouta = *dco / (RANGEA_PRE(ctrl) * RANGEA_MAIN(ctrl));
	*plloutb = *dco / (RANGEB_PRE(ctrl) * RANGEB_MAIN(ctrl));
}


static void ppc_clk_calc(struct clk *clk)
{
	unsigned long mcgc = mfdcr(0xd00);

	if (!(mcgc & 0xc0000000)) {
		/* ppc_clk is clk_ref0 */
		clk->rate = CLK_REF0 / 1000;
	} else {
		unsigned long dco = 0;
		unsigned long pllouta = 0;
		unsigned long plloutb = 0;

		get_ppc_pll(&dco, &pllouta, &plloutb);
		if (((mcgc & 0xc0000000) >> 30) == 1)
			clk->rate = pllouta;
		else
			clk->rate = pllouta >> 1;
	}
}
static struct clk ppc_clk = {
	.name = "ppc_clk",
	.calc = ppc_clk_calc,
};

static void pbus_clk_calc(struct clk *clk)
{
	unsigned long mcgc = mfdcr(0xd00);
	unsigned long mcgs = mfdcr(0xd01);

	if (!(mcgc & 0x08000000)) {
		/* clk_per is clk_ref0 */
		clk->rate = CLK_REF0 / 1000;
	} else if (mcgs & 0x80000000) {
		unsigned long dco = 0;
		unsigned long pllouta = 0;
		unsigned long plloutb = 0;

		get_ppc_pll(&dco, &pllouta, &plloutb);
		clk->rate = plloutb;
	} else
		clk->rate = -1;
}

static struct clk pbus_clk = {
	.name = "clk_per",
	.calc = pbus_clk_calc,
};

struct clk *rate_clks[] = {
	&ppc_clk,
	&pbus_clk,
	NULL
};

static void rate_clk_init(struct clk *clk)
{
	if (clk->calc) {
		clk->calc(clk);
		clk->flags |= CLK_HAS_RATE;
		clk_register(clk);
	} else {
		pr_err("Could not initialize clk %s without a calc routine\n",
		       clk->name);
	}
}

static void rate_clks_init(void)
{
	struct clk **cpp, *clk;

	cpp = rate_clks;
	while ((clk = *cpp++))
		rate_clk_init(clk);
}



static struct clk_interface acp_clk_functions = {
	.clk_get                = acp_clk_get,
	.clk_enable             = acp_clk_enable,
	.clk_disable            = acp_clk_disable,
	.clk_get_rate           = acp_clk_get_rate,
	.clk_put                = acp_clk_put,
	.clk_round_rate         = acp_clk_round_rate,
	.clk_set_rate           = acp_clk_set_rate,
	.clk_set_parent         = NULL,
	.clk_get_parent         = NULL,
};

int __init acp_clk_init(void)
{
	rate_clks_init();

	clk_functions = acp_clk_functions;
	return 0;
}
EXPORT_SYMBOL(acp_clk_init);
