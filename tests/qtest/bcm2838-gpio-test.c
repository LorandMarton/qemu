/*
 * QTest testcase for the BCM2838 (BCM2711) GPIO controller
 *
 * Pin levels from the pulls, external drive ("gpioN" properties) and the
 * output latch; edge and level event detection and the bank interrupts;
 * the reset state from "pins"; the SPI chip enables through the pin mux.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qapi/error.h"

#define GPIO_BASE       0xfe200000
#define GPFSEL0         0x00
#define GPSET0          0x1c
#define GPCLR0          0x28
#define GPLEV0          0x34
#define GPLEV1          0x38
#define GPEDS0          0x40
#define GPEDS1          0x44
#define GPREN0          0x4c
#define GPREN1          0x50
#define GPFEN0          0x58
#define GPHEN0          0x64
#define GPLEN0          0x70
#define GPAREN0         0x7c
#define GPIO_PUP_PDN0   0xe4

#define FSEL_INPUT      0
#define FSEL_OUTPUT     1
#define FSEL_ALT0       4
#define FSEL_ALT3       7

#define PULL_NONE       0
#define PULL_UP         1
#define PULL_DOWN       2

#define GPIO_PATH       "/machine/soc/peripherals/gpio"

/* Interrupt lines: bank 0 (GPIO 0-27), bank 1 (28-45), bank 2, any bank */
#define IRQ_BANK0       0
#define IRQ_BANK1       1
#define IRQ_ANY         3

static QTestState *gpio_init(void)
{
    QTestState *qts = qtest_init("-M raspi4b");

    qtest_irq_intercept_out_named(qts, GPIO_PATH, "sysbus-irq");
    return qts;
}

static void set_fsel(QTestState *qts, int pin, uint32_t fsel)
{
    uint32_t reg = GPIO_BASE + GPFSEL0 + (pin / 10) * 4;
    uint32_t val = qtest_readl(qts, reg);

    val = deposit32(val, (pin % 10) * 3, 3, fsel);
    qtest_writel(qts, reg, val);
}

static void set_pull(QTestState *qts, int pin, uint32_t pull)
{
    uint32_t reg = GPIO_BASE + GPIO_PUP_PDN0 + (pin / 16) * 4;
    uint32_t val = qtest_readl(qts, reg);

    val = deposit32(val, (pin % 16) * 2, 2, pull);
    qtest_writel(qts, reg, val);
}

static void set_drive(QTestState *qts, int pin, const char *value)
{
    g_autofree char *prop = g_strdup_printf("gpio%d", pin);
    QDict *resp;

    resp = qtest_qmp(qts, "{ 'execute': 'qom-set', 'arguments': "
                     "{ 'path': %s, 'property': %s, 'value': %s } }",
                     GPIO_PATH, prop, value);
    g_assert(qdict_haskey(resp, "return"));
    qobject_unref(resp);
}

static bool level(QTestState *qts, int pin)
{
    uint32_t reg = GPIO_BASE + (pin < 32 ? GPLEV0 : GPLEV1);

    return qtest_readl(qts, reg) & BIT(pin % 32);
}

static void test_pull(void)
{
    QTestState *qts = gpio_init();

    set_fsel(qts, 5, FSEL_INPUT);
    set_pull(qts, 5, PULL_UP);
    g_assert_true(level(qts, 5));
    set_pull(qts, 5, PULL_DOWN);
    g_assert_false(level(qts, 5));
    set_pull(qts, 5, PULL_NONE);
    g_assert_false(level(qts, 5));

    /* Pins of the second register */
    set_pull(qts, 40, PULL_UP);
    g_assert_true(level(qts, 40));

    qtest_quit(qts);
}

static void test_drive(void)
{
    QTestState *qts = gpio_init();
    QDict *resp;

    set_fsel(qts, 5, FSEL_INPUT);
    set_pull(qts, 5, PULL_DOWN);
    set_drive(qts, 5, "high");
    g_assert_true(level(qts, 5));

    resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                     "{ 'path': %s, 'property': 'gpio5' } }", GPIO_PATH);
    g_assert_cmpstr(qdict_get_str(resp, "return"), ==, "high");
    qobject_unref(resp);

    set_pull(qts, 5, PULL_UP);
    set_drive(qts, 5, "low");
    g_assert_false(level(qts, 5));

    /* Released: back to the pull */
    set_drive(qts, 5, "none");
    g_assert_true(level(qts, 5));

    /* The drive is outside the SoC: a reset leaves the pin driven */
    set_drive(qts, 5, "high");
    qtest_system_reset(qts);
    set_pull(qts, 5, PULL_DOWN);
    g_assert_true(level(qts, 5));

    resp = qtest_qmp(qts, "{ 'execute': 'qom-set', 'arguments': "
                     "{ 'path': %s, 'property': 'gpio5', 'value': 'on' } }",
                     GPIO_PATH);
    g_assert(qdict_haskey(resp, "error"));
    qobject_unref(resp);

    qtest_quit(qts);
}

static void test_output(void)
{
    QTestState *qts = gpio_init();

    set_drive(qts, 7, "low");
    set_fsel(qts, 7, FSEL_OUTPUT);
    qtest_writel(qts, GPIO_BASE + GPSET0, BIT(7));
    g_assert_true(level(qts, 7));
    qtest_writel(qts, GPIO_BASE + GPCLR0, BIT(7));
    g_assert_false(level(qts, 7));

    /* An input reads the pin again, not the latch */
    qtest_writel(qts, GPIO_BASE + GPSET0, BIT(7));
    set_fsel(qts, 7, FSEL_INPUT);
    g_assert_false(level(qts, 7));

    qtest_quit(qts);
}

/*
 * GPIO8 in ALT0 is SPI0 CE0: the pin reads the controller's chip enable,
 * and chip select 0 follows the pin, also as a GPIO (Linux "cs-gpios").
 */
static void test_spi_ce(void)
{
    QTestState *qts = qtest_init("-M raspi4b");

    qtest_irq_intercept_out_named(qts, GPIO_PATH, "spi-cs");

    set_fsel(qts, 8, FSEL_ALT0);
    g_assert_true(level(qts, 8));
    qtest_set_irq_in(qts, GPIO_PATH, "spi-ce", 0, 0);
    g_assert_false(level(qts, 8));
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_set_irq_in(qts, GPIO_PATH, "spi-ce", 0, 1);
    g_assert_true(level(qts, 8));
    g_assert_true(qtest_get_irq(qts, 0));

    set_fsel(qts, 8, FSEL_OUTPUT);
    qtest_writel(qts, GPIO_BASE + GPCLR0, BIT(8));
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_writel(qts, GPIO_BASE + GPSET0, BIT(8));
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_edge_irq(void)
{
    QTestState *qts = gpio_init();

    set_fsel(qts, 5, FSEL_INPUT);
    set_pull(qts, 5, PULL_DOWN);
    qtest_writel(qts, GPIO_BASE + GPEDS0, 0xffffffff);
    qtest_writel(qts, GPIO_BASE + GPREN0, BIT(5));

    set_drive(qts, 5, "high");
    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPEDS0), ==, BIT(5));
    g_assert_true(qtest_get_irq(qts, IRQ_BANK0));
    g_assert_true(qtest_get_irq(qts, IRQ_ANY));
    g_assert_false(qtest_get_irq(qts, IRQ_BANK1));

    /* Write 1 to clear */
    qtest_writel(qts, GPIO_BASE + GPEDS0, BIT(5));
    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPEDS0), ==, 0);
    g_assert_false(qtest_get_irq(qts, IRQ_BANK0));
    g_assert_false(qtest_get_irq(qts, IRQ_ANY));

    /* A falling edge is not detected without GPFEN */
    set_drive(qts, 5, "low");
    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPEDS0), ==, 0);
    qtest_writel(qts, GPIO_BASE + GPFEN0, BIT(5));
    set_drive(qts, 5, "high");
    set_drive(qts, 5, "low");
    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPEDS0), ==, BIT(5));
    qtest_writel(qts, GPIO_BASE + GPEDS0, BIT(5));

    /* Asynchronous rising edge, bank 1 */
    set_fsel(qts, 34, FSEL_INPUT);
    set_pull(qts, 34, PULL_DOWN);
    qtest_writel(qts, GPIO_BASE + GPREN1, 0);
    qtest_writel(qts, GPIO_BASE + GPAREN0 + 4, BIT(34 - 32));
    set_drive(qts, 34, "high");
    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPEDS1), ==, BIT(34 - 32));
    g_assert_true(qtest_get_irq(qts, IRQ_BANK1));
    g_assert_false(qtest_get_irq(qts, IRQ_BANK0));
    qtest_writel(qts, GPIO_BASE + GPEDS1, BIT(34 - 32));
    g_assert_false(qtest_get_irq(qts, IRQ_BANK1));

    qtest_quit(qts);
}

static void test_level_detect(void)
{
    QTestState *qts = gpio_init();

    set_fsel(qts, 9, FSEL_INPUT);
    set_pull(qts, 9, PULL_UP);
    qtest_writel(qts, GPIO_BASE + GPEDS0, 0xffffffff);

    /* High level detect latches again while the level persists */
    qtest_writel(qts, GPIO_BASE + GPHEN0, BIT(9));
    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPEDS0), ==, BIT(9));
    qtest_writel(qts, GPIO_BASE + GPEDS0, BIT(9));
    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPEDS0), ==, BIT(9));
    g_assert_true(qtest_get_irq(qts, IRQ_BANK0));

    /* Low level detect */
    qtest_writel(qts, GPIO_BASE + GPHEN0, 0);
    qtest_writel(qts, GPIO_BASE + GPEDS0, BIT(9));
    g_assert_false(qtest_get_irq(qts, IRQ_BANK0));
    qtest_writel(qts, GPIO_BASE + GPLEN0, BIT(9));
    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPEDS0), ==, 0);
    set_drive(qts, 9, "low");
    g_assert_cmphex(qtest_readl(qts, GPIO_BASE + GPEDS0), ==, BIT(9));

    qtest_quit(qts);
}


/*
 * "pins" on raspicm4: the user's entries apply after the board's, at every
 * reset; the board's other pins stay as the CM4 firmware leaves them.
 */
static void test_pins(void)
{
    QTestState *qts = qtest_init("-M raspicm4 -global "
                                 "bcm2838-gpio.pins=19:op:pd:hi,20:ip:pu:hi,"
                                 "21:ip:pd:hi,19:op:pd:lo,19:op:pn:hi");

    for (int i = 0; i < 2; i++) {
        uint32_t fsel1 = qtest_readl(qts, GPIO_BASE + GPFSEL0 + 4);
        uint32_t pup1 = qtest_readl(qts, GPIO_BASE + GPIO_PUP_PDN0 + 4);
        uint32_t pup2 = qtest_readl(qts, GPIO_BASE + GPIO_PUP_PDN0 + 8);
        uint32_t lev0 = qtest_readl(qts, GPIO_BASE + GPLEV0);

        /* 19: the last entry; 20: its pull; 21: driven by the board */
        g_assert_cmpuint(extract32(fsel1, 9 * 3, 3), ==, FSEL_OUTPUT);
        g_assert_cmpuint(extract32(pup1, 3 * 2, 2), ==, PULL_NONE);
        g_assert_cmpuint(extract32(pup1, 4 * 2, 2), ==, PULL_UP);
        g_assert_cmpuint(extract32(pup1, 5 * 2, 2), ==, PULL_DOWN);
        g_assert_cmphex(lev0 & (BIT(19) | BIT(20) | BIT(21) | BIT(22)), ==,
                        BIT(19) | BIT(20) | BIT(21));
        /* the board's: 34 in ALT3 (SD1, the wireless chip), no pull */
        g_assert_cmpuint(extract32(qtest_readl(qts, GPIO_BASE + GPFSEL0 + 12),
                                   4 * 3, 3), ==, FSEL_ALT3);
        g_assert_cmpuint(extract32(pup2, 2 * 2, 2), ==, PULL_NONE);

        set_fsel(qts, 19, FSEL_INPUT);
        set_pull(qts, 20, PULL_DOWN);
        qtest_system_reset(qts);
    }
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/bcm2838-gpio/pull", test_pull);
    qtest_add_func("/bcm2838-gpio/drive", test_drive);
    qtest_add_func("/bcm2838-gpio/output", test_output);
    qtest_add_func("/bcm2838-gpio/edge-irq", test_edge_irq);
    qtest_add_func("/bcm2838-gpio/level-detect", test_level_detect);
    qtest_add_func("/bcm2838-gpio/pins", test_pins);
    qtest_add_func("/bcm2838-gpio/spi-ce", test_spi_ce);

    return g_test_run();
}
