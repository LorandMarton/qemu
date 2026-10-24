/*
 * QTest testcase for USB on the BCM2838 (BCM2711)
 *
 * The xHCI capability parameters (interface version 1.1, 64-byte contexts,
 * one USB 2.0 port; the device slots of the sysbus xHCI default), and the
 * USB2514 hub of the Compute Module 4 IO
 * board on that port, high speed, with a full-speed device behind it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define XHCI_BASE       0xfe9c0000
#define XHCI_HCIVERSION 0x02
#define XHCI_HCSPARAMS1 0x04
#define XHCI_HCCPARAMS1 0x10
#define XHCI_HCCPARAMS2 0x1c

static void test_xhci_params(void)
{
    QTestState *qts = qtest_init("-M raspi4b");
    uint32_t hcs1 = qtest_readl(qts, XHCI_BASE + XHCI_HCSPARAMS1);

    g_assert_cmpuint(hcs1 & 0xff, ==, 64);          /* device slots */
    g_assert_cmpuint((hcs1 >> 8) & 0x7ff, ==, 1);   /* interrupters */
    g_assert_cmpuint(hcs1 >> 24, ==, 1);            /* ports */
    g_assert_cmphex(qtest_readw(qts, XHCI_BASE + XHCI_HCIVERSION), ==, 0x110);
    g_assert_cmphex(qtest_readl(qts, XHCI_BASE + XHCI_HCCPARAMS1) & 0x5, ==,
                    0x5);                           /* AC64, CSZ */
    g_assert_cmphex(qtest_readl(qts, XHCI_BASE + XHCI_HCCPARAMS2), ==, 0);
    qtest_quit(qts);
}

static void test_usb2514(void)
{
    QTestState *qts = qtest_init("-M raspi4b -device usb2514,port=1 "
                                 "-device usb-kbd,port=1.3,usb_version=1");
    g_autofree char *usb = qtest_hmp(qts, "info usb");

    g_assert_nonnull(strstr(usb, "Port 1, Speed 480 Mb/s, Product USB2514"));
    g_assert_nonnull(strstr(usb, "Port 1.3, Speed 12 Mb/s"));
    /* nothing else: no hub added on the way */
    g_assert_null(strstr(usb, "QEMU USB Hub"));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/bcm2838-usb/xhci-params", test_xhci_params);
    if (qtest_has_device("usb2514") && qtest_has_device("usb-kbd")) {
        qtest_add_func("/bcm2838-usb/usb2514", test_usb2514);
    }

    return g_test_run();
}
