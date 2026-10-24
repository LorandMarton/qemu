/*
 * QTest testcase for the BCM2838 asynchronous AXI bridges (ASB)
 *
 * What Linux's bcm2835-power relies on: the bridge ID, and ACK following
 * REQ_STOP, written with the PM password; writes without it are ignored.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define ASB_BASE            0xfe00a000
#define RPIVID_ASB_BASE     0xfec11000
#define AXI_BRDG_ID         0x20
#define BRDG_ID             0x62726467 /* "brdg" */
#define V3D_M_CTRL          0x0c
#define REQ_STOP            (1u << 0)
#define ACK                 (1u << 1)
#define EMPTY               (1u << 2)
#define PM_PASSWORD         0x5a000000

static void test_bridge(const void *data)
{
    uint64_t base = (uintptr_t)data;
    QTestState *qts = qtest_init("-M raspi4b");

    g_assert_cmphex(qtest_readl(qts, base + AXI_BRDG_ID), ==, BRDG_ID);
    g_assert_cmphex(qtest_readl(qts, base + V3D_M_CTRL), ==, EMPTY);

    qtest_writel(qts, base + V3D_M_CTRL, PM_PASSWORD | REQ_STOP);
    g_assert_cmphex(qtest_readl(qts, base + V3D_M_CTRL), ==,
                    REQ_STOP | ACK | EMPTY);
    /* no password: ignored */
    qtest_writel(qts, base + V3D_M_CTRL, 0);
    g_assert_cmphex(qtest_readl(qts, base + V3D_M_CTRL), ==,
                    REQ_STOP | ACK | EMPTY);
    qtest_writel(qts, base + V3D_M_CTRL, PM_PASSWORD);
    g_assert_cmphex(qtest_readl(qts, base + V3D_M_CTRL), ==, EMPTY);

    /* the ID is read-only */
    qtest_writel(qts, base + AXI_BRDG_ID, PM_PASSWORD);
    g_assert_cmphex(qtest_readl(qts, base + AXI_BRDG_ID), ==, BRDG_ID);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("/bcm2838-asb/pm", (void *)(uintptr_t)ASB_BASE,
                        test_bridge);
    qtest_add_data_func("/bcm2838-asb/rpivid",
                        (void *)(uintptr_t)RPIVID_ASB_BASE, test_bridge);
    return g_test_run();
}
