/*
 * QTest testcase for the BCM2838 UARTs
 *
 * UART0 (PL011) on a host serial port: the guest's line settings reach
 * the host tty. A pty stands in for the USB serial adapter; Linux keeps a
 * pty at CS8 without parity, so it carries the rate and the stop bits.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#ifdef CONFIG_LINUX
#include <termios.h>

#define UART0           0xfe201000
#define UARTIBRD        0x24
#define UARTFBRD        0x28
#define UARTLCR_H       0x2c
#define UARTCR          0x30

#define LCR_PEN         (1 << 1)
#define LCR_EPS         (1 << 2)
#define LCR_STP2        (1 << 3)
#define LCR_FEN         (1 << 4)
#define LCR_WLEN(bits)  (((bits) - 5) << 5)
#define CR_UART_ON      0x301   /* UARTEN, TXE, RXE */

/* The PL011 clock is 48 MHz; LCR_H latches the divisor */
static void uart_setup(QTestState *qts, uint32_t ibrd, uint32_t fbrd,
                       uint32_t lcr)
{
    qtest_writel(qts, UART0 + UARTCR, 0);
    qtest_writel(qts, UART0 + UARTIBRD, ibrd);
    qtest_writel(qts, UART0 + UARTFBRD, fbrd);
    qtest_writel(qts, UART0 + UARTLCR_H, lcr);
    qtest_writel(qts, UART0 + UARTCR, CR_UART_ON);
}

static void test_line_settings(void)
{
    static const struct {
        uint32_t ibrd, fbrd, lcr;
        speed_t speed;
        tcflag_t cstopb;
    } cases[] = {
        /* 115176 baud, 8N1 */
        { 26, 3, LCR_FEN | LCR_WLEN(8), B115200, 0 },
        /* 57605 baud, 7E2 */
        { 52, 5, LCR_FEN | LCR_WLEN(7) | LCR_PEN | LCR_EPS | LCR_STP2,
          B57600, CSTOPB },
        /* 9600 baud, 8O1 */
        { 312, 32, LCR_FEN | LCR_WLEN(8) | LCR_PEN, B9600, 0 },
    };
    QTestState *qts;
    struct termios t;
    int master, slave;

    master = posix_openpt(O_RDWR | O_NOCTTY);
    g_assert_cmpint(master, >=, 0);
    g_assert_cmpint(grantpt(master), ==, 0);
    g_assert_cmpint(unlockpt(master), ==, 0);
    slave = open(ptsname(master), O_RDWR | O_NOCTTY);
    g_assert_cmpint(slave, >=, 0);

    qts = qtest_initf("-M raspi4b -chardev serial,id=s0,path=%s "
                      "-serial chardev:s0", ptsname(master));

    for (int i = 0; i < ARRAY_SIZE(cases); i++) {
        uart_setup(qts, cases[i].ibrd, cases[i].fbrd, cases[i].lcr);
        g_assert_cmpint(tcgetattr(slave, &t), ==, 0);
        g_assert_cmpuint(cfgetospeed(&t), ==, cases[i].speed);
        g_assert_cmphex(t.c_cflag & CSTOPB, ==, cases[i].cstopb);
    }

    qtest_quit(qts);
    close(slave);
    close(master);
}
#endif

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
#ifdef CONFIG_LINUX
    qtest_add_func("/bcm2838-uart/pl011/line-settings", test_line_settings);
#endif
    return g_test_run();
}
