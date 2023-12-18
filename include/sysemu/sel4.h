/*
 * QEMU seL4 support
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef SYSEMU_SEL4_H
#define SYSEMU_SEL4_H

#ifdef NEED_CPU_H
# ifdef CONFIG_SEL4
#  define CONFIG_SEL4_IS_POSSIBLE
# endif
#else
# define CONFIG_SEL4_IS_POSSIBLE
#endif

#define SEL4_VPCI_INTERRUPTS (32)

#ifdef CONFIG_SEL4_IS_POSSIBLE

extern bool sel4_allowed;
extern bool sel4_ext_vpci_bus_allowed;
extern bool sel4_ext_msi_allowed;
extern bool sel4_irqfds_allowed;
extern bool sel4_msi_via_irqfd_allowed;

#define sel4_enabled()                  (sel4_allowed)
#define sel4_ext_vpci_bus_enabled()     (sel4_ext_vpci_bus_allowed)
#define sel4_ext_msi_enabled()          (sel4_ext_msi_allowed)
#define sel4_irqfds_enabled()           (sel4_irqfds_allowed)
#define sel4_msi_via_irqfd_enabled()    (sel4_msi_via_irqfd_allowed)

#else /* !CONFIG_SEL4_IS_POSSIBLE */

#define sel4_enabled()                  (false)
#define sel4_ext_vpci_bus_enabled()     (false)
#define sel4_ext_msi_enabled()          (false)
#define sel4_irqfds_enabled()           (false)
#define sel4_msi_via_irqfd_enabled()    (false)

#endif /* CONFIG_SEL4_IS_POSSIBLE */

typedef enum {
    SEL4_REGION_RAM,
    SEL4_REGION_PCIE_MMIO,
    SEL4_REGION_PCIE_PIO,
} SeL4MemoryRegion;

MemMapEntry sel4_region_get(SeL4MemoryRegion region);
int sel4_mmio_region_add(MemoryRegionSection *section);
int sel4_mmio_region_del(MemoryRegionSection *section);
void sel4_register_pci_device(PCIDevice *d);
void sel4_set_irq(unsigned int irq, bool);

int sel4_pcihost_set_irq_num(DeviceState *dev, int index, int gsi);

#endif
