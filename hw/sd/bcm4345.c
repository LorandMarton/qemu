/*
 * Broadcom BCM4345 (Cypress CYW43455) SDIO WLAN chip
 *
 * The chip of the Raspberry Pi 3B+, 4 and Compute Module 4 (BCM4345/6,
 * revision 6), as Linux brcmfmac sees it: an SDIO card with three
 * functions; F1 gives access to the AXI backplane (ChipCommon with its
 * PMU, the 802.11 core, the ARM Cortex-R4 with its TCM, the SDIO device
 * core, GCI) through a 32 KiB window, F2 carries the SDPCM frames between
 * the host and the firmware, F3 (Bluetooth over SDIO) is unused.
 *
 * The firmware the host downloads into the TCM is not executed: when the
 * host releases the CR4 from halt, a model of the firmware (bcm4345-wl.c)
 * takes over the SDPCM protocol.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "net/net.h"
#include "bcm4345-internal.h"
#include "trace.h"

/* SDIO identity (02d0:a9a6, as the CM4's chip reports) */
#define BCM4345_SDIO_VENDOR     0x02d0
#define BCM4345_SDIO_DEVICE     0xa9a6

/* ChipCommon chipid: AI interconnect, 5 cores, package 2, rev 6, 0x4345 */
#define BCM4345_CHIPID          0x15264345

/* Backplane addresses */
#define BP_ENUM_BASE            0x18000000
#define BP_CORE_BASE(i)         (BP_ENUM_BASE + (i) * 0x1000)
#define BP_WRAP_BASE(i)         (BP_ENUM_BASE + 0x100000 + (i) * 0x1000)
#define BP_EROM_BASE            0x1810f000
#define BP_EROM_SIZE            0x100
#define BP_CORE_SIZE            0x1000

/* F1 address space */
#define F1_SB_OFT_ADDR_MASK     0x07fff
#define F1_SB_ACCESS_2_4B_FLAG  0x08000
#define F1_SPROM_CS             0x10000
#define F1_GPIO_SELECT          0x10005
#define F1_GPIO_OUT             0x10006
#define F1_GPIO_EN              0x10007
#define F1_WATERMARK            0x10008
#define F1_DEVICE_CTL           0x10009
#define F1_SBADDRLOW            0x1000a
#define F1_SBADDRMID            0x1000b
#define F1_SBADDRHIGH           0x1000c
#define F1_FRAMECTRL            0x1000d
#define F1_CHIPCLKCSR           0x1000e
#define F1_SDIOPULLUP           0x1000f
#define F1_WFRAMEBCLO           0x10019
#define F1_WFRAMEBCHI           0x1001a
#define F1_RFRAMEBCLO           0x1001b
#define F1_RFRAMEBCHI           0x1001c
#define F1_MESBUSYCTRL          0x1001d
#define F1_WAKEUPCTRL           0x1001e
#define F1_SLEEPCSR             0x1001f

/* CHIPCLKCSR */
#define CLKCSR_FORCE_ALP        0x01
#define CLKCSR_FORCE_HT         0x02
#define CLKCSR_FORCE_ILP        0x04
#define CLKCSR_ALP_AVAIL_REQ    0x08
#define CLKCSR_HT_AVAIL_REQ     0x10
#define CLKCSR_FORCE_HW_CLKREQ_OFF 0x20
#define CLKCSR_ALP_AVAIL        0x40
#define CLKCSR_HT_AVAIL         0x80
#define CLKCSR_MASK             0x3f

/* DEVICE_CTL */
#define DEVCTL_CA_INT_ONLY      0x04

/* FRAMECTRL */
#define SFC_RF_TERM             0x01
#define SFC_WF_TERM             0x02

/* SLEEPCSR */
#define SLEEPCSR_KSO            0x01
#define SLEEPCSR_DEVON          0x02

/* AI wrapper registers */
#define AI_IOCTL                0x408
#define  AI_IOCTL_CLK           0x0001
#define  AI_IOCTL_FGC           0x0002
#define  AI_IOCTL_CR4_CPUHALT   0x0020
#define AI_IOSTATUS             0x500
#define AI_RESETCTRL            0x800
#define  AI_RESETCTRL_RESET     0x0001
#define AI_RESETSTATUS          0x804

/* ChipCommon registers */
#define CC_CHIPID               0x000
#define CC_CAPABILITIES         0x004
#define  CC_CAP_PMU             (1u << 28)
#define CC_GPIOOUT              0x064
#define CC_GPIOOUTEN            0x068
#define CC_GPIOCONTROL          0x06c
#define CC_WATCHDOG             0x080
#define CC_CAPABILITIES_EXT     0x0ac
#define CC_EROMPTR              0x0fc
#define CC_PMUCONTROL           0x600
#define CC_PMUCAPABILITIES      0x604
#define CC_PMUSTATUS            0x608
#define CC_PMUWATCHDOG          0x634
#define CC_PMUCAPABILITIES_EXT  0x64c
#define CC_CHIPCONTROL_ADDR     0x650
#define CC_CHIPCONTROL_DATA     0x654
#define CC_REGCONTROL_ADDR      0x658
#define CC_REGCONTROL_DATA      0x65c
#define CC_PLLCONTROL_ADDR      0x660
#define CC_PLLCONTROL_DATA      0x664

/* PMU revision 24, 32 resources */
#define BCM4345_PMUCAPS         ((32 << 8) | 24)
/* PMU status: HT and ALP available */
#define BCM4345_PMUSTATUS       0x000000c4

/* ARM CR4 registers */
#define CR4_CAP                 0x04
#define CR4_BANKIDX             0x40
#define CR4_BANKINFO            0x44
#define CR4_BANKPDA             0x4c
/* 5 TCM banks (1 ATCM, 4 BTCM) of 160 KiB */
#define CR4_NUM_ABANKS          1
#define CR4_NUM_BBANKS          4
#define CR4_BANK_SIZE           (BCM4345_RAM_SIZE / 5)

/* SDIO device core registers */
#define SD_CORECONTROL          0x000
#define SD_CORESTATUS           0x004
#define SD_INTSTATUS            0x020
#define SD_HOSTINTMASK          0x024
#define SD_INTMASK              0x028
#define SD_SBINTSTATUS          0x02c
#define SD_SBINTMASK            0x030
#define SD_TOSBMAILBOX          0x040
#define SD_TOHOSTMAILBOX        0x044
#define SD_TOSBMAILBOXDATA      0x048
#define SD_TOHOSTMAILBOXDATA    0x04c

/* intstatus */
#define I_HMB_FC_STATE          (1u << 4)
#define I_HMB_FC_CHANGE         (1u << 5)
#define I_HMB_FRAME_IND         (1u << 6)
#define I_HMB_HOST_INT          (1u << 7)
#define I_CHIPACTIVE            (1u << 29)

/* tohostmailboxdata */
#define HMB_DATA_DEVREADY       0x0002
#define HMB_DATA_FWHALT         0x0010
#define HMB_DATA_VERSION_SHIFT  16
#define SDPCM_PROT_VERSION      4

/* SDPCM headers */
#define SDPCM_HWHDR_LEN         4
#define SDPCM_SWHDR_LEN         8
#define SDPCM_HDRLEN            (SDPCM_HWHDR_LEN + SDPCM_SWHDR_LEN)
/*
 * Hardware extension header of the frames of a chain (bus:rxglom): the
 * length after the hardware header, the last-frame flag; the tail padding
 */
#define SDPCM_HWEXT_LEN         8
#define SDPCM_HWEXT_LASTFRM     (1u << 24)
/* Frames the host may send ahead of the last one received */
#define SDPCM_TX_WINDOW         16

/* sdpcm_shared, at the top of the RAM below the NVRAM */
#define SDPCM_SHARED_VERSION    0x0003
#define SDPCM_SHARED_ADDR       (BCM4345_RAM_BASE + BCM4345_RAM_SIZE - 0x4000)
#define SDPCM_SHARED_SIZE       0x40

/* Firmware start-up, from the CR4 leaving reset to the first frame */
#define BCM4345_BOOT_NS         (40 * SCALE_MS)

typedef struct BCM4345Core {
    uint16_t id;
    uint8_t rev;
    uint8_t nmw;
    uint8_t nsw;
} BCM4345Core;

static const BCM4345Core bcm4345_cores[BCM4345_NUM_CORES] = {
    [BCM4345_CORE_CC]   = { 0x800, 54, 0, 1 },
    [BCM4345_CORE_D11]  = { 0x812, 54, 1, 1 },
    [BCM4345_CORE_CR4]  = { 0x83e, 7, 2, 1 },
    [BCM4345_CORE_SDIO] = { 0x829, 21, 1, 1 },
    [BCM4345_CORE_GCI]  = { 0x840, 8, 0, 0 },
};

static uint32_t bcm4345_erom[BP_EROM_SIZE / 4];

static void bcm4345_build_erom(void)
{
    unsigned n = 0;

    for (unsigned i = 0; i < BCM4345_NUM_CORES; i++) {
        const BCM4345Core *c = &bcm4345_cores[i];

        /* CompIdentA, CompIdentB: Broadcom (0x4bf), part, revision, ports */
        bcm4345_erom[n++] = (0x4bf << 20) | (c->id << 8) | 0x1;
        bcm4345_erom[n++] = (c->rev << 24) | (c->nsw << 19) |
                            (c->nmw << 14) | (c->nsw << 9) |
                            (c->nmw << 4) | 0x1;
        if (c->nmw) {
            /* master port */
            bcm4345_erom[n++] = (i << 8) | 0x3;
        }
        /* slave port 0, 4 KiB registers */
        bcm4345_erom[n++] = BP_CORE_BASE(i) | 0x5;
        if (c->nmw + c->nsw) {
            /* wrapper: master (3) or slave (2) */
            bcm4345_erom[n++] = BP_WRAP_BASE(i) | ((c->nmw ? 3 : 2) << 6) |
                                0x5;
        }
    }
    bcm4345_erom[n++] = 0xf;
    assert(n <= ARRAY_SIZE(bcm4345_erom));
}

/* Host interrupt (F1) */
static void bcm4345_update_irq(BCM4345State *s)
{
    uint32_t pending = s->intstatus & s->hostintmask;

    if (s->devctl & DEVCTL_CA_INT_ONLY) {
        pending &= I_CHIPACTIVE;
    }
    sdio_card_set_irq(SDIO_CARD(s), 1, pending != 0);
}

static void bcm4345_set_intstatus(BCM4345State *s, uint32_t bits)
{
    s->intstatus |= bits;
    bcm4345_update_irq(s);
}

/* F2 frames to the host */

static void bcm4345_rx_flush(BCM4345State *s)
{
    s->rx_head = s->rx_tail = 0;
    s->rx_pos = 0;
    s->rx_frame = false;
}

static uint16_t bcm4345_rx_frame_len(BCM4345State *s)
{
    return lduw_le_p(s->rxbuf + s->rx_head);
}

static void bcm4345_rx_pop(BCM4345State *s)
{
    if (s->rx_head != s->rx_tail) {
        s->rx_head += 2 + bcm4345_rx_frame_len(s);
        if (s->rx_head == s->rx_tail) {
            s->rx_head = s->rx_tail = 0;
        }
    }
    s->rx_pos = 0;
    s->rx_frame = false;
    if (bcm4345_rx_has_room(s)) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

bool bcm4345_rx_has_room(BCM4345State *s)
{
    return s->rx_tail - s->rx_head + 2 * KiB <= BCM4345_RXBUF_SIZE / 2;
}

void bcm4345_send(BCM4345State *s, unsigned channel, const uint8_t *hdr,
                  size_t hdr_len, const uint8_t *data, size_t len)
{
    size_t flen = SDPCM_HDRLEN + hdr_len + len;
    uint8_t *f;

    if (!s->fw_ready || flen > UINT16_MAX) {
        return;
    }
    if (s->rx_tail + 2 + flen > BCM4345_RXBUF_SIZE && s->rx_head) {
        memmove(s->rxbuf, s->rxbuf + s->rx_head, s->rx_tail - s->rx_head);
        s->rx_tail -= s->rx_head;
        s->rx_head = 0;
    }
    if (s->rx_tail + 2 + flen > BCM4345_RXBUF_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "bcm4345: F2 receive queue full, "
                      "frame dropped\n");
        return;
    }

    stw_le_p(s->rxbuf + s->rx_tail, flen);
    f = s->rxbuf + s->rx_tail + 2;
    /* hardware header: length, its complement */
    stw_le_p(f, flen);
    stw_le_p(f + 2, ~flen);
    /* software header: sequence, channel, data offset; window at read */
    f[4] = s->rx_seq++;
    f[5] = channel;
    f[6] = 0;
    f[7] = SDPCM_HDRLEN;
    stl_le_p(f + 8, 0);
    memcpy(f + SDPCM_HDRLEN, hdr, hdr_len);
    memcpy(f + SDPCM_HDRLEN + hdr_len, data, len);
    s->rx_tail += 2 + flen;

    trace_bcm4345_send(channel, flen);
    bcm4345_set_intstatus(s, I_HMB_FRAME_IND);
}

static void bcm4345_f2_read(BCM4345State *s, uint8_t *buf, size_t len)
{
    uint16_t flen;
    uint8_t *f;
    size_t n;

    if (!s->rx_in_cmd) {
        /* a CMD53 reads the frame that was first in the queue when it began */
        s->rx_in_cmd = true;
        if (!s->rx_frame && s->rx_head != s->rx_tail) {
            s->rx_frame = true;
            s->rx_pos = 0;
            /* the window as of now */
            f = s->rxbuf + s->rx_head + 2;
            s->rx_window = s->host_seq + SDPCM_TX_WINDOW;
            f[9] = s->rx_window;
        }
    }
    if (!s->rx_frame) {
        memset(buf, 0, len);
        return;
    }

    flen = bcm4345_rx_frame_len(s);
    f = s->rxbuf + s->rx_head + 2;
    n = s->rx_pos < flen ? MIN(len, flen - s->rx_pos) : 0;
    memcpy(buf, f + s->rx_pos, n);
    memset(buf + n, 0, len - n);
    s->rx_pos += len;
}

static void bcm4345_f2_read_done(BCM4345State *s)
{
    s->rx_in_cmd = false;
    if (s->rx_frame && s->rx_pos >= bcm4345_rx_frame_len(s)) {
        bcm4345_rx_pop(s);
    }
}

/* F2 frames from the host */

/* A frame, its software header after HWLEN bytes of hardware headers */
static void bcm4345_host_frame(BCM4345State *s, const uint8_t *f, size_t len,
                               size_t hwlen)
{
    const uint8_t *sw = f + hwlen;
    unsigned channel = sw[1] & 0xf;
    unsigned doff = sw[3];
    const uint8_t *p = f + doff;
    size_t plen;

    if (doff < hwlen + SDPCM_SWHDR_LEN || doff > len) {
        qemu_log_mask(LOG_GUEST_ERROR, "bcm4345: bad SDPCM data offset %u\n",
                      doff);
        return;
    }
    s->host_seq = sw[0] + 1;
    plen = len - doff;
    trace_bcm4345_receive(channel, len);

    switch (channel) {
    case SDPCM_CONTROL_CHANNEL:
        bcm4345_wl_control(s->wl, p, plen);
        break;
    case SDPCM_DATA_CHANNEL:
        if (plen < BDC_HEADER_LEN || BDC_HEADER_LEN + p[3] * 4 > plen) {
            qemu_log_mask(LOG_GUEST_ERROR, "bcm4345: short data frame\n");
            break;
        }
        bcm4345_wl_transmit(s->wl, p[2] & 0xf, p[1] & 0x7,
                            p + BDC_HEADER_LEN + p[3] * 4,
                            plen - BDC_HEADER_LEN - p[3] * 4);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "bcm4345: frame on SDPCM channel %u\n",
                      channel);
        break;
    }

    /*
     * Credit: the host learns the window from the frames it reads; with
     * none to read and half of it used, an empty frame carries a new one.
     */
    if (s->rx_head == s->rx_tail &&
        (uint8_t)(s->rx_window - s->host_seq) < SDPCM_TX_WINDOW / 2) {
        bcm4345_send(s, SDPCM_DATA_CHANNEL, NULL, 0, NULL, 0);
    }
}

static void bcm4345_f2_write(BCM4345State *s, const uint8_t *buf, size_t len)
{
    size_t n = MIN(len, BCM4345_TXBUF_SIZE - s->tx_len);

    memcpy(s->txbuf + s->tx_len, buf, n);
    s->tx_len += n;
}

/*
 * A chain of frames (bus:rxglom), in one transfer: the hardware header of
 * the first gives the length of the chain, that of each frame and its tail
 * padding are in its extension header
 */
static void bcm4345_host_chain(BCM4345State *s, size_t total)
{
    const size_t hwlen = SDPCM_HWHDR_LEN + SDPCM_HWEXT_LEN;
    size_t off = 0, len;
    const uint8_t *f;
    uint32_t ext;

    while (total - off >= hwlen + SDPCM_SWHDR_LEN) {
        f = s->txbuf + off;
        ext = ldl_le_p(f + SDPCM_HWHDR_LEN);
        len = SDPCM_HWHDR_LEN + (ext & 0xffff);
        if (len < hwlen + SDPCM_SWHDR_LEN || len > total - off ||
            (off && (uint16_t)~(lduw_le_p(f) ^ lduw_le_p(f + 2)))) {
            break;
        }
        bcm4345_host_frame(s, f, len, hwlen);
        if (ext & SDPCM_HWEXT_LASTFRM) {
            return;
        }
        off += len + lduw_le_p(f + SDPCM_HWHDR_LEN + 6);
    }
    qemu_log_mask(LOG_GUEST_ERROR, "bcm4345: bad SDPCM frame chain at %zu of "
                  "%zu bytes\n", off, total);
}

static void bcm4345_f2_write_done(BCM4345State *s)
{
    bool glom = bcm4345_wl_rx_glom(s->wl);
    uint16_t flen, chk;

    if (s->tx_len < SDPCM_HWHDR_LEN) {
        s->tx_len = 0;
        return;
    }
    flen = lduw_le_p(s->txbuf);
    chk = lduw_le_p(s->txbuf + 2);
    if ((uint16_t)~(flen ^ chk) ||
        flen < SDPCM_HDRLEN + (glom ? SDPCM_HWEXT_LEN : 0)) {
        qemu_log_mask(LOG_GUEST_ERROR, "bcm4345: bad SDPCM frame header "
                      "0x%04x 0x%04x\n", flen, chk);
        s->tx_len = 0;
        return;
    }
    if (s->tx_len < flen) {
        /* the rest comes in the next CMD53 */
        return;
    }
    s->tx_len = 0;
    if (!s->fw_ready) {
        return;
    }
    if (glom) {
        bcm4345_host_chain(s, flen);
    } else {
        bcm4345_host_frame(s, s->txbuf, flen, SDPCM_HWHDR_LEN);
    }
}

/* Firmware */

static void bcm4345_post_devready(BCM4345State *s)
{
    s->tohostmailboxdata = HMB_DATA_DEVREADY |
                           (SDPCM_PROT_VERSION << HMB_DATA_VERSION_SHIFT);
    bcm4345_set_intstatus(s, I_HMB_HOST_INT);
}

static void bcm4345_boot_done(void *opaque)
{
    BCM4345State *s = opaque;
    uint32_t shared = SDPCM_SHARED_ADDR - BCM4345_RAM_BASE;

    trace_bcm4345_firmware("running");
    s->fw_ready = true;
    s->rx_seq = 0;
    s->host_seq = 0;
    s->rx_window = SDPCM_TX_WINDOW;
    bcm4345_rx_flush(s);
    /* the firmware reads its image and the NVRAM token first */
    bcm4345_wl_start(s->wl);

    /* sdpcm_shared: version, no trap or assert, no console */
    memset(s->ram + shared, 0, SDPCM_SHARED_SIZE);
    stl_le_p(s->ram + shared, SDPCM_SHARED_VERSION);
    /* the last word of the RAM points to it */
    stl_le_p(s->ram + BCM4345_RAM_SIZE - 4, SDPCM_SHARED_ADDR);

    if (SDIO_CARD(s)->ioe & (1 << 2)) {
        bcm4345_post_devready(s);
    }
}

static void bcm4345_cpu_update(BCM4345State *s)
{
    uint32_t ioctl = s->wrap_ioctl[BCM4345_CORE_CR4];
    bool running = !(s->wrap_resetctrl[BCM4345_CORE_CR4] & AI_RESETCTRL_RESET)
                   && (ioctl & AI_IOCTL_CLK) &&
                   !(ioctl & AI_IOCTL_CR4_CPUHALT);

    if (running == s->cpu_running) {
        return;
    }
    s->cpu_running = running;
    if (running) {
        trace_bcm4345_firmware("start");
        timer_mod(s->boot_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + BCM4345_BOOT_NS);
    } else {
        trace_bcm4345_firmware("halt");
        timer_del(s->boot_timer);
        if (s->fw_ready) {
            s->fw_ready = false;
            bcm4345_wl_stop(s->wl);
        }
        bcm4345_rx_flush(s);
        s->tx_len = 0;
    }
}

/* Backplane */

static uint32_t bcm4345_cc_read(BCM4345State *s, uint32_t off)
{
    switch (off) {
    case CC_CHIPID:
        return BCM4345_CHIPID;
    case CC_CAPABILITIES:
        return CC_CAP_PMU;
    case CC_GPIOOUT:
        return s->cc_gpioout;
    case CC_GPIOOUTEN:
        return s->cc_gpioouten;
    case CC_GPIOCONTROL:
        return s->cc_gpiocontrol;
    case CC_WATCHDOG:
        return s->cc_watchdog;
    case CC_CAPABILITIES_EXT:
        return 0;
    case CC_EROMPTR:
        return BP_EROM_BASE;
    case CC_PMUCONTROL:
        return s->pmucontrol;
    case CC_PMUCAPABILITIES:
        return BCM4345_PMUCAPS;
    case CC_PMUSTATUS:
        return BCM4345_PMUSTATUS;
    case CC_PMUWATCHDOG:
        return s->pmu_watchdog;
    case CC_PMUCAPABILITIES_EXT:
        return 0;
    case CC_CHIPCONTROL_ADDR:
        return s->chipcontrol_addr;
    case CC_CHIPCONTROL_DATA:
        return s->chipcontrol_addr < ARRAY_SIZE(s->chipcontrol) ?
               s->chipcontrol[s->chipcontrol_addr] : 0;
    case CC_REGCONTROL_ADDR:
        return s->regcontrol_addr;
    case CC_REGCONTROL_DATA:
        return s->regcontrol_addr < ARRAY_SIZE(s->regcontrol) ?
               s->regcontrol[s->regcontrol_addr] : 0;
    case CC_PLLCONTROL_ADDR:
        return s->pllcontrol_addr;
    case CC_PLLCONTROL_DATA:
        return s->pllcontrol_addr < ARRAY_SIZE(s->pllcontrol) ?
               s->pllcontrol[s->pllcontrol_addr] : 0;
    default:
        return 0;
    }
}

static void bcm4345_reset_backplane(BCM4345State *s);

static void bcm4345_cc_write(BCM4345State *s, uint32_t off, uint32_t val)
{
    switch (off) {
    case CC_GPIOOUT:
        s->cc_gpioout = val;
        break;
    case CC_GPIOOUTEN:
        s->cc_gpioouten = val;
        break;
    case CC_GPIOCONTROL:
        s->cc_gpiocontrol = val;
        break;
    case CC_WATCHDOG:
    case CC_PMUWATCHDOG:
        if (val) {
            /* the watchdog resets the backplane (a few ILP ticks later) */
            trace_bcm4345_firmware("watchdog reset");
            bcm4345_reset_backplane(s);
        }
        break;
    case CC_PMUCONTROL:
        s->pmucontrol = val;
        break;
    case CC_CHIPCONTROL_ADDR:
        s->chipcontrol_addr = val;
        break;
    case CC_CHIPCONTROL_DATA:
        if (s->chipcontrol_addr < ARRAY_SIZE(s->chipcontrol)) {
            s->chipcontrol[s->chipcontrol_addr] = val;
        }
        break;
    case CC_REGCONTROL_ADDR:
        s->regcontrol_addr = val;
        break;
    case CC_REGCONTROL_DATA:
        if (s->regcontrol_addr < ARRAY_SIZE(s->regcontrol)) {
            s->regcontrol[s->regcontrol_addr] = val;
        }
        break;
    case CC_PLLCONTROL_ADDR:
        s->pllcontrol_addr = val;
        break;
    case CC_PLLCONTROL_DATA:
        if (s->pllcontrol_addr < ARRAY_SIZE(s->pllcontrol)) {
            s->pllcontrol[s->pllcontrol_addr] = val;
        }
        break;
    default:
        break;
    }
}

static uint32_t bcm4345_cr4_read(BCM4345State *s, uint32_t off)
{
    switch (off) {
    case CR4_CAP:
        return (CR4_NUM_BBANKS << 4) | CR4_NUM_ABANKS;
    case CR4_BANKIDX:
        return s->cr4_bankidx;
    case CR4_BANKINFO:
        if (s->cr4_bankidx < CR4_NUM_ABANKS + CR4_NUM_BBANKS) {
            return CR4_BANK_SIZE / 8192 - 1;
        }
        return 0;
    default:
        return 0;
    }
}

static uint32_t bcm4345_sdio_core_read(BCM4345State *s, uint32_t off)
{
    switch (off) {
    case SD_CORECONTROL:
        return s->corecontrol;
    case SD_INTSTATUS:
        return s->intstatus;
    case SD_HOSTINTMASK:
        return s->hostintmask;
    case SD_INTMASK:
        return s->intmask;
    case SD_SBINTMASK:
        return s->sbintmask;
    case SD_TOSBMAILBOX:
        return s->tosbmailbox;
    case SD_TOSBMAILBOXDATA:
        return s->tosbmailboxdata;
    case SD_TOHOSTMAILBOXDATA:
        return s->tohostmailboxdata;
    default:
        return 0;
    }
}

static void bcm4345_sdio_core_write(BCM4345State *s, uint32_t off,
                                    uint32_t val)
{
    switch (off) {
    case SD_CORECONTROL:
        s->corecontrol = val;
        break;
    case SD_INTSTATUS:
        s->intstatus &= ~val;
        bcm4345_update_irq(s);
        break;
    case SD_HOSTINTMASK:
        s->hostintmask = val;
        bcm4345_update_irq(s);
        break;
    case SD_INTMASK:
        s->intmask = val;
        break;
    case SD_SBINTMASK:
        s->sbintmask = val;
        break;
    case SD_TOSBMAILBOX:
        /* the firmware takes the host's ack or request at once */
        s->tosbmailbox = 0;
        break;
    case SD_TOSBMAILBOXDATA:
        s->tosbmailboxdata = val;
        break;
    default:
        break;
    }
}

static int bcm4345_bp_core(uint32_t addr, uint32_t *off)
{
    if (addr >= BP_ENUM_BASE &&
        addr < BP_CORE_BASE(BCM4345_NUM_CORES)) {
        *off = addr & (BP_CORE_SIZE - 1);
        return (addr - BP_ENUM_BASE) / BP_CORE_SIZE;
    }
    return -1;
}

static int bcm4345_bp_wrap(uint32_t addr, uint32_t *off)
{
    if (addr >= BP_WRAP_BASE(0) &&
        addr < BP_WRAP_BASE(BCM4345_NUM_CORES)) {
        *off = addr & (BP_CORE_SIZE - 1);
        return (addr - BP_WRAP_BASE(0)) / BP_CORE_SIZE;
    }
    return -1;
}

static bool bcm4345_bp_is_ram(uint32_t addr, uint32_t len)
{
    return addr >= BCM4345_RAM_BASE &&
           addr - BCM4345_RAM_BASE + len <= BCM4345_RAM_SIZE;
}

static uint32_t bcm4345_bp_read32(BCM4345State *s, uint32_t addr)
{
    uint32_t off, woff, val = 0;
    int core, wrap;

    addr &= ~3;
    core = bcm4345_bp_core(addr, &off);
    wrap = bcm4345_bp_wrap(addr, &woff);
    if (bcm4345_bp_is_ram(addr, 4)) {
        return ldl_le_p(s->ram + addr - BCM4345_RAM_BASE);
    } else if (addr == 0) {
        val = s->rstvec;
    } else if (core >= 0) {
        switch (core) {
        case BCM4345_CORE_CC:
            val = bcm4345_cc_read(s, off);
            break;
        case BCM4345_CORE_CR4:
            val = bcm4345_cr4_read(s, off);
            break;
        case BCM4345_CORE_SDIO:
            val = bcm4345_sdio_core_read(s, off);
            break;
        default:
            break;
        }
    } else if (wrap >= 0) {
        switch (woff) {
        case AI_IOCTL:
            val = s->wrap_ioctl[wrap];
            break;
        case AI_RESETCTRL:
            val = s->wrap_resetctrl[wrap];
            break;
        default:
            break;
        }
    } else if (addr >= BP_EROM_BASE && addr < BP_EROM_BASE + BP_EROM_SIZE) {
        val = bcm4345_erom[(addr - BP_EROM_BASE) / 4];
    } else {
        qemu_log_mask(LOG_UNIMP, "bcm4345: backplane read 0x%08x\n", addr);
    }
    trace_bcm4345_bp_read(addr, val);
    return val;
}

static void bcm4345_bp_write32(BCM4345State *s, uint32_t addr, uint32_t val)
{
    uint32_t off, woff;
    int core, wrap;

    addr &= ~3;
    trace_bcm4345_bp_write(addr, val);
    core = bcm4345_bp_core(addr, &off);
    wrap = bcm4345_bp_wrap(addr, &woff);
    if (bcm4345_bp_is_ram(addr, 4)) {
        stl_le_p(s->ram + addr - BCM4345_RAM_BASE, val);
    } else if (addr == 0) {
        s->rstvec = val;
    } else if (core >= 0) {
        switch (core) {
        case BCM4345_CORE_CC:
            bcm4345_cc_write(s, off, val);
            break;
        case BCM4345_CORE_CR4:
            if (off == CR4_BANKIDX) {
                s->cr4_bankidx = val;
            }
            break;
        case BCM4345_CORE_SDIO:
            bcm4345_sdio_core_write(s, off, val);
            break;
        default:
            break;
        }
    } else if (wrap >= 0) {
        switch (woff) {
        case AI_IOCTL:
            s->wrap_ioctl[wrap] = val & 0xffff;
            break;
        case AI_RESETCTRL:
            s->wrap_resetctrl[wrap] = val & AI_RESETCTRL_RESET;
            break;
        default:
            break;
        }
        if (wrap == BCM4345_CORE_CR4) {
            bcm4345_cpu_update(s);
        }
    } else {
        qemu_log_mask(LOG_UNIMP, "bcm4345: backplane write 0x%08x 0x%08x\n",
                      addr, val);
    }
}

static void bcm4345_bp_read(BCM4345State *s, uint32_t addr, uint8_t *buf,
                            size_t len)
{
    if (bcm4345_bp_is_ram(addr, len)) {
        memcpy(buf, s->ram + addr - BCM4345_RAM_BASE, len);
        return;
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = bcm4345_bp_read32(s, addr + i) >> (8 * ((addr + i) & 3));
    }
}

static void bcm4345_bp_write(BCM4345State *s, uint32_t addr,
                             const uint8_t *buf, size_t len)
{
    size_t i = 0;

    if (bcm4345_bp_is_ram(addr, len)) {
        memcpy(s->ram + addr - BCM4345_RAM_BASE, buf, len);
        return;
    }
    while (i < len) {
        uint32_t a = addr + i;

        if (!(a & 3) && len - i >= 4) {
            bcm4345_bp_write32(s, a, ldl_le_p(buf + i));
            i += 4;
        } else {
            /* byte write: read-modify-write of the register */
            uint32_t shift = 8 * (a & 3);
            uint32_t v = bcm4345_bp_read32(s, a);

            v = (v & ~(0xffu << shift)) | (buf[i] << shift);
            bcm4345_bp_write32(s, a, v);
            i++;
        }
    }
}

/* F1 */

static uint8_t bcm4345_clkcsr(BCM4345State *s)
{
    uint8_t v = s->clkcsr;

    if (v & (CLKCSR_FORCE_HT | CLKCSR_HT_AVAIL_REQ)) {
        v |= CLKCSR_HT_AVAIL | CLKCSR_ALP_AVAIL;
    } else {
        v |= CLKCSR_ALP_AVAIL;
    }
    return v;
}

static uint8_t bcm4345_f1_misc_read(BCM4345State *s, uint32_t addr)
{
    uint16_t left;

    switch (addr) {
    case F1_GPIO_SELECT:
        return s->gpio_sel;
    case F1_GPIO_OUT:
        return s->gpio_out;
    case F1_GPIO_EN:
        return s->gpio_en;
    case F1_WATERMARK:
        return s->watermark;
    case F1_DEVICE_CTL:
        return s->devctl;
    case F1_SBADDRLOW:
        return (s->sbaddr >> 8) & 0x80;
    case F1_SBADDRMID:
        return s->sbaddr >> 16;
    case F1_SBADDRHIGH:
        return s->sbaddr >> 24;
    case F1_CHIPCLKCSR:
        return bcm4345_clkcsr(s);
    case F1_SDIOPULLUP:
        return s->pullup;
    case F1_RFRAMEBCLO:
    case F1_RFRAMEBCHI:
        left = 0;
        if (s->rx_frame && s->rx_pos < bcm4345_rx_frame_len(s)) {
            left = bcm4345_rx_frame_len(s) - s->rx_pos;
        }
        return addr == F1_RFRAMEBCLO ? left : left >> 8;
    case F1_MESBUSYCTRL:
        return s->mesbusyctrl;
    case F1_WAKEUPCTRL:
        return s->wakeupctrl;
    case F1_SLEEPCSR:
        return s->sleepcsr;
    default:
        return 0;
    }
}

static void bcm4345_f1_misc_write(BCM4345State *s, uint32_t addr,
                                  uint8_t val)
{
    switch (addr) {
    case F1_GPIO_SELECT:
        s->gpio_sel = val;
        break;
    case F1_GPIO_OUT:
        s->gpio_out = val;
        break;
    case F1_GPIO_EN:
        s->gpio_en = val;
        break;
    case F1_WATERMARK:
        s->watermark = val;
        break;
    case F1_DEVICE_CTL:
        s->devctl = val;
        bcm4345_update_irq(s);
        break;
    case F1_SBADDRLOW:
        s->sbaddr = (s->sbaddr & 0xffff0000) | ((val & 0x80) << 8);
        break;
    case F1_SBADDRMID:
        s->sbaddr = (s->sbaddr & 0xff00ffff) | (val << 16);
        break;
    case F1_SBADDRHIGH:
        s->sbaddr = (s->sbaddr & 0x00ffffff) | (val << 24);
        break;
    case F1_FRAMECTRL:
        if (val & SFC_RF_TERM) {
            bcm4345_rx_pop(s);
        }
        if (val & SFC_WF_TERM) {
            s->tx_len = 0;
        }
        break;
    case F1_CHIPCLKCSR:
        s->clkcsr = val & CLKCSR_MASK;
        break;
    case F1_SDIOPULLUP:
        s->pullup = val;
        break;
    case F1_MESBUSYCTRL:
        s->mesbusyctrl = val;
        break;
    case F1_WAKEUPCTRL:
        s->wakeupctrl = val;
        break;
    case F1_SLEEPCSR:
        /* the device is on while kept so */
        s->sleepcsr = (val & SLEEPCSR_KSO) ? SLEEPCSR_KSO | SLEEPCSR_DEVON : 0;
        break;
    default:
        break;
    }
}

static uint32_t bcm4345_f1_bp_addr(BCM4345State *s, uint32_t addr)
{
    return s->sbaddr | (addr & F1_SB_OFT_ADDR_MASK);
}

static uint8_t bcm4345_read_byte(SDIOCard *card, unsigned fn, uint32_t addr)
{
    BCM4345State *s = BCM4345(card);
    uint8_t v = 0;

    switch (fn) {
    case 0:
        switch (addr) {
        case 0xf0:
            return s->cardcap;
        case 0xf1:
            return s->cardctrl;
        case 0xf2:
            return s->sepint;
        default:
            return 0;
        }
    case 1:
        if (addr >= F1_SPROM_CS) {
            return bcm4345_f1_misc_read(s, addr);
        }
        bcm4345_bp_read(s, bcm4345_f1_bp_addr(s, addr), &v, 1);
        return v;
    case 2:
        bcm4345_f2_read(s, &v, 1);
        return v;
    default:
        return 0;
    }
}

static void bcm4345_write_byte(SDIOCard *card, unsigned fn, uint32_t addr,
                               uint8_t value)
{
    BCM4345State *s = BCM4345(card);

    switch (fn) {
    case 0:
        switch (addr) {
        case 0xf0:
            s->cardcap = value;
            break;
        case 0xf1:
            s->cardctrl = value;
            break;
        case 0xf2:
            s->sepint = value;
            break;
        default:
            break;
        }
        break;
    case 1:
        if (addr >= F1_SPROM_CS) {
            bcm4345_f1_misc_write(s, addr, value);
        } else {
            bcm4345_bp_write(s, bcm4345_f1_bp_addr(s, addr), &value, 1);
        }
        break;
    case 2:
        bcm4345_f2_write(s, &value, 1);
        break;
    default:
        break;
    }
}

static void bcm4345_read(SDIOCard *card, unsigned fn, uint32_t addr,
                         bool incr, uint8_t *buf, size_t len)
{
    BCM4345State *s = BCM4345(card);

    if (fn == 2) {
        bcm4345_f2_read(s, buf, len);
    } else if (fn == 1 && incr && addr < F1_SPROM_CS) {
        bcm4345_bp_read(s, bcm4345_f1_bp_addr(s, addr), buf, len);
    } else {
        for (size_t i = 0; i < len; i++) {
            buf[i] = bcm4345_read_byte(card, fn, addr + (incr ? i : 0));
        }
    }
}

static void bcm4345_write(SDIOCard *card, unsigned fn, uint32_t addr,
                          bool incr, const uint8_t *buf, size_t len)
{
    BCM4345State *s = BCM4345(card);

    if (fn == 2) {
        bcm4345_f2_write(s, buf, len);
    } else if (fn == 1 && incr && addr < F1_SPROM_CS) {
        bcm4345_bp_write(s, bcm4345_f1_bp_addr(s, addr), buf, len);
    } else {
        for (size_t i = 0; i < len; i++) {
            bcm4345_write_byte(card, fn, addr + (incr ? i : 0), buf[i]);
        }
    }
}

static void bcm4345_transfer_done(SDIOCard *card, unsigned fn, bool write)
{
    BCM4345State *s = BCM4345(card);

    if (fn != 2) {
        return;
    }
    if (write) {
        bcm4345_f2_write_done(s);
    } else {
        bcm4345_f2_read_done(s);
    }
}

static void bcm4345_enable(SDIOCard *card, unsigned fn, bool enable)
{
    BCM4345State *s = BCM4345(card);

    if (fn != 2) {
        return;
    }
    if (enable && s->fw_ready) {
        bcm4345_post_devready(s);
    } else if (!enable) {
        bcm4345_rx_flush(s);
        s->tx_len = 0;
    }
}

static bool bcm4345_ready(SDIOCard *card, unsigned fn)
{
    BCM4345State *s = BCM4345(card);

    return fn != 2 || s->fw_ready;
}

static void bcm4345_reset_backplane(BCM4345State *s)
{
    for (unsigned i = 0; i < BCM4345_NUM_CORES; i++) {
        s->wrap_ioctl[i] = AI_IOCTL_CLK;
        s->wrap_resetctrl[i] = 0;
    }
    /* the ROM bootloader idles with the CR4 halted */
    s->wrap_ioctl[BCM4345_CORE_CR4] |= AI_IOCTL_CR4_CPUHALT;
    bcm4345_cpu_update(s);

    s->cc_watchdog = 0;
    s->pmu_watchdog = 0;
    s->cc_gpioout = s->cc_gpioouten = s->cc_gpiocontrol = 0;
    s->pmucontrol = 0;
    s->chipcontrol_addr = s->regcontrol_addr = s->pllcontrol_addr = 0;
    memset(s->chipcontrol, 0, sizeof(s->chipcontrol));
    memset(s->regcontrol, 0, sizeof(s->regcontrol));
    memset(s->pllcontrol, 0, sizeof(s->pllcontrol));
    s->cr4_bankidx = 0;
    s->rstvec = 0;

    s->corecontrol = 0;
    s->intstatus = 0;
    s->hostintmask = 0;
    s->intmask = 0;
    s->sbintmask = 0;
    s->tosbmailbox = 0;
    s->tosbmailboxdata = 0;
    s->tohostmailboxdata = 0;
    bcm4345_rx_flush(s);
    s->rx_in_cmd = false;
    s->tx_len = 0;
    bcm4345_update_irq(s);
}

static void bcm4345_io_reset(SDIOCard *card)
{
    BCM4345State *s = BCM4345(card);

    s->cardcap = 0;
    s->cardctrl = 0;
    s->sepint = 0;
    s->sbaddr = 0;
    s->gpio_sel = s->gpio_out = s->gpio_en = 0;
    s->watermark = 0;
    s->devctl = 0;
    s->clkcsr = 0;
    s->pullup = 0;
    s->mesbusyctrl = 0;
    s->wakeupctrl = 0;
    s->sleepcsr = SLEEPCSR_KSO | SLEEPCSR_DEVON;
    bcm4345_reset_backplane(s);
}

static NetClientInfo bcm4345_net_info;

static bool bcm4345_can_receive(NetClientState *nc)
{
    BCM4345State *s = qemu_get_nic_opaque(nc);

    return bcm4345_wl_can_receive(s->wl);
}

static ssize_t bcm4345_receive(NetClientState *nc, const uint8_t *buf,
                               size_t len)
{
    BCM4345State *s = qemu_get_nic_opaque(nc);

    return bcm4345_wl_receive(s->wl, buf, len);
}

static NetClientInfo bcm4345_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = bcm4345_can_receive,
    .receive = bcm4345_receive,
};

static void bcm4345_realize(DeviceState *dev, Error **errp)
{
    BCM4345State *s = BCM4345(dev);
    BCM4345Class *bc = BCM4345_GET_CLASS(dev);
    Error *err = NULL;

    bc->parent_realize(dev, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&bcm4345_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);

    s->ram = g_malloc0(BCM4345_RAM_SIZE);
    s->boot_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bcm4345_boot_done, s);
    s->wl = bcm4345_wl_new(s);
}

static void bcm4345_unrealize(DeviceState *dev)
{
    BCM4345State *s = BCM4345(dev);

    bcm4345_wl_free(s->wl);
    timer_free(s->boot_timer);
    g_free(s->ram);
    qemu_del_nic(s->nic);
}

static const VMStateDescription vmstate_bcm4345 = {
    .name = "bcm4345",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(parent_obj, BCM4345State, 1, vmstate_sdio_card,
                       SDIOCard),
        VMSTATE_UINT8(cardcap, BCM4345State),
        VMSTATE_UINT8(cardctrl, BCM4345State),
        VMSTATE_UINT8(sepint, BCM4345State),
        VMSTATE_UINT32(sbaddr, BCM4345State),
        VMSTATE_UINT8(gpio_sel, BCM4345State),
        VMSTATE_UINT8(gpio_out, BCM4345State),
        VMSTATE_UINT8(gpio_en, BCM4345State),
        VMSTATE_UINT8(watermark, BCM4345State),
        VMSTATE_UINT8(devctl, BCM4345State),
        VMSTATE_UINT8(clkcsr, BCM4345State),
        VMSTATE_UINT8(pullup, BCM4345State),
        VMSTATE_UINT8(mesbusyctrl, BCM4345State),
        VMSTATE_UINT8(wakeupctrl, BCM4345State),
        VMSTATE_UINT8(sleepcsr, BCM4345State),
        VMSTATE_UINT32_ARRAY(wrap_ioctl, BCM4345State, BCM4345_NUM_CORES),
        VMSTATE_UINT32_ARRAY(wrap_resetctrl, BCM4345State,
                             BCM4345_NUM_CORES),
        VMSTATE_UINT32(cc_watchdog, BCM4345State),
        VMSTATE_UINT32(cc_gpioout, BCM4345State),
        VMSTATE_UINT32(cc_gpioouten, BCM4345State),
        VMSTATE_UINT32(cc_gpiocontrol, BCM4345State),
        VMSTATE_UINT32(pmucontrol, BCM4345State),
        VMSTATE_UINT32(pmu_watchdog, BCM4345State),
        VMSTATE_UINT32(chipcontrol_addr, BCM4345State),
        VMSTATE_UINT32_ARRAY(chipcontrol, BCM4345State, 8),
        VMSTATE_UINT32(regcontrol_addr, BCM4345State),
        VMSTATE_UINT32_ARRAY(regcontrol, BCM4345State, 8),
        VMSTATE_UINT32(pllcontrol_addr, BCM4345State),
        VMSTATE_UINT32_ARRAY(pllcontrol, BCM4345State, 8),
        VMSTATE_UINT32(cr4_bankidx, BCM4345State),
        VMSTATE_UINT32(rstvec, BCM4345State),
        VMSTATE_UINT32(corecontrol, BCM4345State),
        VMSTATE_UINT32(intstatus, BCM4345State),
        VMSTATE_UINT32(hostintmask, BCM4345State),
        VMSTATE_UINT32(intmask, BCM4345State),
        VMSTATE_UINT32(sbintmask, BCM4345State),
        VMSTATE_UINT32(tosbmailbox, BCM4345State),
        VMSTATE_UINT32(tosbmailboxdata, BCM4345State),
        VMSTATE_UINT32(tohostmailboxdata, BCM4345State),
        VMSTATE_BUFFER_POINTER_UNSAFE(ram, BCM4345State, 0,
                                      BCM4345_RAM_SIZE),
        VMSTATE_BOOL(cpu_running, BCM4345State),
        VMSTATE_BOOL(fw_ready, BCM4345State),
        VMSTATE_TIMER_PTR(boot_timer, BCM4345State),
        VMSTATE_BUFFER(rxbuf, BCM4345State),
        VMSTATE_UINT32(rx_head, BCM4345State),
        VMSTATE_UINT32(rx_tail, BCM4345State),
        VMSTATE_UINT32(rx_pos, BCM4345State),
        VMSTATE_BOOL(rx_in_cmd, BCM4345State),
        VMSTATE_BOOL(rx_frame, BCM4345State),
        VMSTATE_UINT8(rx_seq, BCM4345State),
        VMSTATE_UINT8(rx_window, BCM4345State),
        VMSTATE_BUFFER(txbuf, BCM4345State),
        VMSTATE_UINT32(tx_len, BCM4345State),
        VMSTATE_UINT8(host_seq, BCM4345State),
        VMSTATE_STRUCT_POINTER(wl, BCM4345State, vmstate_bcm4345_wl,
                               BCM4345Wl),
        VMSTATE_END_OF_LIST()
    },
};

static const Property bcm4345_properties[] = {
    DEFINE_NIC_PROPERTIES(BCM4345State, conf),
};

static void bcm4345_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SDIOCardClass *sc = SDIO_CARD_CLASS(klass);
    BCM4345Class *bc = BCM4345_CLASS(klass);

    dc->desc = "Broadcom BCM4345 (CYW43455) SDIO WLAN";
    dc->vmsd = &vmstate_bcm4345;
    device_class_set_props(dc, bcm4345_properties);
    dc->unrealize = bcm4345_unrealize;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);

    device_class_set_parent_realize(dc, bcm4345_realize,
                                    &bc->parent_realize);

    sc->vendor = BCM4345_SDIO_VENDOR;
    sc->device = BCM4345_SDIO_DEVICE;
    sc->num_functions = 3;
    sc->fn_class[3] = 0x02;     /* F3: Bluetooth Type-A */
    sc->max_blksize[0] = 64;
    sc->max_blksize[1] = 64;
    sc->max_blksize[2] = 512;
    sc->max_blksize[3] = 64;

    sc->read_byte = bcm4345_read_byte;
    sc->write_byte = bcm4345_write_byte;
    sc->read = bcm4345_read;
    sc->write = bcm4345_write;
    sc->transfer_done = bcm4345_transfer_done;
    sc->enable = bcm4345_enable;
    sc->ready = bcm4345_ready;
    sc->io_reset = bcm4345_io_reset;

    bcm4345_build_erom();
}

static void bcm4345_init(Object *obj)
{
    BCM4345State *s = BCM4345(obj);

    /* settable at run time, to model the link */
    s->path_loss = BCM4345_PATH_LOSS;
    object_property_add_uint8_ptr(obj, "path-loss", &s->path_loss,
                                  OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "path-loss",
                                    "Loss (dB) of the frames received from "
                                    "senders that give their power only");
}

static const TypeInfo bcm4345_types[] = {
    {
        .name           = TYPE_BCM4345,
        .parent         = TYPE_SDIO_CARD,
        .instance_size  = sizeof(BCM4345State),
        .instance_init  = bcm4345_init,
        .class_size     = sizeof(BCM4345Class),
        .class_init     = bcm4345_class_init,
    },
};

DEFINE_TYPES(bcm4345_types)
