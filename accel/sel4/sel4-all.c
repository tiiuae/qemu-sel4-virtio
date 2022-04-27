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
#include "sysemu/cpus.h"
#include "hw/boards.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"

#include <stdarg.h>

void tii_printf(const char *fmt, ...);

bool sel4_allowed;

void sel4_register_pci_device(PCIDevice *d);
static void send_qemu_op(uint32_t op, uint32_t param);

static QemuThread sel4_virtio_thread;
static QemuMutex vmm_send_mutex;

static void *do_sel4_virtio(void *opaque);

static void qemu_pci_read(void *out, uint32_t address, int len, unsigned int pcidev);
static void qemu_pci_write(void *out, uint32_t address, int len, unsigned int pcidev);

static void vmm_doorbell_ring(void);
static void vmm_doorbell_wait(void);

void sel4_set_irq(unsigned int irq, bool);

#define UIO_INDEX_DATAPORT  1
#define UIO_INDEX_EVENT_BAR 0

#define DP_CTRL 0
#define DP_MEM  1

typedef struct {
    void *data;
    void *event;
    int fd;
    size_t size;
    const char *filename;
} dataport_t;

static dataport_t dataports[] = {
    {
        .fd = -1,
        .size = 0x100000,
        .filename = "/dev/uio0",
    },
    {
        .fd = -1,
        .size = 128ULL << 20,
        .filename = "/dev/uio1",
    },
};

void seL4_Yield(void){}

#define QEMU
#include "sel4-qemu.h"

static void qemu_send_msg(const rpcmsg_t *msg);


static int dataport_open(dataport_t *dp)
{
    if (dp->fd != -1) {
        return 0;
    }

    dp->fd = open(dp->filename, O_RDWR);
    if (dp->fd < 0) {
        return -1;
    }

    dp->data = mmap(NULL, dp->size, PROT_READ | PROT_WRITE, MAP_SHARED, dp->fd, UIO_INDEX_DATAPORT * 0x1000);
    if (dp->data == (void *) -1) {
        return -1;
    }

    dp->event = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, dp->fd, UIO_INDEX_EVENT_BAR * 0x1000);
    if (dp->event == (void *) -1) {
        return -1;
    }

    tii_printf("dataport \"%s\" opened\n", dp->filename);

    return 0;
}

static void open_dataports(void)
{
    static int already_opened = 0;

    if (already_opened) {
        return;
    }
    already_opened = 1;

    if (dataport_open(&dataports[DP_CTRL])) {
        perror("cannot open dataport\n");
        exit(1);
    }
    if (dataport_open(&dataports[DP_MEM])) {
        perror("cannot open dataport\n");
        exit(1);
    }

    qemu_mutex_init(&vmm_send_mutex);

    qemu_thread_create(&sel4_virtio_thread, "seL4 virtio",
        do_sel4_virtio, NULL, QEMU_THREAD_JOINABLE);
}

static void sel4_setup_post(MachineState *ms, AccelState *accel)
{
    printf("running sel4_setup_post\n");
}

static int dataport_wait(dataport_t *dp)
{
    uint32_t val;
    return read(dp->fd, &val, sizeof val);
}

static int dataport_emit(dataport_t *dp)
{
    ((uint32_t *) dp->event)[0] = 1;
    return 0;
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

static void qemu_pci_read(void *out, uint32_t address, int len, unsigned int pcidev)
{
    qemu_mutex_lock_iothread();
    if (pcidev > 16) {
        address_space_read(&address_space_memory, address, MEMTXATTRS_UNSPECIFIED, out, len);
    } else {
        PCIDevice *dev = pci_devs[pcidev];

        uint32_t val = dev->config_read(dev, address, len);
        memcpy(out, &val, len);
    }
    qemu_mutex_unlock_iothread();
}

static void qemu_pci_write(void *out, uint32_t address, int len, unsigned int pcidev)
{
    qemu_mutex_lock_iothread();
    if (pcidev > 16) {
        address_space_write(&address_space_memory, address, MEMTXATTRS_UNSPECIFIED, out, len);
    } else {
        PCIDevice *dev = pci_devs[pcidev];

        uint32_t val = 0;
        memcpy(&val, out, len);

        dev->config_write(dev, address, val, len);
    }
    qemu_mutex_unlock_iothread();
}

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
    pci_devs[pci_dev_count] = d;
    printf("Registering PCI device to VMM\n");
    send_qemu_op(QEMU_OP_REGISTER_PCI_DEV | (pci_dev_count << QEMU_PCIDEV_SHIFT), 0);
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

static void vmm_doorbell_ring(void)
{
    dataport_emit(&dataports[DP_CTRL]);
}

static void vmm_doorbell_wait(void)
{
    int err = dataport_wait(&dataports[DP_CTRL]);
    if (err < 0) {
        printf("Error: %s\n", strerror(errno));
        abort();
    }
}

static void send_qemu_op(uint32_t op, uint32_t param)
{
    rpcmsg_t msg = {
        .mr0 = op,
        .mr1 = param,
    };

    qemu_send_msg(&msg);
}

void sel4_set_irq(unsigned int irq, bool state)
{
    send_qemu_op(state ? QEMU_OP_SET_IRQ : QEMU_OP_CLR_IRQ, irq);
}

static void sel4_change_state_handler(void *opaque, bool running, RunState state)
{
    if (running) {
        open_dataports();
        printf("Starting user VM\n");
        send_qemu_op(QEMU_OP_START_VM, 0);
    }
}

static void qemu_send_msg(const rpcmsg_t *msg)
{
    qemu_mutex_lock(&vmm_send_mutex);
    rpcmsg_t *reply = rpcmsg_queue_tail(tx_queue);
    if (!reply) {
        printf("QEMU TX queue full\n");
        abort();
    }
    *reply = *msg;
    smp_mb();
    rpcmsg_queue_advance_tail(tx_queue);
    vmm_doorbell_ring();
    qemu_mutex_unlock(&vmm_send_mutex);
}

static void qemu_request_read(rpcmsg_t *msg)
{
    rpcmsg_t reply = *msg;
    qemu_pci_read(&reply.mr3, reply.mr1, reply.mr2, QEMU_PCIDEV(reply.mr0));
    qemu_send_msg(&reply);
}

static void qemu_request_write(rpcmsg_t *msg)
{
    rpcmsg_t reply = *msg;
    /* Writes are synchronous, hence we need to reply. We can do it before
     * doing the actual write, since any new fault cannot be served before
     * we finish this function anyway. */
    qemu_send_msg(&reply);
    qemu_pci_write(&reply.mr3, reply.mr1, reply.mr2, QEMU_PCIDEV(reply.mr0));
    uintptr_t addr = reply.mr1;
    if (!is_raw_address(addr) && pci_base[QEMU_PCIDEV(reply.mr0)]) {
        addr += pci_base[QEMU_PCIDEV(reply.mr0)];
    }
    sel4_listener_region_t *lr = lr_find_match(addr, reply.mr2);
    if (lr) {
        event_notifier_set(lr->n);
    }
}

static void qemu_putc_log(rpcmsg_t *msg)
{
    rpcmsg_t reply = *msg;

    for (size_t i = 0; i < logbuffer->sz; i++) {
        if (logbuffer->data[i] != 0x0d) {
            tii_printf("%c", logbuffer->data[i]);
        }
    }

    qemu_send_msg(&reply);
}

static void handle_qemu_request(rpcmsg_t *msg)
{
    switch (QEMU_OP(msg->mr0)) {
    case QEMU_OP_READ:
        qemu_request_read(msg);
        break;
    case QEMU_OP_WRITE:
        qemu_request_write(msg);
        break;
    case QEMU_OP_PUTC_LOG:
        qemu_putc_log(msg);
        break;
    default:
        printf("Invalid operation %"PRIu32"\n", QEMU_OP(msg->mr0));
        abort();
    }
}

static void *do_sel4_virtio(void *opaque)
{
    for (;;) {
        rpcmsg_t *msg = rpcmsg_queue_head(rx_queue);
        if (msg) {
            handle_qemu_request(msg);
            rpcmsg_queue_advance_head(rx_queue);
	    continue;
        }

        vmm_doorbell_wait();
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
    open_dataports();

    MachineClass *mc = MACHINE_GET_CLASS(ms);

    memory_listener_register(&sel4_io_listener, &address_space_memory);

    qemu_add_vm_change_state_handler(sel4_change_state_handler, NULL);

    return 0;
}

static void sel4_accel_class_init(ObjectClass *oc, void *data)
{
    AccelClass *ac = ACCEL_CLASS(oc);

    ac->name = "seL4";
    ac->init_machine = sel4_init;
    ac->setup_post = sel4_setup_post;
    ac->allowed = &sel4_allowed;
}

#define TYPE_SEL4_ACCEL ACCEL_CLASS_NAME("sel4")

static const TypeInfo sel4_accel_type = {
    .name = TYPE_SEL4_ACCEL,
    .parent = TYPE_ACCEL,
    .class_init = sel4_accel_class_init,
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
