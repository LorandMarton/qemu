/*
 * QTest testcase for the BCM2838 (BCM2711) PCIe root complex
 *
 * MISC_STATUS reports the link (PHYLINKUP, DL_ACTIVE) only with an
 * endpoint behind the root port, as Linux brcm-pcie expects of an empty
 * slot. The capabilities of the root port's own configuration space,
 * which is at the start of the controller's registers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "standard-headers/linux/pci_regs.h"

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

/* Offset of capability @id in the standard list, 0 if absent */
static uint8_t find_cap(QTestState *qts, uint8_t id)
{
    uint8_t pos = qtest_readb(qts, PCIE_BASE + PCI_CAPABILITY_LIST);

    for (int n = 0; pos && n < 48; n++) {
        if (qtest_readb(qts, PCIE_BASE + pos + PCI_CAP_LIST_ID) == id) {
            return pos;
        }
        pos = qtest_readb(qts, PCIE_BASE + pos + PCI_CAP_LIST_NEXT);
    }
    return 0;
}

static void test_root_port(void)
{
    QTestState *qts = qtest_init("-M raspi4b");
    uint8_t pm, exp;
    uint16_t pmc;

    g_assert_cmphex(qtest_readl(qts, PCIE_BASE + PCI_VENDOR_ID), ==,
                    0x271114e4);
    g_assert_cmphex(qtest_readw(qts, PCIE_BASE + PCI_STATUS) &
                    PCI_STATUS_CAP_LIST, ==, PCI_STATUS_CAP_LIST);

    /* Power management version 3, PME from D0 and D3hot */
    pm = find_cap(qts, PCI_CAP_ID_PM);
    g_assert_cmphex(pm, ==, 0x48);
    pmc = qtest_readw(qts, PCIE_BASE + pm + PCI_PM_PMC);
    g_assert_cmphex(pmc & PCI_PM_CAP_VER_MASK, ==, 3);
    g_assert_cmphex(pmc & PCI_PM_CAP_PME_MASK, ==,
                    PCI_PM_CAP_PME_D0 | PCI_PM_CAP_PME_D3hot);

    /* Express root port: no extended tags, no hot-plug */
    exp = find_cap(qts, PCI_CAP_ID_EXP);
    g_assert_cmphex(exp, ==, 0xac);
    g_assert_cmphex((qtest_readw(qts, PCIE_BASE + exp + PCI_EXP_FLAGS) &
                     PCI_EXP_FLAGS_TYPE) >> 4, ==, PCI_EXP_TYPE_ROOT_PORT);
    g_assert_cmphex(qtest_readl(qts, PCIE_BASE + exp + PCI_EXP_DEVCAP) &
                    PCI_EXP_DEVCAP_EXT_TAG, ==, 0);
    g_assert_cmphex(qtest_readl(qts, PCIE_BASE + exp + PCI_EXP_SLTCAP) &
                    PCI_EXP_SLTCAP_HPC, ==, 0);

    /* Power state: D3hot and back */
    qtest_writew(qts, PCIE_BASE + pm + PCI_PM_CTRL, 3);
    g_assert_cmphex(qtest_readw(qts, PCIE_BASE + pm + PCI_PM_CTRL) &
                    PCI_PM_CTRL_STATE_MASK, ==, 3);
    qtest_writew(qts, PCIE_BASE + pm + PCI_PM_CTRL, 0);
    g_assert_cmphex(qtest_readw(qts, PCIE_BASE + pm + PCI_PM_CTRL) &
                    PCI_PM_CTRL_STATE_MASK, ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (qtest_has_device("pci-testdev")) {
        qtest_add_func("/bcm2838-pcie/link", test_link);
    }
    qtest_add_func("/bcm2838-pcie/root-port", test_root_port);
    return g_test_run();
}
