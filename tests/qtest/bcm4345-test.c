/*
 * QTest testcase for the BCM4345 SDIO WLAN chip of the raspicm4 machine
 *
 * The chip sits on the Arasan SDHCI (EMMC1, GPIO 34-39); the test drives it
 * as Linux brcmfmac does: SDIO enumeration and the CIS, the F1 backplane
 * window (ChipCommon chip ID, the CR4 TCM), the firmware start, and a BCDC
 * query over an F2 SDPCM frame.  The SDIO card itself: the FBRs, the F1
 * card interrupt through the controller, and CMD15.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "libqtest.h"
#include "libqos/sdhci-cmd.h"

#define SDHCI_BASE          0xfe300000
#define SDHC_HOSTCTL        0x28
#define SDHC_NORINTSTS      0x30
#define SDHC_NORINTSTSEN    0x34
#define SDHC_NIS_CARDINT    (1u << 8)
#define SDHC_EIS_CMDTIMEOUT (1u << 16)

/* response types: R4 (48 bit), R5/R6 (48 bit, CRC and index), R1b */
#define RSP_R4              0x02
#define RSP_R5              0x1a
#define RSP_R1B             0x1b

/* CMD52/CMD53 argument */
#define IO_RW               (1u << 31)
#define IO_FN(f)            ((uint32_t)(f) << 28)
#define IO_RAW              (1u << 27)
#define IO_INCR             (1u << 26)
#define IO_ADDR(a)          ((uint32_t)(a) << 9)

#define CCCR_IOE            0x02
#define CCCR_IOR            0x03
#define CCCR_IEN            0x04
#define CCCR_INT            0x05
#define CCCR_CIS_PTR        0x09
#define CISTPL_MANFID       0x20
#define CISTPL_END          0xff
#define FBR(fn)             ((fn) * 0x100)
#define FBR_STD_IF          0x00
#define FBR_CIS_PTR         0x09
#define FBR_BLKSIZE         0x10

/* F1 */
#define F1_ACCESS_2_4B      0x08000
#define F1_SBADDRLOW        0x1000a
#define F1_SBADDRMID        0x1000b
#define F1_SBADDRHIGH       0x1000c
#define F1_WINDOW           0x8000

/* backplane */
#define CC_CHIPID           0x18000000
#define SD_INTSTATUS        0x18003020
#define SD_HOSTINTMASK      0x18003024
#define SD_TOHOSTMBXDATA    0x1800304c
#define CR4_WRAP_IOCTL      0x18102408
#define CR4_WRAP_RESETCTRL  0x18102800
#define AI_IOCTL_CLK        0x0001
#define AI_IOCTL_CPUHALT    0x0020
#define I_HMB_FRAME_IND     (1u << 6)
#define I_HMB_HOST_INT      (1u << 7)
#define RAM_BASE            0x198000
#define RAM_SIZE            0xc8000

/* SDPCM, BCDC */
#define SDPCM_HDRLEN        12
#define SDPCM_DATA_CHANNEL  2
#define SDPCM_TX_WINDOW     16
#define BDC_LEN             4
#define BDC_VERSION         0x20
#define BCDC_LEN            16
#define BCDC_ERROR          0x01
#define WLC_GET_VAR         262
#define BCME_UNSUPPORTED    (-23)

static void sd_cmd(QTestState *qts, uint8_t cmd, uint32_t arg, uint16_t rsp)
{
    qtest_writel(qts, SDHCI_BASE + SDHC_NORINTSTS, 0xffffffff);
    qtest_writel(qts, SDHCI_BASE + SDHC_ARGUMENT, arg);
    qtest_writew(qts, SDHCI_BASE + SDHC_TRNMOD, 0);
    qtest_writew(qts, SDHCI_BASE + SDHC_CMDREG, (cmd << 8) | rsp);
}

static uint32_t sd_rsp(QTestState *qts)
{
    return qtest_readl(qts, SDHCI_BASE + SDHC_RSPREG0);
}

static uint8_t cmd52_read(QTestState *qts, unsigned fn, uint32_t addr)
{
    uint32_t r5;

    sd_cmd(qts, 52, IO_FN(fn) | IO_ADDR(addr), RSP_R5);
    r5 = sd_rsp(qts);
    /* no error flags: COM_CRC, ILLEGAL, ERROR, FUNCTION, OUT_OF_RANGE */
    g_assert_cmphex(r5 & 0xcb00, ==, 0);
    return r5 & 0xff;
}

static void cmd52_write(QTestState *qts, unsigned fn, uint32_t addr,
                        uint8_t val)
{
    sd_cmd(qts, 52, IO_RW | IO_FN(fn) | IO_ADDR(addr) | val, RSP_R5);
    g_assert_cmphex(sd_rsp(qts) & 0xcb00, ==, 0);
}

/* CMD53 in byte mode, LEN a multiple of 4 up to 512 */
static void cmd53(QTestState *qts, bool write, unsigned fn, uint32_t addr,
                  uint8_t *buf, size_t len)
{
    g_assert(len && len <= 512 && !(len % 4));
    qtest_writel(qts, SDHCI_BASE + SDHC_NORINTSTS, 0xffffffff);
    qtest_writew(qts, SDHCI_BASE + SDHC_BLKSIZE, len);
    qtest_writew(qts, SDHCI_BASE + SDHC_BLKCNT, 1);
    qtest_writel(qts, SDHCI_BASE + SDHC_ARGUMENT,
                 (write ? IO_RW : 0) | IO_FN(fn) | IO_INCR | IO_ADDR(addr) |
                 (len & 0x1ff));
    qtest_writew(qts, SDHCI_BASE + SDHC_TRNMOD,
                 write ? SDHC_TRNS_WRITE : SDHC_TRNS_READ);
    qtest_writew(qts, SDHCI_BASE + SDHC_CMDREG,
                 (53 << 8) | SDHC_CMD_DATA_PRESENT | RSP_R5);
    g_assert_cmphex(sd_rsp(qts) & 0xcb00, ==, 0);
    for (size_t i = 0; i < len; i += 4) {
        if (write) {
            qtest_writel(qts, SDHCI_BASE + SDHC_BDATA, ldl_le_p(buf + i));
        } else {
            stl_le_p(buf + i, qtest_readl(qts, SDHCI_BASE + SDHC_BDATA));
        }
    }
}

static void bp_window(QTestState *qts, uint32_t addr)
{
    cmd52_write(qts, 1, F1_SBADDRLOW, (addr >> 8) & 0x80);
    cmd52_write(qts, 1, F1_SBADDRMID, addr >> 16);
    cmd52_write(qts, 1, F1_SBADDRHIGH, addr >> 24);
}

static void bp_access(QTestState *qts, bool write, uint32_t addr,
                      uint8_t *buf, size_t len)
{
    bp_window(qts, addr & ~(F1_WINDOW - 1));
    cmd53(qts, write, 1, (addr & (F1_WINDOW - 1)) | F1_ACCESS_2_4B, buf,
          len);
}

static uint32_t bp_readl(QTestState *qts, uint32_t addr)
{
    uint8_t b[4];

    bp_access(qts, false, addr, b, sizeof(b));
    return ldl_le_p(b);
}

static void bp_writel(QTestState *qts, uint32_t addr, uint32_t val)
{
    uint8_t b[4];

    stl_le_p(b, val);
    bp_access(qts, true, addr, b, sizeof(b));
}

static uint16_t wlan_rca;

/* Power-on to the command state: CMD5 twice, CMD3, CMD7 */
static QTestState *wlan_init(void)
{
    QTestState *qts = qtest_init("-M raspicm4 -nic none");
    uint32_t r4, rca;

    qtest_writeb(qts, SDHCI_BASE + SDHC_SWRST, SDHC_RESET_ALL);
    qtest_writew(qts, SDHCI_BASE + SDHC_CLKCON,
                 SDHC_CLOCK_INT_EN | SDHC_CLOCK_SDCLK_EN);

    sd_cmd(qts, 0, 0, 0);
    sd_cmd(qts, 5, 0, RSP_R4);
    r4 = sd_rsp(qts);
    /* not ready yet, three functions, no memory, 3.2-3.4 V */
    g_assert_cmphex(r4 >> 31, ==, 0);
    g_assert_cmpuint((r4 >> 28) & 7, ==, 3);
    g_assert_cmphex(r4 & (1 << 27), ==, 0);
    g_assert_cmphex(r4 & 0x300000, ==, 0x300000);

    sd_cmd(qts, 5, r4 & 0xffffff, RSP_R4);
    g_assert_cmphex(sd_rsp(qts) >> 31, ==, 1);

    sd_cmd(qts, 3, 0, RSP_R5);
    rca = sd_rsp(qts) >> 16;
    g_assert_cmpuint(rca, !=, 0);
    sd_cmd(qts, 7, rca << 16, RSP_R1B);
    wlan_rca = rca;
    return qts;
}

static void test_cis(void)
{
    QTestState *qts = wlan_init();
    uint32_t ptr = 0;
    uint8_t code = 0, len, manfid[4];

    for (int i = 0; i < 3; i++) {
        ptr |= cmd52_read(qts, 0, CCCR_CIS_PTR + i) << (8 * i);
    }
    while (code != CISTPL_END && code != CISTPL_MANFID) {
        code = cmd52_read(qts, 0, ptr);
        len = cmd52_read(qts, 0, ptr + 1);
        if (code == CISTPL_MANFID) {
            g_assert_cmpuint(len, ==, sizeof(manfid));
            for (int i = 0; i < sizeof(manfid); i++) {
                manfid[i] = cmd52_read(qts, 0, ptr + 2 + i);
            }
        }
        ptr += 2 + len;
    }
    /* Broadcom 02d0:a9a6, as the CM4's chip identifies */
    g_assert_cmphex(code, ==, CISTPL_MANFID);
    g_assert_cmphex(lduw_le_p(manfid), ==, 0x02d0);
    g_assert_cmphex(lduw_le_p(manfid + 2), ==, 0xa9a6);

    qtest_quit(qts);
}

/* The function registers: interface class, CIS pointer, block size */
static void test_fbr(void)
{
    static const uint8_t std_if[] = { 0, 0, 0, 0x02 };   /* F3 Bluetooth */
    QTestState *qts = wlan_init();

    for (unsigned fn = 1; fn <= 3; fn++) {
        uint32_t ptr = 0;

        g_assert_cmphex(cmd52_read(qts, 0, FBR(fn) + FBR_STD_IF), ==,
                        std_if[fn]);
        for (int i = 0; i < 3; i++) {
            ptr |= cmd52_read(qts, 0, FBR(fn) + FBR_CIS_PTR + i) << (8 * i);
        }
        g_assert_cmphex(ptr, ==, 0x1000 + fn * 0x100);

        cmd52_write(qts, 0, FBR(fn) + FBR_BLKSIZE, 0x40 * fn);
        cmd52_write(qts, 0, FBR(fn) + FBR_BLKSIZE + 1, fn == 2 ? 2 : 0);
        g_assert_cmphex(cmd52_read(qts, 0, FBR(fn) + FBR_BLKSIZE), ==,
                        0x40 * fn);
        g_assert_cmphex(cmd52_read(qts, 0, FBR(fn) + FBR_BLKSIZE + 1), ==,
                        fn == 2 ? 2 : 0);
    }
    /* no function 4 */
    g_assert_cmphex(cmd52_read(qts, 0, FBR(4) + FBR_CIS_PTR), ==, 0);

    qtest_quit(qts);
}

static void test_backplane(void)
{
    QTestState *qts = wlan_init();
    uint8_t buf[64], back[64];

    /* BCM4345 revision 6, AI interconnect, 5 cores, package 2 */
    g_assert_cmphex(bp_readl(qts, CC_CHIPID), ==, 0x15264345);

    /* the TCM, at both ends */
    for (int i = 0; i < sizeof(buf); i++) {
        buf[i] = i * 7 + 1;
    }
    bp_access(qts, true, RAM_BASE, buf, sizeof(buf));
    bp_access(qts, true, RAM_BASE + RAM_SIZE - sizeof(buf), buf, sizeof(buf));
    bp_access(qts, false, RAM_BASE, back, sizeof(back));
    g_assert(!memcmp(buf, back, sizeof(buf)));
    memset(back, 0, sizeof(back));
    bp_access(qts, false, RAM_BASE + RAM_SIZE - sizeof(buf), back,
              sizeof(back));
    g_assert(!memcmp(buf, back, sizeof(buf)));

    qtest_quit(qts);
}

/* Download an image trailer, release the CR4, wait for DEVREADY */
static QTestState *wlan_start(void)
{
    static const char id[] =
        "43455c0-roml/43455_sdio-ag-pno-aoe-pktfilter-pktctx-lpc-pwropt-"
        "txbf-wfds-mfp-dfsradar-wowlpf-idsup-idauth-noclminc-clm_min-obss-"
        "obssdump-swdiv Version: 7.45.241 (1a2f2fa CY) CRC: 349757d5 "
        "Date: Mon 2021-11-01 00:40:07 PDT Ucode Ver: 1043.2164 "
        "FWID 01-00000000";
    uint8_t img[512] = { 0 };
    QTestState *qts = wlan_init();

    /* halted, out of reset, then the image */
    bp_writel(qts, CR4_WRAP_IOCTL, AI_IOCTL_CLK | AI_IOCTL_CPUHALT);
    bp_writel(qts, CR4_WRAP_RESETCTRL, 0);
    memcpy(img + 4, id, sizeof(id));
    for (size_t off = 0; off < sizeof(img); off += 64) {
        bp_access(qts, true, RAM_BASE + 0x1000 + off, img + off, 64);
    }

    cmd52_write(qts, 0, CCCR_IOE, 0x06);
    g_assert_cmphex(cmd52_read(qts, 0, CCCR_IOR) & 0x06, ==, 0x02);
    bp_writel(qts, CR4_WRAP_IOCTL, AI_IOCTL_CLK);
    qtest_clock_step(qts, 100 * SCALE_MS);

    g_assert_cmphex(cmd52_read(qts, 0, CCCR_IOR) & 0x06, ==, 0x06);
    g_assert_cmphex(bp_readl(qts, SD_INTSTATUS) & I_HMB_HOST_INT, !=, 0);
    /* DEVREADY, SDPCM protocol version 4 */
    g_assert_cmphex(bp_readl(qts, SD_TOHOSTMBXDATA), ==, 0x00040002);
    /* the last word of the RAM points to sdpcm_shared, version 3 */
    g_assert_cmphex(bp_readl(qts, RAM_BASE + RAM_SIZE - 4), ==,
                    RAM_BASE + RAM_SIZE - 0x4000);
    g_assert_cmphex(bp_readl(qts, RAM_BASE + RAM_SIZE - 0x4000) & 0xff, ==,
                    3);
    return qts;
}

/* A BCDC query of the iovar NAME; returns the status, the reply in OUT */
static int32_t wlan_get_var(QTestState *qts, const char *name, uint8_t seq,
                            char *out, size_t outlen)
{
    const size_t dlen = 128;
    const size_t flen = SDPCM_HDRLEN + BCDC_LEN + dlen;
    uint8_t f[SDPCM_HDRLEN + BCDC_LEN + 128] = { 0 };
    uint16_t rlen;
    uint32_t flags;

    g_assert(flen == sizeof(f));
    stw_le_p(f, flen);
    stw_le_p(f + 2, (uint16_t)~flen);
    f[4] = seq;
    f[7] = SDPCM_HDRLEN;
    stl_le_p(f + SDPCM_HDRLEN, WLC_GET_VAR);
    stl_le_p(f + SDPCM_HDRLEN + 4, dlen);
    stl_le_p(f + SDPCM_HDRLEN + 8, (uint32_t)(seq + 1) << 16);
    pstrcpy((char *)f + SDPCM_HDRLEN + BCDC_LEN, dlen, name);
    cmd53(qts, true, 2, 0, f, sizeof(f));

    g_assert_cmphex(bp_readl(qts, SD_INTSTATUS) & I_HMB_FRAME_IND, !=, 0);
    bp_writel(qts, SD_INTSTATUS, I_HMB_FRAME_IND);

    /* the hardware header first, then the rest of the frame */
    memset(f, 0, sizeof(f));
    cmd53(qts, false, 2, 0, f, 4);
    rlen = lduw_le_p(f);
    g_assert_cmphex((uint16_t)~lduw_le_p(f + 2), ==, rlen);
    g_assert_cmpuint(rlen, ==, flen);
    cmd53(qts, false, 2, 0, f + 4, flen - 4);

    /* the control channel, the request ID back */
    g_assert_cmphex(f[5] & 0xf, ==, 0);
    g_assert_cmpuint(f[7], ==, SDPCM_HDRLEN);
    g_assert_cmphex(ldl_le_p(f + SDPCM_HDRLEN), ==, WLC_GET_VAR);
    flags = ldl_le_p(f + SDPCM_HDRLEN + 8);
    g_assert_cmphex(flags >> 16, ==, seq + 1);
    pstrcpy(out, MIN(outlen, dlen + 1), (char *)f + SDPCM_HDRLEN + BCDC_LEN);
    g_assert_cmpint(!!(flags & BCDC_ERROR), ==,
                    ldl_le_p(f + SDPCM_HDRLEN + 12) != 0);
    return ldl_le_p(f + SDPCM_HDRLEN + 12);
}

static void test_firmware(void)
{
    QTestState *qts = wlan_start();
    char out[129];

    /* the version string from the image trailer */
    g_assert_cmpint(wlan_get_var(qts, "ver", 0, out, sizeof(out)), ==, 0);
    g_assert_cmpstr(out, ==, "wl0: Nov  1 2021 00:40:07 version 7.45.241 "
                    "(1a2f2fa CY) FWID 01-00000000\n");

    /* the CM4's WLAN firmware has no gscan */
    g_assert_cmpint(wlan_get_var(qts, "pfn_gscan_cfg", 1, out, sizeof(out)),
                    ==, BCME_UNSUPPORTED);

    qtest_quit(qts);
}

/* A data frame to interface 0: SDPCM, BDC, an Ethernet header, 2 bytes */
static void wlan_send_data(QTestState *qts, uint8_t seq)
{
    const size_t flen = SDPCM_HDRLEN + BDC_LEN + 16;
    uint8_t f[SDPCM_HDRLEN + BDC_LEN + 16] = { 0 };

    stw_le_p(f, flen);
    stw_le_p(f + 2, (uint16_t)~flen);
    f[4] = seq;
    f[5] = SDPCM_DATA_CHANNEL;
    f[7] = SDPCM_HDRLEN;
    f[SDPCM_HDRLEN] = BDC_VERSION;
    memset(f + SDPCM_HDRLEN + BDC_LEN, 0xff, 6);
    stw_be_p(f + SDPCM_HDRLEN + BDC_LEN + 12, 0x0800);
    cmd53(qts, true, 2, 0, f, sizeof(f));
}

/*
 * The host may send only up to the window of the last frame it read.  When
 * it sends without frames to read, the device gives it a new window in an
 * empty frame, also after windows that came with replies.
 */
static void test_credit(void)
{
    QTestState *qts = wlan_start();
    uint8_t f[SDPCM_HDRLEN];
    char out[129];
    uint8_t seq;

    for (seq = 0; seq < 20; seq++) {
        g_assert_cmpint(wlan_get_var(qts, "ver", seq, out, sizeof(out)), ==, 0);
    }
    /* the window is now 20 + 16 */
    for (; !(bp_readl(qts, SD_INTSTATUS) & I_HMB_FRAME_IND); seq++) {
        g_assert_cmpuint(seq, <, 20 + SDPCM_TX_WINDOW);
        wlan_send_data(qts, seq);
    }
    bp_writel(qts, SD_INTSTATUS, I_HMB_FRAME_IND);

    cmd53(qts, false, 2, 0, f, sizeof(f));
    g_assert_cmpuint(lduw_le_p(f), ==, SDPCM_HDRLEN);
    g_assert_cmphex(f[5] & 0xf, ==, SDPCM_DATA_CHANNEL);
    g_assert_cmpuint(f[9], ==, (uint8_t)(seq + SDPCM_TX_WINDOW));

    qtest_quit(qts);
}

/*
 * The chip's host interrupt is the F1 card interrupt: it reaches the
 * controller's Card Interrupt status while IEN enables F1 and the master
 * bit, and drops when the host clears the source.
 */
static void test_card_int(void)
{
    QTestState *qts = wlan_start();

    /* DEVREADY is pending, but masked in the chip */
    g_assert_cmphex(cmd52_read(qts, 0, CCCR_INT), ==, 0);
    qtest_writel(qts, SDHCI_BASE + SDHC_NORINTSTSEN, SDHC_NIS_CARDINT);
    bp_writel(qts, SD_HOSTINTMASK, I_HMB_HOST_INT);
    g_assert_cmphex(qtest_readl(qts, SDHCI_BASE + SDHC_NORINTSTS) &
                    SDHC_NIS_CARDINT, ==, 0);

    cmd52_write(qts, 0, CCCR_IEN, 0x02);
    g_assert_cmphex(cmd52_read(qts, 0, CCCR_INT), ==, 0x02);
    g_assert_cmphex(qtest_readl(qts, SDHCI_BASE + SDHC_NORINTSTS) &
                    SDHC_NIS_CARDINT, ==, 0);

    cmd52_write(qts, 0, CCCR_IEN, 0x03);
    g_assert_cmphex(qtest_readl(qts, SDHCI_BASE + SDHC_NORINTSTS) &
                    SDHC_NIS_CARDINT, ==, SDHC_NIS_CARDINT);

    bp_writel(qts, SD_INTSTATUS, I_HMB_HOST_INT);
    g_assert_cmphex(cmd52_read(qts, 0, CCCR_INT), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SDHCI_BASE + SDHC_NORINTSTS) &
                    SDHC_NIS_CARDINT, ==, 0);

    qtest_quit(qts);
}

/* CMD15 to another address is ignored; to the card's, it stops answering */
static void test_inactive(void)
{
    QTestState *qts = wlan_init();

    qtest_writel(qts, SDHCI_BASE + SDHC_NORINTSTSEN, SDHC_EIS_CMDTIMEOUT);
    sd_cmd(qts, 15, (wlan_rca + 1) << 16, 0);
    g_assert_cmphex(cmd52_read(qts, 0, CCCR_IOE), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SDHCI_BASE + SDHC_NORINTSTS) &
                    SDHC_EIS_CMDTIMEOUT, ==, 0);

    sd_cmd(qts, 15, wlan_rca << 16, 0);
    sd_cmd(qts, 52, IO_FN(0) | IO_ADDR(CCCR_IOE), RSP_R5);
    g_assert_cmphex(qtest_readl(qts, SDHCI_BASE + SDHC_NORINTSTS) &
                    SDHC_EIS_CMDTIMEOUT, ==, SDHC_EIS_CMDTIMEOUT);
    /* not even CMD0 brings it back */
    sd_cmd(qts, 0, 0, 0);
    sd_cmd(qts, 5, 0, RSP_R4);
    g_assert_cmphex(qtest_readl(qts, SDHCI_BASE + SDHC_NORINTSTS) &
                    SDHC_EIS_CMDTIMEOUT, ==, SDHC_EIS_CMDTIMEOUT);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/bcm4345/cis", test_cis);
    qtest_add_func("/bcm4345/fbr", test_fbr);
    qtest_add_func("/bcm4345/backplane", test_backplane);
    qtest_add_func("/bcm4345/firmware", test_firmware);
    qtest_add_func("/bcm4345/credit", test_credit);
    qtest_add_func("/bcm4345/card-int", test_card_int);
    qtest_add_func("/bcm4345/inactive", test_inactive);

    return g_test_run();
}
