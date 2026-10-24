/*
 * BCM2835 VCHIQ, the VideoCore side of the message queue to the ARM
 *
 * VCHIQ is the message queue between Linux on the ARM ("brcm,bcm2835-vchiq"
 * and "brcm,bcm2711-vchiq") and the VideoCore firmware. The ARM allocates
 * the shared memory: slot zero, which holds the state of both sides, then
 * the message slots of each side. It initialises its own (slave) side and
 * hands the memory over with the RPI_FIRMWARE_VCHIQ_INIT property, after
 * which the VideoCore (master) initialises its side. Each side writes its
 * messages into its own slots, and the other side gives a slot back once
 * it has read it. A side signals an event of the other one by setting
 * "fired" and, if the other side "armed" it, ringing its doorbell: the
 * ARM writes BELL2, the VideoCore raises the doorbell interrupt, which a
 * read of BELL0 clears.
 *
 * The model is the VideoCore side, as Linux 6.6 (VCHIQ_VERSION 8) expects
 * it: it connects, opens the services it implements and closes the others
 * at once, which a client sees as a service the firmware does not have.
 * The services are vc-sm-cma's shared memory service (SMEM), which imports
 * the ARM buffers the VideoCore uses, and MMAL (mmal, in
 * bcm2835_vchiq_mmal.c), the multimedia components behind the codec and
 * ISP drivers. Bulk transfers and synchronous messages are not
 * implemented: a bulk transfer is aborted.
 *
 * The messages are handled in a bottom half, after the doorbell, and each
 * may get one reply. The model reads the next ARM message only once there
 * is room for a reply in its slots, so a slow ARM holds the messages back
 * instead of losing replies.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "hw/misc/bcm2835_vchiq.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "system/dma.h"
#include "trace.h"

/* Doorbells, from the VCHIQ base */
REG32(BELL0, 0x00)                      /* to the ARM: read clears */
REG32(BELL1, 0x04)
REG32(BELL2, 0x08)                      /* to the VideoCore: write rings */
REG32(BELL3, 0x0c)

#define BELL_ACTIVE             BIT(2)

#define FOURCC(a, b, c, d) \
    ((uint32_t)(a) << 24 | (b) << 16 | (c) << 8 | (d))

#define VCHIQ_MAGIC             FOURCC('V', 'C', 'H', 'I')
#define VCHIQ_VERSION           8
#define VCHIQ_VERSION_MIN       3
#define VCHIQ_SLOT_SIZE         4096
#define VCHIQ_SLOT_MASK         (VCHIQ_SLOT_SIZE - 1)
#define VCHIQ_MAX_SLOTS         128
#define VCHIQ_MAX_SLOTS_PER_SIDE 64
#define VCHIQ_HEADER_SIZE       8

/* struct remote_event */
#define EVENT_ARMED             0x0
#define EVENT_FIRED             0x4

/* struct vchiq_shared_state */
#define SHARED_INITIALISED      0x00
#define SHARED_SLOT_FIRST       0x04
#define SHARED_SLOT_LAST        0x08
#define SHARED_SLOT_SYNC        0x0c
#define SHARED_TRIGGER          0x10
#define SHARED_TX_POS           0x1c
#define SHARED_RECYCLE          0x20
#define SHARED_SLOT_QUEUE_RECYCLE 0x2c
#define SHARED_SYNC_TRIGGER     0x30
#define SHARED_SYNC_RELEASE     0x3c
#define SHARED_SLOT_QUEUE       0x48
#define SHARED_DEBUG \
    (SHARED_SLOT_QUEUE + 4 * VCHIQ_MAX_SLOTS_PER_SIDE)
#define SHARED_SIZE             (SHARED_DEBUG + 4 * 11)

/* struct vchiq_slot_zero */
#define ZERO_MAGIC              0x00
#define ZERO_VERSION            0x04    /* and version_min, 16 bits each */
#define ZERO_SLOT_ZERO_SIZE     0x08
#define ZERO_SLOT_SIZE          0x0c
#define ZERO_MAX_SLOTS          0x10
#define ZERO_MAX_SLOTS_PER_SIDE 0x14
#define ZERO_MASTER             0x20
#define ZERO_SLAVE              (ZERO_MASTER + SHARED_SIZE)
#define ZERO_SIZE               (ZERO_SLAVE + SHARED_SIZE + 4 * VCHIQ_MAX_SLOTS)

/* The VideoCore is the master, the ARM the slave */
#define VC(field)               (ZERO_MASTER + SHARED_##field)
#define ARM(field)              (ZERO_SLAVE + SHARED_##field)

/* Message types and ids */
enum {
    MSG_PADDING,
    MSG_CONNECT,
    MSG_OPEN,
    MSG_OPENACK,
    MSG_CLOSE,
    MSG_DATA,
    MSG_BULK_RX,
    MSG_BULK_TX,
    MSG_BULK_RX_DONE,
    MSG_BULK_TX_DONE,
    MSG_PAUSE,
    MSG_RESUME,
    MSG_REMOTE_USE,
    MSG_REMOTE_RELEASE,
    MSG_REMOTE_USE_ACTIVE,
};

#define MSGID(type, src, dst)   ((uint32_t)(type) << 24 | (src) << 12 | (dst))
#define MSGID_TYPE(id)          ((id) >> 24)
#define MSGID_SRC(id)           (((id) >> 12) & 0xfff)
#define MSGID_DST(id)           ((id) & 0xfff)

/* A bulk transfer that was not done */
#define BULK_ACTUAL_ABORTED     -1

/* struct vchiq_open_payload: fourcc, client_id, version, version_min */
#define OPEN_PAYLOAD_SIZE       12

static uint32_t vchiq_stride(uint32_t size)
{
    return ROUND_UP(VCHIQ_HEADER_SIZE + size, 8);
}

static uint32_t vchiq_ld(BCM2835VchiqState *s, uint32_t offset)
{
    return address_space_ldl_le(&s->dma_as, s->channelbase + offset,
                                MEMTXATTRS_UNSPECIFIED, NULL);
}

static void vchiq_st(BCM2835VchiqState *s, uint32_t offset, uint32_t value)
{
    address_space_stl_le(&s->dma_as, s->channelbase + offset, value,
                         MEMTXATTRS_UNSPECIFIED, NULL);
}

static void bcm2835_vchiq_update_irq(BCM2835VchiqState *s)
{
    qemu_set_irq(s->irq, !!(s->bell0 & BELL_ACTIVE));
}

/* Signal an event of the ARM */
static void vchiq_signal(BCM2835VchiqState *s, uint32_t event)
{
    vchiq_st(s, event + EVENT_FIRED, 1);
    /* fired before reading armed, as the ARM arms before reading fired */
    smp_mb();
    if (vchiq_ld(s, event + EVENT_ARMED)) {
        s->bell0 |= BELL_ACTIVE;
        bcm2835_vchiq_update_irq(s);
    }
}

/* The slot index at @pos in the slot queue of a side, checked */
static int vchiq_slot(BCM2835VchiqState *s, uint32_t side, uint32_t pos)
{
    uint32_t queue = (pos / VCHIQ_SLOT_SIZE) % VCHIQ_MAX_SLOTS_PER_SIDE;
    uint32_t slot = vchiq_ld(s, side + SHARED_SLOT_QUEUE + 4 * queue);

    if (slot < vchiq_ld(s, side + SHARED_SLOT_FIRST) ||
        slot > vchiq_ld(s, side + SHARED_SLOT_LAST)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid slot %u in the %s "
                      "queue\n", __func__, slot,
                      side == ZERO_MASTER ? "VideoCore" : "ARM");
        return -1;
    }
    return slot;
}

/* Whether a message of @size fits in the VideoCore slots now */
static bool vchiq_tx_room(BCM2835VchiqState *s, uint32_t size)
{
    uint32_t pos = s->tx_pos;
    uint32_t space = VCHIQ_SLOT_SIZE - (pos & VCHIQ_SLOT_MASK);

    if (vchiq_stride(size) > space) {
        pos += space;
    }
    if (pos & VCHIQ_SLOT_MASK) {
        return true;
    }
    /* the next slot must have been given back by the ARM */
    return (int32_t)(vchiq_ld(s, VC(SLOT_QUEUE_RECYCLE)) * VCHIQ_SLOT_SIZE -
                     pos) > 0;
}

static bool vchiq_send(BCM2835VchiqState *s, uint32_t msgid,
                       const void *data, uint32_t size)
{
    uint32_t stride = vchiq_stride(size);
    uint32_t pos = s->tx_pos;
    uint32_t space = VCHIQ_SLOT_SIZE - (pos & VCHIQ_SLOT_MASK);
    uint32_t header[2];
    hwaddr addr;
    int slot;

    if (stride > VCHIQ_SLOT_SIZE || !vchiq_tx_room(s, size)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: no room for message 0x%08x\n",
                      __func__, msgid);
        return false;
    }
    if (stride > space) {
        /* fill the rest of the slot */
        slot = vchiq_slot(s, ZERO_MASTER, pos);
        if (slot < 0) {
            return false;
        }
        header[0] = cpu_to_le32(MSGID(MSG_PADDING, 0, 0));
        header[1] = cpu_to_le32(space - VCHIQ_HEADER_SIZE);
        dma_memory_write(&s->dma_as, s->channelbase + slot * VCHIQ_SLOT_SIZE +
                         (pos & VCHIQ_SLOT_MASK), header, sizeof(header),
                         MEMTXATTRS_UNSPECIFIED);
        pos += space;
    }

    slot = vchiq_slot(s, ZERO_MASTER, pos);
    if (slot < 0) {
        return false;
    }
    addr = s->channelbase + slot * VCHIQ_SLOT_SIZE + (pos & VCHIQ_SLOT_MASK);
    header[0] = cpu_to_le32(msgid);
    header[1] = cpu_to_le32(size);
    dma_memory_write(&s->dma_as, addr, header, sizeof(header),
                     MEMTXATTRS_UNSPECIFIED);
    dma_memory_write(&s->dma_as, addr + VCHIQ_HEADER_SIZE, data, size,
                     MEMTXATTRS_UNSPECIFIED);
    s->tx_pos = pos + stride;
    trace_bcm2835_vchiq_tx(MSGID_TYPE(msgid), MSGID_SRC(msgid),
                           MSGID_DST(msgid), size);

    /* the message is complete before the ARM sees the new position */
    smp_wmb();
    vchiq_st(s, VC(TX_POS), s->tx_pos);
    vchiq_signal(s, ARM(TRIGGER));
    return true;
}

/* Give an ARM slot back once the model has read all of it */
static void vchiq_recycle(BCM2835VchiqState *s, uint32_t slot)
{
    uint32_t recycle = vchiq_ld(s, ARM(SLOT_QUEUE_RECYCLE));

    vchiq_st(s, ARM(SLOT_QUEUE) + 4 * (recycle % VCHIQ_MAX_SLOTS_PER_SIDE),
             slot);
    /* the queue entry is written before the ARM sees it counted */
    smp_wmb();
    vchiq_st(s, ARM(SLOT_QUEUE_RECYCLE), recycle + 1);
    vchiq_signal(s, ARM(RECYCLE));
}

/* SMEM: vc-sm-cma, the VideoCore shared memory service */

enum {
    SM_ALLOC,
    SM_LOCK,
    SM_UNLOCK,
    SM_UNLOCK_NOANS,
    SM_FREE,
    SM_RESIZE,
    SM_WALK_ALLOC,
    SM_ACTION_CLEAN,
    SM_IMPORT,
    SM_CLIENT_VERSION,
    SM_VC_MEM_REQUEST_REPLY,
    SM_RELEASED,
    SM_VC_MEM_REQUEST,
};

/* trans_id of a message that the VideoCore starts */
#define SM_FROM_VC              0x80000000

static void bcm2835_vchiq_sm_data(BCM2835VchiqState *s, unsigned int svc,
                                  const uint8_t *buf, uint32_t len)
{
    uint32_t type, trans_id, reply[5];
    int i;

    if (len < 8) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: short message\n", __func__);
        return;
    }
    type = ldl_le_p(buf);
    trans_id = ldl_le_p(buf + 4);
    trace_bcm2835_vchiq_sm(type, trans_id);

    switch (type) {
    case SM_CLIENT_VERSION:
        /* the client supports SM_RELEASED; there is no reply */
        break;
    case SM_IMPORT:
        /* type, addr, size, kernel_id, allocator, name */
        if (len < 8 + 20) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: short import\n", __func__);
            return;
        }
        stl_le_p(&reply[0], trans_id);
        stl_le_p(&reply[1], 0);
        for (i = 0; i < BCM2835_VCHIQ_MAX_IMPORTS; i++) {
            BCM2835VchiqImport *imp = &s->import[i];

            if (!imp->handle) {
                do {
                    s->import_handle++;
                } while (!s->import_handle);
                imp->handle = s->import_handle;
                imp->addr = ldl_le_p(buf + 8 + 4);
                imp->size = ldl_le_p(buf + 8 + 8);
                imp->kernel_id = ldl_le_p(buf + 8 + 12);
                stl_le_p(&reply[1], imp->handle);
                trace_bcm2835_vchiq_sm_import(imp->handle, imp->addr,
                                              imp->size);
                break;
            }
        }
        if (i == BCM2835_VCHIQ_MAX_IMPORTS) {
            qemu_log_mask(LOG_UNIMP, "%s: more than %d imports\n", __func__,
                          BCM2835_VCHIQ_MAX_IMPORTS);
        }
        bcm2835_vchiq_send_data(s, svc, reply, 8);
        break;
    case SM_FREE:
        /* res_handle, res_mem; the VideoCore no longer uses the memory */
        if (len < 8 + 4) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: short free\n", __func__);
            return;
        }
        for (i = 0; i < BCM2835_VCHIQ_MAX_IMPORTS; i++) {
            BCM2835VchiqImport *imp = &s->import[i];

            if (imp->handle && imp->handle == ldl_le_p(buf + 8)) {
                stl_le_p(&reply[0], SM_FROM_VC | SM_RELEASED);
                stl_le_p(&reply[1], imp->addr);
                stl_le_p(&reply[2], imp->size);
                stl_le_p(&reply[3], imp->kernel_id);
                stl_le_p(&reply[4], imp->handle);
                memset(imp, 0, sizeof(*imp));
                bcm2835_vchiq_send_data(s, svc, reply, sizeof(reply));
                return;
            }
        }
        qemu_log_mask(LOG_GUEST_ERROR, "%s: free of unknown handle 0x%x\n",
                      __func__, ldl_le_p(buf + 8));
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: message type %u\n", __func__, type);
        break;
    }
}

static const BCM2835VchiqServiceDef bcm2835_vchiq_sm_service = {
    .fourcc = FOURCC('S', 'M', 'E', 'M'),
    .version = 1,
    .version_min = 0,
    .data = bcm2835_vchiq_sm_data,
};

static const BCM2835VchiqServiceDef *const bcm2835_vchiq_services[] = {
    &bcm2835_vchiq_sm_service,
    &bcm2835_vchiq_mmal_service,
};

static const BCM2835VchiqServiceDef *bcm2835_vchiq_service_def(uint32_t fourcc)
{
    for (int i = 0; i < ARRAY_SIZE(bcm2835_vchiq_services); i++) {
        if (bcm2835_vchiq_services[i]->fourcc == fourcc) {
            return bcm2835_vchiq_services[i];
        }
    }
    return NULL;
}

bool bcm2835_vchiq_send_data(BCM2835VchiqState *s, unsigned int svc,
                             const void *buf, uint32_t len)
{
    assert(svc < BCM2835_VCHIQ_MAX_SERVICES && s->service[svc].fourcc);
    assert(len <= BCM2835_VCHIQ_MAX_REPLY);
    return vchiq_send(s, MSGID(MSG_DATA, svc + 1, s->service[svc].arm_port),
                      buf, len);
}

static void bcm2835_vchiq_open(BCM2835VchiqState *s, uint32_t arm_port,
                               const uint8_t *buf, uint32_t size)
{
    const BCM2835VchiqServiceDef *def = NULL;
    uint32_t fourcc = 0;
    int16_t version = 0, version_min = 0;
    uint16_t ack;

    if (size >= OPEN_PAYLOAD_SIZE) {
        fourcc = ldl_le_p(buf);
        version = lduw_le_p(buf + 8);
        version_min = lduw_le_p(buf + 10);
        def = bcm2835_vchiq_service_def(fourcc);
        if (def && (version < def->version_min ||
                    def->version < version_min)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: service %08x version %d "
                          "(min %d) is not supported\n", __func__, fourcc,
                          version, version_min);
            def = NULL;
        }
    }
    for (int i = 0; def && i < BCM2835_VCHIQ_MAX_SERVICES; i++) {
        if (!s->service[i].fourcc) {
            s->service[i].fourcc = fourcc;
            s->service[i].arm_port = arm_port;
            trace_bcm2835_vchiq_open(fourcc, arm_port, i + 1);
            stw_le_p(&ack, def->version);
            vchiq_send(s, MSGID(MSG_OPENACK, i + 1, arm_port), &ack,
                       sizeof(ack));
            return;
        }
    }
    if (def) {
        qemu_log_mask(LOG_UNIMP, "%s: more than %d services\n", __func__,
                      BCM2835_VCHIQ_MAX_SERVICES);
    }
    /* no such service */
    trace_bcm2835_vchiq_open(fourcc, arm_port, 0);
    vchiq_send(s, MSGID(MSG_CLOSE, 0, arm_port), NULL, 0);
}

static int bcm2835_vchiq_find(BCM2835VchiqState *s, uint32_t msgid)
{
    unsigned int svc = MSGID_DST(msgid) - 1;

    if (svc >= BCM2835_VCHIQ_MAX_SERVICES || !s->service[svc].fourcc ||
        s->service[svc].arm_port != MSGID_SRC(msgid)) {
        return -1;
    }
    return svc;
}

static void bcm2835_vchiq_close(BCM2835VchiqState *s, unsigned int svc)
{
    const BCM2835VchiqServiceDef *def =
        bcm2835_vchiq_service_def(s->service[svc].fourcc);

    if (def->close) {
        def->close(s, svc);
    }
    memset(&s->service[svc], 0, sizeof(s->service[svc]));
}

static void bcm2835_vchiq_message(BCM2835VchiqState *s, uint32_t msgid,
                                  const uint8_t *buf, uint32_t size)
{
    uint32_t done[1];
    int svc;

    trace_bcm2835_vchiq_rx(MSGID_TYPE(msgid), MSGID_SRC(msgid),
                           MSGID_DST(msgid), size);
    switch (MSGID_TYPE(msgid)) {
    case MSG_PADDING:
    case MSG_REMOTE_USE:
    case MSG_REMOTE_RELEASE:
    case MSG_REMOTE_USE_ACTIVE:
        /* the VideoCore never sleeps */
        break;
    case MSG_CONNECT:
        vchiq_send(s, MSGID(MSG_CONNECT, 0, 0), NULL, 0);
        break;
    case MSG_OPEN:
        bcm2835_vchiq_open(s, MSGID_SRC(msgid), buf, size);
        break;
    case MSG_CLOSE:
        svc = bcm2835_vchiq_find(s, msgid);
        if (svc >= 0) {
            uint32_t arm_port = s->service[svc].arm_port;

            bcm2835_vchiq_close(s, svc);
            vchiq_send(s, MSGID(MSG_CLOSE, svc + 1, arm_port), NULL, 0);
        }
        break;
    case MSG_DATA:
        svc = bcm2835_vchiq_find(s, msgid);
        if (svc < 0) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: data for closed port %u\n",
                          __func__, MSGID_DST(msgid));
            break;
        }
        bcm2835_vchiq_service_def(s->service[svc].fourcc)->data(s, svc, buf,
                                                                size);
        break;
    case MSG_BULK_RX:
    case MSG_BULK_TX:
        qemu_log_mask(LOG_UNIMP, "%s: bulk transfers are not implemented\n",
                      __func__);
        svc = bcm2835_vchiq_find(s, msgid);
        if (svc >= 0) {
            stl_le_p(&done[0], BULK_ACTUAL_ABORTED);
            vchiq_send(s, MSGID(MSGID_TYPE(msgid) + MSG_BULK_RX_DONE -
                                MSG_BULK_RX, svc + 1,
                                s->service[svc].arm_port),
                       done, sizeof(done));
        }
        break;
    case MSG_PAUSE:
    case MSG_RESUME:
        qemu_log_mask(LOG_UNIMP, "%s: pause and resume are not "
                      "implemented\n", __func__);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid message 0x%08x\n",
                      __func__, msgid);
        break;
    }
}

/* Read the ARM messages and handle them, while there is room for replies */
static void bcm2835_vchiq_process(void *opaque)
{
    BCM2835VchiqState *s = opaque;
    uint8_t buf[VCHIQ_SLOT_SIZE];

    if (!s->channelbase) {
        return;
    }
    vchiq_st(s, VC(TRIGGER) + EVENT_FIRED, 0);
    vchiq_st(s, VC(RECYCLE) + EVENT_FIRED, 0);
    if (vchiq_ld(s, VC(SYNC_TRIGGER) + EVENT_FIRED)) {
        vchiq_st(s, VC(SYNC_TRIGGER) + EVENT_FIRED, 0);
        qemu_log_mask(LOG_UNIMP, "%s: synchronous messages are not "
                      "implemented\n", __func__);
    }
    /* see the events cleared before what the ARM did before firing them */
    smp_mb();

    while (s->rx_pos != vchiq_ld(s, ARM(TX_POS))) {
        uint32_t offset = s->rx_pos & VCHIQ_SLOT_MASK;
        uint32_t header[2], msgid, size;
        int slot = vchiq_slot(s, ZERO_SLAVE, s->rx_pos);
        hwaddr addr;

        if (slot < 0) {
            return;
        }
        addr = s->channelbase + slot * VCHIQ_SLOT_SIZE + offset;
        dma_memory_read(&s->dma_as, addr, header, sizeof(header),
                        MEMTXATTRS_UNSPECIFIED);
        msgid = le32_to_cpu(header[0]);
        size = le32_to_cpu(header[1]);
        if (size > VCHIQ_SLOT_SIZE - VCHIQ_HEADER_SIZE - offset) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: message 0x%08x of %u bytes "
                          "overflows its slot\n", __func__, msgid, size);
            return;
        }
        if (!vchiq_tx_room(s, BCM2835_VCHIQ_MAX_REPLY)) {
            /* the ARM recycles a slot and rings again */
            return;
        }
        dma_memory_read(&s->dma_as, addr + VCHIQ_HEADER_SIZE, buf, size,
                        MEMTXATTRS_UNSPECIFIED);
        bcm2835_vchiq_message(s, msgid, buf, size);

        s->rx_pos += vchiq_stride(size);
        if (!(s->rx_pos & VCHIQ_SLOT_MASK)) {
            vchiq_recycle(s, slot);
        }
    }
}

static void bcm2835_vchiq_reset_queue(BCM2835VchiqState *s)
{
    s->channelbase = 0;
    s->rx_pos = 0;
    s->tx_pos = 0;
    s->import_handle = 0;
    memset(s->service, 0, sizeof(s->service));
    memset(s->import, 0, sizeof(s->import));
    memset(s->component, 0, sizeof(s->component));
}

uint32_t bcm2835_vchiq_init(BCM2835VchiqState *s, uint32_t channelbase)
{
    uint32_t version, first, last, sync, i;
    const char *error = NULL;

    bcm2835_vchiq_reset_queue(s);
    s->channelbase = channelbase;

    version = vchiq_ld(s, ZERO_VERSION);
    if (vchiq_ld(s, ZERO_MAGIC) != VCHIQ_MAGIC) {
        error = "bad magic";
    } else if ((int16_t)version < VCHIQ_VERSION_MIN ||
               (int16_t)(version >> 16) > VCHIQ_VERSION) {
        error = "unsupported version";
    } else if (vchiq_ld(s, ZERO_SLOT_ZERO_SIZE) != ZERO_SIZE ||
               vchiq_ld(s, ZERO_SLOT_SIZE) != VCHIQ_SLOT_SIZE ||
               vchiq_ld(s, ZERO_MAX_SLOTS) != VCHIQ_MAX_SLOTS ||
               vchiq_ld(s, ZERO_MAX_SLOTS_PER_SIDE) !=
               VCHIQ_MAX_SLOTS_PER_SIDE) {
        error = "unsupported slot zero layout";
    } else if (!vchiq_ld(s, ARM(INITIALISED))) {
        error = "the ARM side is not initialised";
    }
    for (int side = ZERO_MASTER; !error && side <= ZERO_SLAVE;
         side += SHARED_SIZE) {
        first = vchiq_ld(s, side + SHARED_SLOT_FIRST);
        last = vchiq_ld(s, side + SHARED_SLOT_LAST);
        sync = vchiq_ld(s, side + SHARED_SLOT_SYNC);
        if (first < 1 || first > last || last >= VCHIQ_MAX_SLOTS ||
            last - first >= VCHIQ_MAX_SLOTS_PER_SIDE ||
            sync < 1 || sync >= VCHIQ_MAX_SLOTS) {
            error = "invalid slots";
        }
    }
    trace_bcm2835_vchiq_init(channelbase, error ? error : "ok");
    if (error) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: 0x%08x: %s\n", __func__,
                      channelbase, error);
        s->channelbase = 0;
        return 1;
    }

    /* Our side: all our slots are free, our events are armed */
    first = vchiq_ld(s, VC(SLOT_FIRST));
    last = vchiq_ld(s, VC(SLOT_LAST));
    for (i = 0; i <= last - first; i++) {
        vchiq_st(s, VC(SLOT_QUEUE) + 4 * i, first + i);
    }
    vchiq_st(s, VC(SLOT_QUEUE_RECYCLE), i);
    vchiq_st(s, VC(TX_POS), 0);
    vchiq_st(s, VC(TRIGGER) + EVENT_ARMED, 1);
    vchiq_st(s, VC(RECYCLE) + EVENT_ARMED, 1);
    vchiq_st(s, VC(SYNC_TRIGGER) + EVENT_ARMED, 1);
    /* the synchronous slot is empty */
    vchiq_st(s, vchiq_ld(s, VC(SLOT_SYNC)) * VCHIQ_SLOT_SIZE,
             MSGID(MSG_PADDING, 0, 0));
    /* our side is complete before the ARM sees it initialised */
    smp_wmb();
    vchiq_st(s, VC(INITIALISED), 1);
    return 0;
}

static uint64_t bcm2835_vchiq_read(void *opaque, hwaddr addr, unsigned size)
{
    BCM2835VchiqState *s = opaque;
    uint32_t value = 0;

    switch (addr) {
    case A_BELL0:
        value = s->bell0;
        s->bell0 = 0;
        bcm2835_vchiq_update_irq(s);
        break;
    case A_BELL1:
    case A_BELL2:
    case A_BELL3:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
    return value;
}

static void bcm2835_vchiq_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    BCM2835VchiqState *s = opaque;

    switch (addr) {
    case A_BELL2:
        qemu_bh_schedule(s->bh);
        break;
    case A_BELL0:
    case A_BELL1:
    case A_BELL3:
        /* nobody listens */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps bcm2835_vchiq_ops = {
    .read = bcm2835_vchiq_read,
    .write = bcm2835_vchiq_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void bcm2835_vchiq_reset_hold(Object *obj, ResetType type)
{
    BCM2835VchiqState *s = BCM2835_VCHIQ(obj);

    s->bell0 = 0;
    bcm2835_vchiq_reset_queue(s);
}

static void bcm2835_vchiq_reset_exit(Object *obj, ResetType type)
{
    BCM2835VchiqState *s = BCM2835_VCHIQ(obj);

    bcm2835_vchiq_update_irq(s);
}

static void bcm2835_vchiq_init_obj(Object *obj)
{
    BCM2835VchiqState *s = BCM2835_VCHIQ(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2835_vchiq_ops, s,
                          TYPE_BCM2835_VCHIQ, BCM2835_VCHIQ_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static void bcm2835_vchiq_realize(DeviceState *dev, Error **errp)
{
    BCM2835VchiqState *s = BCM2835_VCHIQ(dev);

    if (!s->dma_mr) {
        error_setg(errp, "%s: 'dma-mr' link not set", TYPE_BCM2835_VCHIQ);
        return;
    }
    address_space_init(&s->dma_as, s->dma_mr, TYPE_BCM2835_VCHIQ "-memory");
    s->bh = qemu_bh_new_guarded(bcm2835_vchiq_process, s,
                                &dev->mem_reentrancy_guard);
}

static void bcm2835_vchiq_unrealize(DeviceState *dev)
{
    BCM2835VchiqState *s = BCM2835_VCHIQ(dev);

    qemu_bh_delete(s->bh);
    address_space_destroy(&s->dma_as);
}

static int bcm2835_vchiq_post_load(void *opaque, int version_id)
{
    BCM2835VchiqState *s = opaque;

    for (int i = 0; i < BCM2835_VCHIQ_MAX_SERVICES; i++) {
        if (s->service[i].fourcc &&
            !bcm2835_vchiq_service_def(s->service[i].fourcc)) {
            return -EINVAL;
        }
    }
    /* messages may have arrived while the BH was pending */
    if (s->channelbase) {
        qemu_bh_schedule(s->bh);
    }
    return 0;
}

static const VMStateDescription vmstate_bcm2835_vchiq_service = {
    .name = TYPE_BCM2835_VCHIQ "/service",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(fourcc, BCM2835VchiqService),
        VMSTATE_UINT32(arm_port, BCM2835VchiqService),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_bcm2835_vchiq_import = {
    .name = TYPE_BCM2835_VCHIQ "/import",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(handle, BCM2835VchiqImport),
        VMSTATE_UINT32(addr, BCM2835VchiqImport),
        VMSTATE_UINT32(size, BCM2835VchiqImport),
        VMSTATE_UINT32(kernel_id, BCM2835VchiqImport),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_bcm2835_mmal_port = {
    .name = TYPE_BCM2835_VCHIQ "/mmal-port",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(buffer_num, BCM2835MmalPort),
        VMSTATE_UINT32(buffer_size, BCM2835MmalPort),
        VMSTATE_UINT32(es_type, BCM2835MmalPort),
        VMSTATE_UINT32(encoding, BCM2835MmalPort),
        VMSTATE_UINT32(encoding_variant, BCM2835MmalPort),
        VMSTATE_UINT32(bitrate, BCM2835MmalPort),
        VMSTATE_UINT32(flags, BCM2835MmalPort),
        VMSTATE_UINT32_ARRAY(es, BCM2835MmalPort, BCM2835_MMAL_ES_WORDS),
        VMSTATE_UINT32(extradata_size, BCM2835MmalPort),
        VMSTATE_UINT8_ARRAY(extradata, BCM2835MmalPort,
                            BCM2835_MMAL_EXTRADATA_SIZE),
        VMSTATE_INT32_ARRAY(crop, BCM2835MmalPort, 4),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_bcm2835_mmal_component = {
    .name = TYPE_BCM2835_VCHIQ "/mmal-component",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(def, BCM2835MmalComponent),
        VMSTATE_UINT32(service, BCM2835MmalComponent),
        VMSTATE_UINT32(enabled, BCM2835MmalComponent),
        VMSTATE_STRUCT_ARRAY(port, BCM2835MmalComponent,
                             BCM2835_MMAL_MAX_PORTS, 1,
                             vmstate_bcm2835_mmal_port, BCM2835MmalPort),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_bcm2835_vchiq = {
    .name = TYPE_BCM2835_VCHIQ,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = bcm2835_vchiq_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(bell0, BCM2835VchiqState),
        VMSTATE_UINT32(channelbase, BCM2835VchiqState),
        VMSTATE_UINT32(rx_pos, BCM2835VchiqState),
        VMSTATE_UINT32(tx_pos, BCM2835VchiqState),
        VMSTATE_UINT32(import_handle, BCM2835VchiqState),
        VMSTATE_STRUCT_ARRAY(service, BCM2835VchiqState,
                             BCM2835_VCHIQ_MAX_SERVICES, 1,
                             vmstate_bcm2835_vchiq_service,
                             BCM2835VchiqService),
        VMSTATE_STRUCT_ARRAY(import, BCM2835VchiqState,
                             BCM2835_VCHIQ_MAX_IMPORTS, 1,
                             vmstate_bcm2835_vchiq_import,
                             BCM2835VchiqImport),
        VMSTATE_STRUCT_ARRAY(component, BCM2835VchiqState,
                             BCM2835_MMAL_MAX_COMPONENTS, 1,
                             vmstate_bcm2835_mmal_component,
                             BCM2835MmalComponent),
        VMSTATE_END_OF_LIST()
    }
};

static const Property bcm2835_vchiq_props[] = {
    DEFINE_PROP_LINK("dma-mr", BCM2835VchiqState, dma_mr, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

static void bcm2835_vchiq_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = bcm2835_vchiq_realize;
    dc->unrealize = bcm2835_vchiq_unrealize;
    dc->vmsd = &vmstate_bcm2835_vchiq;
    device_class_set_props(dc, bcm2835_vchiq_props);
    rc->phases.hold = bcm2835_vchiq_reset_hold;
    rc->phases.exit = bcm2835_vchiq_reset_exit;
}

static const TypeInfo bcm2835_vchiq_types[] = {
    {
        .name = TYPE_BCM2835_VCHIQ,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(BCM2835VchiqState),
        .instance_init = bcm2835_vchiq_init_obj,
        .class_init = bcm2835_vchiq_class_init,
    },
};

DEFINE_TYPES(bcm2835_vchiq_types)
