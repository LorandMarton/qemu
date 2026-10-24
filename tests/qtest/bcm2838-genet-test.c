/*
 * QTest testcase for the BCM2838 GENET receive filter
 *
 * Frames come in over a socket netdev. A frame that matches an enabled
 * hardware filter goes to the ring of that filter in the filter-to-ring
 * map; every other frame goes to the default ring 16, as Linux bcmgenet's
 * rxnfc rules expect.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define GENET_BASE          0xfd580000

#define RDMA_DESC(i)        (0x2000 + (i) * 12)
#define RDMA_RING(r)        (0x2c00 + (r) * 0x40)
#define RING_WRITE_PTR      0x00
#define RING_PROD_INDEX     0x08
#define RING_BUF_SIZE       0x10
#define RING_START_ADDR     0x14
#define RING_END_ADDR       0x1c
#define RDMA_RING_CFG       0x3040
#define RDMA_CTRL           0x3044
#define RDMA_INDEX2RING(i)  (0x30b0 + (i) * 4)

#define HFB_FLT(f)          (0x8000 + (f) * 128 * 4)
#define HFB_CTRL            0xfc00
#define HFB_FLT_ENABLE(i)   (0xfc04 + (i) * 4)
#define HFB_FLT_LEN(i)      (0xfc1c + (i) * 4)
#define HFB_FILTER_CNT      48

#define DEFAULT_RING        16
#define DESCS_PER_RING      4
#define BUF_SIZE            2048
#define BUF_BASE            0x100000
#define STATUS_LEN          64      /* receive status block, then 2 bytes */
#define FRAME_LEN           60

/* the filters under test and their rings */
#define FLT_LOW             3       /* enabled in the second register */
#define FLT_HIGH            40      /* enabled in the first register */
#define RING_LOW            1
#define RING_HIGH           2

static const unsigned rings[] = { RING_LOW, RING_HIGH, DEFAULT_RING };

static void genet_writel(QTestState *qts, uint32_t off, uint32_t val)
{
    qtest_writel(qts, GENET_BASE + off, val);
}

static uint32_t genet_readl(QTestState *qts, uint32_t off)
{
    return qtest_readl(qts, GENET_BASE + off);
}

/* Ring R uses the R-th group of DESCS_PER_RING descriptors */
static void ring_init(QTestState *qts, unsigned r, unsigned group)
{
    unsigned first = group * DESCS_PER_RING;

    for (unsigned i = first; i < first + DESCS_PER_RING; i++) {
        genet_writel(qts, RDMA_DESC(i) + 4, BUF_BASE + i * BUF_SIZE);
        genet_writel(qts, RDMA_DESC(i) + 8, 0);
    }
    genet_writel(qts, RDMA_RING(r) + RING_START_ADDR, first * 3);
    genet_writel(qts, RDMA_RING(r) + RING_END_ADDR,
                 (first + DESCS_PER_RING - 1) * 3);
    genet_writel(qts, RDMA_RING(r) + RING_WRITE_PTR, first * 3);
    genet_writel(qts, RDMA_RING(r) + RING_BUF_SIZE, BUF_SIZE);
}

/*
 * Filter F: LEN bytes of pattern words, each word two bytes (15:8, 7:0)
 * and a nibble mask (19:16).
 */
static void filter_set(QTestState *qts, unsigned f, const uint32_t *words,
                       unsigned len)
{
    unsigned i = (HFB_FILTER_CNT - 1 - f) / 4;
    uint32_t reg;

    for (unsigned w = 0; w < DIV_ROUND_UP(len, 2); w++) {
        genet_writel(qts, HFB_FLT(f) + w * 4, words[w]);
    }
    reg = genet_readl(qts, HFB_FLT_LEN(i));
    reg = deposit32(reg, 8 * (f % 4), 8, len);
    genet_writel(qts, HFB_FLT_LEN(i), reg);
}

static void filter_enable(QTestState *qts, unsigned f, bool on)
{
    uint32_t off = HFB_FLT_ENABLE(f < 32);
    uint32_t reg = genet_readl(qts, off);

    genet_writel(qts, off, deposit32(reg, f % 32, 1, on));
}

static QTestState *genet_init(int *fd)
{
    static const uint32_t ethertype_low[] = {
        /* any addresses; EtherType 0x88b5; then 4x in the next byte */
        0, 0, 0, 0, 0, 0, 0xf0000 | 0x88b5, 0x80000 | 0x4000,
    };
    static const uint32_t ethertype_high[] = {
        0, 0, 0, 0, 0, 0, 0xf0000 | 0x88b6,
    };
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M raspi4b -nic socket,fd=%d,model=bcm2838-genet",
                      sv[1]);
    close(sv[1]);
    *fd = sv[0];

    for (unsigned i = 0; i < ARRAY_SIZE(rings); i++) {
        ring_init(qts, rings[i], i);
    }
    genet_writel(qts, RDMA_RING_CFG,
                 BIT(RING_LOW) | BIT(RING_HIGH) | BIT(DEFAULT_RING));
    genet_writel(qts, RDMA_CTRL, 1 | BIT(1 + RING_LOW) | BIT(1 + RING_HIGH) |
                 BIT(1 + DEFAULT_RING));

    filter_set(qts, FLT_LOW, ethertype_low, 15);
    filter_set(qts, FLT_HIGH, ethertype_high, 14);
    genet_writel(qts, RDMA_INDEX2RING(FLT_LOW / 8),
                 RING_LOW << (4 * (FLT_LOW % 8)));
    genet_writel(qts, RDMA_INDEX2RING(FLT_HIGH / 8),
                 RING_HIGH << (4 * (FLT_HIGH % 8)));
    filter_enable(qts, FLT_LOW, true);
    filter_enable(qts, FLT_HIGH, true);
    genet_writel(qts, HFB_CTRL, 1);
    return qts;
}

static uint32_t prod_index(QTestState *qts, unsigned r)
{
    return genet_readl(qts, RDMA_RING(r) + RING_PROD_INDEX) & 0xffff;
}

/* Send a broadcast frame; return the ring it arrived on */
static unsigned genet_rx(QTestState *qts, int fd, uint16_t ethertype,
                         uint8_t byte14)
{
    uint8_t pkt[4 + FRAME_LEN] = { 0 };
    uint8_t *frame = pkt + 4;
    uint32_t before[ARRAY_SIZE(rings)];
    uint8_t back[FRAME_LEN];
    gint64 end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    unsigned hit = UINT_MAX;

    for (unsigned i = 0; i < ARRAY_SIZE(rings); i++) {
        before[i] = prod_index(qts, rings[i]);
    }
    stl_be_p(pkt, FRAME_LEN);
    memset(frame, 0xff, 6);
    stw_be_p(frame + 12, ethertype);
    frame[14] = byte14;
    g_assert_cmpint(send(fd, pkt, sizeof(pkt), 0), ==, sizeof(pkt));

    do {
        qtest_clock_step(qts, 100);
        for (unsigned i = 0; i < ARRAY_SIZE(rings); i++) {
            if (prod_index(qts, rings[i]) != before[i]) {
                g_assert_cmpuint(hit, ==, UINT_MAX);
                g_assert_cmpuint(prod_index(qts, rings[i]), ==,
                                 before[i] + 1);
                hit = i;
            }
        }
    } while (hit == UINT_MAX && g_get_monotonic_time() < end);
    g_assert_cmpuint(hit, !=, UINT_MAX);

    /* the frame, after the status block, in the ring's buffer */
    qtest_memread(qts, BUF_BASE +
                  (hit * DESCS_PER_RING + before[hit] % DESCS_PER_RING) *
                  BUF_SIZE + STATUS_LEN + 2, back, sizeof(back));
    g_assert(!memcmp(back, frame, sizeof(back)));
    return rings[hit];
}

static void test_filter(void)
{
    int fd;
    QTestState *qts = genet_init(&fd);

    g_assert_cmpuint(genet_rx(qts, fd, 0x88b5, 0x45), ==, RING_LOW);
    g_assert_cmpuint(genet_rx(qts, fd, 0x88b5, 0x4f), ==, RING_LOW);
    /* the nibble mask: the high nibble must be 4 */
    g_assert_cmpuint(genet_rx(qts, fd, 0x88b5, 0x55), ==, DEFAULT_RING);
    g_assert_cmpuint(genet_rx(qts, fd, 0x88b6, 0x55), ==, RING_HIGH);
    g_assert_cmpuint(genet_rx(qts, fd, 0x0800, 0x45), ==, DEFAULT_RING);

    /* a disabled filter, then the whole block */
    filter_enable(qts, FLT_HIGH, false);
    g_assert_cmpuint(genet_rx(qts, fd, 0x88b6, 0x55), ==, DEFAULT_RING);
    g_assert_cmpuint(genet_rx(qts, fd, 0x88b5, 0x45), ==, RING_LOW);
    genet_writel(qts, HFB_CTRL, 0);
    g_assert_cmpuint(genet_rx(qts, fd, 0x88b5, 0x45), ==, DEFAULT_RING);

    qtest_quit(qts);
    close(fd);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/bcm2838-genet/filter", test_filter);
    return g_test_run();
}
