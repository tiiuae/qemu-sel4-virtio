/*
 * Copyright 2022, Technology Innovation Institute
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qemu/accel.h"
#include "qemu/atomic.h"
#include "sysemu/cpus.h"
#include "sysemu/runstate.h"
#include "hw/boards.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "migration/vmstate.h"

#include <stdarg.h>
#include <sys/ioctl.h>
#include <sel4/sel4_virt.h>

void tii_printf(const char *fmt, ...);

static MemoryRegion ram_mr;
bool sel4_allowed;

void sel4_register_pci_device(PCIDevice *d);

static QemuThread sel4_virtio_thread;

static void *do_sel4_virtio(void *opaque);

/* FIXME: we might need to create interrupt controller for seL4 for
 * qemu_set_irq to call. */
void sel4_set_irq(unsigned int irq, bool);

typedef struct SeL4State
{
    AccelState parent_obj;

    int fd;
    int vmfd;
    int ioreqfd;
    struct sel4_iohandler_buffer *ioreq_buffer;
} SeL4State;

#define TYPE_SEL4_ACCEL ACCEL_CLASS_NAME("sel4")

DECLARE_INSTANCE_CHECKER(SeL4State, SEL4_STATE, TYPE_SEL4_ACCEL)

static int sel4_ioctl(SeL4State *s, int type, ...)
{
    int ret;
    void *arg;
    va_list ap;

    va_start(ap, type);
    arg = va_arg(ap, void *);
    va_end(ap);

    ret = ioctl(s->fd, type, arg);
    if (ret == -1) {
        ret = -errno;
    }
    return ret;
}

static int sel4_vm_ioctl(SeL4State *s, int type, ...)
{
    int ret;
    void *arg;
    va_list ap;

    va_start(ap, type);
    arg = va_arg(ap, void *);
    va_end(ap);

    ret = ioctl(s->vmfd, type, arg);
    if (ret == -1) {
        ret = -errno;
    }
    return ret;
}

void seL4_Yield(void){}

static void sel4_setup_post(MachineState *ms, AccelState *accel)
{
    SeL4State *s = SEL4_STATE(ms->accelerator);

    qemu_thread_create(&sel4_virtio_thread, "seL4 virtio",
        do_sel4_virtio, s, QEMU_THREAD_JOINABLE);
}

static int is_raw_address(uint32_t address)
{
    if (address >= (1ULL << 20))
        return 1;

    return 0;
}

void qmp_ringbuf_write(const char *, const char *, bool, int, Error **);
char *qmp_ringbuf_read(const char *device, int64_t size, bool has_format, int format, Error **errp);

void tii_printf(const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(tmp, fmt, ap);
    va_end(ap);
    Error *err;
    qmp_ringbuf_write("debug", tmp, false, 0, &err);
}

static unsigned int pci_dev_count;
static PCIDevice *pci_devs[16];
static uintptr_t pci_base[16];
static unsigned int pci_base_count;

static int pci_resolve_irq(PCIDevice *pci_dev, int irq_num)
{
    PCIBus *bus;
    for (;;) {
        bus = pci_get_bus(pci_dev);
        irq_num = bus->map_irq(pci_dev, irq_num);
        if (bus->set_irq)
            break;
        pci_dev = bus->parent_dev;
    }
    return irq_num;
}

void sel4_register_pci_device(PCIDevice *d)
{
    SeL4State *s = SEL4_STATE(current_accel());
    struct sel4_vpci_device vpcidev = {
            .pcidev = pci_dev_count,
    };

    pci_devs[pci_dev_count] = d;
    printf("Registering PCI device to VMM\n");

    if (sel4_vm_ioctl(s, SEL4_CREATE_VPCI_DEVICE, &vpcidev)) {
        fprintf(stderr, "Failed to register PCI device: %m\n");
    }

    pci_dev_count++;

    // INTX = 1 -> IRQ 0
    printf("IRQ for this device is %d\n", pci_resolve_irq(d, 0));
}

typedef struct {
    Int128 size;
    hwaddr offset_within_address_space;
    EventNotifier *n;
} sel4_listener_region_t;

#define LR_MAX 512

static sel4_listener_region_t lrs[LR_MAX];
static unsigned int nlrs;

static int lr_match(sel4_listener_region_t *lr, hwaddr addr, Int128 size)
{
    if (addr >= lr->offset_within_address_space + int128_get64(lr->size)) {
        return 0;
    }
    if (addr + int128_get64(size) < lr->offset_within_address_space) {
        return 0;
    }
    return 1;
}

static sel4_listener_region_t *lr_find_match(hwaddr addr, Int128 size)
{
    int i;

    for (i = 0; i < nlrs; i++) {
        if (lr_match(&lrs[i], addr, size)) {
            return &lrs[i];
        }
    }

    return NULL;
}

void sel4_set_irq(unsigned int irq, bool state)
{
    SeL4State *s = SEL4_STATE(current_accel());
    struct sel4_irqline irqline = {
        .irq = irq,
        .op = state ? SEL4_IRQ_OP_SET : SEL4_IRQ_OP_CLR,
    };

    if (sel4_vm_ioctl(s, SEL4_SET_IRQLINE, &irqline))
        fprintf(stderr, "Failed to set irq: %m\n");
}

static void sel4_change_state_handler(void *opaque, bool running, RunState state)
{
    SeL4State *s = opaque;
    if (running) {
        printf("Starting user VM\n");
        if (sel4_vm_ioctl(s, SEL4_START_VM, 0))
            fprintf(stderr, "Failed to start user VM: %m\n");
    }
}

static void sel4_mmio_do_io(struct sel4_ioreq_mmio *mmio)
{
    sel4_listener_region_t *lr = NULL;

    qemu_mutex_lock_iothread();
    switch (mmio->direction) {
    case SEL4_IO_DIR_WRITE:
        address_space_write(&address_space_memory, mmio->addr, MEMTXATTRS_UNSPECIFIED, &mmio->data, mmio->len);
        lr = lr_find_match(mmio->addr, mmio->len);
        break;
    case SEL4_IO_DIR_READ:
        address_space_read(&address_space_memory, mmio->addr, MEMTXATTRS_UNSPECIFIED, &mmio->data, mmio->len);
        break;
    default:
        error_report("sel4: invalid mmio direction (%d)", mmio->direction);
        break;
    }
    qemu_mutex_unlock_iothread();

    if (lr) {
        event_notifier_set(lr->n);
    }
}

static void sel4_pci_do_io(struct sel4_ioreq_pci *pci)
{
    PCIDevice *dev = pci_devs[pci->pcidev];
    uint32_t val;
    sel4_listener_region_t *lr = NULL;

    qemu_mutex_lock_iothread();
    switch (pci->direction) {
    case SEL4_IO_DIR_WRITE:
        val = 0;
        memcpy(&val, &pci->data, pci->len);
        dev->config_write(dev, pci->addr, val, pci->len);
        lr = lr_find_match(pci->addr + pci_base[pci->pcidev], pci->len);
        break;
    case SEL4_IO_DIR_READ:
        val = dev->config_read(dev, pci->addr, pci->len);
        memcpy(&pci->data, &val, pci->len);
        break;
    default:
        error_report("sel4: invalid pci direction (%d)", pci->direction);
        break;
    }
    qemu_mutex_unlock_iothread();

    if (lr) {
        event_notifier_set(lr->n);
    }
}

static inline handle_ioreq(SeL4State *s)
{
    struct sel4_ioreq *ioreq;
    int slot;

    for (slot = 0; slot < SEL4_MAX_IOREQS; slot++) {
        ioreq = &s->ioreq_buffer->request_slots[slot];
        if (qatomic_load_acquire(&ioreq->state) == SEL4_IOREQ_STATE_PROCESSING) {
            switch (ioreq->type) {
            case SEL4_IOREQ_TYPE_MMIO:
                sel4_mmio_do_io(&ioreq->req.mmio);
                break;
            case SEL4_IOREQ_TYPE_PCI:
                sel4_pci_do_io(&ioreq->req.pci);
                break;
            default:
                fprintf(stderr, "sel4: unknown ioreq type (%"PRIu32")", ioreq->type);
                break;
            }

            sel4_vm_ioctl(s, SEL4_NOTIFY_IO_HANDLED, slot);
        }
    }
}

static void *do_sel4_virtio(void *opaque)
{
    SeL4State *s = opaque;
    int rc;

    for (;;) {
        rc = sel4_vm_ioctl(s, SEL4_WAIT_IO, 0);
        if (rc)
            continue;

        handle_ioreq(s);
    }

    return NULL;
}

static bool ioeventfd_exists(MemoryRegionSection *section, EventNotifier *e)
{
    for (int i = 0; i < nlrs; i++) {
        if (lrs[i].offset_within_address_space == section->offset_within_address_space &&
            lrs[i].size == section->size &&
            lrs[i].n == e) {
            return true;
        }
    }
    return false;
}

static void sel4_io_ioeventfd_add(MemoryListener *listener,
                                  MemoryRegionSection *section,
                                  bool match_data, uint64_t data,
                                  EventNotifier *e)
{
    if (ioeventfd_exists(section, e)) {
        return;
    }

    sel4_listener_region_t *lr = &lrs[nlrs++];

    tii_printf("%s: mr=%p offset=0x%"PRIx64" size=0x%"PRIx64" notifier=%p\n", __func__, section->mr,
               section->offset_within_address_space, int128_get64(section->size), e);

    lr->offset_within_address_space = section->offset_within_address_space;
    lr->size = section->size;
    lr->n = e;
}

static void sel4_io_ioeventfd_del(MemoryListener *listener,
                                 MemoryRegionSection *section,
                                 bool match_data, uint64_t data,
                                 EventNotifier *e)

{
    tii_printf("WARNING: %s UNIMPLEMENTED\n", __func__);
}

static void sel4_region_add(MemoryListener *listener, MemoryRegionSection *section)
{
    tii_printf("%s entered, region name %s, offset within region 0x%lx, size 0x%lx\n", __func__,
               memory_region_name(section->mr), (uint64_t) section->offset_within_address_space,
               (uint64_t) section->size);

    if (!strcmp(memory_region_name(section->mr), "virtio-pci")) {
        pci_base[pci_base_count] = section->offset_within_address_space;
        tii_printf("translating accesses to PCI device %d to offset %"PRIxPTR"\n", pci_base_count,
                   pci_base[pci_base_count]);
        pci_base_count++;
    }
}

static void sel4_region_del(MemoryListener *listener, MemoryRegionSection *section)
{
    tii_printf("WARNING: %s entered, but no real implementation\n", __func__);
}

static MemoryListener sel4_io_listener = {
    .eventfd_add = sel4_io_ioeventfd_add,
    .eventfd_del = sel4_io_ioeventfd_del,
    .region_add = sel4_region_add,
    .region_del = sel4_region_del,
};

static int sel4_init(MachineState *ms)
{
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    SeL4State *s = SEL4_STATE(ms->accelerator);
    int rc;
    struct sel4_vm_params params = {
        .ram_size = ms->ram_size,
    };
    void *ram = NULL;

    s->fd = open("/dev/sel4", O_RDWR);
    if (s->fd == -1) {
        fprintf(stderr, "sel4: Failed to open kernel module: %m\n");
        return -errno;
    }

    do {
        rc = sel4_ioctl(s, SEL4_CREATE_VM, &params);
    } while (rc == -EINTR);

    if (rc < 0) {
        fprintf(stderr, "sel4: create VM failed: %d %s\n", -rc,
                strerror(-rc));
        goto err;
    }

    s->vmfd = rc;

    /* setup ram */
    ram = mmap(NULL, ms->ram_size, PROT_READ | PROT_WRITE,
               MAP_SHARED, s->vmfd, 0);
    if (!ram) {
        fprintf(stderr, "sel4: ram mmap failed: %m\n");
        goto err;
    }

    memory_region_init_ram_ptr(&ram_mr, OBJECT(ms), "virt.ram",
                               ms->ram_size, ram);
    vmstate_register_ram_global(&ram_mr);
    ms->ram = &ram_mr;

    /* do not allocate RAM from generic code */
    mc->default_ram_id = NULL;

    rc = sel4_vm_ioctl(s, SEL4_CREATE_IO_HANDLER, 0);
    if (rc < 0) {
        fprintf(stderr, "sel4: create IO handler failed: %d %s\n", -rc,
                strerror(-rc));
        goto err;
    }
    s->ioreqfd = rc;

    s->ioreq_buffer = mmap(NULL, sizeof(*s->ioreq_buffer),
                           PROT_READ | PROT_WRITE, MAP_SHARED, s->ioreqfd, 0);
    if (!s->ioreq_buffer) {
        fprintf(stderr, "sel4: iohandler mmap failed %m\n");
        goto err;
    }


    memory_listener_register(&sel4_io_listener, &address_space_memory);

    qemu_add_vm_change_state_handler(sel4_change_state_handler, s);

    return 0;

err:
    if (s->ioreq_buffer)
        munmap(s->ioreq_buffer, sizeof(*s->ioreq_buffer));

    if (ram)
        munmap(ram, ms->ram_size);

    if (s->vmfd >= 0)
        close(s->vmfd);

    if (s->fd >= 0)
        close(s->fd);

    return rc;
}

static void sel4_accel_class_init(ObjectClass *oc, void *data)
{
    AccelClass *ac = ACCEL_CLASS(oc);

    ac->name = "seL4";
    ac->init_machine = sel4_init;
    ac->setup_post = sel4_setup_post;
    ac->allowed = &sel4_allowed;
}

static void sel4_accel_instance_init(Object *obj)
{
    SeL4State *s = SEL4_STATE(obj);

    s->fd = -1;
    s->vmfd = -1;
    s->ioreqfd = -1;
    s->ioreq_buffer = NULL;
}

static const TypeInfo sel4_accel_type = {
    .name = TYPE_SEL4_ACCEL,
    .parent = TYPE_ACCEL,
    .instance_init = sel4_accel_instance_init,
    .class_init = sel4_accel_class_init,
    .instance_size = sizeof(SeL4State),
};

static void sel4_accel_ops_class_init(ObjectClass *oc, void *data)
{
    AccelOpsClass *ops = ACCEL_OPS_CLASS(oc);

    ops->create_vcpu_thread = dummy_start_vcpu_thread;
}

static const TypeInfo sel4_accel_ops_type = {
    .name = ACCEL_OPS_NAME("sel4"),
    .parent = TYPE_ACCEL_OPS,
    .class_init = sel4_accel_ops_class_init,
    .abstract = true,
};

static void sel4_type_init(void)
{
    type_register_static(&sel4_accel_type);
    type_register_static(&sel4_accel_ops_type);
}
type_init(sel4_type_init);
