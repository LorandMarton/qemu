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
 * The mini UART divisor latch, and its rate on a host serial port.
 *
 * UART2..UART5: 0x200 apart, on -serial 3..6, on UART0's interrupt.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define UART0           0xfe201000
#define UART2           0xfe201400
#define UART_STRIDE     0x200
#define UARTDR          0x00
#define UARTFR          0x18
#define UARTIBRD        0x24
#define UARTFBRD        0x28
#define UARTLCR_H       0x2c
#define UARTCR          0x30
#define UARTIMSC        0x38
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
#define INT_TX          (1 << 5)

/* GIC distributor; UART0 and UART2..UART5 share SPI 121 (ID 153) */
#define GICD            0xff841000
#define GICD_ISPENDR    0x200
#define GICD_ICPENDR    0x280
#define UART_IRQ_ID     (32 + 121)

#define SDHOST_SDEDM    0xfe202034

#define AUX_BASE        0xfe215000
#define AUX_MU_IO       0x40
#define AUX_MU_IER      0x44
#define AUX_MU_LCR      0x4c
#define AUX_MU_MSR      0x58
#define AUX_MU_STAT     0x64
#define AUX_MU_BAUD     0x68
#define MU_LCR_DLAB     BIT(7)
#define MU_LCR_8BIT     3
#define MU_MSR_CTS      BIT(4)
#define MU_STAT_CTS     BIT(7)

/* -serial order of the machine: UART0, mini UART, UART2..UART5 */
static QTestState *uart_init(void)
{
    return qtest_init("-M raspi4b "
                      "-chardev ringbuf,id=mu -chardev ringbuf,id=u3 "
                      "-serial null -serial chardev:mu "
                      "-serial null -serial chardev:u3");
}

static char *ringbuf_read(QTestState *qts, const char *id)
{
    QDict *resp;
    char *data;

    resp = qtest_qmp(qts, "{ 'execute': 'ringbuf-read', 'arguments': "
                     "{ 'device': %s, 'size': 64, 'format': 'utf8' } }", id);
    data = g_strdup(qdict_get_str(resp, "return"));
    qobject_unref(resp);
    return data;
}

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

static bool uart_irq_pending(QTestState *qts)
{
    uint32_t reg = GICD + GICD_ISPENDR + UART_IRQ_ID / 32 * 4;

    return qtest_readl(qts, reg) & BIT(UART_IRQ_ID % 32);
}

static void test_extra_uarts(void)
{
    QTestState *qts = qtest_init("-M raspi4b -serial null -serial null "
                                 "-chardev ringbuf,id=u2 -serial chardev:u2 "
                                 "-chardev ringbuf,id=u3 -serial chardev:u3 "
                                 "-chardev ringbuf,id=u4 -serial chardev:u4 "
                                 "-chardev ringbuf,id=u5 -serial chardev:u5");

    for (int n = 2; n <= 5; n++) {
        uint32_t base = UART2 + (n - 2) * UART_STRIDE;
        g_autofree char *id = g_strdup_printf("u%d", n);
        g_autofree char *sent = g_strdup_printf("%d", n);
        g_autofree char *out = NULL;

        qtest_writel(qts, GICD + GICD_ICPENDR + UART_IRQ_ID / 32 * 4,
                     BIT(UART_IRQ_ID % 32));
        g_assert_false(uart_irq_pending(qts));

        qtest_writel(qts, base + UARTCR, CR_UART_ON);
        qtest_writel(qts, base + UARTIMSC, INT_TX);
        qtest_writel(qts, base + UARTDR, sent[0]);
        out = ringbuf_read(qts, id);
        g_assert_cmpstr(out, ==, sent);
        g_assert_true(uart_irq_pending(qts));

        qtest_writel(qts, base + UARTIMSC, 0);
    }

    /* SDHOST follows UART5's window */
    g_assert_cmphex(qtest_readl(qts, SDHOST_SDEDM), ==, 0xc60f);

    qtest_quit(qts);
}

static void test_aux_dlab(void)
{
    QTestState *qts = uart_init();
    g_autofree char *out1 = NULL;
    g_autofree char *out2 = NULL;

    /* Divisor latch access: IO and IER hold the baud rate register */
    qtest_writel(qts, AUX_BASE + AUX_MU_LCR, MU_LCR_DLAB | MU_LCR_8BIT);
    qtest_writel(qts, AUX_BASE + AUX_MU_IO, 0x0e);
    qtest_writel(qts, AUX_BASE + AUX_MU_IER, 0x01);
    g_assert_cmphex(qtest_readl(qts, AUX_BASE + AUX_MU_IO), ==, 0x0e);
    g_assert_cmphex(qtest_readl(qts, AUX_BASE + AUX_MU_IER), ==, 0x01);
    qtest_writel(qts, AUX_BASE + AUX_MU_LCR, MU_LCR_8BIT);
    g_assert_cmphex(qtest_readl(qts, AUX_BASE + AUX_MU_BAUD), ==, 0x010e);
    g_assert_cmphex(qtest_readl(qts, AUX_BASE + AUX_MU_LCR), ==, MU_LCR_8BIT);
    out1 = ringbuf_read(qts, "mu");
    g_assert_cmpstr(out1, ==, "");

    qtest_writel(qts, AUX_BASE + AUX_MU_IO, 'x');
    out2 = ringbuf_read(qts, "mu");
    g_assert_cmpstr(out2, ==, "x");

    qtest_quit(qts);
}

#ifdef CONFIG_LINUX
#include <termios.h>

/* A pty for -chardev serial: returns the master, *slave is the tty */
static int pty_open(int *slave)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);

    g_assert_cmpint(master, >=, 0);
    g_assert_cmpint(grantpt(master), ==, 0);
    g_assert_cmpint(unlockpt(master), ==, 0);
    *slave = open(ptsname(master), O_RDWR | O_NOCTTY);
    g_assert_cmpint(*slave, >=, 0);
    return master;
}

static speed_t pty_speed(int slave)
{
    struct termios t;

    g_assert_cmpint(tcgetattr(slave, &t), ==, 0);
    return cfgetospeed(&t);
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

    master = pty_open(&slave);
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

/* rate = 500 MHz VPU clock / 8 (BAUD + 1), nearest standard rate */
static void test_aux_host_line(void)
{
    QTestState *qts;
    int master, slave;

    master = pty_open(&slave);
    qts = qtest_initf("-M raspi4b -chardev serial,id=mu,path=%s "
                      "-serial null -serial chardev:mu", ptsname(master));

    /* 115313 baud through the BAUD register */
    qtest_writel(qts, AUX_BASE + AUX_MU_LCR, MU_LCR_8BIT);
    qtest_writel(qts, AUX_BASE + AUX_MU_BAUD, 541);
    g_assert_cmpuint(pty_speed(slave), ==, B115200);

    /* 9600 baud through the divisor latch, set when DLAB clears */
    qtest_writel(qts, AUX_BASE + AUX_MU_LCR, MU_LCR_DLAB | MU_LCR_8BIT);
    qtest_writel(qts, AUX_BASE + AUX_MU_IO, 6509 & 0xff);
    qtest_writel(qts, AUX_BASE + AUX_MU_IER, 6509 >> 8);
    g_assert_cmpuint(pty_speed(slave), ==, B115200);
    qtest_writel(qts, AUX_BASE + AUX_MU_LCR, MU_LCR_8BIT);
    g_assert_cmpuint(pty_speed(slave), ==, B9600);

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
    qtest_add_func("/bcm2838-uart/aux/host-line", test_aux_host_line);
#endif
    qtest_add_func("/bcm2838-uart/pl011/cts-flow-control",
                   test_cts_flow_control);
    qtest_add_func("/bcm2838-uart/aux/dlab", test_aux_dlab);
    qtest_add_func("/bcm2838-uart/extra-uarts", test_extra_uarts);
    return g_test_run();
}
