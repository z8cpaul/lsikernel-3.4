#
# Makefile for the linux kernel.
#
obj-y					+= axxia.o
obj-y					+= clock.o
obj-y                                   += io.o
obj-y					+= ncr.o
obj-y					+= timers.o
obj-y					+= pci.o
obj-y					+= ddr_retention.o ddr_shutdown.o
obj-$(CONFIG_I2C)			+= i2c.o
obj-$(CONFIG_AXXIA_RIO)			+= rapidio.o
obj-$(CONFIG_SMP)			+= platsmp.o headsmp.o
obj-$(CONFIG_ARCH_AXXIA_GIC)		+= axxia-gic.o
obj-$(CONFIG_HOTPLUG_CPU)		+= hotplug.o
