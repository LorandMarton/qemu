/*
 * QTest testcase for the BCM2838 thermal sensor
 *
 * The die temperature is a property in millidegrees Celsius; the sensor
 * reads the nearest 10-bit code, which Linux turns back into the values
 * a CM4 reports (39920, 41381, ...). The temperature survives a reset.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define THERMAL_STAT        (0xfd5d2000 + 0x200)
#define STAT_DATA           0x3ff
#define STAT_VALID          (1u << 10 | 1u << 16)
#define THERMAL_PATH        "/machine/soc/peripherals/thermal"

/* temperature = 410040 - 487 * code, in millidegrees (Linux bcm2711_thermal) */
static int temperature(QTestState *qts)
{
    uint32_t stat = qtest_readl(qts, THERMAL_STAT);

    g_assert_cmphex(stat & STAT_VALID, ==, STAT_VALID);
    return 410040 - 487 * (stat & STAT_DATA);
}

static bool set_temperature(QTestState *qts, int64_t mc)
{
    QDict *rsp = qtest_qmp(qts, "{ 'execute': 'qom-set', 'arguments': "
                           "{ 'path': %s, 'property': 'temperature', "
                           "'value': %" PRId64 " } }", THERMAL_PATH, mc);
    bool ok = qdict_haskey(rsp, "return");

    qobject_unref(rsp);
    return ok;
}

static void test_temperature(void)
{
    QTestState *qts = qtest_init("-M raspi4b");
    QDict *rsp;

    /* 25 degrees by default: the nearest code */
    g_assert_cmpint(temperature(qts), ==, 24823);

    /* values a CM4 reports */
    g_assert_true(set_temperature(qts, 39920));
    g_assert_cmpint(temperature(qts), ==, 39920);
    g_assert_true(set_temperature(qts, 40407));
    g_assert_cmpint(temperature(qts), ==, 40407);
    /* the nearest code */
    g_assert_true(set_temperature(qts, 40200));
    g_assert_cmpint(temperature(qts), ==, 40407);

    /* outside what the 10 bits can tell */
    g_assert_false(set_temperature(qts, 410041));
    g_assert_false(set_temperature(qts, 410040 - 487 * 1023 - 1));
    g_assert_cmpint(temperature(qts), ==, 40407);

    /* the environment's, not the chip's: kept across a reset */
    rsp = qtest_qmp(qts, "{ 'execute': 'system_reset' }");
    qobject_unref(rsp);
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_cmpint(temperature(qts), ==, 40407);
    qtest_quit(qts);
}

static void test_global(void)
{
    QTestState *qts = qtest_init("-M raspicm4 "
                                 "-global bcm2838-thermal.temperature=41381");

    g_assert_cmpint(temperature(qts), ==, 41381);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/bcm2838-thermal/temperature", test_temperature);
    qtest_add_func("/bcm2838-thermal/global", test_global);
    return g_test_run();
}
