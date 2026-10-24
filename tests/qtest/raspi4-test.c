/*
 * QTest testcase for the Raspberry Pi 4 boards (raspi4b, raspicm4)
 *
 * raspicm4 as a CM4 Rev 1.0 8 GiB reports itself: the board revision and
 * the ARM memory below the VideoCore carve-out; the generic timer at the
 * 54 MHz crystal frequency; the device tree as the firmware passes it:
 * the firmware's memory ranges and seeds, packed, just below the initrd.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
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

/* A flattened device tree, built and read just enough for these tests */
#define FDT_MAGIC           0xd00dfeed
#define FDT_BEGIN_NODE      1
#define FDT_END_NODE        2
#define FDT_PROP            3
#define FDT_NOP             4
#define FDT_END             9
#define FDT_HDR_SIZE        40
#define FDT_RSV_SIZE        16

static void fdt_u32(GByteArray *b, uint32_t v)
{
    v = cpu_to_be32(v);
    g_byte_array_append(b, (uint8_t *)&v, 4);
}

static void fdt_pad(GByteArray *b)
{
    static const uint8_t zero[4];

    g_byte_array_append(b, zero, -b->len & 3);
}

static void fdt_node(GByteArray *b, const char *name)
{
    fdt_u32(b, FDT_BEGIN_NODE);
    g_byte_array_append(b, (const uint8_t *)name, strlen(name) + 1);
    fdt_pad(b);
}

static void fdt_prop(GByteArray *b, GString *strings, const char *name,
                     const void *val, uint32_t len)
{
    fdt_u32(b, FDT_PROP);
    fdt_u32(b, len);
    fdt_u32(b, strings->len);
    g_string_append_len(strings, name, strlen(name) + 1);
    g_byte_array_append(b, val, len);
    fdt_pad(b);
}

static void fdt_prop_u32(GByteArray *b, GString *strings, const char *name,
                         uint32_t v)
{
    v = cpu_to_be32(v);
    fdt_prop(b, strings, name, &v, 4);
}

/*
 * The nodes the firmware's edits need, as in the BCM2711 trees: 2 address
 * cells, 1 size cell, /chosen and /memory@0.
 */
static char *write_dtb(void)
{
    static const uint32_t reg[3] = { 0, 0, 0 };
    g_autoptr(GByteArray) st = g_byte_array_new();
    g_autoptr(GByteArray) blob = g_byte_array_new();
    g_autoptr(GString) strings = g_string_new(NULL);
    char *path;
    int fd;

    fdt_node(st, "");
    fdt_prop_u32(st, strings, "#address-cells", 2);
    fdt_prop_u32(st, strings, "#size-cells", 1);
    fdt_node(st, "chosen");
    fdt_u32(st, FDT_END_NODE);
    fdt_node(st, "memory@0");
    fdt_prop(st, strings, "device_type", "memory", 7);
    fdt_prop(st, strings, "reg", reg, sizeof(reg));
    fdt_u32(st, FDT_END_NODE);
    fdt_u32(st, FDT_END_NODE);
    fdt_u32(st, FDT_END);

    fdt_u32(blob, FDT_MAGIC);
    fdt_u32(blob, FDT_HDR_SIZE + FDT_RSV_SIZE + st->len + strings->len);
    fdt_u32(blob, FDT_HDR_SIZE + FDT_RSV_SIZE);
    fdt_u32(blob, FDT_HDR_SIZE + FDT_RSV_SIZE + st->len);
    fdt_u32(blob, FDT_HDR_SIZE);
    fdt_u32(blob, 17);
    fdt_u32(blob, 16);
    fdt_u32(blob, 0);
    fdt_u32(blob, strings->len);
    fdt_u32(blob, st->len);
    fdt_u32(blob, 0);
    fdt_u32(blob, 0);
    fdt_u32(blob, 0);
    fdt_u32(blob, 0);
    g_byte_array_append(blob, st->data, st->len);
    g_byte_array_append(blob, (uint8_t *)strings->str, strings->len);

    fd = g_file_open_tmp("raspi4-test-XXXXXX.dtb", &path, NULL);
    g_assert(fd >= 0);
    g_assert_cmpint(write(fd, blob->data, blob->len), ==, blob->len);
    close(fd);
    return path;
}

static uint32_t be32_at(const uint8_t *p)
{
    return ldl_be_p(p);
}

/* The value of property NAME of node PATH ("/" or "/a/b"), or NULL */
static const uint8_t *fdt_find(const uint8_t *fdt, const char *path,
                               const char *name, uint32_t *len)
{
    const uint8_t *st = fdt + be32_at(fdt + 8);
    const char *strings = (const char *)fdt + be32_at(fdt + 12);
    g_autoptr(GString) cur = g_string_new(NULL);
    gsize depth_len[16];
    int depth = 0;
    uint32_t off = 0;

    for (;;) {
        uint32_t tag = be32_at(st + off);

        off += 4;
        switch (tag) {
        case FDT_BEGIN_NODE: {
            const char *n = (const char *)st + off;

            g_assert(depth < ARRAY_SIZE(depth_len));
            depth_len[depth++] = cur->len;
            if (depth > 1) {
                g_string_append_printf(cur, "/%s", n);
            }
            off += ROUND_UP(strlen(n) + 1, 4);
            break;
        }
        case FDT_END_NODE:
            g_string_truncate(cur, depth_len[--depth]);
            break;
        case FDT_PROP: {
            uint32_t l = be32_at(st + off);
            const char *n = strings + be32_at(st + off + 4);

            if (!strcmp(cur->len ? cur->str : "/", path) && !strcmp(n, name)) {
                *len = l;
                return st + off + 8;
            }
            off += 8 + ROUND_UP(l, 4);
            break;
        }
        case FDT_NOP:
            break;
        default:
            g_assert_cmpuint(tag, ==, FDT_END);
            return NULL;
        }
    }
}

static uint64_t fdt_cells(const uint8_t *p, int cells)
{
    return cells == 2 ? (uint64_t)be32_at(p) << 32 | be32_at(p + 4)
                      : be32_at(p);
}

static char *write_file(const char *pattern, size_t size)
{
    g_autofree uint8_t *data = g_malloc0(size);
    char *path;
    int fd = g_file_open_tmp(pattern, &path, NULL);

    g_assert(fd >= 0);
    g_assert_cmpint(write(fd, data, size), ==, size);
    close(fd);
    return path;
}


/*
 * The firmware's tree: /memory@0 with its RAM ranges (ARM memory below the
 * carve-out, then up to the peripherals, then 2 GiB ranges), the seeds, and
 * the tree packed, just below the initrd at a 256-byte boundary.
 */
static void test_cm4_dtb(void)
{
    static const uint64_t ranges[][2] = {
        { 0, CM4_ARM_MEMORY },
        { 0x40000000, 0xfc000000 - 0x40000000 },
        { 4 * GiB, 2 * GiB },
        { 6 * GiB, 2 * GiB },
    };
    g_autofree char *dtb = write_dtb();
    g_autofree char *kernel = write_file("raspi4-test-XXXXXX.kernel", 4096);
    g_autofree char *initrd = write_file("raspi4-test-XXXXXX.initrd", 65536);
    g_autofree char *dump = g_strdup_printf("%s.dump", dtb);
    g_autofree uint8_t *mem = NULL;
    g_autofree gchar *fdt = NULL;
    const uint8_t *p;
    uint64_t initrd_start, dtb_addr;
    uint32_t len = 0, totalsize;
    gsize size;
    QTestState *qts;
    QDict *rsp;

    qts = qtest_initf("-M raspicm4 -kernel %s -initrd %s -dtb %s",
                      kernel, initrd, dtb);
    rsp = qtest_qmp(qts, "{ 'execute': 'dumpdtb', "
                    "'arguments': { 'filename': %s } }", dump);
    g_assert(qdict_haskey(rsp, "return"));
    qobject_unref(rsp);
    g_assert(g_file_get_contents(dump, &fdt, &size, NULL));
    p = (const uint8_t *)fdt;

    /* packed: no free space after the strings */
    totalsize = be32_at(p + 4);
    g_assert_cmpuint(totalsize, ==, size);
    g_assert_cmpuint(totalsize, ==, be32_at(p + 12) + be32_at(p + 32));

    p = (const uint8_t *)fdt_find((uint8_t *)fdt, "/memory@0", "reg", &len);
    g_assert_nonnull(p);
    g_assert_cmpuint(len, ==, sizeof(ranges) / sizeof(ranges[0][0]) / 2 * 12);
    for (int i = 0; i < ARRAY_SIZE(ranges); i++) {
        g_assert_cmphex(fdt_cells(p + 12 * i, 2), ==, ranges[i][0]);
        g_assert_cmphex(fdt_cells(p + 12 * i + 8, 1), ==, ranges[i][1]);
    }
    g_assert_nonnull(fdt_find((uint8_t *)fdt, "/chosen", "kaslr-seed", &len));
    g_assert_cmpuint(len, ==, 8);
    g_assert_nonnull(fdt_find((uint8_t *)fdt, "/chosen", "rng-seed", &len));
    g_assert_cmpuint(len, ==, 32);

    /* in guest memory below the initrd, as dumped */
    p = fdt_find((uint8_t *)fdt, "/chosen", "linux,initrd-start", &len);
    g_assert_nonnull(p);
    initrd_start = fdt_cells(p, len / 4);
    dtb_addr = (initrd_start - totalsize) & ~0xffull;
    mem = g_malloc(totalsize);
    qtest_memread(qts, dtb_addr, mem, totalsize);
    /* the same tree but for rng-seed, new at each reset */
    {
        uint8_t *m = (uint8_t *)fdt_find(mem, "/chosen", "rng-seed", &len);
        uint8_t *d = (uint8_t *)fdt_find((uint8_t *)fdt, "/chosen",
                                         "rng-seed", &len);

        g_assert(m - mem == d - (uint8_t *)fdt);
        g_assert(memcmp(m, d, len) != 0);
        memset(m, 0, len);
        memset(d, 0, len);
    }
    g_assert(memcmp(mem, fdt, totalsize) == 0);

    qtest_quit(qts);
    unlink(dump);
    unlink(dtb);
    unlink(kernel);
    unlink(initrd);
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
    qtest_add_func("/raspi4/cm4/dtb", test_cm4_dtb);
    qtest_add_data_func("/raspi4/raspi4b/cntfrq", "raspi4b", test_cntfrq);
    qtest_add_data_func("/raspi4/raspicm4/cntfrq", "raspicm4", test_cntfrq);
    return g_test_run();
}
