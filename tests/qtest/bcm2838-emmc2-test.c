/*
 * QTest testcase for an eMMC on the BCM2838 EMMC2 controller
 *
 * The identity of a real part from the cid and ext-csd properties: CMD2
 * returns the CID, CMD8 the EXT_CSD properties segment and read-only
 * modes bytes, while the geometry stays that of the image.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "libqos/sdhci-cmd.h"

#define EMMC2           0xfe340000
#define SDHC_PWRCON     0x29

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

static QTestState *emmc_init(const uint8_t *ext_csd)
{
    g_autofree char *cid_hex = hex(cid, sizeof(cid));
    g_autofree char *ext_csd_hex = hex(ext_csd, 512);
    QTestState *qts;

    qts = qtest_initf("-M raspicm4 -blockdev null-co,node-name=d0,"
                      "read-zeroes=on,size=%" PRIu64 " "
                      "-device emmc,drive=d0,bus=/generic-sdhci/sd-bus,"
                      "boot-partition-size=%" PRIu64 ",boot-config=8,"
                      "cid=%s,ext-csd=%s",
                      (uint64_t)(EMMC_SIZE + 2 * BOOT_SIZE),
                      (uint64_t)BOOT_SIZE,
                      cid_hex, ext_csd_hex);

    qtest_writeb(qts, EMMC2 + SDHC_PWRCON, 0x0f);
    qtest_writew(qts, EMMC2 + SDHC_CLKCON,
                 SDHC_CLOCK_INT_EN | SDHC_CLOCK_SDCLK_EN);
    emmc_cmd(qts, 0, 0, RSP_NONE);
    emmc_cmd(qts, 1, 0x40ff8080, RSP_48);
    return qts;
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
    return g_test_run();
}
