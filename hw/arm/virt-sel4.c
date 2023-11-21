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

void sel4_virt_memmap_init(VirtMachineState *vms)
{
    if (!vms->memmap) {
        fprintf(stderr, "No memmap set for machine\n");
        exit(1);
    }

    vms->memmap[VIRT_MEM] = sel4_region_get(SEL4_REGION_RAM);
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

