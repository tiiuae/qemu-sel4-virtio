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
/* Uncomment to use PCI bus from seL4 instead of qemu */
// #  define CONFIG_SEL4_PCI
# endif
#else
# define CONFIG_SEL4_IS_POSSIBLE
#endif

#ifdef CONFIG_SEL4_IS_POSSIBLE

extern bool sel4_allowed;
extern bool sel4_irqfds_allowed;
extern bool sel4_resamplefds_allowed;
extern bool sel4_msi_via_irqfd_allowed;

#define sel4_enabled()                  (sel4_allowed)
#define sel4_irqfds_enabled()           (sel4_irqfds_allowed)
#define sel4_resamplefds_enabled()      (sel4_resamplefds_allowed)
#define sel4_msi_via_irqfd_enabled()    (sel4_msi_via_irqfd_allowed)

void sel4_set_irq(unsigned int irq, bool);

#else /* !CONFIG_SEL4_IS_POSSIBLE */

#define sel4_enabled() 0
#define sel4_irqfds_enabled()           (false)
#define sel4_resamplefds_enabled()      (false)
#define sel4_msi_via_irqfd_enabled()    (false)

#endif /* CONFIG_SEL4_IS_POSSIBLE */

int sel4_add_msi_route(int vector, PCIDevice *dev);
int sel4_add_irqfd_notifier(EventNotifier *n, EventNotifier *rn, int virq);
int sel4_remove_irqfd_notifier(EventNotifier *n, int virq);
#endif
