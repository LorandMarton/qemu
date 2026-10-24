/*
 * QTest testcase for the BCM2835 VCHIQ (VideoCore side)
 *
 * The test is the ARM side as Linux 6.6 implements it: it sets up slot
 * zero and its slots, hands them over with RPI_FIRMWARE_VCHIQ_INIT, and
 * talks to the services of the model through the slots and doorbells.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define MBOX_BASE           0xfe00b880
#define MBOX0_READ          0x00
#define MBOX1_WRITE         0x20
#define MBOX_CHAN_PROPERTY  8
#define TAG_VCHIQ_INIT      0x00048010

#define VCHIQ_BASE          0xfe00b840
#define BELL0               0x00
#define BELL2               0x08
#define BELL_ACTIVE         (1u << 2)
#define VCHIQ_PATH          "/machine/soc/peripherals/vchiq"

#define BUF_ADDR            0x10000
#define SLOTS               0x200000
/* the bus address Linux gives: the RAM at 0xc0000000 of the VideoCore */
#define CHANNELBASE         (0xc0000000 | SLOTS)

#define SLOT_SIZE           4096
#define FOURCC(a, b, c, d)  ((uint32_t)(a) << 24 | (b) << 16 | (c) << 8 | (d))

/* struct vchiq_slot_zero and struct vchiq_shared_state */
#define SHARED_SIZE         372
#define MASTER              0x20
#define SLAVE               (MASTER + SHARED_SIZE)
#define INITIALISED         0x00
#define SLOT_FIRST          0x04
#define SLOT_LAST           0x08
#define SLOT_SYNC           0x0c
#define TRIGGER             0x10
#define TX_POS              0x1c
#define RECYCLE             0x20
#define SLOT_QUEUE_RECYCLE  0x2c
#define SLOT_QUEUE          0x48
#define ARMED               0
#define FIRED               4

/* 64 slots after slot zero: half for each side, the first one synchronous */
#define VC_FIRST            2
#define VC_LAST             32
#define ARM_FIRST           34
#define ARM_LAST            64
#define NUM_SLOTS           31

enum {
    MSG_PADDING, MSG_CONNECT, MSG_OPEN, MSG_OPENACK, MSG_CLOSE, MSG_DATA,
    MSG_BULK_RX, MSG_BULK_TX, MSG_BULK_RX_DONE, MSG_BULK_TX_DONE,
};

#define MSGID(type, src, dst)   ((uint32_t)(type) << 24 | (src) << 12 | (dst))

/* MMAL */
#define MMAL_MAGIC          0x6c616d6d
#define MMAL_COMPONENT_CREATE   4
#define MMAL_COMPONENT_DESTROY  5
#define MMAL_PORT_INFO_GET      8
#define MMAL_PORT_INFO_SET      9
#define MMAL_PORT_ACTION        10
#define MMAL_PARAMETER_GET      15
#define MMAL_SUCCESS        0
#define MMAL_ENOSPC         2
#define MMAL_EINVAL         3
#define MMAL_ENOSYS         4
#define MMAL_PORT_OUTPUT    3
#define MMAL_PARAMETER_SUPPORTED_ENCODINGS 1

/* SMEM */
#define SM_FREE             4
#define SM_IMPORT           8
#define SM_CLIENT_VERSION   9
#define SM_RELEASED         11

typedef struct Arm {
    QTestState *qts;
    uint32_t tx_pos;            /* our position */
    uint32_t rx_pos;            /* position in the VideoCore stream */
    uint32_t context;
    uint32_t held[NUM_SLOTS];   /* VideoCore slots we did not give back */
    uint32_t num_held;
} Arm;

static uint32_t rd(Arm *a, uint32_t offset)
{
    return qtest_readl(a->qts, SLOTS + offset);
}

static void wr(Arm *a, uint32_t offset, uint32_t value)
{
    qtest_writel(a->qts, SLOTS + offset, value);
}

static uint32_t vchiq_init(QTestState *qts, uint32_t channelbase)
{
    qtest_writel(qts, BUF_ADDR, 4 * 7);
    qtest_writel(qts, BUF_ADDR + 4, 0);
    qtest_writel(qts, BUF_ADDR + 8, TAG_VCHIQ_INIT);
    qtest_writel(qts, BUF_ADDR + 12, 4);
    qtest_writel(qts, BUF_ADDR + 16, 0);
    qtest_writel(qts, BUF_ADDR + 20, channelbase);
    qtest_writel(qts, BUF_ADDR + 24, 0);
    qtest_writel(qts, MBOX_BASE + MBOX1_WRITE, BUF_ADDR | MBOX_CHAN_PROPERTY);
    g_assert_cmphex(qtest_readl(qts, MBOX_BASE + MBOX0_READ), ==,
                    BUF_ADDR | MBOX_CHAN_PROPERTY);
    return qtest_readl(qts, BUF_ADDR + 20);
}

/* vchiq_init_slots() and the slave part of vchiq_init_state() */
static void arm_init(Arm *a, QTestState *qts)
{
    memset(a, 0, sizeof(*a));
    a->qts = qts;
    qtest_memset(qts, SLOTS, 0, 65 * SLOT_SIZE);
    wr(a, 0x00, FOURCC('V', 'C', 'H', 'I'));
    wr(a, 0x04, 3 << 16 | 8);                   /* version 8, minimum 3 */
    wr(a, 0x08, MASTER + 2 * SHARED_SIZE + 4 * 128);
    wr(a, 0x0c, SLOT_SIZE);
    wr(a, 0x10, 128);
    wr(a, 0x14, 64);
    wr(a, MASTER + SLOT_SYNC, VC_FIRST - 1);
    wr(a, MASTER + SLOT_FIRST, VC_FIRST);
    wr(a, MASTER + SLOT_LAST, VC_LAST);
    wr(a, SLAVE + SLOT_SYNC, ARM_FIRST - 1);
    wr(a, SLAVE + SLOT_FIRST, ARM_FIRST);
    wr(a, SLAVE + SLOT_LAST, ARM_LAST);
    for (int i = 0; i < NUM_SLOTS; i++) {
        wr(a, SLAVE + SLOT_QUEUE + 4 * i, ARM_FIRST + i);
    }
    wr(a, SLAVE + SLOT_QUEUE_RECYCLE, NUM_SLOTS);
    wr(a, SLAVE + TRIGGER + ARMED, 1);
    wr(a, SLAVE + RECYCLE + ARMED, 1);
    wr(a, SLAVE + INITIALISED, 1);
}

static void ring(Arm *a, uint32_t event)
{
    wr(a, MASTER + event + FIRED, 1);
    if (rd(a, MASTER + event + ARMED)) {
        qtest_writel(a->qts, VCHIQ_BASE + BELL2, 0);
    }
}

/* Queue a message, as queue_message() does */
static void arm_send(Arm *a, uint32_t msgid, const void *data, uint32_t size)
{
    uint32_t stride = ROUND_UP(8 + size, 8);
    uint32_t space = SLOT_SIZE - (a->tx_pos % SLOT_SIZE);
    uint32_t slot;
    uint64_t addr;

    if (stride > space) {
        slot = rd(a, SLAVE + SLOT_QUEUE + 4 * (a->tx_pos / SLOT_SIZE % 64));
        addr = SLOTS + slot * SLOT_SIZE + a->tx_pos % SLOT_SIZE;
        qtest_writel(a->qts, addr, MSGID(MSG_PADDING, 0, 0));
        qtest_writel(a->qts, addr + 4, space - 8);
        a->tx_pos += space;
    }
    if (a->tx_pos % SLOT_SIZE == 0) {
        /* the VideoCore gave the next slot back */
        g_assert_cmpint((int32_t)(rd(a, SLAVE + SLOT_QUEUE_RECYCLE) *
                                  SLOT_SIZE - a->tx_pos), >, 0);
    }
    slot = rd(a, SLAVE + SLOT_QUEUE + 4 * (a->tx_pos / SLOT_SIZE % 64));
    addr = SLOTS + slot * SLOT_SIZE + a->tx_pos % SLOT_SIZE;
    qtest_writel(a->qts, addr, msgid);
    qtest_writel(a->qts, addr + 4, size);
    if (size) {
        qtest_memwrite(a->qts, addr + 8, data, size);
    }
    a->tx_pos += stride;
    wr(a, SLAVE + TX_POS, a->tx_pos);
    ring(a, TRIGGER);
}

static bool vc_has_message(Arm *a)
{
    /* each qtest command lets the main loop run the model's bottom half */
    for (int i = 0; i < 100; i++) {
        if (rd(a, MASTER + TX_POS) != a->rx_pos) {
            return true;
        }
    }
    return false;
}

/* Give a VideoCore slot back, as release_slot() does */
static void arm_recycle(Arm *a, uint32_t slot)
{
    uint32_t recycle = rd(a, MASTER + SLOT_QUEUE_RECYCLE);

    wr(a, MASTER + SLOT_QUEUE + 4 * (recycle % 64), slot);
    wr(a, MASTER + SLOT_QUEUE_RECYCLE, recycle + 1);
    ring(a, RECYCLE);
}

/* The next VideoCore message; its slot goes back once it is all read */
static uint32_t arm_recv_hold(Arm *a, uint32_t *msgid, void *data,
                              uint32_t max, bool give_back)
{
    uint32_t slot, size;
    uint64_t addr;

    do {
        g_assert_true(vc_has_message(a));
        slot = rd(a, MASTER + SLOT_QUEUE + 4 * (a->rx_pos / SLOT_SIZE % 64));
        g_assert_cmpuint(slot, >=, VC_FIRST);
        g_assert_cmpuint(slot, <=, VC_LAST);
        addr = SLOTS + slot * SLOT_SIZE + a->rx_pos % SLOT_SIZE;
        *msgid = qtest_readl(a->qts, addr);
        size = qtest_readl(a->qts, addr + 4);
        g_assert_cmpuint(a->rx_pos % SLOT_SIZE + ROUND_UP(8 + size, 8), <=,
                         SLOT_SIZE);
        g_assert_cmpuint(size, <=, max);
        if (size) {
            qtest_memread(a->qts, addr + 8, data, size);
        }
        a->rx_pos += ROUND_UP(8 + size, 8);
        if (a->rx_pos % SLOT_SIZE == 0) {
            if (give_back) {
                arm_recycle(a, slot);
            } else {
                g_assert_cmpuint(a->num_held, <, NUM_SLOTS);
                a->held[a->num_held++] = slot;
            }
        }
    } while (*msgid == MSGID(MSG_PADDING, 0, 0));
    return size;
}

static uint32_t arm_recv(Arm *a, uint32_t *msgid, void *data, uint32_t max)
{
    return arm_recv_hold(a, msgid, data, max, true);
}

static void arm_connect(Arm *a)
{
    uint32_t msgid;

    arm_send(a, MSGID(MSG_CONNECT, 0, 0), NULL, 0);
    g_assert_cmpuint(arm_recv(a, &msgid, NULL, 0), ==, 0);
    g_assert_cmphex(msgid, ==, MSGID(MSG_CONNECT, 0, 0));
}

/* Open a service from our port; returns the VideoCore port, 0 if closed */
static uint32_t arm_open(Arm *a, uint32_t fourcc, uint32_t port,
                         uint16_t version, uint16_t version_min)
{
    uint32_t open[3] = {
        cpu_to_le32(fourcc), 0, cpu_to_le32(version_min << 16 | version)
    };
    uint32_t msgid, size;
    uint16_t ack;

    arm_send(a, MSGID(MSG_OPEN, port, 0), open, sizeof(open));
    size = arm_recv(a, &msgid, &ack, sizeof(ack));
    if (msgid >> 24 == MSG_CLOSE) {
        g_assert_cmphex(msgid, ==, MSGID(MSG_CLOSE, 0, port));
        return 0;
    }
    g_assert_cmpuint(msgid >> 24, ==, MSG_OPENACK);
    g_assert_cmpuint(msgid & 0xfff, ==, port);
    g_assert_cmpuint(size, ==, 2);
    g_assert_cmpuint(le16_to_cpu(ack), ==, version);
    return (msgid >> 12) & 0xfff;
}

static QTestState *vchiq_start(Arm *a)
{
    QTestState *qts = qtest_init("-M raspi4b");

    arm_init(a, qts);
    g_assert_cmphex(vchiq_init(qts, CHANNELBASE), ==, 0);
    return qts;
}

static void test_init(void)
{
    QTestState *qts = qtest_init("-M raspi4b");
    Arm a;

    arm_init(&a, qts);
    /* no slot zero there */
    g_assert_cmphex(vchiq_init(qts, 0xc0000000 | 0x300000), !=, 0);
    g_assert_cmpuint(rd(&a, MASTER + INITIALISED), ==, 0);

    g_assert_cmphex(vchiq_init(qts, CHANNELBASE), ==, 0);
    g_assert_cmpuint(rd(&a, MASTER + INITIALISED), ==, 1);
    /* our slots are all free, our events armed */
    for (int i = 0; i < NUM_SLOTS; i++) {
        g_assert_cmpuint(rd(&a, MASTER + SLOT_QUEUE + 4 * i), ==,
                         VC_FIRST + i);
    }
    g_assert_cmpuint(rd(&a, MASTER + SLOT_QUEUE_RECYCLE), ==, NUM_SLOTS);
    g_assert_cmpuint(rd(&a, MASTER + TX_POS), ==, 0);
    g_assert_cmpuint(rd(&a, MASTER + TRIGGER + ARMED), ==, 1);
    g_assert_cmpuint(rd(&a, MASTER + RECYCLE + ARMED), ==, 1);
    qtest_quit(qts);
}

static void test_connect(void)
{
    Arm a;
    QTestState *qts = vchiq_start(&a);

    qtest_irq_intercept_out_named(qts, VCHIQ_PATH, "sysbus-irq");
    g_assert_false(qtest_get_irq(qts, 0));
    arm_connect(&a);
    /* the model rang our doorbell for the trigger event */
    g_assert_cmpuint(rd(&a, SLAVE + TRIGGER + FIRED), ==, 1);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(qtest_readl(qts, VCHIQ_BASE + BELL0), ==, BELL_ACTIVE);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(qtest_readl(qts, VCHIQ_BASE + BELL0), ==, 0);

    /* not armed: fired, no doorbell */
    wr(&a, SLAVE + TRIGGER + ARMED, 0);
    wr(&a, SLAVE + TRIGGER + FIRED, 0);
    arm_connect(&a);
    g_assert_cmpuint(rd(&a, SLAVE + TRIGGER + FIRED), ==, 1);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

static void test_services(void)
{
    Arm a;
    QTestState *qts = vchiq_start(&a);
    uint32_t mmal, msgid;

    arm_connect(&a);
    /* a service the firmware does not have, a version it does not support */
    g_assert_cmpuint(arm_open(&a, FOURCC('X', 'X', 'X', 'X'), 3, 1, 0), ==, 0);
    g_assert_cmpuint(arm_open(&a, FOURCC('m', 'm', 'a', 'l'), 4, 20, 16),
                     ==, 0);

    mmal = arm_open(&a, FOURCC('m', 'm', 'a', 'l'), 5, 15, 10);
    g_assert_cmpuint(mmal, !=, 0);
    g_assert_cmpuint(arm_open(&a, FOURCC('S', 'M', 'E', 'M'), 6, 1, 0), !=,
                     mmal);

    /* closing: the model closes its end too */
    arm_send(&a, MSGID(MSG_CLOSE, 5, mmal), NULL, 0);
    g_assert_cmpuint(arm_recv(&a, &msgid, NULL, 0), ==, 0);
    g_assert_cmphex(msgid, ==, MSGID(MSG_CLOSE, mmal, 5));
    /* its port is free again */
    g_assert_cmpuint(arm_open(&a, FOURCC('m', 'm', 'a', 'l'), 7, 15, 10), ==,
                     mmal);
    qtest_quit(qts);
}

/* An MMAL call; returns the reply payload size */
static uint32_t mmal_call(Arm *a, uint32_t port, uint32_t vc_port,
                          uint32_t type, const uint32_t *in, uint32_t in_len,
                          uint32_t *out, uint32_t out_max)
{
    uint32_t msg[128] = {
        cpu_to_le32(MMAL_MAGIC), cpu_to_le32(type), 0,
        cpu_to_le32(++a->context), 0, 0,
    };
    uint32_t reply[128], msgid, size;

    memcpy(msg + 6, in, in_len);
    arm_send(a, MSGID(MSG_DATA, port, vc_port), msg, 24 + in_len);
    size = arm_recv(a, &msgid, reply, sizeof(reply));
    g_assert_cmphex(msgid, ==, MSGID(MSG_DATA, vc_port, port));
    g_assert_cmpuint(size, >=, 24 + 4);
    g_assert_cmphex(le32_to_cpu(reply[0]), ==, MMAL_MAGIC);
    g_assert_cmpuint(le32_to_cpu(reply[1]), ==, type);
    g_assert_cmpuint(le32_to_cpu(reply[3]), ==, a->context);
    g_assert_cmpuint(size - 24, <=, out_max);
    memcpy(out, reply + 6, size - 24);
    return size - 24;
}

static void test_mmal(void)
{
    Arm a;
    QTestState *qts = vchiq_start(&a);
    uint32_t in[70] = { 0 }, out[128];
    uint32_t vc, handle;

    arm_connect(&a);
    vc = arm_open(&a, FOURCC('m', 'm', 'a', 'l'), 5, 15, 10);

    /* a component the firmware does not have */
    strcpy((char *)&in[1], "ril.nothing");
    mmal_call(&a, 5, vc, MMAL_COMPONENT_CREATE, in, 4 + 128 + 4, out,
              sizeof(out));
    g_assert_cmpuint(le32_to_cpu(out[0]), ==, MMAL_ENOSYS);

    /* ril.isp: one input, three outputs, no clock */
    memset(in, 0, sizeof(in));
    strcpy((char *)&in[1], "ril.isp");
    g_assert_cmpuint(mmal_call(&a, 5, vc, MMAL_COMPONENT_CREATE, in,
                               4 + 128 + 4, out, sizeof(out)), ==, 20);
    g_assert_cmpuint(le32_to_cpu(out[0]), ==, MMAL_SUCCESS);
    handle = le32_to_cpu(out[1]);
    g_assert_cmpuint(handle, !=, 0);
    g_assert_cmpuint(le32_to_cpu(out[2]), ==, 1);
    g_assert_cmpuint(le32_to_cpu(out[3]), ==, 3);
    g_assert_cmpuint(le32_to_cpu(out[4]), ==, 0);

    /* the statistics output: its port and buffer size */
    in[0] = cpu_to_le32(handle);
    in[1] = cpu_to_le32(MMAL_PORT_OUTPUT);
    in[2] = cpu_to_le32(2);
    mmal_call(&a, 5, vc, MMAL_PORT_INFO_GET, in, 12, out, sizeof(out));
    g_assert_cmpuint(le32_to_cpu(out[0]), ==, MMAL_SUCCESS);
    g_assert_cmpuint(le32_to_cpu(out[5]), ==, 4);          /* port handle */
    g_assert_cmpuint(le32_to_cpu(out[6 + 2]), ==, MMAL_PORT_OUTPUT);
    g_assert_cmphex(le32_to_cpu(out[6 + 3]), ==, 4 << 16 | 2);
    g_assert_cmpuint(le32_to_cpu(out[6 + 7]), ==, 10824);  /* size min */
    g_assert_cmphex(le32_to_cpu(out[6 + 16 + 1]), ==, 0x54415453); /* STAT */

    /* its encodings, then the input's, which need more room than given */
    in[0] = cpu_to_le32(handle);
    in[1] = cpu_to_le32(4);
    in[2] = cpu_to_le32(MMAL_PARAMETER_SUPPORTED_ENCODINGS);
    in[3] = cpu_to_le32(8 + 4 * 70);
    mmal_call(&a, 5, vc, MMAL_PARAMETER_GET, in, 16, out, sizeof(out));
    g_assert_cmpuint(le32_to_cpu(out[0]), ==, MMAL_SUCCESS);
    g_assert_cmpuint(le32_to_cpu(out[2]), ==, 8 + 4);
    g_assert_cmphex(le32_to_cpu(out[3]), ==, 0x54415453);
    in[1] = cpu_to_le32(1);
    in[3] = cpu_to_le32(8 + 4 * 2);
    mmal_call(&a, 5, vc, MMAL_PARAMETER_GET, in, 16, out, sizeof(out));
    g_assert_cmpuint(le32_to_cpu(out[0]), ==, MMAL_ENOSPC);
    g_assert_cmpuint(le32_to_cpu(out[2]), ==, 8 + 4 * 54);
    g_assert_cmphex(le32_to_cpu(out[3]), ==, 0x31384142);  /* BA81 */

    /* a format the port does not have */
    memset(in, 0, sizeof(in));
    in[0] = cpu_to_le32(handle);
    in[1] = cpu_to_le32(MMAL_PORT_OUTPUT);
    in[2] = cpu_to_le32(0);
    in[3 + 16 + 1] = cpu_to_le32(0x34363248);              /* H264 */
    mmal_call(&a, 5, vc, MMAL_PORT_INFO_SET, in, 12 + 268, out, sizeof(out));
    g_assert_cmpuint(le32_to_cpu(out[0]), ==, MMAL_EINVAL);
    /* one it has: the port keeps it */
    in[3 + 16 + 1] = cpu_to_le32(0x3231564e);              /* NV12 */
    in[3 + 16 + 8] = cpu_to_le32(640);
    in[3 + 16 + 8 + 1] = cpu_to_le32(480);
    mmal_call(&a, 5, vc, MMAL_PORT_INFO_SET, in, 12 + 268, out, sizeof(out));
    g_assert_cmpuint(le32_to_cpu(out[0]), ==, MMAL_SUCCESS);
    mmal_call(&a, 5, vc, MMAL_PORT_INFO_GET, in, 12, out, sizeof(out));
    g_assert_cmphex(le32_to_cpu(out[6 + 16 + 1]), ==, 0x3231564e);
    g_assert_cmpuint(le32_to_cpu(out[6 + 16 + 8]), ==, 640);
    g_assert_cmpuint(le32_to_cpu(out[6 + 16 + 8 + 1]), ==, 480);

    /* buffers are not processed: a port cannot be enabled */
    in[0] = cpu_to_le32(handle);
    in[1] = cpu_to_le32(2);
    in[2] = cpu_to_le32(1);
    mmal_call(&a, 5, vc, MMAL_PORT_ACTION, in, 12 + 64, out, sizeof(out));
    g_assert_cmpuint(le32_to_cpu(out[0]), ==, MMAL_ENOSYS);

    in[0] = cpu_to_le32(handle);
    mmal_call(&a, 5, vc, MMAL_COMPONENT_DESTROY, in, 4, out, sizeof(out));
    g_assert_cmpuint(le32_to_cpu(out[0]), ==, MMAL_SUCCESS);
    mmal_call(&a, 5, vc, MMAL_COMPONENT_DESTROY, in, 4, out, sizeof(out));
    g_assert_cmpuint(le32_to_cpu(out[0]), ==, MMAL_EINVAL);
    qtest_quit(qts);
}

static void test_smem(void)
{
    Arm a;
    QTestState *qts = vchiq_start(&a);
    uint32_t msg[16], reply[8], msgid, vc;

    arm_connect(&a);
    vc = arm_open(&a, FOURCC('S', 'M', 'E', 'M'), 9, 1, 0);

    /* the client version has no reply */
    msg[0] = cpu_to_le32(SM_CLIENT_VERSION);
    msg[1] = cpu_to_le32(1);
    msg[2] = cpu_to_le32(2);
    arm_send(&a, MSGID(MSG_DATA, 9, vc), msg, 12);

    /* type, addr, size, kernel_id, allocator, name[32] */
    memset(msg, 0, sizeof(msg));
    msg[0] = cpu_to_le32(SM_IMPORT);
    msg[1] = cpu_to_le32(2);
    msg[3] = cpu_to_le32(0xc1000000);
    msg[4] = cpu_to_le32(0x10000);
    msg[5] = cpu_to_le32(77);
    arm_send(&a, MSGID(MSG_DATA, 9, vc), msg, 8 + 20 + 32);
    g_assert_cmpuint(arm_recv(&a, &msgid, reply, sizeof(reply)), ==, 8);
    g_assert_cmphex(msgid, ==, MSGID(MSG_DATA, vc, 9));
    g_assert_cmpuint(le32_to_cpu(reply[0]), ==, 2);       /* trans_id */
    g_assert_cmpuint(le32_to_cpu(reply[1]), !=, 0);

    /* freed: the VideoCore tells it no longer uses the memory */
    msg[0] = cpu_to_le32(SM_FREE);
    msg[1] = cpu_to_le32(3);
    msg[2] = reply[1];
    msg[3] = 0;
    arm_send(&a, MSGID(MSG_DATA, 9, vc), msg, 16);
    g_assert_cmpuint(arm_recv(&a, &msgid, reply + 2, 20), ==, 20);
    g_assert_cmphex(le32_to_cpu(reply[2]), ==, 0x80000000 | SM_RELEASED);
    g_assert_cmphex(le32_to_cpu(reply[3]), ==, 0xc1000000);
    g_assert_cmpuint(le32_to_cpu(reply[4]), ==, 0x10000);
    g_assert_cmpuint(le32_to_cpu(reply[5]), ==, 77);
    g_assert_cmpuint(le32_to_cpu(reply[6]), ==, le32_to_cpu(reply[1]));
    qtest_quit(qts);
}

static void test_slots(void)
{
    Arm a;
    QTestState *qts = vchiq_start(&a);
    uint32_t in[37] = { 0 }, reply[128], msgid, vc, size, recycled, full;
    uint8_t data[56] = { 0 };

    /* Our slots come back once read: 40 slots of messages, round the queue */
    arm_connect(&a);
    for (int i = 0; i < 40 * SLOT_SIZE / 64; i++) {
        arm_send(&a, MSGID(MSG_CONNECT, 0, 0), data, sizeof(data));
        arm_recv(&a, &msgid, NULL, 0);
    }
    full = a.tx_pos / SLOT_SIZE;
    g_assert_cmpuint(full, >=, 40);
    g_assert_cmpuint(rd(&a, SLAVE + SLOT_QUEUE_RECYCLE), ==, NUM_SLOTS + full);
    /* the last one we filled went back last */
    g_assert_cmpuint(rd(&a, SLAVE + SLOT_QUEUE +
                        4 * ((NUM_SLOTS + full - 1) % 64)), ==,
                     rd(&a, SLAVE + SLOT_QUEUE + 4 * ((full - 1) % 64)));
    g_assert_cmpuint(rd(&a, SLAVE + RECYCLE + FIRED), ==, 1);

    /*
     * The model's slots, filled with port information: while we hold
     * them, it reads no more of our messages; it goes on once we give one
     * back
     */
    vc = arm_open(&a, FOURCC('m', 'm', 'a', 'l'), 5, 15, 10);
    strcpy((char *)&in[1], "ril.isp");
    mmal_call(&a, 5, vc, MMAL_COMPONENT_CREATE, in, 4 + 128 + 4, reply,
              sizeof(reply));
    memset(in, 0, sizeof(in));
    in[0] = cpu_to_le32(MMAL_MAGIC);
    in[1] = cpu_to_le32(MMAL_PORT_INFO_GET);
    in[6] = reply[1];
    in[7] = cpu_to_le32(MMAL_PORT_OUTPUT);
    for (;;) {
        arm_send(&a, MSGID(MSG_DATA, 5, vc), in, 24 + 12);
        if (!vc_has_message(&a)) {
            break;
        }
        size = arm_recv_hold(&a, &msgid, reply, sizeof(reply), false);
        g_assert_cmpuint(size, ==, 24 + 24 + 64 + 32 + 44 + 128);
    }
    g_assert_cmpuint(a.num_held, >=, NUM_SLOTS - 2);
    recycled = rd(&a, SLAVE + SLOT_QUEUE_RECYCLE);
    arm_recycle(&a, a.held[0]);
    arm_recv(&a, &msgid, reply, sizeof(reply));
    g_assert_cmphex(msgid, ==, MSGID(MSG_DATA, vc, 5));
    g_assert_cmpuint(rd(&a, SLAVE + SLOT_QUEUE_RECYCLE), >=, recycled);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/bcm2835-vchiq/init", test_init);
    qtest_add_func("/bcm2835-vchiq/connect", test_connect);
    qtest_add_func("/bcm2835-vchiq/services", test_services);
    qtest_add_func("/bcm2835-vchiq/mmal", test_mmal);
    qtest_add_func("/bcm2835-vchiq/smem", test_smem);
    qtest_add_func("/bcm2835-vchiq/slots", test_slots);

    return g_test_run();
}
