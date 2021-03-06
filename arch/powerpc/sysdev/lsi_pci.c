/*
* LSI specific PCI-Express support;
*/

#undef DEBUG

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/bootmem.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include <linux/io.h>
#include <asm/pci-bridge.h>
#include <asm/machdep.h>
#include <asm/dcr.h>
#include <asm/dcr-regs.h>
#include <mm/mmu_decl.h>

#include "ppc4xx_pci.h"
#include "../../../drivers/misc/lsi-ncr.h"

#include <linux/interrupt.h>

static int acp_plx;

#undef PRINT_CONFIG_ACCESSES
/*#define PRINT_CONFIG_ACCESSES*/

static int dma_offset_set;
static u32 last_mpage;
static u32 last_port;

#define U64_TO_U32_LOW(val)	((u32)((val) & 0x00000000ffffffffULL))
#define U64_TO_U32_HIGH(val)	((u32)((val) >> 32))

#define RES_TO_U32_LOW(val)	\
	((sizeof(resource_size_t) > sizeof(u32)) ? U64_TO_U32_LOW(val) : (val))
#define RES_TO_U32_HIGH(val)	\
	((sizeof(resource_size_t) > sizeof(u32)) ? U64_TO_U32_HIGH(val) : (0))


#define ACPX1_PCIE_MPAGE_UPPER(n) (0x1010 + (n * 8))
#define ACPX1_PCIE_MPAGE_LOWER(n) (0x1014 + (n * 8))

static void __init
fixup_acp_pci_bridge(struct pci_dev *dev)
{
	/* if we aren't a PCIe don't bother */
	if (!pci_find_capability(dev, PCI_CAP_ID_EXP))
		return ;

	/*
	 * Set the class appropriately for a bridge device
	 */
	printk(KERN_INFO
		"PCI: Setting PCI Class to PCI_CLASS_BRIDGE_HOST for"
		" %04x:%04x\n", dev->vendor, dev->device);

	dev->class = PCI_CLASS_BRIDGE_HOST << 8;

	/*
	 * Make the bridge transparent
	 */
	dev->transparent = 1;

	return ;
}

DECLARE_PCI_FIXUP_HEADER(0x1000, 0x5101, fixup_acp_pci_bridge);
DECLARE_PCI_FIXUP_HEADER(0x1000, 0x5108, fixup_acp_pci_bridge);
DECLARE_PCI_FIXUP_HEADER(0x1000, 0x5102, fixup_acp_pci_bridge);

static int __init acp_parse_dma_ranges(struct pci_controller *hose,
					  void __iomem *reg,
					  struct resource *res)
{
	u64 size;
	const u32 *ranges;
	int rlen;
	int pna = of_n_addr_cells(hose->dn);
	int np = pna + 5;

	/* Default */
	res->start = 0;
	size = 0x80000000;
	res->end = size - 1;
	res->flags = IORESOURCE_MEM | IORESOURCE_PREFETCH;

	/* Get dma-ranges property */
	ranges = of_get_property(hose->dn, "dma-ranges", &rlen);
	if (ranges == NULL) {
		printk(KERN_INFO "Not able to parse dma-ranges hence using defaults\n");
		goto out;
	}

	/* Walk it */
	while ((rlen -= np * 4) >= 0) {
		u32 pci_space = ranges[0];
		u64 pci_addr = of_read_number(ranges + 1, 2);
		u64 cpu_addr = of_translate_dma_address(hose->dn, ranges + 3);
		size = of_read_number(ranges + pna + 3, 2);
		ranges += np;
		if (cpu_addr == OF_BAD_ADDR || size == 0)
			continue;

		/* We only care about memory */
		if ((pci_space & 0x03000000) != 0x02000000)
			continue;

		/* We currently only support memory at 0, and pci_addr
		 * within 32 bits space
		 */
		if (cpu_addr != 0 || pci_addr > 0xffffffff) {
			printk(KERN_WARNING "%s: Ignored unsupported" \
				"dma range"\
			       " 0x%016llx...0x%016llx -> 0x%016llx\n",\
			       hose->dn->full_name,
			       pci_addr, pci_addr + size - 1, cpu_addr);
			continue;
		}

		/* Check if not prefetchable */
		if (!(pci_space & 0x40000000))
			res->flags &= ~IORESOURCE_PREFETCH;


		/* Use that */
		res->start = pci_addr;
		/* Beware of 32 bits resources */
		if (sizeof(resource_size_t) == sizeof(u32) &&
		    (pci_addr + size) > 0x100000000ull)
			res->end = 0xffffffff;
		else
			res->end = res->start + size - 1;
		break;
	}

	/* We only support one global DMA offset */
	if (dma_offset_set && pci_dram_offset != res->start) {
		printk(KERN_ERR "%s: dma-ranges(s) mismatch\n",
		       hose->dn->full_name);
		return -ENXIO;
	}

	/* Check that we can fit all of memory as we don't support
	 * DMA bounce buffers
	 */
	if (size < total_memory) {
		printk(KERN_ERR "%s: dma-ranges too small "\
		       "(size=%llx total_memory=%llx)\n",\
		       hose->dn->full_name, size, (u64)total_memory);
		return -ENXIO;
	}

	/* Check that base is a multiple of size*/
	if ((res->start & (size - 1)) != 0) {
		printk(KERN_ERR "%s: dma-ranges unaligned\n",
		       hose->dn->full_name);
		return -ENXIO;
	}

	/* Check that we are fully contained within 32 bits space */
	if (res->end > 0xffffffff) {
		printk(KERN_ERR "%s: dma-ranges outside of 32 bits space\n",
		       hose->dn->full_name);
		return -ENXIO;
	}
	return 0;

out:
	dma_offset_set = 1;
	pci_dram_offset = res->start;
	hose->dma_window_base_cur = res->start;
	hose->dma_window_size = resource_size(res);

	printk(KERN_INFO "ACP PCI DMA offset set to 0x%08lx\n",
	       pci_dram_offset);
	printk(KERN_INFO "ACP PCI DMA window base to 0x%016llx\n",
	       (unsigned long long)hose->dma_window_base_cur);
	printk(KERN_INFO "DMA window size 0x%016llx\n",
	       (unsigned long long)hose->dma_window_size);
	return 0;
}


#define MAX_PCIE_BUS_MAPPED	0x40

struct pciex_port {
	struct pci_controller	*hose;
	struct device_node	*node;
	unsigned int		index;
	int			endpoint;
	int			link;
	int			has_ibpre;
	unsigned int		sdr_base;
	dcr_host_t		dcrs;
	struct resource		cfg_space;
	struct resource		utl_regs;
	void __iomem		*utl_base;
	int	acpChipType;
};

static struct pciex_port *acp_pciex_ports;
static unsigned int acp_pciex_port_count;

struct pciex_hwops {
	bool want_sdr;
	int (*core_init)(struct device_node *np);
	int (*port_init_hw)(struct pciex_port *port);
	int (*setup_utl)(struct pciex_port *port);
	void (*check_link)(struct pciex_port *port);
};

static struct pciex_hwops *acp_pcie_hwops;


static int __init
acp_pciex_core_init(struct device_node *np)
{
	return 3;
}

static int
acp_pciex_port_init_hw(struct pciex_port *port)
{
	return 0;
}

static int
acp_pciex_setup_utl(struct pciex_port *port)
{
	return 0;
}

static struct pciex_hwops hwops __initdata = {
	.core_init      = acp_pciex_core_init,
	.port_init_hw   = acp_pciex_port_init_hw,
	.setup_utl      = acp_pciex_setup_utl
};

/* Check that the core has been initied and if not, do it */
static int __init acp_pciex_check_core_init(struct device_node *np)
{
	static int core_init;
	int count = -ENODEV;

	if (core_init++)
		return 0;

	if (of_device_is_compatible(np, "lsi,plb-pciex"))
		acp_pcie_hwops = &hwops;

	if (acp_pcie_hwops == NULL) {
		printk(KERN_WARNING "PCIE: unknown host type %s\n",
		       np->full_name);
		return -ENODEV;
	}

	count = acp_pcie_hwops->core_init(np);
	if (count > 0) {
		acp_pciex_ports =
		       kzalloc(count * sizeof(struct pciex_port),
			       GFP_KERNEL);
		if (acp_pciex_ports) {
			acp_pciex_port_count = count;
			return 0;
		}
		printk(KERN_WARNING "PCIE: failed to allocate ports array\n");
		return -ENOMEM;
	}
	return -ENODEV;
}


static int acp_pciex_validate_bdf(struct pciex_port *port,
				     struct pci_bus *bus,
				     unsigned int devfn)
{
	static int message;

	/* Endpoint can not generate upstream(remote) config cycles */
	if (port->endpoint && bus->number != port->hose->first_busno)
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* Check we are within the mapped range */
	if (bus->number > port->hose->last_busno) {
		if (!message) {
			printk(KERN_WARNING "Warning! Probing bus %u"\
			       " out of range !\n", bus->number);
			message++;
		}
		return PCIBIOS_DEVICE_NOT_FOUND;
	}

	/* The root complex has only one device / function */
	if (bus->number == port->hose->first_busno && devfn != 0)
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* The other side of the RC has only one device as well */
	if (bus->number == (port->hose->first_busno + 1) &&
	    PCI_SLOT(devfn) != 0)
		return PCIBIOS_DEVICE_NOT_FOUND;

	return 0;
}

static void __iomem *pciex_acp_get_config_base(struct pciex_port *port,
						  struct pci_bus *bus,
						  unsigned int devfn)
{
	int relbus;
	unsigned mpage;
	u32 addr;
	int dev, fn;
	int cfg_type;

	/* Remove the casts when we finally remove the stupid volatile
	 * in struct pci_controller
	 */
	if (bus->number == port->hose->first_busno)
		return (void __iomem *)port->hose->cfg_addr;

	relbus = bus->number - (port->hose->first_busno + 1);

	/*
	 * Set MPAGE0 to map config access for this BDF
	 */

	dev = ((devfn & 0xf8) >> 3);
	fn  = devfn & 0x7;

	if (dev > 31)
		return NULL;

#ifdef CONFIG_ACP_X1V1
	/* v1 only supports fn=0 */
	if (fn)
		return NULL;
#else
	/* v2 only supports fn0-3 and bus0-63 */
	if (port->acpChipType == 1)
		if ((fn > 3) || (bus->number > 63))
				return NULL;
#endif
	if (relbus && (bus->number != bus->primary))
		cfg_type = 1;
	else
		cfg_type = 0;

	/* build the mpage register */
	mpage = (bus->number << 11) |
		(dev << 6) |
		(cfg_type << 5);

	mpage |= 0x10;   /* enable MPAGE for configuration access */

	if (5 > port->acpChipType)
		mpage |= 1;

	/* the function number moved for X2 */
	if (port->acpChipType < 2)
		mpage |= (fn << 17);
	else
		mpage |= (fn << 19);

	if ((mpage != last_mpage) || (port->index != last_port)) {
		addr = ((u32) port->hose->cfg_addr) +
		       ACPX1_PCIE_MPAGE_LOWER(7);
		out_le32((u32 *) addr, mpage);
		last_mpage = mpage;
		last_port = port->index;
	}

	return (void __iomem *)port->hose->cfg_data;
}

static int
pciex_acp_read_config(struct pci_bus *bus, unsigned int devfn,
			     int offset, int len, u32 *val)
{
	struct pci_controller *hose = (struct pci_controller *) bus->sysdata;
	struct pciex_port *port =
		&acp_pciex_ports[hose->indirect_type];
	void __iomem *addr;
	u32 bus_addr;
	u32 val32;
	u32 mcsr;
	int bo = offset & 0x3;
	int rc = PCIBIOS_SUCCESSFUL;

	BUG_ON(hose != port->hose);

	if (acp_pciex_validate_bdf(port, bus, devfn) != 0)
		return PCIBIOS_DEVICE_NOT_FOUND;

	addr = pciex_acp_get_config_base(port, bus, devfn);

	if (!addr) {
		*val = 0;
		return PCIBIOS_DEVICE_NOT_FOUND;
	}

	/*
	 * Reading from configuration space of non-existing device can
	 * generate transaction errors. For the read duration we suppress
	 * assertion of machine check exceptions to avoid those.
	 */
	mtmsr(mfmsr() & ~(MSR_ME));
	__asm__ __volatile__("msync");

	/*
	 * addressing is different for local config access vs.
	 * access through the mapped config space.
	 */
	if (bus->number == port->hose->first_busno) {
		int wo = offset & 0xfffffffc;
		bus_addr = (u32) addr + wo;

	} else {

		/*
		 * mapped config space only supports 32-bit access
		 *
		 *  AXI address [3:0] is not used at all.
		 *  AXI address[9:4] becomes register number.
		 *  AXI address[13:10] becomes Ext. register number
		 *  AXI address[17:14] becomes 1st DWBE
		 *	for configuration read only.
		 *  AXI address[29:27] is used to select
		 *	one of 8 Mpage registers.
		 */

		bus_addr = (u32) addr + (offset << 2);

		switch (len) {
		case 1:
			bus_addr |=  ((1 << bo)) << 14;
			break;
		case 2:
			bus_addr |=  ((3 << bo)) << 14;
			break;
		default:
			bus_addr |=  (0xf) << 14;
			break;
		}

	}

	/*
	 * do the read
	 */

	val32 = in_le32((u32 *)bus_addr);

	switch (len) {
	case 1:
		*val = (val32 >> (bo * 8)) & 0xff;
		break;
	case 2:
		*val = (val32 >> (bo * 8)) & 0xffff;
		break;
	default:
		*val = val32;
		break;
	}

	__asm__ __volatile__("msync");

	mcsr = mfspr(SPRN_MCSR);
	if (mcsr != 0) {
		mtspr(SPRN_MCSR, 0);
		__asm__ __volatile__("msync");

#ifdef PRINT_CONFIG_ACCESSES
		printk(KERN_INFO
		       "acp_read_config : %3d [%3d..%3d] fn=0x%04x o=0x%04x"
			" l=%d a=0x%08x machine check!! 0x%08x\n",
		       bus->number, hose->first_busno, hose->last_busno,
		       devfn, offset, len, bus_addr, mcsr);
#endif
		pr_debug(
			 "acp_read_config : bus=%3d [%3d..%3d] devfn=0x%04x"
			" offset=0x%04x len=%d, addr=0x%08x"
			" machine check!! 0x%08x\n",
			 bus->number, hose->first_busno, hose->last_busno,
			 devfn, offset, len, bus_addr, mcsr);
		*val = 0;
		rc =  PCIBIOS_DEVICE_NOT_FOUND;
	} else {
#ifdef PRINT_CONFIG_ACCESSES
		printk(KERN_INFO
		       "acp_read_config : %3d [%3d..%3d] fn=0x%04x o=0x%04x"
			" l=%d a=0x%08x v=0x%08x\n",
		       bus->number, hose->first_busno, hose->last_busno,
		       devfn, offset, len, bus_addr, *val);
#endif
		pr_debug(
			 "acp_read_config : bus=%3d [%3d..%3d] devfn=0x%04x"
			" offset=0x%04x len=%d, addr=0x%08x val=0x%08x\n",
			 bus->number, hose->first_busno, hose->last_busno,
			 devfn, offset, len, bus_addr, *val);
	}

	/* re-enable machine checks */
	mtmsr(mfmsr() | (MSR_ME));
	__asm__ __volatile__("msync");

	return rc;
}

static int
pciex_acp_write_config(struct pci_bus *bus,
			      unsigned int devfn,
			      int offset, int len, u32 val)
{
	struct pci_controller *hose = (struct pci_controller *) bus->sysdata;
	struct pciex_port *port =
		&acp_pciex_ports[hose->indirect_type];
	void __iomem *addr;
	u32 bus_addr;
	u32 val32;

	if (acp_pciex_validate_bdf(port, bus, devfn) != 0)
		return PCIBIOS_DEVICE_NOT_FOUND;

	addr = pciex_acp_get_config_base(port, bus, devfn);

	if (!addr)
		return PCIBIOS_DEVICE_NOT_FOUND;

	/*
	 * addressing is different for local config access vs.
	 * access through the mapped config space. We need to
	 * translate the offset for mapped config access
	 */
	if (bus->number == port->hose->first_busno) {
		/* the local ACP RC only supports 32-bit dword config access,
		 * so if this is a byte or 16-bit word access we need to
		 * perform a read-modify write
		 */
		if (len == 4) {
			bus_addr = (u32) addr + offset;
		} else {
			int bs = ((offset & 0x3) * 8);

			bus_addr = (u32) addr + (offset & 0xfffffffc);
			val32 = in_le32((u32 *)bus_addr);

			if (len == 2) {
				val32 = (val32 & ~(0xffff << bs)) |
					((val & 0xffff) << bs);
			} else {
				val32 = (val32 & ~(0xff << bs)) |
					((val & 0xff) << bs);
			}

			val = val32;
			len = 4;
		}

	} else
		bus_addr = (u32) addr + (offset << 2) + (offset & 0x3);

#ifdef PRINT_CONFIG_ACCESSES
	printk(KERN_INFO
	       "acp_write_config: %3d [%3d..%3d] fn=0x%04x o=0x%04x l=%d"
		" a=0x%08x v=0x%08x\n",
	       bus->number, hose->first_busno, hose->last_busno,
	       devfn, offset, len, bus_addr, val);
#endif
	pr_debug(
		 "acp_write_config: bus=%3d [%3d..%3d] devfn=0x%04x"
		" offset=0x%04x len=%d, addr=0x%08x val=0x%08x\n",
		 bus->number, hose->first_busno, hose->last_busno,
		 devfn, offset, len, bus_addr, val);


	switch (len) {
	case 1:
		out_8((u8 *)(bus_addr), val);
		break;
	case 2:
		out_le16((u16 *)(bus_addr), val);
		break;
	default:
		out_le32((u32 *)(bus_addr), val);
		break;
	}

	return PCIBIOS_SUCCESSFUL;
}


static struct pci_ops acp_pciex_pci_ops = {
	.read  = pciex_acp_read_config,
	.write = pciex_acp_write_config,
};

static int __init acp_setup_one_pciex_POM(struct pciex_port	*port,
				     struct pci_controller	*hose,
				     void __iomem		*mbase,
				     u64			plb_addr,
				     u64			pci_addr,
				     u64			size,
				     unsigned int		flags,
				     int			index)
{
	u32 pciah, pcial;
		int i, num_pages;
		u32 mpage_lower;

	/* Allows outbound memory mapping size that is not a power of 2 */
	if ((index < 2 && size < 0x100000) ||
	    (index == 2 && size < 0x100) ||
	    (plb_addr & (size - 1)) != 0) {
		printk(KERN_WARNING "%s: Resource out of range\n",
		       hose->dn->full_name);
		return -1;
	}

	/* Calculate register values */
	pciah = RES_TO_U32_HIGH(pci_addr);
	pcial = RES_TO_U32_LOW(pci_addr);
		/* ACP X1 setup MPAGE registers */

		printk(KERN_INFO
		       "setting outbound window %d with plb_add=0x%012llx,"
			" pci_addr=0x%012llx, size=0x%012llx\n",
		       index, plb_addr, pci_addr, size);

		/*
		 * MPAGE7 is dedicated to config access, so we only
		 *  have 7 128MB pages available for memory i/o.
		 *  Calculate how many pages we need
		 */
		if (size > (7 * 0x08000000)) {
			printk(KERN_WARNING "%s: Resource size 0x%012llx out of range\n",
			hose->dn->full_name, size);
			return -1;
		}

		num_pages = ((size - 1) >> 27) + 1;
		for (i = 0; i < num_pages; i++) {
			mpage_lower = (pcial & 0xf8000000);

			if (5 > port->acpChipType)
				mpage_lower |= 1;

			out_le32(mbase + ACPX1_PCIE_MPAGE_UPPER(i), pciah);
			out_le32(mbase + ACPX1_PCIE_MPAGE_LOWER(i),
				 mpage_lower);
			pcial += 0x08000000;
		}

	return 0;
}

static void __init acp_configure_pciex_POMs(struct pciex_port *port,
					       struct pci_controller *hose,
					       void __iomem *mbase)
{
	int i, j, found_isa_hole = 0;

	/* Setup outbound memory windows */
	for (i = j = 0; i < 3; i++) {
		struct resource *res = &hose->mem_resources[i];

		/* we only care about memory windows */
		if (!(res->flags & IORESOURCE_MEM))
			continue;
		if (j > 1) {
			printk(KERN_WARNING "%s: Too many ranges\n",
			       port->node->full_name);
			break;
		}

		/* Configure the resource */
		if (acp_setup_one_pciex_POM(port, hose, mbase,
					res->start,
				       res->start - hose->pci_mem_offset,
				       resource_size(res),
				       res->flags,
				       j) == 0) {
			j++;

			/* If the resource PCI address is 0 then we have our
			 * ISA memory hole
			 */
			if (res->start == hose->pci_mem_offset)
				found_isa_hole = 1;
		}
	}

	/* Handle ISA memory hole if not already covered */
	if (j <= 1 && !found_isa_hole && hose->isa_mem_size)
		if (acp_setup_one_pciex_POM(port, hose, mbase,
					       hose->isa_mem_phys, 0,
					       hose->isa_mem_size, 0, j) == 0)
			printk(KERN_INFO "%s: Legacy ISA memory support enabled\n",
			       hose->dn->full_name);

	/* Configure IO, always 64K starting at 0. We hard wire it to 64K !
	 * Note also that it -has- to be region index 2 on this HW
	 */
	if (hose->io_resource.flags & IORESOURCE_IO)
		acp_setup_one_pciex_POM(port, hose, mbase,
					   hose->io_base_phys, 0,
					   0x10000, IORESOURCE_IO, 2);
}

static void __init
configure_acp_pciex_PIMs(struct pciex_port *port,
				struct pci_controller *hose,
				void __iomem *mbase,
				struct resource *res)
{
	resource_size_t size = res->end - res->start + 1;
	u64 sa;
	void __iomem *tpage_base = mbase + 0x1050;

	if (port->endpoint) {
		resource_size_t ep_addr = 0;
		resource_size_t ep_size = 32 << 20;

		/* Currently we map a fixed 64MByte window to PLB address
		 * 0 (SDRAM). This should probably be configurable via a dts
		 * property.
		 */

		/* Calculate window size */
		sa = (0xffffffffffffffffull << ilog2(ep_size));

		/* TODO: */

		out_le32(mbase + PCI_BASE_ADDRESS_0, RES_TO_U32_LOW(ep_addr));
		out_le32(mbase + PCI_BASE_ADDRESS_1, RES_TO_U32_HIGH(ep_addr));
	} else {
		/* Calculate window size */
		sa = (0xffffffffffffffffull << ilog2(size));

		if (res->flags & IORESOURCE_PREFETCH)
			sa |= 0x8;

		printk(KERN_INFO
		       "configure inbound mapping from 0x%012llx-0x%012llx "
		       "(0x%08llx bytes)\n", res->start, res->end, size);

		/*
		  HACK!! Since PCI legacy support is disabled
		  in our config, we reusethe isa_mem_size
		  field to save the size of our inbound
		  window.  We use this elsewhere to set up the
		  dma_base.
		*/

		pci_dram_offset = size;
		hose->dma_window_base_cur = size;

		out_le32(mbase + PCI_BASE_ADDRESS_0, RES_TO_U32_LOW(size));
		out_le32(mbase + PCI_BASE_ADDRESS_1, RES_TO_U32_HIGH(size));

		if (5 == port->acpChipType) {
			printk(KERN_WARNING "Setting SIZE for 2500\n");
			out_le32(mbase + 0x11f4, 0xf0000000UL);
		}

		/*
		 * set up the TPAGE registers
		 *
		 * We set the MSB of each TPAGE to select 128-bit AXI access.
		 * For the address field we simply program an incrementing value
		 * to map consecutive pages
		 */
		if (acp_plx == 0) {
			int i;

			for (i = 0; i < 8; i++) {
				out_le32(tpage_base, (0x80000000 + i));
				tpage_base += 4;
			}
		} else {
			out_le32(tpage_base, 0x0);  /* tpg 0  0x0, not used */
			tpage_base += 4;
			out_le32(tpage_base, 0x0);  /* tpg 1  0x0, not used */
			tpage_base += 4;
			out_le32(tpage_base, 0x0);  /* tpg 2  0x0, not used */
			tpage_base += 4;
			out_le32(tpage_base, 0x0);  /* tpg 3  0x0, not used */
			tpage_base += 4;
			out_le32(tpage_base, 0x0);  /* tpg 4  0x0, not used */
			tpage_base += 4;
			out_le32(tpage_base, 0x0);  /* tpg 5  0x0, not used */
			tpage_base += 4;
			out_le32(tpage_base, 0x0);  /* tpg 6  0x0 for dyn map */
			tpage_base += 4;
			/*
			  tpage 7
			  point to 0x20,0000,0000
			  tpage size = 512MB, 32bit AXI bus access
			*/
			out_le32(tpage_base, 0x00000800);
			printk(KERN_INFO
			       "configure inbound mapping tpage 7 to "
			       "0x00000800\n");
		}
	}
}


static irqreturn_t
acp_pcie_isr(int irq, void *arg)
{
	struct pci_controller *hose = (struct pci_controller *) arg;
	void __iomem *mbase = (void __iomem *)hose->cfg_addr;

	u32 intr_status;
	u32 msg_fifo_stat;
	u32 msg_fifo_info;
	u8  externalPciIntr = 0;


	/* read the PEI interrupt status register */
	intr_status = in_le32(mbase + 0x10c0);
	in_le32(mbase + 0x10c4);

	/* check if this is a PCIe message from an external device */
	if (intr_status & 0x00000010) {
		externalPciIntr = 1;

		msg_fifo_stat = in_le32(mbase + 0x10b4);

		/* loop until the message fifo is empty */
		while ((msg_fifo_stat & 0x01) == 0)  {
			u8 bus, dev, fn;
			u8 msg_type;
			msg_fifo_info = in_le32(mbase + 0x10b0);

			bus = (msg_fifo_info >> 16) & 0xff;
			dev = (msg_fifo_info >> 11) & 0x1f;
			fn  = (msg_fifo_info >>  8) & 0x07;
			msg_type = msg_fifo_info & 0xff;

			/* print out the BDF and message type.
			 * We ignore the common message types.
			 */
			switch (msg_type) {
			case 0x20:  /*    assert_INTa */
			case 0x21:  /*    assert_INTb */
			case 0x22:  /*    assert_INTc */
			case 0x23:  /*    assert_INTd */
			case 0x24:  /* de-assert_INTa */
			case 0x25:  /* de-assert_INTb */
			case 0x26:  /* de-assert_INTc */
			case 0x27:  /* de-assert_INTd */
				/* do nothing */
				break;
			default:
				printk(KERN_INFO
				       "BDF %02x:%02x.%x sent msgtype 0x%02x\n",
				       bus, dev, fn, msg_type);
				break;
			}

			/* re-read fifo status */
			msg_fifo_stat = in_le32(mbase + 0x10b4);
		}
	} else {
		/*
		 * Ignore the common interrupts, still need to figure out what
		 * they all mean.
		 */
		if (intr_status & 0xf3ffffab) {
			u32 t2a_err_stat;
			u32 t2a_other_err_stat;
			u32 int_enb;
			u32 linkStatus;
			u32 offset;

			printk(KERN_ERR
			       "ACP_PCIE_ISR: got PEI error interrupt 0x%08x\n",
			       intr_status);

			linkStatus = in_le32(mbase + 0x117c);
			printk(KERN_ERR
			       "link_status (0x117c) = 0x%08x\n",
			       linkStatus);

			if (intr_status & 0x00020000) {
				t2a_err_stat = in_le32(mbase + 0x1170);
				printk(KERN_ERR
				       "t2a_fn_indp_err_stat = 0x%08x\n",
				       t2a_err_stat);
				int_enb = in_le32(mbase + 0x10c4);
				int_enb &= 0xfffdffff;
				out_le32(mbase + 0x10c4, int_enb);
			}

			if (intr_status & 0x00040000) {
				t2a_other_err_stat = in_le32(mbase + 0x1174);
				printk(KERN_ERR
				       "t2a_fn_indp_other_err_stat = 0x%08x\n",
				       t2a_other_err_stat);
				int_enb = in_le32(mbase + 0x10c4);
				int_enb &= 0xfffbffff;
				out_le32(mbase + 0x10c4, int_enb);
			}

			if (intr_status & 0x00000800) {
				printk(KERN_INFO
				       "pci_config = 0x%08x\n",
				       in_le32(mbase + 0x1000));
				printk(KERN_INFO
				       "pci_status = 0x%08x\n",
				       in_le32(mbase + 0x1004));

				int_enb = in_le32(mbase + 0x10c4);
				int_enb &= 0xfffff7ff;
				out_le32(mbase + 0x10c4, int_enb);
			}

			/*
			 * dump all the potentially interesting PEI registers
			 */
			for (offset = 0x114c; offset <= 0x1180; offset += 4) {
				printk(KERN_INFO
				       "  0x%04x : 0x%08x\n",
				       offset, in_le32(mbase + offset));
			}
		}
	}

	/*
	 *  We clear all the interrupts in the PEI status, even though
	 *  interrupts from external devices have not yet been handled.
	 *  That should be okay, since the PCI IRQ in the MPIC won't be
	 *  re-enabled until all external handlers have been called.
	 */
	out_le32(mbase + 0x10c0, intr_status);

	return externalPciIntr ? IRQ_NONE : IRQ_HANDLED;
}

static void __init
acp_pciex_port_setup_hose(struct pciex_port *port)
{
	struct resource dma_window;
	struct pci_controller *hose = NULL;
	const int *bus_range;
	int primary = 0, busses;
	void __iomem *mbase = NULL, *cfg_data = NULL;
	int mappedIrq;
	int err;
	u32 pci_status;
	u32 link_state;
	u32 pci_config;
	u32 version;

	/* Check if primary bridge */
	if (of_get_property(port->node, "primary", NULL))
		primary = 1;

	/* Get bus range if any */
	bus_range = of_get_property(port->node, "bus-range", NULL);

	/* Allocate the host controller data structure */
	hose = pcibios_alloc_controller(port->node);
	if (!hose)
		goto fail;

	/* We stick the port number in "indirect_type" so the config space
	 * ops can retrieve the port data structure easily
	 */
	hose->indirect_type = port->index;

	/* Get bus range */
	hose->first_busno = bus_range ? bus_range[0] : 0x0;
	hose->last_busno = bus_range ? bus_range[1] : 0xff;

	/* Because of how big mapping the config space is (1M per bus), we
	 * limit how many busses we support. In the long run, we could replace
	 * that with something akin to kmap_atomic instead. We set aside 1 bus
	 * for the host itself too.
	 */
	busses = hose->last_busno - hose->first_busno; /* This is off by 1 */
	if (busses > MAX_PCIE_BUS_MAPPED) {
		busses = MAX_PCIE_BUS_MAPPED;
		hose->last_busno = hose->first_busno + busses;
	}

	if (!port->endpoint) {
		/*
		 * map the bottom page of PCIe memory for config space access
		*/
		cfg_data = ioremap(port->cfg_space.start, 0x100000);
		if (cfg_data == NULL) {
			printk(KERN_ERR "%s: Can't map external config space !",
			       port->node->full_name);
			goto fail;
		}
		hose->cfg_data = cfg_data;
	}

	/*
	 * The internal config space has already been mapped, so
	 * just re-use that virtual address.
	 */
	hose->cfg_addr = port->utl_base;

	pr_debug("PCIE %s, bus %d..%d\n", port->node->full_name,
		 hose->first_busno, hose->last_busno);
	pr_debug("     config space mapped at: root @0x%p, other @0x%p\n",
		 hose->cfg_addr, hose->cfg_data);

	/* Setup config space */
	hose->ops = &acp_pciex_pci_ops;
	port->hose = hose;
	mbase = (void __iomem *)hose->cfg_addr;

	if (port->endpoint) {
		/* if we're an endpoint don't do anything else */
		printk(KERN_INFO
		       "PCIE%d: successfully set as endpoint\n",
		       port->index);
		return;
	}

	/* setting up as root complex */
	pci_config = in_le32(mbase + 0x1000);

	pci_status = in_le32(mbase + 0x1004);
	link_state = (pci_status & 0x3f00) >> 8;
	printk("PCIE%d status = 0x%08x : PCI link state = 0x%x\n",
	port->index, pci_status, link_state);

	/* make sure the ACP device is configured as PCI Root Complex */
	if ((pci_status & 0x18) != 0x18) {
		printk(KERN_ERR
		       "ACP device is not PCI Root Complex! status = 0x%08x\n",
		       pci_status);
		goto fail;
	}

	/* make sure the link is up */
	if (link_state != 0xb) {
		/* reset */
		printk(KERN_WARNING "PCI link in bad state - resetting\n");
		pci_config |= 1;
		out_le32(mbase + 0x1000, pci_config);
		msleep(1000);

		pci_status = in_le32(mbase + 0x1004);
		link_state = (pci_status & 0x3f00) >> 8;

		printk(KERN_INFO "PCI link state now = 0x%x\n", link_state);

		if (link_state != 0xb) {
			printk(KERN_ERR
			       "PCI link still in bad state - giving up!\n");
			goto fail;
		}
	}

	/* get the device version */
	if (0 != ncr_read(NCP_REGION_ID(0x16, 0xff), 0x0, 4, &version)) {
		printk(KERN_ERR "Unable to detect ACP revision!\n");
		goto fail;
	}

	port->acpChipType = (version & 0xff);
	printk(KERN_INFO "Using PEI register set for ACP chipType %d\n",
		port->acpChipType);


	/*
	 * Set bus numbers on our root port
	*/
	out_8(mbase + PCI_PRIMARY_BUS, hose->first_busno);
	out_8(mbase + PCI_SECONDARY_BUS, hose->first_busno + 1);
	out_8(mbase + PCI_SUBORDINATE_BUS, hose->last_busno);


	/* Parse outbound mapping resources */
	pci_process_bridge_OF_ranges(hose, port->node, primary);

	/* Parse inbound mapping resources */
	if (acp_parse_dma_ranges(hose, mbase, &dma_window) != 0)
		goto fail;

	/* Configure outbound ranges POMs */
	acp_configure_pciex_POMs(port, hose, mbase);

	/* Configure inbound ranges PIMs */
	configure_acp_pciex_PIMs(port, hose, mbase, &dma_window);

	/*
	 * hook up an interrupt handler
	*/
	printk(KERN_INFO "PCIE%d mapping interrupt\n", port->index);
	mappedIrq = irq_of_parse_and_map(port->node, 0);

	err = request_irq(mappedIrq, acp_pcie_isr,
			  IRQF_SHARED, "acp_pcie", hose);
	if (err) {
		printk(KERN_ERR "request_irq failed!!!!\n");
		goto fail;
	}

	/* unmask PEI interrupts */
	/* for now ignore retry requests, and INT assert/deassert */
	out_le32(mbase + 0x10c4, 0xf3fffffd);

	if (port->acpChipType == 1) {
		/*
		 * for v2 we need to set the 'axi_interface_rdy' bit
		 * this bit didn't exist in X1V1, and means something
		 * else for X2...
		*/
		pci_config = in_le32(mbase + 0x1000);
		pci_config |= 0x00040000;
		out_le32(mbase + 0x1000, pci_config);
	}

	if (!port->endpoint) {
		printk(KERN_INFO "PCIE%d: successfully set as root-complex\n",
		       port->index);
	} else {
	}

	return;
fail:
	if (hose)
		pcibios_free_controller(hose);
	if (cfg_data)
		iounmap(cfg_data);
}

static void __init probe_acp_pciex_bridge(struct device_node *np)
{
	struct pciex_port *port;
	const u32 *pval;
	int portno;
	const char *val;
	const u32 *field;

	/* First, proceed to core initialization as we assume there's
	 * only one PCIe core in the system
	 */
	if (acp_pciex_check_core_init(np))
		return;

	/* Get the port number from the device-tree */
	pval = of_get_property(np, "port", NULL);
	if (pval == NULL) {
		printk(KERN_ERR "PCIE: Can't find port number for %s\n",
		       np->full_name);
		return;
	}
	portno = *pval;
	if (portno >= acp_pciex_port_count) {
		printk(KERN_ERR "PCIE: port number out of range for %s\n",
		       np->full_name);
		return;
	}
	port = &acp_pciex_ports[portno];
	port->index = portno;

	/*
	 * Check if device is enabled
	 */
	if (!of_device_is_available(np)) {
		printk(KERN_INFO "PCIE%d: Port disabled via device-tree\n",
		       port->index);
		return;
	}

	/* Make sure PCIe is enabled in the device tree. */
	field = of_get_property(np, "enabled", NULL);

	if (!field || (field && (0 == *field))) {
		printk(KERN_INFO "%s: Port disabled via device-tree\n",
		       np->full_name);
		return;
	}

	/* Check for the PLX work-around. */
	field = of_get_property(np, "plx", NULL);

	if (field && (0 != *field))
		acp_plx = 1;
	else
		acp_plx = 0;

	port->node = of_node_get(np);

	/* Check if device_type property is set to "pci" or "pci-endpoint".
	 * Resulting from this setup this PCIe port will be configured
	 * as root-complex or as endpoint.
	 */
	val = of_get_property(port->node, "device_type", NULL);
	if (!strcmp(val, "pci-endpoint")) {
		port->endpoint = 1;
	} else if (!strcmp(val, "pci")) {
		port->endpoint = 0;
	} else {
		printk(KERN_ERR "PCIE%d: missing or incorrect device_type for %s\n",
		       portno, np->full_name);
		return;
	}

	/* Fetch config space registers address */
	if (of_address_to_resource(np, 0, &port->cfg_space)) {
		printk(KERN_ERR "%s: Can't get PCI-E config space !",
		       np->full_name);
		return;
	}

	/* Fetch host bridge internal registers address */
	if (of_address_to_resource(np, 1, &port->utl_regs)) {
		printk(KERN_ERR "%s: Can't get UTL register base !",
		       np->full_name);
		return;
	}

	port->utl_base = ioremap(port->utl_regs.start,
		resource_size(&port->utl_regs));

	printk(KERN_INFO
	       "%s PCIE%d config base = 0x%012llx (0x%08x virtual)\n",
	       np->full_name, port->index, port->utl_regs.start,
	       (u32) port->utl_base);

	/* Setup the linux hose data structure */
	acp_pciex_port_setup_hose(port);
}

static int __init pci_acp_find_bridges(void)
{
	struct device_node *np;

	pci_add_flags(PCI_ENABLE_PROC_DOMAINS | PCI_COMPAT_DOMAIN_0);

		for_each_compatible_node(np, NULL, "lsi,plb-pciex")
			probe_acp_pciex_bridge(np);

	return 0;
}
arch_initcall(pci_acp_find_bridges);
