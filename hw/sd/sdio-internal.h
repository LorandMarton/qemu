/*
 * SDIO card
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SDIO_INTERNAL_H
#define SDIO_INTERNAL_H

#include "hw/sd/sd.h"
#include "sdmmc-internal.h"

#define TYPE_SDIO_CARD "sdio-card"
OBJECT_DECLARE_TYPE(SDIOCard, SDIOCardClass, SDIO_CARD)

#define SDIO_MAX_FUNCTIONS      7

/* CCCR (function 0, 0x00-0xff) */
#define SDIO_CCCR_REV           0x00
#define SDIO_CCCR_SD_REV        0x01
#define SDIO_CCCR_IOE           0x02
#define SDIO_CCCR_IOR           0x03
#define SDIO_CCCR_IEN           0x04
#define SDIO_CCCR_INT           0x05
#define SDIO_CCCR_ABORT         0x06
#define  SDIO_ABORT_AS_MASK     0x07
#define  SDIO_ABORT_RES         0x08
#define SDIO_CCCR_BUS_IF        0x07
#define  SDIO_BUS_WIDTH_MASK    0x03
#define  SDIO_BUS_CD_DISABLE    0x80
#define SDIO_CCCR_CAPS          0x08
#define SDIO_CCCR_CIS_PTR       0x09
#define SDIO_CCCR_SUSPEND       0x0c
#define SDIO_CCCR_SELECT        0x0d
#define SDIO_CCCR_EXEC          0x0e
#define SDIO_CCCR_READY         0x0f
#define SDIO_CCCR_BLKSIZE       0x10
#define SDIO_CCCR_POWER         0x12
#define SDIO_CCCR_SPEED         0x13
#define  SDIO_SPEED_SHS         0x01
#define  SDIO_SPEED_EHS         0x02
#define SDIO_CCCR_UHS           0x14
#define SDIO_CCCR_DRIVE         0x15
#define SDIO_CCCR_INT_EXT       0x16
#define SDIO_CCCR_VENDOR        0xf0

/* FBR of function n at 0x100 * n */
#define SDIO_FBR_BASE(fn)       ((fn) * 0x100)
#define SDIO_FBR_STD_IF         0x00
#define SDIO_FBR_STD_IF_EXT     0x01
#define SDIO_FBR_POWER          0x02
#define SDIO_FBR_CIS_PTR        0x09
#define SDIO_FBR_CSA_PTR        0x0c
#define SDIO_FBR_CSA_DATA       0x0f
#define SDIO_FBR_BLKSIZE        0x10

/* CIS area of function 0 */
#define SDIO_CIS_BASE           0x1000
#define SDIO_CIS_SIZE           0x100

/* Register address space of a function (17-bit CMD52/CMD53 address) */
#define SDIO_ADDR_MASK          0x1ffff

struct SDIOCard {
    SDState parent_obj;

    /* Bus state */
    uint32_t state;
    uint16_t rca;
    uint32_t ocr;

    /* CCCR */
    uint8_t ioe;
    uint8_t ien;
    uint8_t bus_if;
    uint8_t speed;
    uint8_t int_ext;
    uint16_t blksize[SDIO_MAX_FUNCTIONS + 1];

    /* Function interrupts (bit n: function n), as signalled by the model */
    uint8_t int_pending;

    /* CMD53 in progress */
    bool xfer_write;
    uint8_t xfer_fn;
    bool xfer_incr;
    uint32_t xfer_addr;
    uint32_t xfer_left;

    /* Card interrupt on DAT[1] */
    bool irq;

    /* CIS area, built at realize */
    uint8_t cis[(SDIO_MAX_FUNCTIONS + 1) * SDIO_CIS_SIZE];
};

/**
 * SDIOCardClass:
 * @vendor, @device: SDIO manufacturer and card ID (CISTPL_MANFID)
 * @num_functions: number of I/O functions (1-7)
 * @fn_class: standard I/O interface code of each function (FBR 0x00)
 * @max_blksize: maximum block size of function 0 and each function
 * @read_byte: read a function register (fn 1-7), or the CCCR vendor
 *   area 0xf0-0xff (fn 0)
 * @write_byte: write a function register or the CCCR vendor area
 * @read: CMD53 read of @len bytes at @addr, the address incrementing
 *   with each byte when @incr; default: @read_byte for each byte
 * @write: CMD53 write; default: @write_byte for each byte
 * @transfer_done: optional, called at the end of each CMD53 data transfer
 *   to or from function @fn, including an abort through the CCCR
 * @enable: optional, function @fn enabled or disabled (CCCR IOEx)
 * @ready: optional, CCCR IORx of function @fn; default: enabled
 * @io_reset: optional, reset of the I/O functions (power on, CCCR RES)
 */
struct SDIOCardClass {
    SDCardClass parent_class;

    uint16_t vendor;
    uint16_t device;
    uint8_t num_functions;
    uint8_t fn_class[SDIO_MAX_FUNCTIONS + 1];
    uint16_t max_blksize[SDIO_MAX_FUNCTIONS + 1];

    uint8_t (*read_byte)(SDIOCard *card, unsigned fn, uint32_t addr);
    void (*write_byte)(SDIOCard *card, unsigned fn, uint32_t addr,
                       uint8_t value);
    void (*read)(SDIOCard *card, unsigned fn, uint32_t addr, bool incr,
                 uint8_t *buf, size_t len);
    void (*write)(SDIOCard *card, unsigned fn, uint32_t addr, bool incr,
                  const uint8_t *buf, size_t len);
    void (*transfer_done)(SDIOCard *card, unsigned fn, bool write);
    void (*enable)(SDIOCard *card, unsigned fn, bool enable);
    bool (*ready)(SDIOCard *card, unsigned fn);
    void (*io_reset)(SDIOCard *card);
};

extern const VMStateDescription vmstate_sdio_card;

/**
 * sdio_card_set_irq: interrupt request of an I/O function
 * @card: card
 * @fn: function (1-7)
 * @level: true while the function requests an interrupt
 *
 * The card signals an interrupt to the host while a function enabled
 * in the CCCR IEN register requests one.
 */
void sdio_card_set_irq(SDIOCard *card, unsigned fn, bool level);

#endif
