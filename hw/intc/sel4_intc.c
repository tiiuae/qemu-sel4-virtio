/*
 * Copyright 2023, Technology Innovation Institute
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "hw/sysbus.h"
#include "qemu/log.h"
#include "hw/qdev-properties.h"
#include "gic_internal.h"
#include "sysemu/sel4.h"

#define TYPE_SEL4_INTC "sel4-intc"
OBJECT_DECLARE_SIMPLE_TYPE(SeL4IntcState, SEL4_INTC)

struct SeL4IntcState {
    SysBusDevice parent_obj;

    /* Number of external interrupts */
    uint32_t num_irqs;
};
typedef struct SeL4IntcState SeL4IntcState;

static void sel4_intc_set_irq(void *opaque, int irq, int level)
{
    SeL4IntcState *s = opaque;

    /* Using following irq encoding for easier integration to arm-virt. The
     * encoding is similar to arm-gic-kvm:
     *  [0..N-1]: SPIs on emulated devices
     *  [N..N+31]: Devices attached to seL4 vPCI
     *
     * On GIC 0-31 is reserved for SGIs and PPIs, but we use that region for
     * seL4 vPCI interrupts. External interrupts (=non-seL4 vPCI
     * devices) are mapped 1:1 to SPIs.
     */
    if (irq < s->num_irqs) {
        sel4_set_irq(irq + SEL4_VPCI_INTERRUPTS, !!level);
    } else {
        int vpci_irq = irq - s->num_irqs;
        if (vpci_irq < 0 || vpci_irq >= SEL4_VPCI_INTERRUPTS) {
            qemu_log_mask(LOG_GUEST_ERROR, "sel4_intc: failed to translate"
                          "interrupt (%d) to vpci interrupt range (%d)\n",
                          irq, vpci_irq);
            return;
        }
        sel4_set_irq(vpci_irq, !!level);
    }
}

static void sel4_intc_realize(DeviceState *dev, Error **errp)
{
    SeL4IntcState *s = SEL4_INTC(dev);

    if (s->num_irqs > GIC_MAXIRQ) {
        error_setg(errp,
                   "requested %u interrupt lines exceeds GIC maximum %d",
                   s->num_irqs, GIC_MAXIRQ);
        return;
    }

    qdev_init_gpio_in(dev, sel4_intc_set_irq, SEL4_VPCI_INTERRUPTS + s->num_irqs);
}

static Property sel4_intc_props[] = {
    DEFINE_PROP_UINT32("num-irqs", SeL4IntcState, num_irqs, 192),
    DEFINE_PROP_END_OF_LIST(),
};

static void sel4_intc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, sel4_intc_props);

    dc->realize = sel4_intc_realize;
}

static const TypeInfo sel4_intc_info = {
    .name = TYPE_SEL4_INTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SeL4IntcState),
    .class_init = sel4_intc_class_init,
};

static void sel4_intc_register_types(void)
{
    type_register_static(&sel4_intc_info);
}

type_init(sel4_intc_register_types)
