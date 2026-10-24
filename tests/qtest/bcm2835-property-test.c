/*
 * QTest testcase for the BCM2835 mailbox property channel
 *
 * The firmware property tags of the BCM2711 firmware model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define MBOX_BASE           0xfe00b880
#define MBOX0_READ          0x00
#define MBOX0_STATUS        0x18
#define MBOX1_WRITE         0x20
#define MBOX_EMPTY          (1u << 30)
#define MBOX_CHAN_PROPERTY  8

#define BUF_ADDR            0x10000
#define CODE_RESPONSE       0x80000000

#define TAG_FW_REVISION     0x00000001
#define TAG_FW_VARIANT      0x00000002
#define TAG_FW_HASH         0x00000003
#define TAG_SET_CLOCK_RATE  0x00038002
#define CLK_ARM             3
#define CLK_CORE            4
#define TAG_GET_GPIO_STATE  0x00030041
#define TAG_SET_GPIO_STATE  0x00038041
#define TAG_GET_GPIO_CONFIG 0x00030043
#define TAG_SET_GPIO_CONFIG 0x00038043

/* start4.elf of 2026-09-14, as a CM4 reports it */
#define FW_REVISION         1789397370
#define FW_VERSION          "a86983925695a7e63166327d7c002d64040ed31d (clean)"
#define FW_ARGS             "-M raspi4b " \
    "-global bcm2835-property.firmware-revision=1789397370 " \
    "-global bcm2835-property.firmware-variant=1 " \
    "-global bcm2835-property.firmware-version=\"" FW_VERSION "\""

/* One tag with a value buffer of BUFSIZE bytes; returns its response length */
static uint32_t property(QTestState *qts, uint32_t tag, uint32_t bufsize)
{
    uint32_t size = 4 * 6 + bufsize;

    qtest_writel(qts, BUF_ADDR, size);
    qtest_writel(qts, BUF_ADDR + 4, 0);
    qtest_writel(qts, BUF_ADDR + 8, tag);
    qtest_writel(qts, BUF_ADDR + 12, bufsize);
    qtest_writel(qts, BUF_ADDR + 16, 0);
    qtest_writel(qts, BUF_ADDR + 20 + bufsize, 0);

    qtest_writel(qts, MBOX_BASE + MBOX1_WRITE, BUF_ADDR | MBOX_CHAN_PROPERTY);
    g_assert_cmphex(qtest_readl(qts, MBOX_BASE + MBOX0_STATUS) & MBOX_EMPTY,
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, MBOX_BASE + MBOX0_READ), ==,
                    BUF_ADDR | MBOX_CHAN_PROPERTY);
    g_assert_cmphex(qtest_readl(qts, BUF_ADDR + 4), ==, CODE_RESPONSE);
    g_assert_cmphex(qtest_readl(qts, BUF_ADDR + 16) & CODE_RESPONSE, ==,
                    CODE_RESPONSE);
    return qtest_readl(qts, BUF_ADDR + 16) & ~CODE_RESPONSE;
}

static void test_default(void)
{
    QTestState *qts = qtest_init("-M raspi4b");

    g_assert_cmpuint(property(qts, TAG_FW_REVISION, 4), ==, 4);
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 20), ==, 346337);
    /* "unknown" variant, no hash */
    g_assert_cmpuint(property(qts, TAG_FW_VARIANT, 4), ==, 4);
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 20), ==, 0);
    g_assert_cmpuint(property(qts, TAG_FW_HASH, 20), ==, 20);
    for (int i = 0; i < 5; i++) {
        g_assert_cmphex(qtest_readl(qts, BUF_ADDR + 20 + 4 * i), ==, 0);
    }
    qtest_quit(qts);
}

static void test_build(void)
{
    static const uint32_t hash[5] = {
        0xa8698392, 0x5695a7e6, 0x3166327d, 0x7c002d64, 0x040ed31d,
    };
    QTestState *qts = qtest_init(FW_ARGS);

    property(qts, TAG_FW_REVISION, 4);
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 20), ==, FW_REVISION);
    /* start4.elf */
    property(qts, TAG_FW_VARIANT, 4);
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 20), ==, 1);
    property(qts, TAG_FW_HASH, 20);
    for (int i = 0; i < 5; i++) {
        g_assert_cmphex(qtest_readl(qts, BUF_ADDR + 20 + 4 * i), ==, hash[i]);
    }
    qtest_quit(qts);
}

/* SET_CLOCK_RATE of the core clock to 200 MHz; returns the rate set */
static uint32_t set_core_clock(QTestState *qts)
{
    qtest_writel(qts, BUF_ADDR + 20, CLK_CORE);
    qtest_writel(qts, BUF_ADDR + 24, 200000000);
    qtest_writel(qts, BUF_ADDR + 28, 0);
    property(qts, TAG_SET_CLOCK_RATE, 12);
    return qtest_readl(qts, BUF_ADDR + 24);
}

static void test_core_clock(void)
{
    g_autofree char *path = NULL;
    QTestState *qts;
    int fd;

    /* the firmware lowers it on request */
    qts = qtest_init("-M raspi4b");
    g_assert_cmpuint(set_core_clock(qts), ==, 200000000);
    qtest_quit(qts);

    /* with enable_uart=1 it keeps it at the boot rate (mini UART baud) */
    fd = g_file_open_tmp("vc-config-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_true(g_file_set_contents(path, "core_freq=500\nenable_uart=1\n",
                                      -1, NULL));
    close(fd);
    qts = qtest_initf("-M raspi4b -global bcm2835-property.vc-config=%s",
                      path);
    g_assert_cmpuint(set_core_clock(qts), ==, 500000000);
    qtest_quit(qts);
    unlink(path);
}

/*
 * The firmware GPIO expander, lines 128..135: the first word of the
 * response is 0 for a line it has, the others its state or configuration.
 */
static void test_gpio_expander(void)
{
    QTestState *qts = qtest_init("-M raspi4b");

    /* 130: an output, active high, driven high */
    qtest_writel(qts, BUF_ADDR + 20, 130);
    qtest_writel(qts, BUF_ADDR + 24, 1);
    qtest_writel(qts, BUF_ADDR + 28, 0);
    qtest_writel(qts, BUF_ADDR + 32, 0);
    qtest_writel(qts, BUF_ADDR + 36, 0);
    qtest_writel(qts, BUF_ADDR + 40, 1);
    property(qts, TAG_SET_GPIO_CONFIG, 24);
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 20), ==, 0);

    qtest_writel(qts, BUF_ADDR + 20, 130);
    g_assert_cmpuint(property(qts, TAG_GET_GPIO_STATE, 8), ==, 8);
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 20), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 24), ==, 1);

    qtest_writel(qts, BUF_ADDR + 20, 130);
    qtest_writel(qts, BUF_ADDR + 24, 0);
    property(qts, TAG_SET_GPIO_STATE, 8);
    qtest_writel(qts, BUF_ADDR + 20, 130);
    property(qts, TAG_GET_GPIO_STATE, 8);
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 24), ==, 0);

    qtest_writel(qts, BUF_ADDR + 20, 130);
    g_assert_cmpuint(property(qts, TAG_GET_GPIO_CONFIG, 20), ==, 20);
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 20), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 24), ==, 1);

    /* no line 136 */
    qtest_writel(qts, BUF_ADDR + 20, 136);
    property(qts, TAG_GET_GPIO_STATE, 8);
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 20), ==, 136);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/bcm2835-property/firmware/default", test_default);
    qtest_add_func("/bcm2835-property/firmware/build", test_build);
    qtest_add_func("/bcm2835-property/core-clock", test_core_clock);
    qtest_add_func("/bcm2835-property/gpio-expander", test_gpio_expander);

    return g_test_run();
}
