/*
 * QTest testcase for the BCM2838 UARTs
 *
 * UART0 (PL011) on a host serial port: the guest's line settings reach
 * the host tty. A pty stands in for the USB serial adapter; Linux keeps a
 * pty at CS8 without parity, so it carries the rate and the stop bits.
 *
 * CTS flow control (has-cts): in loopback mode CR.RTS drives CTS, so the
 * TX FIFO is held until the guest raises RTS.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define UART0           0xfe201000
#define UARTDR          0x00
#define UARTFR          0x18
#define UARTIBRD        0x24
#define UARTFBRD        0x28
#define UARTLCR_H       0x2c
#define UARTCR          0x30
#define UARTRIS         0x3c
#define UARTICR         0x44

#define FR_TXFE         (1 << 7)
#define FR_RXFE         (1 << 4)
#define FR_BUSY         (1 << 3)
#define FR_CTS          (1 << 0)

#define LCR_PEN         (1 << 1)
#define LCR_EPS         (1 << 2)
#define LCR_STP2        (1 << 3)
#define LCR_FEN         (1 << 4)
#define LCR_WLEN(bits)  (((bits) - 5) << 5)

#define CR_UART_ON      0x301   /* UARTEN, TXE, RXE */
#define CR_LBE          (1 << 7)
#define CR_RTS          (1 << 11)
#define CR_CTSEN        (1 << 15)

#define INT_CTS         (1 << 1)

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

static uint32_t uart_fr(QTestState *qts)
{
    return qtest_readl(qts, UART0 + UARTFR);
}

static void test_cts_flow_control(void)
{
    QTestState *qts = qtest_init("-M raspi4b -global pl011.has-cts=on");
    uint32_t cr = CR_UART_ON | CR_LBE | CR_CTSEN;

    uart_setup(qts, 26, 3, LCR_FEN | LCR_WLEN(8));
    qtest_writel(qts, UART0 + UARTCR, cr);
    qtest_writel(qts, UART0 + UARTICR, 0x7ff);
    g_assert_cmphex(uart_fr(qts) & FR_CTS, ==, 0);

    /* CTS deasserted: the bytes stay in the TX FIFO */
    qtest_writel(qts, UART0 + UARTDR, 'a');
    qtest_writel(qts, UART0 + UARTDR, 'b');
    g_assert_cmphex(uart_fr(qts) & (FR_TXFE | FR_RXFE | FR_BUSY), ==,
                    FR_RXFE | FR_BUSY);

    /* RTS asserts CTS through the loopback: the FIFO drains */
    qtest_writel(qts, UART0 + UARTCR, cr | CR_RTS);
    g_assert_cmphex(uart_fr(qts) & (FR_TXFE | FR_RXFE | FR_BUSY | FR_CTS),
                    ==, FR_TXFE | FR_CTS);
    g_assert_cmphex(qtest_readl(qts, UART0 + UARTRIS) & INT_CTS, ==,
                    INT_CTS);
    g_assert_cmphex(qtest_readl(qts, UART0 + UARTDR) & 0xff, ==, 'a');
    g_assert_cmphex(qtest_readl(qts, UART0 + UARTDR) & 0xff, ==, 'b');
    g_assert_cmphex(uart_fr(qts) & FR_RXFE, ==, FR_RXFE);

    /* Without CTSEN the UART sends while CTS is deasserted */
    qtest_writel(qts, UART0 + UARTCR, CR_UART_ON | CR_LBE);
    qtest_writel(qts, UART0 + UARTDR, 'c');
    g_assert_cmphex(uart_fr(qts) & (FR_TXFE | FR_RXFE | FR_CTS), ==,
                    FR_TXFE);
    g_assert_cmphex(qtest_readl(qts, UART0 + UARTDR) & 0xff, ==, 'c');

    qtest_quit(qts);
}

#ifdef CONFIG_LINUX
#include <termios.h>

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
    qtest_add_func("/bcm2838-uart/pl011/cts-flow-control",
                   test_cts_flow_control);
    return g_test_run();
}
