/*
 * QTest testcase for the sysbus xHCI properties
 *
 * sbsa-ref sets "slots" and leaves "intrs" at its default: HCSPARAMS1
 * shows both.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "libqtest.h"

#define SBSA_XHCI       0x60110000
#define HCSPARAMS1      0x04

static void test_sbsa_ref(void)
{
    QTestState *qts = qtest_init("-M sbsa-ref");
    uint32_t hcs = qtest_readl(qts, SBSA_XHCI + HCSPARAMS1);

    g_assert_cmpuint(extract32(hcs, 0, 8), ==, 64);     /* MaxSlots */
    g_assert_cmpuint(extract32(hcs, 8, 11), ==, 16);    /* MaxIntrs */

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/xhci-sysbus/sbsa-ref", test_sbsa_ref);
    return g_test_run();
}
