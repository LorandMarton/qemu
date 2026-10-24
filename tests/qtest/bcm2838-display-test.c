/*
 * QTest testcase for the BCM2838 (BCM2711) display pipeline and V3D
 *
 * The HVS, pixel valve, TXP, HDMI (hot-plug, CEC) and V3D register
 * models, and the PM power domain controls the V3D domain uses.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define PM_BASE             0xfe100000
#define PM_GRAFX            (PM_BASE + 0x10c)
#define PM_PASSWORD         0x5a000000
#define PM_POWUP            0x01
#define PM_POWOK            0x02
#define PM_MEMREP           0x08
#define PM_MRDONE           0x10
#define PM_V3DRSTN          0x40

#define HVS_BASE            0xfe400000
#define HVS_DISPSTAT        (HVS_BASE + 0x04)
#define HVS_DISPCTRL0       (HVS_BASE + 0x40)
#define HVS_DISPSTAT0       (HVS_BASE + 0x48)
#define HVS_DLIST           (HVS_BASE + 0x4000)
#define DISPCTRLX_RESET     (1u << 30)
#define DISPSTATX_EMPTY     (1u << 28)

#define PV0_BASE            0xfe206000
#define PV_CONTROL          0x00
#define PV_V_CONTROL        0x04
#define PV_INTEN            0x24
#define PV_INTSTAT          0x28
#define PV_CONTROL_FIFO_CLR 0x02

#define TXP_DST_CTRL        0xfe00400c
#define TXP_GO              0x01
#define TXP_BUSY            0x02
#define TXP_EI              0x04

#define HDMI0_HOTPLUG       0xfef008a8
#define HDMI0_CEC           0xfef04300
#define HDMI1_CEC           0xfef09300
#define CEC_CNTRL_1         0x10
#define CEC_CNTRL_5         0x20
#define CEC_TX_DATA_1       0x28
#define CEC_START_XMIT      (1u << 20)
#define CEC_TX_STATUS_GOOD  (1u << 30)
#define CEC_TX_EOM          (1u << 31)
#define CEC_TX_SW_RESET     (1u << 27)
#define L2_STATUS           0xfef00100
#define L2_CEC_TX_HDMI0     (1u << 0)
#define L2_CEC_TX_HDMI1     (1u << 8)

#define V3D_HUB             0xfec00000
#define V3D_CORE            0xfec04000
#define V3D_INT_STS         0x50
#define V3D_INT_SET         0x54
#define V3D_INT_CLR         0x58
#define V3D_INT_MSK_STS     0x5c
#define V3D_INT_MSK_CLR     0x64
#define V3D_MMUC_CONTROL    (V3D_HUB + 0x1000)
#define V3D_MMU_CTL         (V3D_HUB + 0x1200)
#define V3D_MMU_DEBUG_INFO  (V3D_HUB + 0x1238)
#define V3D_L2TCACTL        (V3D_CORE + 0x30)
#define V3D_PATH            "/machine/soc/peripherals/v3d"

static void test_powermgt(void)
{
    QTestState *qts = qtest_init("-M raspi4b");

    /* without the password, nothing changes */
    qtest_writel(qts, PM_GRAFX, PM_POWUP);
    g_assert_cmphex(qtest_readl(qts, PM_GRAFX), ==, 0);

    qtest_writel(qts, PM_GRAFX, PM_PASSWORD | PM_POWUP);
    g_assert_cmphex(qtest_readl(qts, PM_GRAFX), ==, PM_POWUP | PM_POWOK);
    qtest_writel(qts, PM_GRAFX, PM_PASSWORD | PM_POWUP | PM_MEMREP |
                 PM_V3DRSTN);
    g_assert_cmphex(qtest_readl(qts, PM_GRAFX), ==,
                    PM_POWUP | PM_POWOK | PM_MEMREP | PM_MRDONE | PM_V3DRSTN);
    qtest_writel(qts, PM_GRAFX, PM_PASSWORD);
    g_assert_cmphex(qtest_readl(qts, PM_GRAFX), ==, 0);

    qtest_quit(qts);
}

static void test_hvs(void)
{
    QTestState *qts = qtest_init("-M raspi4b");

    /* channels stay disabled with an empty FIFO; the reset completes */
    g_assert_cmphex(qtest_readl(qts, HVS_DISPSTAT0), ==, DISPSTATX_EMPTY);
    qtest_writel(qts, HVS_DISPCTRL0, DISPCTRLX_RESET);
    g_assert_cmphex(qtest_readl(qts, HVS_DISPCTRL0), ==, 0);
    g_assert_cmphex(qtest_readl(qts, HVS_DISPSTAT), ==, 0);

    /* display list memory */
    qtest_writel(qts, HVS_DLIST, 0x80000000);
    qtest_writel(qts, HVS_DLIST + 0x3ffc, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, HVS_DLIST), ==, 0x80000000);
    g_assert_cmphex(qtest_readl(qts, HVS_DLIST + 0x3ffc), ==, 0x12345678);

    qtest_quit(qts);
}

static void test_pixelvalve_txp(void)
{
    QTestState *qts = qtest_init("-M raspi4b");

    /* Linux vc4_crtc_bind */
    qtest_writel(qts, PV0_BASE + PV_INTEN, 0);
    qtest_writel(qts, PV0_BASE + PV_INTSTAT, 1 << 7);
    g_assert_cmphex(qtest_readl(qts, PV0_BASE + PV_INTSTAT), ==, 0);

    qtest_writel(qts, PV0_BASE + PV_CONTROL, PV_CONTROL_FIFO_CLR | 1);
    g_assert_cmphex(qtest_readl(qts, PV0_BASE + PV_CONTROL), ==, 1);
    qtest_writel(qts, PV0_BASE + PV_V_CONTROL, 1);
    qtest_writel(qts, PV0_BASE + PV_V_CONTROL, 0);
    g_assert_cmphex(qtest_readl(qts, PV0_BASE + PV_V_CONTROL), ==, 0);

    /* no writeback: GO is a command, BUSY never sets */
    qtest_writel(qts, TXP_DST_CTRL, TXP_EI | TXP_GO);
    g_assert_cmphex(qtest_readl(qts, TXP_DST_CTRL), ==, TXP_EI);

    qtest_quit(qts);
}

/* Starts a CEC transmission of len bytes to dest from logical address 4 */
static void cec_transmit(QTestState *qts, uint64_t cec, int dest,
                         unsigned len)
{
    uint32_t cntrl1 = qtest_readl(qts, cec + CEC_CNTRL_1);

    qtest_writel(qts, cec + CEC_TX_DATA_1, 0x3600 | 0x40 | dest);
    cntrl1 &= ~(CEC_START_XMIT | (0xf << 16));
    qtest_writel(qts, cec + CEC_CNTRL_1, cntrl1);
    qtest_writel(qts, cec + CEC_CNTRL_1,
                 cntrl1 | ((len - 1) << 16) | CEC_START_XMIT);
}

static void cec_ack_tx(QTestState *qts, uint64_t cec)
{
    qtest_writel(qts, cec + CEC_CNTRL_1,
                 qtest_readl(qts, cec + CEC_CNTRL_1) & ~CEC_START_XMIT);
}

static void test_hdmi(void)
{
    QTestState *qts = qtest_init("-M raspi4b");

    /* no display */
    g_assert_cmphex(qtest_readl(qts, HDMI0_HOTPLUG), ==, 0);

    /* directed: the header block is not acknowledged after 28.5 ms */
    cec_transmit(qts, HDMI0_CEC, 0, 2);
    qtest_clock_step(qts, 28400 * 1000);
    g_assert_cmphex(qtest_readl(qts, HDMI0_CEC + CEC_CNTRL_1) & CEC_TX_EOM,
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, L2_STATUS), ==, 0);
    qtest_clock_step(qts, 200 * 1000);
    g_assert_cmphex(qtest_readl(qts, HDMI0_CEC + CEC_CNTRL_1) &
                    (CEC_TX_EOM | CEC_TX_STATUS_GOOD), ==, CEC_TX_EOM);
    g_assert_cmphex(qtest_readl(qts, L2_STATUS), ==, L2_CEC_TX_HDMI0);
    cec_ack_tx(qts, HDMI0_CEC);
    g_assert_cmphex(qtest_readl(qts, L2_STATUS), ==, 0);

    /* broadcast: both blocks are sent, 52.5 ms, and reported good */
    cec_transmit(qts, HDMI1_CEC, 0xf, 2);
    qtest_clock_step(qts, 52400 * 1000);
    g_assert_cmphex(qtest_readl(qts, L2_STATUS), ==, 0);
    qtest_clock_step(qts, 200 * 1000);
    g_assert_cmphex(qtest_readl(qts, HDMI1_CEC + CEC_CNTRL_1) &
                    (CEC_TX_EOM | CEC_TX_STATUS_GOOD), ==,
                    CEC_TX_EOM | CEC_TX_STATUS_GOOD);
    g_assert_cmphex(qtest_readl(qts, L2_STATUS), ==, L2_CEC_TX_HDMI1);
    cec_ack_tx(qts, HDMI1_CEC);

    /* a transmitter reset stops a transmission */
    cec_transmit(qts, HDMI0_CEC, 0, 1);
    qtest_writel(qts, HDMI0_CEC + CEC_CNTRL_5, CEC_TX_SW_RESET);
    qtest_clock_step(qts, 100 * 1000 * 1000);
    g_assert_cmphex(qtest_readl(qts, HDMI0_CEC + CEC_CNTRL_1) & CEC_TX_EOM,
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, L2_STATUS), ==, 0);

    qtest_quit(qts);
}

static void test_v3d(void)
{
    QTestState *qts = qtest_init("-M raspi4b");

    /* identification, as on a CM4 */
    g_assert_cmphex(qtest_readl(qts, V3D_HUB + 0x0c), ==, 0x000e1124);
    g_assert_cmphex(qtest_readl(qts, V3D_HUB + 0x10), ==, 0x00000100);
    g_assert_cmphex(qtest_readl(qts, V3D_HUB + 0x14), ==, 0x00000e00);
    g_assert_cmphex(qtest_readl(qts, V3D_CORE + 0x00), ==, 0x04443356);
    g_assert_cmphex(qtest_readl(qts, V3D_CORE + 0x04), ==, 0x81001422);
    g_assert_cmphex(qtest_readl(qts, V3D_CORE + 0x08), ==, 0x40078121);
    g_assert_cmphex(qtest_readl(qts, V3D_MMU_DEBUG_INFO), ==, 0x620);

    /* Linux v3d_mmu_flush_all, v3d_flush_l2t */
    qtest_writel(qts, V3D_MMU_CTL, 0x1 | 0x4);
    g_assert_cmphex(qtest_readl(qts, V3D_MMU_CTL), ==, 0x1);
    qtest_writel(qts, V3D_MMUC_CONTROL, 0x2 | 0x1);
    g_assert_cmphex(qtest_readl(qts, V3D_MMUC_CONTROL), ==, 0x1);
    qtest_writel(qts, V3D_L2TCACTL, 0x1);
    g_assert_cmphex(qtest_readl(qts, V3D_L2TCACTL), ==, 0);

    /* the hub and the core interrupts share the one output */
    qtest_irq_intercept_out_named(qts, V3D_PATH, "sysbus-irq");
    g_assert_cmphex(qtest_readl(qts, V3D_CORE + V3D_INT_MSK_STS), ==,
                    0xffffffff);
    qtest_writel(qts, V3D_CORE + V3D_INT_SET, 0x1);
    g_assert_cmphex(qtest_readl(qts, V3D_CORE + V3D_INT_STS), ==, 0x1);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_writel(qts, V3D_CORE + V3D_INT_MSK_CLR, 0x1);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writel(qts, V3D_CORE + V3D_INT_CLR, 0x1);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_writel(qts, V3D_HUB + V3D_INT_MSK_CLR, 0x2);
    qtest_writel(qts, V3D_HUB + V3D_INT_SET, 0x2);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writel(qts, V3D_HUB + V3D_INT_CLR, 0x2);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/bcm2838-display/powermgt", test_powermgt);
    qtest_add_func("/bcm2838-display/hvs", test_hvs);
    qtest_add_func("/bcm2838-display/pixelvalve-txp", test_pixelvalve_txp);
    qtest_add_func("/bcm2838-display/hdmi", test_hdmi);
    qtest_add_func("/bcm2838-display/v3d", test_v3d);

    return g_test_run();
}
