/*
 * Broadcom BCM4345 (Cypress CYW43455) SDIO WLAN chip
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef BCM4345_INTERNAL_H
#define BCM4345_INTERNAL_H

#include "qemu/units.h"
#include "net/net.h"
#include "hw/sd/bcm4345.h"
#include "sdio-internal.h"

OBJECT_DECLARE_TYPE(BCM4345State, BCM4345Class, BCM4345)

/* Backplane cores, in EROM order */
enum {
    BCM4345_CORE_CC,
    BCM4345_CORE_D11,
    BCM4345_CORE_CR4,
    BCM4345_CORE_SDIO,
    BCM4345_CORE_GCI,
    BCM4345_NUM_CORES,
};

/* Firmware RAM (ARM CR4 TCM) */
#define BCM4345_RAM_BASE        0x198000
#define BCM4345_RAM_SIZE        0xc8000

/* SDPCM channels */
#define SDPCM_CONTROL_CHANNEL   0
#define SDPCM_EVENT_CHANNEL     1
#define SDPCM_DATA_CHANNEL      2

/* F2 frames to the host: [2-byte length][frame] back to back */
#define BCM4345_RXBUF_SIZE      (128 * KiB)
/* F2 frame from the host */
/* a frame, or a chain of frames: up to the 16-bit length of the SDPCM header */
#define BCM4345_TXBUF_SIZE      (64 * KiB)

/* BDC header of data and event frames: flags, priority, flags2, offset */
#define BDC_HEADER_LEN          4

/* Loss of the radio link (dB), from a sender's power to the signal */
#define BCM4345_PATH_LOSS       75

typedef struct BCM4345Wl BCM4345Wl;

struct BCM4345Class {
    SDIOCardClass parent_class;

    DeviceRealize parent_realize;
};

struct BCM4345State {
    SDIOCard parent_obj;

    NICState *nic;
    NICConf conf;
    uint8_t path_loss;

    /* F0 vendor CCCR (0xf0-0xff) */
    uint8_t cardcap;
    uint8_t cardctrl;
    uint8_t sepint;

    /* F1 registers */
    uint32_t sbaddr;
    uint8_t gpio_sel;
    uint8_t gpio_out;
    uint8_t gpio_en;
    uint8_t watermark;
    uint8_t devctl;
    uint8_t clkcsr;
    uint8_t pullup;
    uint8_t mesbusyctrl;
    uint8_t wakeupctrl;
    uint8_t sleepcsr;

    /* AI wrappers */
    uint32_t wrap_ioctl[BCM4345_NUM_CORES];
    uint32_t wrap_resetctrl[BCM4345_NUM_CORES];

    /* ChipCommon and its PMU */
    uint32_t cc_watchdog;
    uint32_t cc_gpioout;
    uint32_t cc_gpioouten;
    uint32_t cc_gpiocontrol;
    uint32_t pmucontrol;
    uint32_t pmu_watchdog;
    uint32_t chipcontrol_addr;
    uint32_t chipcontrol[8];
    uint32_t regcontrol_addr;
    uint32_t regcontrol[8];
    uint32_t pllcontrol_addr;
    uint32_t pllcontrol[8];

    /* ARM CR4 */
    uint32_t cr4_bankidx;
    uint32_t rstvec;

    /* SDIO device core */
    uint32_t corecontrol;
    uint32_t intstatus;
    uint32_t hostintmask;
    uint32_t intmask;
    uint32_t sbintmask;
    uint32_t tosbmailbox;
    uint32_t tosbmailboxdata;
    uint32_t tohostmailboxdata;

    /* Firmware RAM */
    uint8_t *ram;

    /* Firmware state */
    bool cpu_running;
    bool fw_ready;
    QEMUTimer *boot_timer;

    /* F2 frames to the host */
    uint8_t rxbuf[BCM4345_RXBUF_SIZE];
    uint32_t rx_head;
    uint32_t rx_tail;
    uint32_t rx_pos;
    bool rx_in_cmd;
    bool rx_frame;
    uint8_t rx_seq;
    uint8_t rx_window;          /* in the last frame the host began to read */
    /* F2 frame from the host */
    uint8_t txbuf[BCM4345_TXBUF_SIZE];
    uint32_t tx_len;
    uint8_t host_seq;

    BCM4345Wl *wl;
};

/* Firmware (wl) side, bcm4345-wl.c */
BCM4345Wl *bcm4345_wl_new(BCM4345State *s);
void bcm4345_wl_free(BCM4345Wl *wl);
void bcm4345_wl_start(BCM4345Wl *wl);
void bcm4345_wl_stop(BCM4345Wl *wl);
/* BCDC control message (struct brcmf_proto_bcdc_dcmd and buffer) */
void bcm4345_wl_control(BCM4345Wl *wl, const uint8_t *msg, size_t len);
/* Data frame from the host: interface, priority, Ethernet frame */
void bcm4345_wl_transmit(BCM4345Wl *wl, unsigned ifidx, unsigned prio,
                         const uint8_t *eth, size_t len);
/* Frame from the air (the netdev) */
ssize_t bcm4345_wl_receive(BCM4345Wl *wl, const uint8_t *buf, size_t len);
bool bcm4345_wl_can_receive(BCM4345Wl *wl);
const char *bcm4345_wl_version(BCM4345Wl *wl);
bool bcm4345_wl_rx_glom(BCM4345Wl *wl);
extern const VMStateDescription vmstate_bcm4345_wl;

/* Chip side, bcm4345.c: queue a frame for the host on an SDPCM channel */
void bcm4345_send(BCM4345State *s, unsigned channel, const uint8_t *hdr,
                  size_t hdr_len, const uint8_t *data, size_t len);
bool bcm4345_rx_has_room(BCM4345State *s);

#endif
