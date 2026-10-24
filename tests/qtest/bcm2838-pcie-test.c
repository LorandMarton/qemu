/*
 * QTest testcase for the BCM2838 (BCM2711) PCIe root complex
 *
 * MISC_STATUS reports the link (PHYLINKUP, DL_ACTIVE) only with an
 * endpoint behind the root port, as Linux brcm-pcie expects of an empty
 * slot.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define PCIE_BASE           0xfd500000
#define PCIE_MISC_STATUS    0x4068
#define STATUS_LINK         0x30    /* PHYLINKUP | DL_ACTIVE */

static uint32_t link_status(const char *args)
{
    QTestState *qts = qtest_initf("-M raspi4b %s", args);
    uint32_t status = qtest_readl(qts, PCIE_BASE + PCIE_MISC_STATUS);

    qtest_quit(qts);
    return status & STATUS_LINK;
}

static void test_link(void)
{
    g_assert_cmphex(link_status(""), ==, 0);
    g_assert_cmphex(link_status("-device pci-testdev,bus=pcie.1"), ==,
                    STATUS_LINK);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (qtest_has_device("pci-testdev")) {
        qtest_add_func("/bcm2838-pcie/link", test_link);
    }
    return g_test_run();
}
