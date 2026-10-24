/*
 * QTest testcase for the Raspberry Pi 4 boards (raspi4b, raspicm4)
 *
 * raspicm4 as a CM4 Rev 1.0 8 GiB reports itself: the board revision and
 * the ARM memory below the VideoCore carve-out; the generic timer at the
 * 54 MHz crystal frequency.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
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

#define TAG_GET_BOARD_REV   0x00010002
#define TAG_GET_ARM_MEMORY  0x00010005

#define CM4_REVISION        0xd03140
#define CM4_ARM_MEMORY      ((1024 - 76) * MiB)

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
    return qtest_readl(qts, BUF_ADDR + 16) & ~CODE_RESPONSE;
}

static void test_cm4_identity(void)
{
    QTestState *qts = qtest_init("-M raspicm4");
    QDict *rsp;

    g_assert_cmpuint(property(qts, TAG_GET_BOARD_REV, 4), ==, 4);
    g_assert_cmphex(qtest_readl(qts, BUF_ADDR + 20), ==, CM4_REVISION);
    /* base and size */
    g_assert_cmpuint(property(qts, TAG_GET_ARM_MEMORY, 8), ==, 8);
    g_assert_cmphex(qtest_readl(qts, BUF_ADDR + 20), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BUF_ADDR + 24), ==, CM4_ARM_MEMORY);

    rsp = qtest_qmp(qts, "{ 'execute': 'query-memory-size-summary' }");
    g_assert_cmpuint(qdict_get_int(qdict_get_qdict(rsp, "return"),
                                   "base-memory"), ==, 8 * GiB);
    qobject_unref(rsp);
    qtest_quit(qts);
}

static void test_cntfrq(const void *machine)
{
    QTestState *qts = qtest_initf("-M %s", (const char *)machine);
    QDict *rsp;

    for (int n = 0; n < 4; n++) {
        g_autofree char *path = g_strdup_printf("/machine/soc/cpu[%d]", n);

        rsp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                        "{ 'path': %s, 'property': 'cntfrq' } }", path);
        g_assert_cmpuint(qdict_get_int(rsp, "return"), ==, 54000000);
        qobject_unref(rsp);
    }
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/raspi4/cm4/identity", test_cm4_identity);
    qtest_add_data_func("/raspi4/raspi4b/cntfrq", "raspi4b", test_cntfrq);
    qtest_add_data_func("/raspi4/raspicm4/cntfrq", "raspicm4", test_cntfrq);
    return g_test_run();
}
