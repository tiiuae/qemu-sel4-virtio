#pragma once

#ifdef QEMU
typedef unsigned long seL4_Word;
#define ORIGIN ""
#define debug_printf(fmt, ...) qemu_printf(fmt "\n", ## __VA_ARGS__)
#else
#define ORIGIN "VMM "
#define debug_printf(fmt, ...) printf(fmt "\n", ## __VA_ARGS__)
#endif

typedef struct {
    size_t sz;
    char data[1024];
} logbuffer_t;


#ifndef QEMU
/* to QEMU */
#define tx_queue (((rpcmsg_queue_t *) ctrl) + 0)

/* from QEMU */
#define rx_queue (((rpcmsg_queue_t *) ctrl) + 1)

#define logbuffer ((logbuffer_t *)(rx_queue + 1))
#else
/* from VMM */
#define rx_queue (((rpcmsg_queue_t *) dataports[DP_CTRL].data) + 0)

/* to VMM */
#define tx_queue (((rpcmsg_queue_t *) dataports[DP_CTRL].data) + 1)

#define logbuffer ((logbuffer_t *)(tx_queue + 1))
#endif

/* from VMM to QEMU */
#define QEMU_OP_READ        0
#define QEMU_OP_WRITE       1
#define QEMU_OP_PUTC_LOG    2

/* from QEMU to VMM */
#define QEMU_OP_SET_IRQ     16
#define QEMU_OP_CLR_IRQ     17
#define QEMU_OP_START_VM    18
#define QEMU_OP_REGISTER_PCI_DEV    19

#define QEMU_OP_MASK        0xffULL
#define QEMU_OP_SHIFT       0
#define QEMU_OP(__x__)      ((unsigned int)(((__x__) & QEMU_OP_MASK) >> QEMU_OP_SHIFT))

#define QEMU_PCIDEV_MASK    0xff00ULL
#define QEMU_PCIDEV_SHIFT   8
#define QEMU_PCIDEV(__x__)  ((unsigned int)(((__x__) & QEMU_PCIDEV_MASK) >> QEMU_PCIDEV_SHIFT))

#define QEMU_ID_MASK        0xff0000ULL
#define QEMU_ID_SHIFT       16
#define QEMU_ID(__x__)      ((unsigned int)(((__x__) & QEMU_ID_MASK) >> QEMU_ID_SHIFT))
#define QEMU_ID_FROM(__x__) (((__x__) << QEMU_ID_SHIFT) & QEMU_ID_MASK)

#define QEMU_VCPU_MASK       0xff000000ULL
#define QEMU_VCPU_SHIFT      24
#define QEMU_VCPU(__x__)     ((unsigned int)(((__x__) & QEMU_VCPU_MASK) >> QEMU_VCPU_SHIFT))
#define QEMU_VCPU_NONE       0xff

#define RPCMSG_BUFFER_SIZE  32

typedef struct {
    seL4_Word mr0;
    seL4_Word mr1;
    seL4_Word mr2;
    seL4_Word mr3;
} rpcmsg_t;

typedef struct {
    uint32_t head;
    uint32_t tail;
    uint32_t rsvd[2];
    rpcmsg_t data[RPCMSG_BUFFER_SIZE];
} rpcmsg_queue_t;

#define QUEUE_PREV(_i) ((_i) ? ((_i) - 1) : (RPCMSG_BUFFER_SIZE - 1))
#define QUEUE_NEXT(_i) (((_i) + 1) & (RPCMSG_BUFFER_SIZE - 1))

static void rpcmsg_queue_init(rpcmsg_queue_t *q)
{
    memset(q, 0, sizeof(*q));
}

static void rpcmsg_queue_dump(const char *name, rpcmsg_queue_t *q, unsigned int idx)
{
    unsigned int start_idx = idx;
    char tmp[128];

    sprintf(tmp, ORIGIN "name = %s head = %02d tail = %02d", name, q->head, q->tail);
    debug_printf("%s", tmp);

    do {
        rpcmsg_t *msg = &q->data[idx];
        sprintf(tmp, ORIGIN "%02d: %08"PRIx64" %08"PRIx64" %08"PRIx64" %08"PRIx64,
                idx, msg->mr0, msg->mr1, msg->mr2, msg->mr3);
        debug_printf("%s", tmp);
        idx = QUEUE_PREV(idx);
    } while (idx != start_idx);
}

static inline bool rpcmsg_queue_full(rpcmsg_queue_t *q)
{
    return QUEUE_NEXT(q->tail) == q->head;
}

static inline bool rpcmsg_queue_empty(rpcmsg_queue_t *q)
{
    return q->tail == q->head;
}

static inline rpcmsg_t *rpcmsg_queue_head(rpcmsg_queue_t *q)
{
    return rpcmsg_queue_empty(q) ? NULL : (q->data + q->head);
}

static inline rpcmsg_t *rpcmsg_queue_tail(rpcmsg_queue_t *q)
{

    return rpcmsg_queue_full(q) ? NULL : (q->data + q->tail);
}

static inline void rpcmsg_queue_advance_head(rpcmsg_queue_t *q)
{
    assert(!rpcmsg_queue_empty(q));
    q->head = QUEUE_NEXT(q->head);
}

static inline void rpcmsg_queue_advance_tail(rpcmsg_queue_t *q)
{
    assert(!rpcmsg_queue_full(q));
    q->tail = QUEUE_NEXT(q->tail);
}

static inline void rpcmsg_queue_enqueue(rpcmsg_queue_t *q, rpcmsg_t *msg)
{
    assert(!rpcmsg_queue_full(q));
    memcpy(q->data + q->tail, msg, sizeof(*msg));
    q->tail = QUEUE_NEXT(q->tail);
}
