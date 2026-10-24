/*
 * QTest testcase for an eMMC on the BCM2838 EMMC2 controller
 *
 * The identity of a real part from the cid and ext-csd properties: CMD2
 * returns the CID, CMD8 the EXT_CSD properties segment and read-only
 * modes bytes, while the geometry stays that of the image.
 *
 * Multi-block transfers: what CMD25 wrote, with a CMD23 count or ended by
 * CMD12, is in the image file when the transfer ends, also beyond the
 * size of one host I/O; CMD18 reads it back. With Auto CMD23 the
 * controller sends the count, so the transfer ends without CMD12.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "libqos/sdhci-cmd.h"

#define EMMC2           0xfe340000
#define SDHC_ARGUMENT2  0x00
#define SDHC_PWRCON     0x29
#define SDHC_TRNS_ACMD23 0x0008

#define RSP_NONE        0
#define RSP_136         1
#define RSP_48          2
#define RSP_48_BUSY     3

#define EMMC_RCA        1
/* the user area; the image holds the two boot partitions before it */
#define EMMC_SIZE       (64 * MiB)
#define BOOT_SIZE       (1 * MiB)

/* A Samsung BJTD4R as on a CM4, with a made-up serial number */
static const uint8_t cid[15] = {
    0x15, 0x01, 0x00, 'B', 'J', 'T', 'D', '4', 'R', 0x03,
    0x51, 0x45, 0x4d, 0x55, 0x78,
};

/* EXT_CSD bytes */
#define EXT_CSD_PARTITION_SUPPORT   160
#define EXT_CSD_PART_CONFIG         179
#define EXT_CSD_REV                 192
#define EXT_CSD_CARD_TYPE           196
#define EXT_CSD_SEC_CNT             212
#define EXT_CSD_BOOT_MULT           226
#define EXT_CSD_S_CMD_SET           504

static char *hex(const uint8_t *buf, size_t len)
{
    GString *s = g_string_new(NULL);

    for (size_t i = 0; i < len; i++) {
        g_string_append_printf(s, "%02x", buf[i]);
    }
    return g_string_free(s, false);
}

static void emmc_cmd(QTestState *qts, int cmd, uint32_t arg, int rsp)
{
    sdhci_cmd_regs(qts, EMMC2, 0, 0, arg, 0, cmd << 8 | rsp);
}

/* raspicm4 with an eMMC on EMMC2, @drive its block node d0 */
static QTestState *emmc_start(const char *drive, const char *props)
{
    QTestState *qts;

    qts = qtest_initf("-M raspicm4 %s "
                      "-device emmc,drive=d0,bus=/generic-sdhci/sd-bus,"
                      "boot-partition-size=%" PRIu64 "%s",
                      drive, (uint64_t)BOOT_SIZE, props);

    qtest_writeb(qts, EMMC2 + SDHC_PWRCON, 0x0f);
    qtest_writew(qts, EMMC2 + SDHC_CLKCON,
                 SDHC_CLOCK_INT_EN | SDHC_CLOCK_SDCLK_EN);
    emmc_cmd(qts, 0, 0, RSP_NONE);
    emmc_cmd(qts, 1, 0x40ff8080, RSP_48);
    return qts;
}

static QTestState *emmc_init(const uint8_t *ext_csd)
{
    g_autofree char *cid_hex = hex(cid, sizeof(cid));
    g_autofree char *ext_csd_hex = hex(ext_csd, 512);
    g_autofree char *drive = NULL;
    g_autofree char *props = NULL;

    drive = g_strdup_printf("-blockdev null-co,node-name=d0,read-zeroes=on,"
                            "size=%" PRIu64,
                            (uint64_t)(EMMC_SIZE + 2 * BOOT_SIZE));
    props = g_strdup_printf(",boot-config=8,cid=%s,ext-csd=%s",
                            cid_hex, ext_csd_hex);
    return emmc_start(drive, props);
}

/* CMD2: R2 holds the CID without its CRC, last byte first */
static void read_cid(QTestState *qts, uint8_t *buf)
{
    uint8_t rsp[16];

    emmc_cmd(qts, 2, 0, RSP_136);
    for (int i = 0; i < 4; i++) {
        stl_le_p(&rsp[i * 4], qtest_readl(qts, EMMC2 + SDHC_RSPREG0 + i * 4));
    }
    for (int i = 0; i < 15; i++) {
        buf[i] = rsp[14 - i];
    }
}

/* CMD3, CMD7, then CMD8 reads the EXT_CSD as one block */
static void read_ext_csd(QTestState *qts, uint8_t *buf)
{
    emmc_cmd(qts, 3, EMMC_RCA << 16, RSP_48);
    emmc_cmd(qts, 7, EMMC_RCA << 16, RSP_48_BUSY);
    sdhci_cmd_regs(qts, EMMC2, 512, 1, 0, SDHC_TRNS_READ,
                   8 << 8 | SDHC_CMD_DATA_PRESENT | RSP_48);
    for (int i = 0; i < 512; i += 4) {
        stl_le_p(&buf[i], qtest_readl(qts, EMMC2 + SDHC_BDATA));
    }
}

/* CMD25 of @n blocks at @blk, with a CMD23 count or ended by CMD12 */
static void emmc_write(QTestState *qts, uint32_t blk, const uint8_t *buf,
                       int n, bool cmd23)
{
    if (cmd23) {
        emmc_cmd(qts, 23, n, RSP_48);
    }
    sdhci_cmd_regs(qts, EMMC2, 512, n, blk * 512,
                   SDHC_TRNS_MULTI | SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_WRITE,
                   25 << 8 | SDHC_CMD_DATA_PRESENT | RSP_48);
    for (int i = 0; i < n * 512; i += 4) {
        qtest_writel(qts, EMMC2 + SDHC_BDATA, ldl_le_p(&buf[i]));
    }
    if (!cmd23) {
        emmc_cmd(qts, 12, 0, RSP_48_BUSY);
    }
}

/* CMD18 of @n blocks at @blk, ended by CMD12 */
static void emmc_read(QTestState *qts, uint32_t blk, uint8_t *buf, int n)
{
    sdhci_cmd_regs(qts, EMMC2, 512, n, blk * 512,
                   SDHC_TRNS_MULTI | SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_READ,
                   18 << 8 | SDHC_CMD_DATA_PRESENT | RSP_48);
    for (int i = 0; i < n * 512; i += 4) {
        stl_le_p(&buf[i], qtest_readl(qts, EMMC2 + SDHC_BDATA));
    }
    emmc_cmd(qts, 12, 0, RSP_48_BUSY);
}

static void fill(uint8_t *buf, size_t len, uint32_t seed)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = (i * 7 + seed) ^ (i >> 9);
    }
}

/* the user area of the image file, @n blocks at @blk */
static void file_read(int fd, uint32_t blk, uint8_t *buf, int n)
{
    g_assert_cmpint(pread(fd, buf, n * 512, 2 * BOOT_SIZE + blk * 512), ==,
                    n * 512);
}

/* the image file on EMMC2, the card selected */
static QTestState *emmc_file_init(int *fd, char **path)
{
    g_autofree char *drive = NULL;
    QTestState *qts;

    *fd = g_file_open_tmp("qtest-emmc2-XXXXXX.img", path, NULL);
    g_assert_cmpint(*fd, >=, 0);
    g_assert_cmpint(ftruncate(*fd, EMMC_SIZE + 2 * BOOT_SIZE), ==, 0);
    drive = g_strdup_printf("-blockdev file,node-name=d0,filename=%s", *path);
    qts = emmc_start(drive, "");
    emmc_cmd(qts, 2, 0, RSP_136);
    emmc_cmd(qts, 3, EMMC_RCA << 16, RSP_48);
    emmc_cmd(qts, 7, EMMC_RCA << 16, RSP_48_BUSY);
    return qts;
}

static void test_multi_block(void)
{
    const int big = 1040;   /* 520 KiB, more than one host I/O */
    g_autofree char *path = NULL;
    g_autofree uint8_t *wr = g_malloc(big * 512);
    g_autofree uint8_t *rd = g_malloc(big * 512);
    QTestState *qts;
    int fd;

    qts = emmc_file_init(&fd, &path);

    /* 8 blocks counted by CMD23: in the file when the count is reached */
    fill(wr, 8 * 512, 1);
    emmc_write(qts, 100, wr, 8, true);
    file_read(fd, 100, rd, 8);
    g_assert_cmpmem(rd, 8 * 512, wr, 8 * 512);

    /* 4 blocks ended by CMD12 */
    fill(wr + 8 * 512, 4 * 512, 2);
    emmc_write(qts, 108, wr + 8 * 512, 4, false);
    file_read(fd, 108, rd, 4);
    g_assert_cmpmem(rd, 4 * 512, wr + 8 * 512, 4 * 512);

    /* read back across both and 4 blocks never written */
    memset(wr + 12 * 512, 0, 4 * 512);
    emmc_read(qts, 100, rd, 16);
    g_assert_cmpmem(rd, 16 * 512, wr, 16 * 512);

    /* more than the write buffer, ended by CMD12 */
    fill(wr, big * 512, 3);
    emmc_write(qts, 2000, wr, big, false);
    file_read(fd, 2000, rd, big);
    g_assert_cmpmem(rd, big * 512, wr, big * 512);

    qtest_quit(qts);
    close(fd);
    unlink(path);
}

static void test_auto_cmd23(void)
{
    g_autofree char *path = NULL;
    uint8_t wr[8 * 512], rd[8 * 512];
    QTestState *qts;
    uint32_t status;
    int fd;

    qts = emmc_file_init(&fd, &path);
    fill(wr, sizeof(wr), 4);
    qtest_writel(qts, EMMC2 + SDHC_ARGUMENT2, 8);
    sdhci_cmd_regs(qts, EMMC2, 512, 8, 300 * 512,
                   SDHC_TRNS_MULTI | SDHC_TRNS_BLK_CNT_EN | SDHC_TRNS_WRITE |
                   SDHC_TRNS_ACMD23,
                   25 << 8 | SDHC_CMD_DATA_PRESENT | RSP_48);
    for (int i = 0; i < sizeof(wr); i += 4) {
        qtest_writel(qts, EMMC2 + SDHC_BDATA, ldl_le_p(&wr[i]));
    }

    /* the card had the count: the blocks are written, it is in tran */
    file_read(fd, 300, rd, 8);
    g_assert_cmpmem(rd, sizeof(rd), wr, sizeof(wr));
    emmc_cmd(qts, 13, EMMC_RCA << 16, RSP_48);
    status = qtest_readl(qts, EMMC2 + SDHC_RSPREG0);
    g_assert_cmpuint(extract32(status, 9, 4), ==, 4);

    qtest_quit(qts);
    close(fd);
    unlink(path);
}

static void test_identity(void)
{
    uint8_t preset[512] = { 0 };
    uint8_t got_cid[15];
    uint8_t ext_csd[512];
    QTestState *qts;

    preset[EXT_CSD_PARTITION_SUPPORT] = 0x07;
    preset[EXT_CSD_PART_CONFIG] = 0x48;
    preset[EXT_CSD_REV] = 8;
    preset[EXT_CSD_CARD_TYPE] = 0x57;
    stl_le_p(&preset[EXT_CSD_SEC_CNT], 0xffffffff);
    preset[EXT_CSD_BOOT_MULT] = 0x20;
    preset[EXT_CSD_S_CMD_SET] = 0x01;

    qts = emmc_init(preset);
    read_cid(qts, got_cid);
    g_assert_cmpmem(got_cid, sizeof(got_cid), cid, sizeof(cid));
    read_ext_csd(qts, ext_csd);

    /* the part's properties and read-only modes bytes */
    g_assert_cmpuint(ext_csd[EXT_CSD_REV], ==, 8);
    g_assert_cmpuint(ext_csd[EXT_CSD_CARD_TYPE], ==, 0x57);
    g_assert_cmpuint(ext_csd[EXT_CSD_PARTITION_SUPPORT], ==, 0x07);
    g_assert_cmpuint(ext_csd[EXT_CSD_S_CMD_SET], ==, 0x01);

    /* the geometry of the image, the boot configuration of the device */
    g_assert_cmpuint(ldl_le_p(&ext_csd[EXT_CSD_SEC_CNT]), ==,
                     EMMC_SIZE / 512);
    g_assert_cmpuint(ext_csd[EXT_CSD_BOOT_MULT], ==, BOOT_SIZE / (128 * KiB));
    g_assert_cmpuint(ext_csd[EXT_CSD_PART_CONFIG], ==, 8);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/bcm2838-emmc2/identity", test_identity);
    qtest_add_func("/bcm2838-emmc2/multi-block", test_multi_block);
    qtest_add_func("/bcm2838-emmc2/auto-cmd23", test_auto_cmd23);
    return g_test_run();
}
