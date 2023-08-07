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

    uint32_t num_irq;
};
typedef struct SeL4IntcState SeL4IntcState;

static void sel4_intc_set_irq(void *opaque, int irq, int level)
{
    SeL4IntcState *s = opaque;

    /* Following GIC irq encoding:
     *  [0..N-1] : external interrupts
     *  [N..N+31] : PPI (internal) interrupts for CPU 0
     *  ...
     */
    if (irq < (s->num_irq - GIC_INTERNAL)) {
        sel4_set_irq(irq + GIC_INTERNAL, !!level);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "sel4_intc: not supporting PPIs (%d)\n", irq);
    }
}

static void sel4_intc_realize(DeviceState *dev, Error **errp)
{
    SeL4IntcState *s = SEL4_INTC(dev);

    if (s->num_irq > GIC_MAXIRQ) {
        error_setg(errp,
                   "requested %u interrupt lines exceeds GIC maximum %d",
                   s->num_irq, GIC_MAXIRQ);
        return;
    }
    if (s->num_irq < GIC_INTERNAL) {
        error_setg(errp,
                   "requested %u interrupt less than required %d",
                   s->num_irq, GIC_INTERNAL);
        return;
    }

    // TODO: take num_cpus into account
    qdev_init_gpio_in(dev, sel4_intc_set_irq, s->num_irq);
}

static Property sel4_intc_props[] = {
    DEFINE_PROP_UINT32("num-irq", SeL4IntcState, num_irq, 192),
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
