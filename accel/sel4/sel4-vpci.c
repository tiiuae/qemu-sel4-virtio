/*
 * Dummy PCI Host for seL4 vPCI bus
 *
 * A container PCI host for devices attached to a vPCI bus managed by seL4
 * VMM. The code is based on gpex.
 *
 * Copyright 2023, Technology Innovation Institute
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "qemu/osdep.h"
#include "exec/memory.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "hw/sysbus.h"
#include "hw/pci/pcie_host.h"
#include "hw/pci/pci.h"
#include "hw/irq.h"
#include "sysemu/sel4.h"

#define TYPE_SEL4_PCI_HOST "sel4-pci-host"
OBJECT_DECLARE_SIMPLE_TYPE(SeL4PCIHost, SEL4_PCI_HOST)

typedef struct SeL4PCIHost
{
    PCIExpressHost parent_obj;

    MemoryRegion mmio;
    MemoryRegion ioport;
    /* Default behaviour unmapped accesses */
    MemoryRegion mmio_window;
    MemoryRegion ioport_window;

    qemu_irq irq[SEL4_VPCI_INTERRUPTS];
    int irq_num[SEL4_VPCI_INTERRUPTS];
} SeL4PCIHost;

static void sel4_pcihost_set_irq(void *opaque, int irq, int level)
{
    SeL4PCIHost *s = opaque;

    assert(irq >= 0);
    assert(irq < SEL4_VPCI_INTERRUPTS);

    qemu_set_irq(s->irq[irq], level);
}

static int sel4_pcihost_map_irq_fn(PCIDevice *pci_dev, int pin)
{
    return PCI_SLOT(pci_dev->devfn);
}

int sel4_pcihost_set_irq_num(DeviceState *dev, int index, int gsi)
{
    SeL4PCIHost *s = SEL4_PCI_HOST(dev);

    if (index >= SEL4_VPCI_INTERRUPTS) {
        return -EINVAL;
    }

    s->irq_num[index] = gsi;

    return 0;
}

static PCIINTxRoute sel4_pcihost_route_intx_pin_to_irq(void *opaque, int pin)
{
    PCIINTxRoute route;
    SeL4PCIHost *s = opaque;
    int gsi = s->irq_num[pin];

    route.irq = gsi;
    if (gsi < 0) {
        route.mode = PCI_INTX_DISABLED;
    } else {
        route.mode = PCI_INTX_ENABLED;
    }

    return route;
}

static void sel4_pcihost_realize(DeviceState *dev, Error **errp)
{
    SeL4PCIHost *s = SEL4_PCI_HOST(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    PCIHostState *pci = PCI_HOST_BRIDGE(dev);

    memory_region_init(&s->mmio, OBJECT(s), "sel4_pci_mmio", UINT64_MAX);
    memory_region_init(&s->ioport, OBJECT(s), "sel4_pci_ioport", 64 * 1024);

    /* Create windows for default behaviour */
    memory_region_init_io(&s->mmio_window, OBJECT(s),
                          &unassigned_io_ops, OBJECT(s),
                          "sel4_pci_mmio_window", UINT64_MAX);
    memory_region_init_io(&s->ioport_window, OBJECT(s),
                          &unassigned_io_ops, OBJECT(s),
                          "sel4_pci_ioport_window", 64 * 1024);

    memory_region_add_subregion(&s->mmio_window, 0, &s->mmio);
    memory_region_add_subregion(&s->ioport_window, 0, &s->ioport);
    sysbus_init_mmio(sbd, &s->mmio_window);
    sysbus_init_mmio(sbd, &s->ioport_window);

    for (int i = 0; i < SEL4_VPCI_INTERRUPTS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
        s->irq_num[i] = -1;
    }

    pci->bus = pci_register_root_bus(dev, "sel4-pcie.0", sel4_pcihost_set_irq,
                                     sel4_pcihost_map_irq_fn,
                                     s, &s->mmio, &s->ioport, 0,
                                     SEL4_VPCI_INTERRUPTS, TYPE_PCIE_BUS);
    pci_bus_set_route_irq_fn(pci->bus, sel4_pcihost_route_intx_pin_to_irq);

    qbus_set_hotplug_handler(BUS(pci->bus), OBJECT(s));
}

static const char *sel4_pcihost_root_bus_path(PCIHostState *host_bridge,
                                              PCIBus *rootbus)
{
    return "0000:00";
}

static void sel4_pcihost_device_plug(HotplugHandler *hotplug_dev,
                                     DeviceState *dev, Error **errp)
{
    sel4_register_pci_device(PCI_DEVICE(dev));
}

static void sel4_pcihost_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);
    HotplugHandlerClass *hhc = HOTPLUG_HANDLER_CLASS(klass);

    hc->root_bus_path = sel4_pcihost_root_bus_path;

    dc->realize = sel4_pcihost_realize;
    dc->user_creatable = false;

    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->fw_name = "pci";

    hhc->plug = sel4_pcihost_device_plug;
}

static const TypeInfo sel4_pci_info = {
    .name = TYPE_SEL4_PCI_HOST,
    .parent = TYPE_PCIE_HOST_BRIDGE,
    .instance_size = sizeof(SeL4PCIHost),
    .class_init = sel4_pcihost_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_HOTPLUG_HANDLER },
        { },
    }
};

static void sel4_pci_register_types(void)
{
    type_register_static(&sel4_pci_info);
}

type_init(sel4_pci_register_types)
