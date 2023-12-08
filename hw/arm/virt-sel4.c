/*
 * Copyright 2023, Technology Innovation Institute
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "hw/sysbus.h"
#include "sysemu/sel4.h"
#include "hw/arm/virt-sel4.h"
#include "hw/pci/pcie_host.h"
#include "hw/pci/pci.h"
#include "net/net.h"

extern void tii_printf(const char *fmt, ...);

const char *sel4_virt_mmio_regions[] = {
    "gicv2m",
    "pcie-mmcfg-mmio",
    "gpex_mmio_window",
    "gpex_ioport_window"
};

static inline bool _sel4_virt_allowed_region(char const * const region_name)
{
    for (unsigned i = 0; i < ARRAY_SIZE(sel4_virt_mmio_regions); i++) {
        if (!strcmp(region_name, sel4_virt_mmio_regions[i]))
            return true;
    }
    return false;
}

static inline bool sel4_virt_allowed_region(MemoryRegion *mr)
{
    return _sel4_virt_allowed_region(memory_region_name(mr));
}

static void sel4_virt_region_add(MemoryListener *listener, MemoryRegionSection *section)
{
    tii_printf("%s entered, region name %s, offset within address space 0x%lx, size 0x%lx\n",
               __func__, memory_region_name(section->mr),
               (uint64_t) section->offset_within_address_space,
               (uint64_t) section->size);

    if (sel4_virt_allowed_region(section->mr)) {
        sel4_mmio_region_add(section);
    }
}

static MemoryListener sel4_mmio_mem_listener = {
    .name = "sel4-virt-arm",
    .region_add = sel4_virt_region_add,
};

void sel4_virt_memmap_init(VirtMachineState *vms)
{
    if (!vms->memmap) {
        fprintf(stderr, "No memmap set for machine\n");
        exit(1);
    }

    vms->memmap[VIRT_MEM] = sel4_region_get(SEL4_REGION_RAM);

    memory_listener_register(&sel4_mmio_mem_listener, &address_space_memory);
}

void sel4_virt_create_pcie(VirtMachineState *vms, unsigned int irq_base)
{
    DeviceState *dev;
    MemoryRegion *mmio_alias;
    MemoryRegion *mmio_reg;
    PCIHostState *pci;
    int i;
    MemMapEntry mmio = sel4_region_get(SEL4_REGION_PCIE_MMIO);
    MemMapEntry pio = sel4_region_get(SEL4_REGION_PCIE_PIO);

    dev = qdev_new("sel4-pci-host");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    /* Map mmio region */
    mmio_alias = g_new0(MemoryRegion, 1);
    mmio_reg = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0);
    memory_region_init_alias(mmio_alias, OBJECT(dev), "sel4-pcie-mmio",
                             mmio_reg, mmio.base, mmio.size);
    memory_region_add_subregion_overlap(get_system_memory(), mmio.base, mmio_alias, 1);

    /* Map IO port space */
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, pio.base);

    for (i = 0; i < SEL4_VPCI_INTERRUPTS; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i,
                           qdev_get_gpio_in(vms->gic, irq_base + i));
        sel4_pcihost_set_irq_num(dev, i, irq_base + i);
    }

    pci = PCI_HOST_BRIDGE(dev);

    pci->bypass_iommu = vms->default_bus_bypass_iommu;

    vms->bus = pci->bus;
    if (vms->bus) {
        for (i = 0; i < nb_nics; i++) {
            NICInfo *nd = &nd_table[i];

            if (!nd->model) {
                nd->model = g_strdup("virtio");
            }

            pci_nic_init_nofail(nd, pci->bus, nd->model, NULL);
        }
    }
}

