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
extern bool sel4_ext_vpci_bus;

#define sel4_enabled()              (sel4_allowed)
#define sel4_ext_vpci_bus_enabled() (sel4_ext_vpci_bus)

#else /* !CONFIG_SEL4_IS_POSSIBLE */

#define sel4_enabled()              (false)
#define sel4_ext_vpci_bus_enabled() (false)

#endif /* CONFIG_SEL4_IS_POSSIBLE */

void sel4_set_irq(unsigned int irq, bool);

#endif
