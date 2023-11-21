/*
 * Copyright 2023, Technology Innovation Institute
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QEMU_ARM_SEL4_VIRT_H
#define QEMU_ARM_SEL4_VIRT_H

#include "hw/arm/virt.h"

void sel4_virt_memmap_init(VirtMachineState *vms);
void sel4_virt_create_pcie(VirtMachineState *vms, unsigned int irq_base);

#endif
