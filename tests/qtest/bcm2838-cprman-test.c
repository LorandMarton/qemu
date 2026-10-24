/*
 * QTest testcase for the BCM2711 clock manager (bcm2711-cprman)
 *
 * The clocks at reset are as the CM4 firmware leaves them, and the rates the
 * guest computes from the registers with the 54 MHz crystal are the rates
 * QEMU clocks its devices with: the PL011 UART clock is 48 MHz both ways.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "libqtest.h"

#define CPRMAN_BASE     0xfe101000
#define CM_TIMERCTL     0x0e8
#define CM_TIMERDIV     0x0ec
#define CM_UARTCTL      0x0f0
#define CM_UARTDIV      0x0f4
#define CM_DSI1PCTL     0x160
#define CM_SDCCTL       0x1a8
#define CM_EMMC2CTL     0x1d0
#define CM_EMMC2DIV     0x1d4
#define A2W_PLLD_CTRL   0x1140
#define A2W_PLLB_CTRL   0x11e0
#define A2W_PLLD_FRAC   0x1240
#define A2W_PLLD_PER    0x1540

#define XOSC_HZ         54000000ULL
#define CM_SRC_PLLD     6
#define CM_ENAB         BIT(4)

static uint32_t cm_read(QTestState *qts, uint32_t reg)
{
    return qtest_readl(qts, CPRMAN_BASE + reg);
}

/* The UART clock as Linux's clk-bcm2835 computes it on a BCM2711 */
static uint64_t guest_uart_hz(QTestState *qts)
{
    uint32_t ctrl = cm_read(qts, A2W_PLLD_CTRL);
    uint32_t frac = cm_read(qts, A2W_PLLD_FRAC);
    uint32_t per = extract32(cm_read(qts, A2W_PLLD_PER), 0, 8) ?: 256;
    uint32_t div = cm_read(qts, CM_UARTDIV);
    uint64_t plld;

    g_assert_cmpuint(extract32(cm_read(qts, CM_UARTCTL), 0, 4), ==,
                     CM_SRC_PLLD);
    /* no feedback pre-divider on BCM2711 */
    plld = (XOSC_HZ * (((uint64_t)extract32(ctrl, 0, 10) << 20) +
                       extract32(frac, 0, 20)) /
            extract32(ctrl, 12, 3)) >> 20;
    /* the divider has 12 fractional bits */
    return ((plld / per) << 12) / div;
}

/* The frequency of a clock output of the cprman, from "info qtree" */
static uint64_t qemu_clock_hz(QTestState *qts, const char *clock)
{
    g_autofree char *tree = qtest_hmp(qts, "info qtree");
    g_autofree char *key = g_strdup_printf("\"%s\" freq_hz=", clock);
    const char *p = strstr(tree, key);
    double v;
    char unit[4];

    g_assert_nonnull(p);
    g_assert_cmpint(sscanf(p + strlen(key), "%lf %3s", &v, unit), ==, 2);
    g_assert_cmpstr(unit, ==, "MHz");
    return v * 1000000;
}

static void test_reset(void)
{
    QTestState *qts = qtest_init("-M raspi4b");

    for (int i = 0; i < 2; i++) {
        /* PLLD 3 GHz, PLLB 1.8 GHz (the ARM at 600 MHz) */
        g_assert_cmphex(cm_read(qts, A2W_PLLD_CTRL), ==, 0x00021037);
        g_assert_cmphex(cm_read(qts, A2W_PLLD_FRAC), ==, 0x0008e390);
        g_assert_cmphex(cm_read(qts, A2W_PLLB_CTRL), ==, 0x00021021);
        /* timer 1 MHz from the crystal, EMMC2 100 MHz from PLLD */
        g_assert_cmphex(cm_read(qts, CM_TIMERCTL), ==, 0x00000291);
        g_assert_cmphex(cm_read(qts, CM_TIMERDIV), ==, 0x00036000);
        g_assert_cmphex(cm_read(qts, CM_EMMC2CTL) & CM_ENAB, ==, CM_ENAB);
        g_assert_cmphex(cm_read(qts, CM_EMMC2DIV), ==, 0x00007800);
        /* DSI1 pixel clock enabled, from the DSI1 byte clock */
        g_assert_cmphex(cm_read(qts, CM_DSI1PCTL), ==, 0x00000098);
        /* the BCM2711 has no SDRAM clock: it reads "cm" */
        g_assert_cmphex(cm_read(qts, CM_SDCCTL), ==, 0x0000636d);

        /* a guest change (with the password) is undone by a reset */
        qtest_writel(qts, CPRMAN_BASE + CM_EMMC2DIV, 0x5a000000 | 0x3000);
        g_assert_cmphex(cm_read(qts, CM_EMMC2DIV), ==, 0x00003000);
        qtest_system_reset(qts);
    }
    qtest_quit(qts);
}

static void test_uart_clock(void)
{
    QTestState *qts = qtest_init("-M raspi4b");

    /* 48000001 Hz, as Linux prints it on the CM4: the PLL rounds down */
    g_assert_cmpuint(guest_uart_hz(qts) / 1000, ==, 48000);
    g_assert_cmpuint(qemu_clock_hz(qts, "uart-out") / 1000, ==, 48000);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/bcm2838-cprman/reset", test_reset);
    qtest_add_func("/bcm2838-cprman/uart-clock", test_uart_clock);

    return g_test_run();
}
