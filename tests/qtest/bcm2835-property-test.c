/*
 * QTest testcase for the BCM2835 mailbox property channel
 *
 * The firmware build identity: the revision, variant and hash tags and
 * "vcgencmd version", from the firmware-revision, -variant and -version
 * properties; "vcgencmd measure_temp" from the BCM2711 thermal sensor;
 * "vcgencmd measure_clock" as a CM4 measures its clocks; the displays
 * (none on raspicm4, as on a CM4 without a monitor); the core clock, fixed
 * with enable_uart=1.
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
#define TAG_GENCMD          0x00030080
#define TAG_GET_NUM_DISPLAYS 0x00040013
#define TAG_GET_DISPLAY_ID  0x00040016
#define DISPLAY_ID_HDMI0    2
#define TAG_SET_CLOCK_RATE  0x00038002
#define CLK_ARM             3
#define CLK_CORE            4
#define TAG_GET_GPIO_STATE  0x00030041
#define TAG_SET_GPIO_STATE  0x00038041
#define TAG_GET_GPIO_CONFIG 0x00030043
#define TAG_SET_GPIO_CONFIG 0x00038043
#define TAG_GET_DOMAIN_STATE 0x00030030
#define TAG_SET_DOMAIN_STATE 0x00038030

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

static void test_gencmd_version(void)
{
    /* a CM4's output, with the trailing space of the first line */
    static const char expect[] =
        "Sep 14 2026 14:49:30 " "\n"
        "Copyright (c) 2012 Broadcom\n"
        "version " FW_VERSION " (release) (start)";
    QTestState *qts = qtest_init(FW_ARGS);
    char out[sizeof(expect) + 1];
    uint32_t len;

    qtest_memwrite(qts, BUF_ADDR + 24, "version", 8);
    len = property(qts, TAG_GENCMD, 256);
    g_assert_cmpuint(len, ==, 4 + sizeof(expect));
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 20), ==, 0);
    qtest_memread(qts, BUF_ADDR + 24, out, sizeof(out));
    g_assert_cmpstr(out, ==, expect);
    qtest_quit(qts);
}

/* A gencmd and its output */
static void gencmd(QTestState *qts, const char *cmd, char *out, size_t size)
{
    qtest_memwrite(qts, BUF_ADDR + 24, cmd, strlen(cmd) + 1);
    property(qts, TAG_GENCMD, 256);
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 20), ==, 0);
    qtest_memread(qts, BUF_ADDR + 24, out, size);
    out[size - 1] = '\0';
}

#define THERMAL_STAT        0xfd5d2200
#define THERMAL_PATH        "/machine/soc/peripherals/thermal"

static bool set_temperature(QTestState *qts, int64_t mc)
{
    QDict *resp = qtest_qmp(qts, "{ 'execute': 'qom-set', 'arguments': "
                            "{ 'path': %s, 'property': 'temperature', "
                            "'value': %" PRId64 " } }", THERMAL_PATH, mc);
    bool ok = qdict_haskey(resp, "return");

    qobject_unref(resp);
    return ok;
}

static void test_gencmd_temp(void)
{
    /* sensor temperature, its code, what the firmware prints */
    static const struct {
        int64_t mc;
        uint32_t code;
        const char *out;
    } t[] = {
        { 25000, 791, "temp=24.8'C" },  /* the default */
        { 40894, 758, "temp=40.9'C" },  /* as read on a CM4 */
        { 41381, 757, "temp=41.3'C" },
        { 41500, 757, "temp=41.3'C" },
        { -20000, 883, "temp=-20.0'C" },
    };
    QTestState *qts = qtest_init("-M raspi4b");
    char out[64];

    for (int i = 0; i < ARRAY_SIZE(t); i++) {
        uint32_t stat;

        if (i) {
            g_assert_true(set_temperature(qts, t[i].mc));
        }
        stat = qtest_readl(qts, THERMAL_STAT);
        g_assert_cmpuint(stat & 0x3ff, ==, t[i].code);
        g_assert_cmphex(stat & 0x10400, ==, 0x10400);   /* valid */
        gencmd(qts, "measure_temp", out, sizeof(out));
        g_assert_cmpstr(out, ==, t[i].out);
    }

    /* what the 10 bits cannot tell */
    g_assert_false(set_temperature(qts, 410041));
    g_assert_false(set_temperature(qts, -88162));
    g_assert_true(set_temperature(qts, -88161));
    g_assert_cmpuint(qtest_readl(qts, THERMAL_STAT) & 0x3ff, ==, 0x3ff);

    /* environment, not SoC state: a reset keeps it */
    g_assert_true(set_temperature(qts, 60000));
    qtest_system_reset(qts);
    gencmd(qts, "measure_temp", out, sizeof(out));
    g_assert_cmpstr(out, ==, "temp=59.8'C");
    qtest_quit(qts);
}

static void test_gencmd_clock(void)
{
    /* as on a CM4 at idle, or the nearer of its two readings */
    static const char *const t[][2] = {
        { "arm", "frequency(48)=600169920" },
        { "core", "frequency(1)=500000992" },
        { "h264", "frequency(28)=0" },
        { "isp", "frequency(45)=0" },
        { "v3d", "frequency(46)=250000496" },
        { "uart", "frequency(22)=48001464" },
        { "pwm", "frequency(25)=0" },
        { "emmc", "frequency(50)=250000496" },
        { "pixel", "frequency(29)=75001464" },
        { "vec", "frequency(10)=0" },
        { "hdmi", "frequency(9)=119997072" },
        { "dpi", "frequency(4)=0" },
        { "emmc2", "frequency(51)=99997560" },
        { "pllc", "frequency(100)=2592000000" },
        { "pllb", "frequency(100)=1799999872" },
        { "ARM", "frequency(0)=0" },
    };
    QTestState *qts = qtest_init("-M raspi4b");
    char out[64];

    /* the boot rate, then the idle rate Linux sets */
    gencmd(qts, "measure_clock arm", out, sizeof(out));
    g_assert_cmpstr(out, ==, "frequency(48)=1500345728");
    qtest_writel(qts, BUF_ADDR + 20, CLK_ARM);
    qtest_writel(qts, BUF_ADDR + 24, 600000000);
    qtest_writel(qts, BUF_ADDR + 28, 0);
    property(qts, TAG_SET_CLOCK_RATE, 12);
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 24), ==, 600000000);

    for (int i = 0; i < ARRAY_SIZE(t); i++) {
        g_autofree char *cmd = g_strdup_printf("measure_clock %s", t[i][0]);

        gencmd(qts, cmd, out, sizeof(out));
        g_assert_cmpstr(out, ==, t[i][1]);
    }

    /* no clock named */
    qtest_memwrite(qts, BUF_ADDR + 24, "measure_clock", 14);
    property(qts, TAG_GENCMD, 256);
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 20), !=, 0);
    qtest_memread(qts, BUF_ADDR + 24, out, sizeof(out));
    out[sizeof(out) - 1] = '\0';
    g_assert_cmpstr(out, ==, "error=2 error_msg=\"Invalid arguments\"");
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

static uint32_t domain_state(QTestState *qts, uint32_t tag, uint32_t domain,
                             uint32_t state)
{
    qtest_writel(qts, BUF_ADDR + 20, domain);
    qtest_writel(qts, BUF_ADDR + 24, state);
    g_assert_cmpuint(property(qts, tag, 8), ==, 8);
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 20), ==, domain);
    return qtest_readl(qts, BUF_ADDR + 24);
}

/* The firmware power domains: off at reset, then as set; 32 does not exist */
static void test_power_domains(void)
{
    QTestState *qts = qtest_init("-M raspi4b");

    g_assert_cmpuint(domain_state(qts, TAG_GET_DOMAIN_STATE, 6, 0), ==, 0);
    g_assert_cmpuint(domain_state(qts, TAG_SET_DOMAIN_STATE, 6, 1), ==, 1);
    g_assert_cmpuint(domain_state(qts, TAG_GET_DOMAIN_STATE, 6, 0), ==, 1);
    g_assert_cmpuint(domain_state(qts, TAG_GET_DOMAIN_STATE, 5, 0), ==, 0);
    g_assert_cmpuint(domain_state(qts, TAG_SET_DOMAIN_STATE, 6, 0), ==, 0);
    g_assert_cmpuint(domain_state(qts, TAG_GET_DOMAIN_STATE, 6, 1), ==, 0);
    g_assert_cmpuint(domain_state(qts, TAG_GET_DOMAIN_STATE, 32, 7), ==, 7);
    qtest_quit(qts);
}

/* The display ID of display @n */
static uint32_t display_id(QTestState *qts, uint32_t n)
{
    qtest_writel(qts, BUF_ADDR + 20, n);
    g_assert_cmpuint(property(qts, TAG_GET_DISPLAY_ID, 4), ==, 4);
    return qtest_readl(qts, BUF_ADDR + 20);
}

static void test_displays(void)
{
    /* raspi4b: the framebuffer, on HDMI0 */
    QTestState *qts = qtest_init("-M raspi4b");

    g_assert_cmpuint(property(qts, TAG_GET_NUM_DISPLAYS, 4), ==, 4);
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 20), ==, 1);
    g_assert_cmpuint(display_id(qts, 0), ==, DISPLAY_ID_HDMI0);
    g_assert_cmpuint(display_id(qts, 1), ==, 0);
    qtest_quit(qts);

    /* raspicm4: none, as a CM4 reports with no monitor */
    qts = qtest_init("-M raspicm4");
    g_assert_cmpuint(property(qts, TAG_GET_NUM_DISPLAYS, 4), ==, 4);
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 20), ==, 0);
    g_assert_cmpuint(display_id(qts, 0), ==, 0);
    qtest_quit(qts);

    /* a user setting wins over the machine's */
    qts = qtest_init("-M raspicm4 -global bcm2835-property.num-displays=1");
    g_assert_cmpuint(property(qts, TAG_GET_NUM_DISPLAYS, 4), ==, 4);
    g_assert_cmpuint(qtest_readl(qts, BUF_ADDR + 20), ==, 1);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/bcm2835-property/firmware/default", test_default);
    qtest_add_func("/bcm2835-property/firmware/build", test_build);
    qtest_add_func("/bcm2835-property/gencmd/version", test_gencmd_version);
    qtest_add_func("/bcm2835-property/displays", test_displays);
    qtest_add_func("/bcm2835-property/core-clock", test_core_clock);
    qtest_add_func("/bcm2835-property/gpio-expander", test_gpio_expander);
    qtest_add_func("/bcm2835-property/power-domains", test_power_domains);
    qtest_add_func("/bcm2835-property/gencmd/measure_temp", test_gencmd_temp);
    qtest_add_func("/bcm2835-property/gencmd/measure_clock",
                   test_gencmd_clock);

    return g_test_run();
}
