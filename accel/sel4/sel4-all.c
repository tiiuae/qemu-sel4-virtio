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
#include "sysemu/sel4.h"
#include "hw/boards.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "migration/vmstate.h"

#include "hw/arm/virt.h"

#include <stdarg.h>
#include <sys/ioctl.h>
#include <sel4/sel4_virt.h>

#define PCI_NUM_SLOTS   (32)

#define EVENT_BAR_EMIT_REGISTER 0x0

extern MemMapEntry (*virt_memmap_customize)(const MemMapEntry *base_memmap, int i);

static MemMapEntry sel4_memmap_customize(const MemMapEntry *base_memmap, int i);

void tii_printf(const char *fmt, ...);

static int vmid = 1;
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
    int event_bar_fd;
    void *iobuf;
    void *event_bar;
    sel4_rpc_t rpc;
    MemoryListener mem_listener;
} SeL4State ;

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

static PCIDevice *pci_devs[PCI_NUM_SLOTS];
static uintptr_t pci_base[16];
static unsigned int pci_base_count;

bool using_sel4(void)
{
    return virt_memmap_customize == sel4_memmap_customize;
}

void sel4_register_pci_device(PCIDevice *d)
{
    if (!using_sel4()) {
        return;
    }

    unsigned int slot = PCI_SLOT(d->devfn);

    SeL4State *s = SEL4_STATE(current_accel());
    struct sel4_vpci_device vpcidev = {
        .pcidev = slot,
    };

    pci_devs[slot] = d;
    printf("Registering PCI device %u to VMM\n", slot);

    if (sel4_vm_ioctl(s, SEL4_CREATE_VPCI_DEVICE, &vpcidev)) {
        fprintf(stderr, "Failed to register PCI device: %m\n");
    }
}

void sel4_set_irq(unsigned int irq, bool state)
{
    int err;

    if (!using_sel4()) {
        return;
    }

    SeL4State *s = SEL4_STATE(current_accel());

    if (state) {
        err = sel4_rpc_doorbell(driver_req_set_irqline(&s->rpc, irq));
    } else {
        err = sel4_rpc_doorbell(driver_req_clear_irqline(&s->rpc, irq));
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
        val = dev->config_read(dev, addr, len);
        memcpy(data, &val, len);
    } else {
        val = 0;
        memcpy(&val, data, len);
        dev->config_write(dev, addr, val, len);
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
        fprintf(stderr, "%s failed, addr=0x%lx, dir=%u\n", __func__, addr, dir);
        exit(1);
    }

    return driver_ack_mmio_finish(&s->rpc, slot, data) ? 0 : -1;
}

static int handle_msg(SeL4State *s, rpcmsg_t *msg)
{
    unsigned int op = BIT_FIELD_GET(msg->mr0, RPC_MR0_OP);

    switch (op) {
    case RPC_MR0_OP_MMIO:
        return handle_mmio(s, msg);
    default:
        /* no idea what to do */
        return -1;
    }

    return 0;
}

static inline int sel4_vm_wait_for_io(SeL4State *s)
{
    return sel4_vm_ioctl(s, SEL4_WAIT_IO, 0);
}

static void *do_sel4_virtio(void *opaque)
{
    SeL4State *s = opaque;
    rpcmsg_t *msg;
    int rc;

    for (;;) {
        sel4_rpc_doorbell(&s->rpc);

        rc = sel4_vm_wait_for_io(s);
        if (rc)
            continue;

        while ((msg = rpcmsg_queue_head(s->rpc.rx_queue)) != NULL) {
            seL4_Word state = BIT_FIELD_GET(msg->mr0, RPC_MR0_STATE);

            if (state == RPC_MR0_STATE_RESERVED) {
                /* VMM side is still crafting the message, try again later */
                break;
            } else if (state == RPC_MR0_STATE_PENDING) {
                /* kernel has not examined this yet, try again later */
                break;
            } else if (state != RPC_MR0_STATE_PROCESSING) {
                /* kernel is supposed to discard completed ioeventfds */
                error_report("corrupted msgqueue");
                exit(1);
            }

            rc = handle_msg(s, msg);
            if (rc) {
                error_report("handle_msg() failed (%d)", rc);
                exit(1);
            }

            msg->mr0 = BIT_FIELD_SET(msg->mr0, RPC_MR0_STATE, RPC_MR0_STATE_COMPLETE);

            /* smp_wmb()? */
            rpcmsg_queue_advance_head(s->rpc.rx_queue);
        }
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
    struct sel4_vm_params params = {
        .id = 1,
        .ram_size = ms->ram_size,
    };
    void *ram = NULL;

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

    s->iobuf = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                    s->ioreqfd, 0);
    if (!s->iobuf) {
        fprintf(stderr, "sel4: iohandler mmap failed %m\n");
        goto err;
    }

    rc = sel4_vm_ioctl(s, SEL4_CREATE_EVENT_BAR, 0);
    if (rc < 0) {
        fprintf(stderr, "sel4: create event handler failed: %d %s\n", -rc,
                strerror(-rc));
        goto err;
    }
    s->event_bar_fd = rc;
    s->event_bar = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                        s->event_bar_fd, 0);
    if (!s->iobuf) {
        fprintf(stderr, "sel4: event mmap failed %m\n");
        goto err;
    }

    s->rpc.rx_queue = device_rx_queue(s->iobuf);
    s->rpc.tx_queue = device_tx_queue(s->iobuf);
    s->rpc.doorbell = s2_fault_doorbell;
    s->rpc.doorbell_cookie = (void *)(((uintptr_t)s->event_bar) + EVENT_BAR_EMIT_REGISTER);
    s->mem_listener.eventfd_add = sel4_io_ioeventfd_add;
    s->mem_listener.eventfd_del = sel4_io_ioeventfd_del;
    s->mem_listener.region_add = sel4_region_add;
    s->mem_listener.region_del = sel4_region_del;

    memory_listener_register(&s->mem_listener, &address_space_memory);

    qemu_add_vm_change_state_handler(sel4_change_state_handler, s);

    return 0;

err:
    if (s->event_bar)
        munmap(s->event_bar, 4096);

    if (s->iobuf)
        munmap(s->iobuf, 2 * 4096);

    if (ram)
        munmap(ram, ms->ram_size);

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

static MemMapEntry sel4_memmap_customize(const MemMapEntry *base_memmap, int i)
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

    switch (i) {
    case VIRT_MEM:
        e.base = uservm_ram_base;
        e.size = uservm_ram_size;
        break;
    case VIRT_PCIE_MMIO:
        e.base = uservm_pcie_mmio_base;
        e.size = uservm_pcie_mmio_size;
        break;
    case VIRT_PCIE_PIO:
        e.base = uservm_pcie_mmio_base + uservm_pcie_mmio_size;
        e.size = 0x10000;
        break;
    case VIRT_PCIE_ECAM:
        e.base = uservm_pcie_mmio_base + uservm_pcie_mmio_size + 0x10000;
        e.size = 0x1000000;
        break;
    default:
        return base_memmap[i];
    };

    return e;
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
    memset(&s->rpc, 0, sizeof(s->rpc));

    virt_memmap_customize = sel4_memmap_customize;
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
