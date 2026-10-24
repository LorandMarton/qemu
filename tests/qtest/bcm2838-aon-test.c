/*
 * QTest testcase for the BCM2838 (BCM2711) always-on block
 *
 * The L2 interrupt controller, the HDMI DDC I2C controllers (brcmstb BSC)
 * with an EDID on DDC0, and the DVP clock and reset registers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define AON_BASE            0xfef00000
#define DVP_SW_INIT         (AON_BASE + 0x04)
#define DVP_MISC_CONFIG     (AON_BASE + 0x08)

#define L2_BASE             (AON_BASE + 0x100)
#define L2_STATUS           (L2_BASE + 0x00)
#define L2_SET              (L2_BASE + 0x04)
#define L2_CLEAR            (L2_BASE + 0x08)
#define L2_MASK_STATUS      (L2_BASE + 0x0c)
#define L2_MASK_SET         (L2_BASE + 0x10)
#define L2_MASK_CLEAR       (L2_BASE + 0x14)
#define L2_PATH             "/machine/soc/peripherals/aon-intc"

#define DDC0_BASE           0xfef04500
#define DDC1_BASE           0xfef09500
#define AUTOI2C0_CTRL0      (0xfef00b00 + 0x26c)
#define AUTOI2C1_CTRL0      (0xfef05b00 + 0x26c)
#define BSC_CHIP_ADDRESS    0x00
#define BSC_DATA_IN0        0x04
#define BSC_CNT             0x24
#define BSC_CTL             0x28
#define BSC_IIC_ENABLE      0x2c
#define BSC_DATA_OUT0       0x30
#define BSC_CTLHI           0x50

#define IIC_ENABLE          0x01
#define IIC_INTRP           0x02
#define IIC_NOACK           0x04
#define IIC_NOSTOP          0x10
#define IIC_RESTART         0x40
#define CTL_DTF_READ        0x01
#define CTLHI_DATAREG_SIZE  0x40
#define RELEASE_BSC         0x02

static void test_l2_intc(void)
{
    QTestState *qts = qtest_init("-M raspi4b");

    qtest_irq_intercept_out_named(qts, L2_PATH, "sysbus-irq");

    /* all masked out of reset: an input shows, the output stays low */
    g_assert_cmphex(qtest_readl(qts, L2_MASK_STATUS), ==, 0xffffffff);
    qtest_set_irq_in(qts, L2_PATH, NULL, 4, 1);
    g_assert_cmphex(qtest_readl(qts, L2_STATUS), ==, 1 << 4);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_writel(qts, L2_MASK_CLEAR, 1 << 4);
    g_assert_cmphex(qtest_readl(qts, L2_MASK_STATUS), ==, ~(1u << 4));
    g_assert_true(qtest_get_irq(qts, 0));

    /* level: CLEAR does not clear an asserted input */
    qtest_writel(qts, L2_CLEAR, 1 << 4);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_set_irq_in(qts, L2_PATH, NULL, 4, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    /* software bits: SET, then CLEAR */
    qtest_writel(qts, L2_SET, 1 << 4);
    g_assert_cmphex(qtest_readl(qts, L2_STATUS), ==, 1 << 4);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writel(qts, L2_CLEAR, 1 << 4);
    g_assert_cmphex(qtest_readl(qts, L2_STATUS), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    /* masking again */
    qtest_set_irq_in(qts, L2_PATH, NULL, 4, 1);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writel(qts, L2_MASK_SET, 1 << 4);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

/* One BSC command; returns IIC_ENABLE afterwards */
static uint32_t bsc_cmd(QTestState *qts, uint64_t base, bool read,
                        unsigned len, uint32_t cond)
{
    uint32_t st;

    qtest_writel(qts, base + BSC_CTL, read ? CTL_DTF_READ : 0);
    qtest_writel(qts, base + BSC_CNT, len);
    qtest_writel(qts, base + BSC_IIC_ENABLE, cond | IIC_ENABLE);
    st = qtest_readl(qts, base + BSC_IIC_ENABLE);
    g_assert_cmphex(st & IIC_INTRP, ==, IIC_INTRP);
    qtest_writel(qts, base + BSC_CNT, 0);
    qtest_writel(qts, base + BSC_IIC_ENABLE, 0);
    g_assert_cmphex(qtest_readl(qts, base + BSC_IIC_ENABLE), ==, 0);
    return st;
}

static void test_ddc(void)
{
    QTestState *qts = qtest_init("-M raspi4b "
                                 "-device i2c-ddc,bus=ddc0,address=0x50");

    qtest_writel(qts, AUTOI2C0_CTRL0, RELEASE_BSC);
    qtest_writel(qts, AUTOI2C1_CTRL0, RELEASE_BSC);
    qtest_writel(qts, DDC0_BASE + BSC_CTLHI, CTLHI_DATAREG_SIZE);
    qtest_writel(qts, DDC1_BASE + BSC_CTLHI, CTLHI_DATAREG_SIZE);

    /* EDID offset 0, then a repeated start and 8 bytes: the header */
    qtest_writel(qts, DDC0_BASE + BSC_CHIP_ADDRESS, 0x50 << 1);
    qtest_writel(qts, DDC0_BASE + BSC_DATA_IN0, 0);
    g_assert_cmphex(bsc_cmd(qts, DDC0_BASE, false, 1,
                            IIC_RESTART | IIC_NOSTOP) & IIC_NOACK, ==, 0);
    qtest_writel(qts, DDC0_BASE + BSC_CHIP_ADDRESS, (0x50 << 1) | 1);
    g_assert_cmphex(bsc_cmd(qts, DDC0_BASE, true, 8, 0) & IIC_NOACK, ==, 0);
    g_assert_cmphex(qtest_readl(qts, DDC0_BASE + BSC_DATA_OUT0), ==,
                    0xffffff00);
    g_assert_cmphex(qtest_readl(qts, DDC0_BASE + BSC_DATA_OUT0 + 4), ==,
                    0x00ffffff);

    /* no monitor on HDMI1: the address is not acknowledged */
    qtest_writel(qts, DDC1_BASE + BSC_CHIP_ADDRESS, 0x50 << 1);
    g_assert_cmphex(bsc_cmd(qts, DDC1_BASE, false, 1, 0) & IIC_NOACK, ==,
                    IIC_NOACK);

    qtest_quit(qts);
}

static void test_dvp(void)
{
    QTestState *qts = qtest_init("-M raspi4b");

    /* six resets, two clock gates */
    qtest_writel(qts, DVP_SW_INIT, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, DVP_SW_INIT), ==, 0x3f);
    qtest_writel(qts, DVP_MISC_CONFIG, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, DVP_MISC_CONFIG), ==, 0x18);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/bcm2838-aon/l2-intc", test_l2_intc);
    qtest_add_func("/bcm2838-aon/ddc", test_ddc);
    qtest_add_func("/bcm2838-aon/dvp", test_dvp);

    return g_test_run();
}
