/*
 * Copyright (C) 2008, 2009 Provigent Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Driver for the ARM PrimeCell(tm) General Purpose Input/Output (PL061)
 *
 * Data sheet: ARM DDI 0190B, September 2000
 */
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/bitops.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/device.h>
#ifdef CONFIG_ARM_AMBA
#include <linux/amba/bus.h>
#include <asm/mach/irq.h>
#else
#include <linux/of_platform.h>
#include <linux/of.h>
#endif
#include <linux/amba/pl061.h>
#include <linux/slab.h>
#include <linux/pm.h>

#ifdef CONFIG_PPC
#define readb(addr) ((char)readl(addr))
#define writeb(b, addr) writel(b, addr)

#define GPIO_AFSEL 0x420
#endif

#define GPIODIR 0x400
#define GPIOIS  0x404
#define GPIOIBE 0x408
#define GPIOIEV 0x40C
#define GPIOIE  0x410
#define GPIORIS 0x414
#define GPIOMIS 0x418
#define GPIOIC  0x41C

#define PL061_GPIO_NR	8

#ifdef CONFIG_PM
struct pl061_context_save_regs {
	u8 gpio_data;
	u8 gpio_dir;
	u8 gpio_is;
	u8 gpio_ibe;
	u8 gpio_iev;
	u8 gpio_ie;
};
#endif

#ifdef CONFIG_PPC
static inline void chained_irq_enter(struct irq_chip *chip,
					struct irq_desc *desc) {}

static inline void chained_irq_exit(struct irq_chip *chip,
					struct irq_desc *desc)
{
	if (chip->irq_eoi)
		chip->irq_eoi(&desc->irq_data);
}
#endif

struct pl061_gpio {
	/* Each of the two spinlocks protects a different set of hardware
	 * regiters and data structurs. This decouples the code of the IRQ from
	 * the GPIO code. This also makes the case of a GPIO routine call from
	 * the IRQ code simpler.
	 */
	spinlock_t		lock;		/* GPIO registers */

	void __iomem		*base;
	int			irq_base;
	struct irq_chip_generic	*irq_gc;
	struct gpio_chip	gc;

#ifdef CONFIG_PM
	struct pl061_context_save_regs csave_regs;
#endif
};

static int pl061_direction_input(struct gpio_chip *gc, unsigned offset)
{
	struct pl061_gpio *chip = container_of(gc, struct pl061_gpio, gc);
	unsigned long flags;
	unsigned char gpiodir;

	if (offset >= gc->ngpio)
		return -EINVAL;

	spin_lock_irqsave(&chip->lock, flags);
	gpiodir = readb(chip->base + GPIODIR);
	gpiodir &= ~(1 << offset);
	writeb(gpiodir, chip->base + GPIODIR);
	spin_unlock_irqrestore(&chip->lock, flags);

	return 0;
}

static int pl061_direction_output(struct gpio_chip *gc, unsigned offset,
		int value)
{
	struct pl061_gpio *chip = container_of(gc, struct pl061_gpio, gc);
	unsigned long flags;
	unsigned char gpiodir;

	if (offset >= gc->ngpio)
		return -EINVAL;

	spin_lock_irqsave(&chip->lock, flags);
	writeb(!!value << offset, chip->base + (1 << (offset + 2)));
	gpiodir = readb(chip->base + GPIODIR);
	gpiodir |= 1 << offset;
	writeb(gpiodir, chip->base + GPIODIR);

	/*
	 * gpio value is set again, because pl061 doesn't allow to set value of
	 * a gpio pin before configuring it in OUT mode.
	 */
	writeb(!!value << offset, chip->base + (1 << (offset + 2)));
	spin_unlock_irqrestore(&chip->lock, flags);

	return 0;
}

static int pl061_get_value(struct gpio_chip *gc, unsigned offset)
{
	struct pl061_gpio *chip = container_of(gc, struct pl061_gpio, gc);

	return !!readb(chip->base + (1 << (offset + 2)));
}

static void pl061_set_value(struct gpio_chip *gc, unsigned offset, int value)
{
	struct pl061_gpio *chip = container_of(gc, struct pl061_gpio, gc);

	writeb(!!value << offset, chip->base + (1 << (offset + 2)));
}

static int pl061_to_irq(struct gpio_chip *gc, unsigned offset)
{
	struct pl061_gpio *chip = container_of(gc, struct pl061_gpio, gc);

	if (chip->irq_base <= 0)
		return -EINVAL;

	return chip->irq_base + offset;
}

static int pl061_irq_type(struct irq_data *d, unsigned trigger)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct pl061_gpio *chip = gc->private;
	int offset = d->irq - chip->irq_base;
	unsigned long flags;
	u8 gpiois, gpioibe, gpioiev;

	if (offset < 0 || offset >= PL061_GPIO_NR)
		return -EINVAL;

	raw_spin_lock_irqsave(&gc->lock, flags);

	gpioiev = readb(chip->base + GPIOIEV);

	gpiois = readb(chip->base + GPIOIS);
	if (trigger & (IRQ_TYPE_LEVEL_HIGH | IRQ_TYPE_LEVEL_LOW)) {
		gpiois |= 1 << offset;
		if (trigger & IRQ_TYPE_LEVEL_HIGH)
			gpioiev |= 1 << offset;
		else
			gpioiev &= ~(1 << offset);
	} else
		gpiois &= ~(1 << offset);
	writeb(gpiois, chip->base + GPIOIS);

	gpioibe = readb(chip->base + GPIOIBE);
	if ((trigger & IRQ_TYPE_EDGE_BOTH) == IRQ_TYPE_EDGE_BOTH)
		gpioibe |= 1 << offset;
	else {
		gpioibe &= ~(1 << offset);
		if (trigger & IRQ_TYPE_EDGE_RISING)
			gpioiev |= 1 << offset;
		else if (trigger & IRQ_TYPE_EDGE_FALLING)
			gpioiev &= ~(1 << offset);
	}
	writeb(gpioibe, chip->base + GPIOIBE);

	writeb(gpioiev, chip->base + GPIOIEV);

	raw_spin_unlock_irqrestore(&gc->lock, flags);

	return 0;
}

static void pl061_irq_handler(unsigned irq, struct irq_desc *desc)
{
	unsigned long pending;
	int offset;
	struct pl061_gpio *chip = irq_desc_get_handler_data(desc);
	struct irq_chip *irqchip = irq_desc_get_chip(desc);

	chained_irq_enter(irqchip, desc);

	pending = readb(chip->base + GPIOMIS);
	writeb(pending, chip->base + GPIOIC);
	if (pending) {
		for_each_set_bit(offset, &pending, PL061_GPIO_NR)
			generic_handle_irq(pl061_to_irq(&chip->gc, offset));
	}

	chained_irq_exit(irqchip, desc);
}

static void __init pl061_init_gc(struct pl061_gpio *chip, int irq_base)
{
	struct irq_chip_type *ct;

	chip->irq_gc = irq_alloc_generic_chip("gpio-pl061", 1, irq_base,
					      chip->base, handle_simple_irq);
	chip->irq_gc->private = chip;

	ct = chip->irq_gc->chip_types;
	ct->chip.irq_mask = irq_gc_mask_clr_bit;
	ct->chip.irq_unmask = irq_gc_mask_set_bit;
	ct->chip.irq_set_type = pl061_irq_type;
	ct->chip.irq_set_wake = irq_gc_set_wake;
	ct->regs.mask = GPIOIE;

	irq_setup_generic_chip(chip->irq_gc, IRQ_MSK(PL061_GPIO_NR),
			       IRQ_GC_INIT_NESTED_LOCK, IRQ_NOREQUEST, 0);
}

static int __devinit pl061_probe(struct device *dev,
	struct resource *res, int irq, struct pl061_gpio **retchip)
{
	struct pl061_platform_data *pdata;
	struct pl061_gpio *chip;
	int ret, i;

	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (chip == NULL)
		return -ENOMEM;

	pdata = dev->platform_data;
	if (pdata) {
		chip->gc.base = pdata->gpio_base;
		chip->irq_base = pdata->irq_base;
	} else if (dev->of_node) {
		chip->gc.base = -1;
		chip->irq_base = 0;
	} else {
		ret = -ENODEV;
		goto free_mem;
	}

	if (!request_mem_region(res->start,
				resource_size(res), "pl061")) {
		ret = -EBUSY;
		goto free_mem;
	}

	chip->base = ioremap(res->start, resource_size(res));
	if (chip->base == NULL) {
		ret = -ENOMEM;
		goto release_region;
	}

	spin_lock_init(&chip->lock);

	chip->gc.direction_input = pl061_direction_input;
	chip->gc.direction_output = pl061_direction_output;
	chip->gc.get = pl061_get_value;
	chip->gc.set = pl061_set_value;
	chip->gc.to_irq = pl061_to_irq;
	chip->gc.ngpio = PL061_GPIO_NR;
	chip->gc.label = dev_name(dev);
	chip->gc.dev = dev;
	chip->gc.owner = THIS_MODULE;

	ret = gpiochip_add(&chip->gc);
	if (ret)
		goto iounmap;

	if (retchip)
		*retchip = chip;

	/*
	 * irq_chip support
	 */

	if (chip->irq_base <= 0)
		return 0;

	pl061_init_gc(chip, chip->irq_base);

	writeb(0, chip->base + GPIOIE); /* disable irqs */
	if (irq < 0) {
		ret = -ENODEV;
		goto iounmap;
	}
	irq_set_chained_handler(irq, pl061_irq_handler);
	irq_set_handler_data(irq, chip);

	for (i = 0; i < PL061_GPIO_NR; i++) {
		if (pdata) {
			if (pdata->directions & (1 << i))
				pl061_direction_output(&chip->gc, i,
						pdata->values & (1 << i));
			else
				pl061_direction_input(&chip->gc, i);
		}
	}

	dev_set_drvdata(dev, chip);

	return 0;

iounmap:
	iounmap(chip->base);
release_region:
	release_mem_region(res->start, resource_size(res));
free_mem:
	kfree(chip);

	if (retchip)
		*retchip = NULL;
	return ret;
}

#ifdef CONFIG_PM
static int pl061_suspend(struct device *dev)
{
	struct pl061_gpio *chip = dev_get_drvdata(dev);
	int offset;

	chip->csave_regs.gpio_data = 0;
	chip->csave_regs.gpio_dir = readb(chip->base + GPIODIR);
	chip->csave_regs.gpio_is = readb(chip->base + GPIOIS);
	chip->csave_regs.gpio_ibe = readb(chip->base + GPIOIBE);
	chip->csave_regs.gpio_iev = readb(chip->base + GPIOIEV);
	chip->csave_regs.gpio_ie = readb(chip->base + GPIOIE);

	for (offset = 0; offset < PL061_GPIO_NR; offset++) {
		if (chip->csave_regs.gpio_dir & (1 << offset))
			chip->csave_regs.gpio_data |=
				pl061_get_value(&chip->gc, offset) << offset;
	}

	return 0;
}

static int pl061_resume(struct device *dev)
{
	struct pl061_gpio *chip = dev_get_drvdata(dev);
	int offset;

	for (offset = 0; offset < PL061_GPIO_NR; offset++) {
		if (chip->csave_regs.gpio_dir & (1 << offset))
			pl061_direction_output(&chip->gc, offset,
					chip->csave_regs.gpio_data &
					(1 << offset));
		else
			pl061_direction_input(&chip->gc, offset);
	}

	writeb(chip->csave_regs.gpio_is, chip->base + GPIOIS);
	writeb(chip->csave_regs.gpio_ibe, chip->base + GPIOIBE);
	writeb(chip->csave_regs.gpio_iev, chip->base + GPIOIEV);
	writeb(chip->csave_regs.gpio_ie, chip->base + GPIOIE);

	return 0;
}

static const struct dev_pm_ops pl061_dev_pm_ops = {
	.suspend = pl061_suspend,
	.resume = pl061_resume,
	.freeze = pl061_suspend,
	.restore = pl061_resume,
};
#endif

#ifdef CONFIG_ARM_AMBA
static int __init pl061_amba_probe(struct amba_device *dev, struct amba_id *id)
{
	return pl061_probe(&dev->dev, &dev->res, dev->irq[0], NULL);
}

static struct amba_id pl061_ids[] = {
	{
		.id	= 0x00041061,
		.mask	= 0x000fffff,
	},
	{ 0, 0 },
};

MODULE_DEVICE_TABLE(amba, pl061_ids);

static struct amba_driver pl061_gpio_driver = {
	.drv = {
		.name	= "pl061_gpio",
#ifdef CONFIG_PM
		.pm	= &pl061_dev_pm_ops,
#endif
	},
	.id_table	= pl061_ids,
	.probe		= pl061_amba_probe,
};
#else
static int __devinit pl061_of_probe(struct platform_device *ofdev)
{
	struct resource r_mem;
	struct pl061_platform_data pl061_data = {0};
	int irq;
	const u32 *prop;
	int len;
	struct pl061_gpio *chip = NULL;
	int ret;

	if (of_address_to_resource(ofdev->dev.of_node, 0, &r_mem))
		return -ENODEV;

	pl061_data.gpio_base = 0;
	prop = of_get_property(ofdev->dev.of_node, "cell-index", &len);
	if (!prop || len < sizeof(*prop))
		dev_warn(&ofdev->dev, "no 'cell-index' property\n");
	else
		pl061_data.gpio_base = *prop;

	irq = of_irq_to_resource(ofdev->dev.of_node, 0, NULL);
	pl061_data.irq_base = irq;
	if (irq == NO_IRQ)
		pl061_data.irq_base = (unsigned) -1;

	ofdev->dev.platform_data = &pl061_data;

	ret = pl061_probe(&ofdev->dev, &r_mem, irq, &chip);

	if (ret < 0)
		return ret;

	prop = of_get_property(ofdev->dev.of_node, "pins-map", &len);
	if (!prop || len < sizeof(*prop))
		dev_warn(&ofdev->dev, "no 'pins-map' property\n");
	else
		writeb(*prop, chip->base + GPIO_AFSEL);

	return 0;
}

static struct of_device_id pl061_match[] = {
	{
		.compatible = "amba_pl061",
	},
	{ /* end of list */ },
};

static struct platform_driver pl061_gpio_driver = {
	.driver = {
		.name = "gpio-pl061",
		.of_match_table = pl061_match,
	},
	.probe = pl061_of_probe,
};
#endif

static int __init pl061_gpio_init(void)
{
#ifdef CONFIG_ARM_AMBA
	return amba_driver_register(&pl061_gpio_driver);
#else
	return platform_driver_register(&pl061_gpio_driver);
#endif
}
subsys_initcall(pl061_gpio_init);

MODULE_AUTHOR("Baruch Siach <baruch@tkos.co.il>");
MODULE_DESCRIPTION("PL061 GPIO driver");
MODULE_LICENSE("GPL");
