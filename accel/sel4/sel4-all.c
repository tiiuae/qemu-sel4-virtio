/*
 * Copyright 2022, 2023, 2024, Technology Innovation Institute
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qemu/accel.h"
#include "qemu/atomic.h"
#include "qemu/range.h"
#include "sysemu/cpus.h"
#include "sysemu/runstate.h"
#include "sysemu/sel4.h"
#include "hw/boards.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "migration/vmstate.h"

#include <stdarg.h>
#include <sys/ioctl.h>
#include <sel4/sel4_virt.h>

#define EVENT_BAR_EMIT_REGISTER 0x0

void tii_printf(const char *fmt, ...);

static int vmid = 1;
static MemoryRegion ram_mr;
bool sel4_allowed;
bool sel4_ext_vpci_bus_allowed;
bool sel4_ext_msi_allowed;
bool sel4_irqfds_allowed;
bool sel4_msi_via_irqfd_allowed;

static QemuThread sel4_virtio_thread;

static void *do_sel4_virtio(void *opaque);

typedef struct SeL4State
{
    AccelState parent_obj;

    int fd;
    int vmfd;
    struct {
        int fd;
        void *ptr;
    } maps[NUM_SEL4_MEM_MAP];
    sel4_rpc_t rpc;
    MemoryListener mem_listener;
    DeviceListener dev_listener;
} SeL4State;

#define TYPE_SEL4_ACCEL ACCEL_CLASS_NAME("sel4")

DECLARE_INSTANCE_CHECKER(SeL4State, SEL4_STATE, TYPE_SEL4_ACCEL)

typedef struct SeL4MmioRegion {
    hwaddr start_addr;
    Int128 size;
    QLIST_ENTRY(SeL4MmioRegion) list;
} SeL4MmioRegion;

static QLIST_HEAD(, SeL4MmioRegion) mmio_regions = QLIST_HEAD_INITIALIZER(mmio_regions);

static QemuMutex sel4_mmio_regions_lock;

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

static void sel4_setup_post(MachineState *ms, AccelState *accel)
{
    SeL4State *s = SEL4_STATE(ms->accelerator);

    qemu_thread_create(&sel4_virtio_thread, "seL4 virtio",
        do_sel4_virtio, s, QEMU_THREAD_JOINABLE);
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

static unsigned int pci_dev_count = 0;
static PCIDevice *pci_devs[SEL4_VPCI_INTERRUPTS];

void sel4_register_pci_device(PCIDevice *d)
{
    if (!sel4_enabled()) {
        return;
    }

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
}

void sel4_set_irq(unsigned int irq, bool state)
{
    int err;

    if (!sel4_enabled()) {
        return;
    }

    SeL4State *s = SEL4_STATE(current_accel());

    if (state) {
        err = driver_req_set_irqline(&s->rpc, irq);
    } else {
        err = driver_req_clear_irqline(&s->rpc, irq);
    }

    if (err) {
        fprintf(stderr, "Failed to set IRQ %u\n", irq);
        exit(1);
    }
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

static int sel4_mmio_do_io(unsigned int dir, seL4_Word addr, seL4_Word *data,
                           unsigned int len)
{
    MemTxResult result;

    if (dir == SEL4_IO_DIR_READ) {
        result = address_space_read(&address_space_memory, addr,
                                    MEMTXATTRS_UNSPECIFIED, data, len);
    } else {
        result = address_space_write(&address_space_memory, addr,
                                     MEMTXATTRS_UNSPECIFIED, data, len);
    }

    return (result == MEMTX_OK) ? 0 : -1;
}

static int sel4_pci_do_io(unsigned int addr_space, unsigned int dir,
                          seL4_Word addr, seL4_Word *data,
                          unsigned int len)
{
    if (addr_space >= ARRAY_SIZE(pci_devs)) {
        return -1;
    }

    PCIDevice *dev = pci_devs[addr_space];
    if (!dev) {
        return -1;
    }

    uint64_t val;

    if (dir == SEL4_IO_DIR_READ) {
        val = pci_host_config_read_common(dev, addr, pci_config_size(dev), len);;
        memcpy(data, &val, len);
    } else {
        val = 0;
        memcpy(&val, data, len);
        pci_host_config_write_common(dev, addr, pci_config_size(dev), val, len);
    }

    return 0;
}

static inline int handle_mmio(SeL4State *s, rpcmsg_t *req)
{
    int err;

    seL4_Word dir = BIT_FIELD_GET(req->mr0, RPC_MR0_MMIO_DIRECTION);
    seL4_Word as = BIT_FIELD_GET(req->mr0, RPC_MR0_MMIO_ADDR_SPACE);
    seL4_Word len = BIT_FIELD_GET(req->mr0, RPC_MR0_MMIO_LENGTH);
    seL4_Word slot = BIT_FIELD_GET(req->mr0, RPC_MR0_MMIO_SLOT);
    seL4_Word addr = req->mr1;
    seL4_Word data = req->mr2;

    qemu_mutex_lock_iothread();

    if (as == AS_GLOBAL) {
        err = sel4_mmio_do_io(dir, addr, &data, len);
    } else {
        err = sel4_pci_do_io(as, dir, addr, &data, len);
    }

    qemu_mutex_unlock_iothread();

    if (err) {
        fprintf(stderr, "%s failed, addr=0x%lx, dir=%lu\n", __func__, addr, dir);
        exit(1);
    }

    err = driver_ack_mmio_finish(&s->rpc, slot, data);
    if (err) {
        return RPCMSG_STATE_ERROR;
    }

    return RPCMSG_STATE_FREE;
}

static unsigned int rpc_process(rpcmsg_t *msg, void *cookie)
{
    unsigned int next_state = RPCMSG_STATE_ERROR;
    SeL4State *s = cookie;

    switch (QEMU_OP(msg->mr0)) {
    case QEMU_OP_MMIO:
        next_state = handle_mmio(s, msg);
        break;
    default:
        fprintf(stderr, "Unknown op %u\n", QEMU_OP(msg->mr0));
        break;
    }

    return next_state;
}

static void *do_sel4_virtio(void *opaque)
{
    SeL4State *s = opaque;
    int rc;

    for (;;) {
        rc = sel4_vm_ioctl(s, SEL4_WAIT_IO, 0);
        if (rc)
            continue;

        sel4_rpc_rx_process(&s->rpc, rpc_process, s);
    }

    return NULL;
}

static int sel4_ioeventfd_set(SeL4State *s, int fd, hwaddr addr, uint32_t val,
                              bool assign, uint32_t size, bool datamatch)
{
    struct sel4_ioeventfd_config config = {
        .fd = fd,
        .addr = addr,
        .len = size,
        .data = datamatch ? val : 0,
        .flags = 0,
    };

    if (datamatch) {
        config.flags |= SEL4_IOEVENTFD_FLAG_DATAMATCH;
    }

    if (!assign) {
        config.flags |= SEL4_IOEVENTFD_FLAG_DEASSIGN;
    }

    if (sel4_vm_ioctl(s, SEL4_IOEVENTFD, &config) < 0) {
        return -errno;
    }

    return 0;
}

static void sel4_io_ioeventfd_add(MemoryListener *listener,
                                  MemoryRegionSection *section,
                                  bool match_data, uint64_t data,
                                  EventNotifier *e)
{
    SeL4State *s = container_of(listener, SeL4State, mem_listener);
    int fd = event_notifier_get_fd(e);
    int rc;

    rc = sel4_ioeventfd_set(s, fd, section->offset_within_address_space,
                            data, true, int128_get64(section->size),
                            match_data);
    if (rc < 0) {
        fprintf(stderr, "%s: error adding ioeventfd: %s (%d)\n",
                __func__, strerror(-rc), -rc);
        abort();
    }
}

static void sel4_io_ioeventfd_del(MemoryListener *listener,
                                 MemoryRegionSection *section,
                                 bool match_data, uint64_t data,
                                 EventNotifier *e)

{
    SeL4State *s = container_of(listener, SeL4State, mem_listener);
    int fd = event_notifier_get_fd(e);
    int rc;

    rc = sel4_ioeventfd_set(s, fd, section->offset_within_address_space,
                            data, false, int128_get64(section->size),
                            match_data);
    if (rc < 0) {
        fprintf(stderr, "%s: error deleting ioeventfd: %s (%d)\n",
                __func__, strerror(-rc), -rc);
        abort();
    }
}

uint32_t sel4_msi_data_to_gsi(uint32_t data)
{
    return data & 0xffff;
}

void sel4_msi_trigger(PCIDevice *dev, MSIMessage msg)
{
    assert(sel4_ext_msi_enabled());

    uint32_t irq = sel4_msi_data_to_gsi(msg.data);
    sel4_set_irq(irq, true);
    sel4_set_irq(irq, false);
}

int sel4_add_msi_route(int vector, PCIDevice *dev)
{
    MSIMessage msg = {0, 0};

    if (pci_available && dev) {
        msg = pci_get_msi_message(dev, vector);
    }

    // direct gsi mapping
    return sel4_msi_data_to_gsi(msg.data);
}

static int sel4_assign_irqfd(SeL4State *s, EventNotifier *event,
                             EventNotifier *resample, int virq,
                             bool assign)
{
    int fd = event_notifier_get_fd(event);
    int rfd = resample ? event_notifier_get_fd(resample) : -1;

    struct sel4_irqfd_config irqfd = {
        .fd = fd,
        .virq = virq,
        .flags = assign ? 0 : SEL4_IRQFD_FLAG_DEASSIGN,
    };

    if (rfd != -1) {
        assert(assign);
        error_report("sel4: irqfd resamplefd not supported");
    }

    if (!sel4_irqfds_enabled()) {
        return -ENOSYS;
    }

    int rc = sel4_vm_ioctl(s, SEL4_IRQFD, &irqfd);
    if (rc) {
        error_report("sel4: error adding irqfd: %s (%d)",
                     strerror(-rc), -rc);
    }

    return rc;
}

int sel4_add_irqfd_notifier(EventNotifier *n, EventNotifier *rn, int virq)
{
    SeL4State *s = SEL4_STATE(current_accel());
    return sel4_assign_irqfd(s, n, rn, virq, true);
}

int sel4_remove_irqfd_notifier(EventNotifier *n, int virq)
{
    SeL4State *s = SEL4_STATE(current_accel());
    return sel4_assign_irqfd(s, n, NULL, virq, false);
}

static void sel4_dev_realize(DeviceListener *listener,
                             DeviceState *dev)
{
    if (object_dynamic_cast(OBJECT(dev), TYPE_PCI_DEVICE)) {
        PCIDevice *pci_dev = PCI_DEVICE(dev);
        pci_dev->msi_trigger = sel4_msi_trigger;
    }
}

static DeviceListener sel4_dev_listener = {
    .realize = sel4_dev_realize,
};

static unsigned long sel4_mem_map_size(MachineState *ms, unsigned int index)
{
    switch (index) {
    case SEL4_MEM_MAP_RAM:
        return ms->ram_size;
    case SEL4_MEM_MAP_IOBUF:
        return IOBUF_NUM_PAGES * 4096;
    case SEL4_MEM_MAP_EVENT_BAR:
        return 4096;
    default:
        break;
    }

    return 0;
}

static void s2_fault_doorbell(void *cookie)
{
    uint32_t *event_bar = cookie;

    /* data does not matter, just the write fault */
    event_bar[0] = 1;
}

static int sel4_init(MachineState *ms)
{
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    SeL4State *s = SEL4_STATE(ms->accelerator);
    int rc;
    int i;
    struct sel4_vm_params params = {
        .id = 1,
        .ram_size = ms->ram_size,
    };

    char *p = getenv("VMID");
    if (p) {
        vmid = atoi(p);
    }
    params.id = vmid;

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

    for (i = 0; i < NUM_SEL4_MEM_MAP; i++) {
        rc = sel4_vm_ioctl(s, SEL4_CREATE_IO_HANDLER, i);
        if (rc < 0) {
            fprintf(stderr, "sel4: create IO handler failed: %d %s\n", -rc,
                    strerror(-rc));
            goto err;
        }

        s->maps[i].fd = rc;
        s->maps[i].ptr = mmap(NULL, sel4_mem_map_size(ms, i),
                              PROT_READ | PROT_WRITE, MAP_SHARED,
                              s->maps[i].fd, 0);
        if (s->maps[i].ptr == MAP_FAILED) {
            fprintf(stderr, "sel4: iohandler mmap failed %m\n");
            goto err;
        }
    }

    /* setup ram */
    memory_region_init_ram_ptr(&ram_mr, OBJECT(ms), "virt.ram",
                               ms->ram_size, s->maps[SEL4_MEM_MAP_RAM].ptr);
    vmstate_register_ram_global(&ram_mr);
    ms->ram = &ram_mr;

    /* do not allocate RAM from generic code */
    mc->default_ram_id = NULL;

    rc = sel4_rpc_init(&s->rpc,
                       device_rx_queue(s->maps[SEL4_MEM_MAP_IOBUF].ptr),
                       device_tx_queue(s->maps[SEL4_MEM_MAP_IOBUF].ptr),
                       RPCMSG_STATE_DEVICE_USER,
                       s2_fault_doorbell,
                       (void *)(((uintptr_t)s->maps[SEL4_MEM_MAP_EVENT_BAR].ptr) + EVENT_BAR_EMIT_REGISTER));
    if (rc) {
        fprintf(stderr, "sel4: sel4_rpc_init() failed: %d\n", rc);
        goto err;
    }

    s->mem_listener.eventfd_add = sel4_io_ioeventfd_add;
    s->mem_listener.eventfd_del = sel4_io_ioeventfd_del;

    memory_listener_register(&s->mem_listener, &address_space_memory);

    sel4_ext_vpci_bus_allowed = true;
    sel4_ext_msi_allowed = true;
    sel4_irqfds_allowed = true;

    if (sel4_ext_msi_enabled()) {
        s->dev_listener = sel4_dev_listener;
        device_listener_register(&s->dev_listener);

        msi_nonbroken = true;
        sel4_msi_via_irqfd_allowed = sel4_irqfds_enabled();
    }

    qemu_add_vm_change_state_handler(sel4_change_state_handler, s);

    return 0;

err:
    i = NUM_SEL4_MEM_MAP;
    while (i) {
        i--;
        if (s->maps[i].ptr) {
            munmap(s->maps[i].ptr, sel4_mem_map_size(ms, i));
            close(s->maps[i].fd);
        }
    }

    if (s->vmfd >= 0)
        close(s->vmfd);

    if (s->fd >= 0)
        close(s->fd);

    return rc;
}

static unsigned long uservm_ram_base;
static unsigned long uservm_ram_size;
static unsigned long uservm_pcie_mmio_base;
static unsigned long uservm_pcie_mmio_size;

static int parse_kernel_bootargs(void)
{
    FILE *fp;
    char buffer[4096];
    char *s, *arg, *sp;
    int ret = -1;
    int id;

    if ((fp = fopen("/proc/cmdline", "r")) == NULL) {
        goto out;
    }
    if (fgets(buffer, sizeof buffer, fp) == NULL) {
        goto close_fp;
    }

    for (s = buffer; (arg = strtok_r(s, " \t\n", &sp)) != NULL; s = NULL) {
        if (strncmp(arg, "uservm=", 7)) {
            continue;
        }
        if (sscanf(arg + 7, "%d,%lx,%lx,%lx,%lx",
            &id,
            &uservm_ram_base,
            &uservm_ram_size,
            &uservm_pcie_mmio_base,
            &uservm_pcie_mmio_size) != 5) {
            fprintf(stderr, "Improper %s in bootargs\n", arg);
            goto close_fp;
        }
        if (id == vmid) {
            ret = 0;
            break;
        }
    }

    fprintf(stderr, "User VM %d, shared RAM %zu bytes at 0x%"PRIxPTR", PCI MMIO %zu bytes at 0x%"PRIxPTR"\n",
            id, uservm_ram_size, uservm_ram_base,
            uservm_pcie_mmio_size, uservm_pcie_mmio_base);

close_fp:
    fclose(fp);

out:
    return ret;
}

MemMapEntry sel4_region_get(SeL4MemoryRegion region)
{
    static bool init = false;
    MemMapEntry e;

    if (!init) {
        if (parse_kernel_bootargs()) {
            fprintf(stderr, "No uservm details given in kernel bootargs\n");
            exit(1);
        }
        init = true;
    }

    switch (region) {
    case SEL4_REGION_RAM:
        e.base = uservm_ram_base;
        e.size = uservm_ram_size;
        break;
    case SEL4_REGION_PCIE_MMIO:
        e.base = uservm_pcie_mmio_base;
        e.size = uservm_pcie_mmio_size;
        break;
    case SEL4_REGION_PCIE_PIO:
        e.base = uservm_pcie_mmio_base + uservm_pcie_mmio_size;
        e.size = 0x10000;
        break;
    default:
        fprintf(stderr, "Invalid memory region %d\n", region);
        exit(1);
    };

    return e;
}

static int sel4_set_mmio_region(SeL4State *s,
                                hwaddr addr,
                                uint64_t size,
                                bool set)
{
    struct sel4_mmio_region_config config = {
        .gpa = addr,
        .len = size,
        .flags = set ? 0 : SEL4_MMIO_REGION_FREE,
    };

    if (sel4_vm_ioctl(s, SEL4_MMIO_REGION, &config) < 0) {
        return -errno;
    }

    return 0;
}

int sel4_mmio_region_add(MemoryRegionSection *section)
{
    SeL4MmioRegion *entry;
    SeL4State *s = SEL4_STATE(current_accel());
    int rc = 0;

    QEMU_LOCK_GUARD(&sel4_mmio_regions_lock);

    QLIST_FOREACH(entry, &mmio_regions, list) {
        if (ranges_overlap(entry->start_addr, int128_get64(entry->size),
                           section->offset_within_address_space,
                           int128_get64(section->size))) {
            /* Already registered */
            return 1;
        }
    }

    if (memory_region_is_ram(section->mr)) {
        fprintf(stderr, "%s: mmio region is RAM\n", __func__);
        return -1;
    }

    rc = sel4_set_mmio_region(s, section->offset_within_address_space,
                              int128_get64(section->size), true);
    if (rc) {
        fprintf(stderr, "%s: mmio region register failed: %d\n",
                __func__, rc);
        return rc;
    }

    entry = g_try_new0(SeL4MmioRegion, 1);
    if (!entry) {
        fprintf(stderr, "%s: entry allocation failed\n", __func__);
        return -ENOMEM;
    }

    entry->start_addr = section->offset_within_address_space;
    entry->size = section->size;

    QLIST_INSERT_HEAD(&mmio_regions, entry, list);

    return rc;
}

int sel4_mmio_region_del(MemoryRegionSection *section)
{
    SeL4MmioRegion *entry, *tmp;
    SeL4State *s = SEL4_STATE(current_accel());

    QEMU_LOCK_GUARD(&sel4_mmio_regions_lock);
    QLIST_FOREACH_SAFE(entry, &mmio_regions, list, tmp) {
        if (entry->start_addr == section->offset_within_address_space &&
            int128_get64(entry->size) == int128_get64(section->size)) {
            QLIST_REMOVE(entry, list);
            sel4_set_mmio_region(s, entry->start_addr, int128_get64(entry->size), false);
            g_free(entry);
        }
    }
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
    int i;
    SeL4State *s = SEL4_STATE(obj);

    s->fd = -1;
    s->vmfd = -1;
    for (i = 0; i < NUM_SEL4_MEM_MAP; i++) {
        s->maps[i].ptr = NULL;
        s->maps[i].fd = -1;
    }

    qemu_mutex_init(&sel4_mmio_regions_lock);
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
