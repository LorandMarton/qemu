/*
 * QTest testcase for BCM283x DMA engine (on Raspberry Pi 3)
 * and its interrupts coming to Interrupt Controller.
 *
 * Copyright (c) 2022 Auriga LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

/* Offsets in raspi3b platform: */
#define RASPI3_DMA_BASE 0x3f007000
#define RASPI3_IC_BASE  0x3f00b200

/* Used register/fields definitions */

/* DMA engine registers: */
#define BCM2708_DMA_CS         0
#define BCM2708_DMA_ACTIVE     (1 << 0)
#define BCM2708_DMA_INT        (1 << 2)
#define BCM2708_DMA_ISPAUSED   (1 << 4)
#define BCM2708_DMA_ISHELD     (1 << 5)
#define BCM2708_DMA_ABORT      (1 << 30)
#define BCM2708_DMA_RESET      (1u << 31)

#define BCM2708_DMA_ADDR       0x04

#define BCM2708_DMA_NEXTCB     0x1c
#define BCM2708_DMA_DEBUG      0x20
#define BCM2708_DMA_DEBUG_LITE (1 << 28)

#define BCM2708_DMA_INT_STATUS 0xfe0

/* DMA Transfer Info fields: */
#define BCM2708_DMA_INT_EN     (1 << 0)
#define BCM2708_DMA_D_INC      (1 << 4)
#define BCM2708_DMA_D_DREQ     (1 << 6)
#define BCM2708_DMA_S_INC      (1 << 8)
#define BCM2708_DMA_S_DREQ     (1 << 10)
#define BCM2708_DMA_PERMAP(n)  ((n) << 16)
#define BCM2708_DMA_END        (1 << 1)

/* Interrupt controller registers: */
#define IRQ_PENDING_BASIC      0x00
#define IRQ_GPU_PENDING1_AGGR  (1 << 8)
#define IRQ_PENDING_1          0x04
#define IRQ_ENABLE_1           0x10

/* Data for the test: */
#define SCB_ADDR   256
#define S_ADDR     32
#define D_ADDR     64
#define TXFR_LEN   32
const uint32_t check_data = 0x12345678;

static void bcm2835_dma_test_interrupt(int dma_c, int irq_line)
{
    uint64_t dma_base = RASPI3_DMA_BASE + dma_c * 0x100;
    int gpu_irq_line = 16 + irq_line;

    /* Check that interrupts are silent by default: */
    writel(RASPI3_IC_BASE + IRQ_ENABLE_1, 1 << gpu_irq_line);
    int isr = readl(dma_base + BCM2708_DMA_INT_STATUS);
    g_assert_cmpint(isr, ==, 0);
    uint32_t reg0 = readl(dma_base + BCM2708_DMA_CS);
    g_assert_cmpint(reg0, ==, 0);
    uint32_t ic_pending = readl(RASPI3_IC_BASE + IRQ_PENDING_BASIC);
    g_assert_cmpint(ic_pending, ==, 0);
    uint32_t gpu_pending1 = readl(RASPI3_IC_BASE + IRQ_PENDING_1);
    g_assert_cmpint(gpu_pending1, ==, 0);

    /* Prepare Control Block: */
    writel(SCB_ADDR + 0, BCM2708_DMA_S_INC | BCM2708_DMA_D_INC |
                         BCM2708_DMA_INT_EN); /* transfer info */
    writel(SCB_ADDR + 4, S_ADDR);             /* source address */
    writel(SCB_ADDR + 8, D_ADDR);             /* destination address */
    writel(SCB_ADDR + 12, TXFR_LEN);          /* transfer length */
    writel(dma_base + BCM2708_DMA_ADDR, SCB_ADDR);

    writel(S_ADDR, check_data);
    for (int word = S_ADDR + 4; word < S_ADDR + TXFR_LEN; word += 4) {
        writel(word, ~check_data);
    }
    /* Perform the transfer: */
    writel(dma_base + BCM2708_DMA_CS, BCM2708_DMA_ACTIVE);

    /* Check that destination == source: */
    uint32_t data = readl(D_ADDR);
    g_assert_cmpint(data, ==, check_data);
    for (int word = D_ADDR + 4; word < D_ADDR + TXFR_LEN; word += 4) {
        data = readl(word);
        g_assert_cmpint(data, ==, ~check_data);
    }

    /* Check that interrupt status is set both in DMA and IC controllers: */
    isr = readl(RASPI3_DMA_BASE + BCM2708_DMA_INT_STATUS);
    g_assert_cmpint(isr, ==, 1 << dma_c);

    ic_pending = readl(RASPI3_IC_BASE + IRQ_PENDING_BASIC);
    g_assert_cmpint(ic_pending, ==, IRQ_GPU_PENDING1_AGGR);

    gpu_pending1 = readl(RASPI3_IC_BASE + IRQ_PENDING_1);
    g_assert_cmpint(gpu_pending1, ==, 1 << gpu_irq_line);

    /* Clean up, clear interrupt: */
    writel(dma_base + BCM2708_DMA_CS, BCM2708_DMA_INT);
}

static void bcm2835_dma_test_interrupts(void)
{
    /* DMA engines 0--10 have separate IRQ lines, 11--14 - only one: */
    bcm2835_dma_test_interrupt(0,  0);
    bcm2835_dma_test_interrupt(10, 10);
    bcm2835_dma_test_interrupt(11, 11);
    bcm2835_dma_test_interrupt(14, 11);
}

/*
 * A peripheral whose model does not drive its DREQ (11, the EMMC
 * controller): the transfer runs unpaced, as the guest driver expects.
 */
static void bcm2835_dma_test_unpaced_dreq(void)
{
    uint64_t dma_base = RASPI3_DMA_BASE + 2 * 0x100;

    writel(SCB_ADDR + 0, BCM2708_DMA_S_INC | BCM2708_DMA_D_INC |
                         BCM2708_DMA_S_DREQ | BCM2708_DMA_PERMAP(11));
    writel(SCB_ADDR + 4, S_ADDR);
    writel(SCB_ADDR + 8, D_ADDR);
    writel(SCB_ADDR + 12, TXFR_LEN);
    writel(SCB_ADDR + 20, 0);
    writel(dma_base + BCM2708_DMA_ADDR, SCB_ADDR);
    for (int word = S_ADDR; word < S_ADDR + TXFR_LEN; word += 4) {
        writel(word, check_data + word);
    }
    writel(dma_base + BCM2708_DMA_CS, BCM2708_DMA_ACTIVE);

    g_assert_cmphex(readl(dma_base + BCM2708_DMA_CS) &
                    (BCM2708_DMA_ACTIVE | BCM2708_DMA_END), ==,
                    BCM2708_DMA_END);
    for (int word = 0; word < TXFR_LEN; word += 4) {
        g_assert_cmphex(readl(D_ADDR + word), ==, check_data + S_ADDR + word);
    }
}

/* Channels 7 to 14 are lite channels; Linux tells them by DEBUG.LITE */
static void bcm2835_dma_test_lite(void)
{
    for (int c = 0; c < 15; c++) {
        uint32_t debug = readl(RASPI3_DMA_BASE + c * 0x100 + BCM2708_DMA_DEBUG);

        g_assert_cmpint(!!(debug & BCM2708_DMA_DEBUG_LITE), ==,
                        c >= 7 && c <= 14);
    }
}

/*
 * DREQ pacing with a peripheral that drives its DREQ (6, as dreq-inputs
 * says): the channel is held while DREQ is low, can be paused there and
 * its NEXTCONBK changed, and resumes; ABORT ends the current control block
 * and goes on with the next. The paced block has a byte length (10): the
 * last access is two bytes.
 */
#define PACED_DMA       (RASPI3_DMA_BASE + 3 * 0x100)
#define CB1             0x1000
#define CB2             0x1020
#define SRC1            0x1100
#define DST1            0x1200
#define SRC2            0x1300
#define DST2            0x1400
#define DMA_PATH        "/machine/soc/peripherals/dma"

static void paced_setup(QTestState *qts)
{
    qtest_writel(qts, CB1 + 0, BCM2708_DMA_S_INC | BCM2708_DMA_D_INC |
                               BCM2708_DMA_D_DREQ | BCM2708_DMA_PERMAP(6));
    qtest_writel(qts, CB1 + 4, SRC1);
    qtest_writel(qts, CB1 + 8, DST1);
    qtest_writel(qts, CB1 + 12, 10);
    qtest_writel(qts, CB1 + 20, CB2);
    qtest_writel(qts, CB2 + 0, BCM2708_DMA_S_INC | BCM2708_DMA_D_INC);
    qtest_writel(qts, CB2 + 4, SRC2);
    qtest_writel(qts, CB2 + 8, DST2);
    qtest_writel(qts, CB2 + 12, 4);
    qtest_writel(qts, CB2 + 20, 0);
    for (int i = 0; i < 3; i++) {
        qtest_writel(qts, SRC1 + i * 4, 0x11111111 * (i + 1));
        qtest_writel(qts, DST1 + i * 4, 0xffffffff);
    }
    qtest_writel(qts, SRC2, 0xcafe0000);
    qtest_writel(qts, DST2, 0xffffffff);
    qtest_set_irq_in(qts, DMA_PATH, "dreq", 6, 0);
    qtest_writel(qts, PACED_DMA + BCM2708_DMA_CS, BCM2708_DMA_RESET);
    qtest_writel(qts, PACED_DMA + BCM2708_DMA_ADDR, CB1);
    qtest_writel(qts, PACED_DMA + BCM2708_DMA_CS, BCM2708_DMA_ACTIVE);
}

static uint32_t paced_cs(QTestState *qts)
{
    return qtest_readl(qts, PACED_DMA + BCM2708_DMA_CS) &
           (BCM2708_DMA_ACTIVE | BCM2708_DMA_END | BCM2708_DMA_ISPAUSED |
            BCM2708_DMA_ISHELD);
}

static void bcm2835_dma_test_paced(void)
{
    QTestState *qts = qtest_init("-machine raspi3b "
                                 "-global bcm2835-dma.dreq-inputs=0x40");

    /* DREQ low: held, nothing moved */
    paced_setup(qts);
    g_assert_cmphex(paced_cs(qts), ==,
                    BCM2708_DMA_ACTIVE | BCM2708_DMA_ISHELD);
    g_assert_cmphex(qtest_readl(qts, DST1), ==, 0xffffffff);

    /* Paused, it does not run on DREQ; the next CB is now none */
    qtest_writel(qts, PACED_DMA + BCM2708_DMA_CS, 0);
    g_assert_cmphex(paced_cs(qts), ==, BCM2708_DMA_ISPAUSED);
    qtest_writel(qts, PACED_DMA + BCM2708_DMA_NEXTCB, 0);
    qtest_set_irq_in(qts, DMA_PATH, "dreq", 6, 1);
    g_assert_cmphex(qtest_readl(qts, DST1), ==, 0xffffffff);

    /* Resumed: 10 bytes, and the chain ends there */
    qtest_writel(qts, PACED_DMA + BCM2708_DMA_CS, BCM2708_DMA_ACTIVE);
    g_assert_cmphex(paced_cs(qts), ==, BCM2708_DMA_END |
                    BCM2708_DMA_ISPAUSED);
    g_assert_cmphex(qtest_readl(qts, DST1), ==, 0x11111111);
    g_assert_cmphex(qtest_readl(qts, DST1 + 4), ==, 0x22222222);
    g_assert_cmphex(qtest_readl(qts, DST1 + 8), ==, 0xffff3333);
    g_assert_cmphex(qtest_readl(qts, DST2), ==, 0xffffffff);

    /* ABORT while held: CB1 is dropped, CB2 runs */
    paced_setup(qts);
    qtest_writel(qts, PACED_DMA + BCM2708_DMA_CS,
                 BCM2708_DMA_ACTIVE | BCM2708_DMA_ABORT);
    g_assert_cmphex(paced_cs(qts), ==, BCM2708_DMA_END |
                    BCM2708_DMA_ISPAUSED);
    g_assert_cmphex(qtest_readl(qts, DST1), ==, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, DST2), ==, 0xcafe0000);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int ret;
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/bcm2835/dma/test_interrupts",
                   bcm2835_dma_test_interrupts);
    qtest_add_func("/bcm2835/dma/unpaced_dreq",
                   bcm2835_dma_test_unpaced_dreq);
    qtest_add_func("/bcm2835/dma/lite", bcm2835_dma_test_lite);
    qtest_add_func("/bcm2835/dma/paced", bcm2835_dma_test_paced);
    qtest_start("-machine raspi3b");
    ret = g_test_run();
    qtest_end();
    return ret;
}
