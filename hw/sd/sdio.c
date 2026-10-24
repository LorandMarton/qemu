/*
 * SDIO card
 *
 * The I/O card of the "SD Specifications Part E1, SDIO Simplified
 * Specification" version 3.00 on the SD bus (SD mode): initialization
 * (CMD5, CMD3, CMD7), the CCCR, FBR and CIS of function 0, CMD52 and
 * CMD53 (byte and block mode) and the card interrupt. The I/O functions
 * are provided by the subclass (SDIOCardClass); no memory portion (not a
 * combo card), no SPI mode, no UHS-I signalling, no suspend/resume.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "hw/sd/sd.h"
#include "sdio-internal.h"
#include "trace.h"

/* Card states (IO_CURRENT_STATE of R5: DIS, CMD, TRN) */
enum {
    SDIO_STATE_INIT,        /* waiting for CMD5 with an operating voltage */
    SDIO_STATE_READY,       /* initialized, waiting for CMD3 */
    SDIO_STATE_STANDBY,     /* DIS: has an RCA, not selected */
    SDIO_STATE_COMMAND,     /* CMD: selected */
    SDIO_STATE_TRANSFER,    /* TRN: CMD53 data transfer */
    SDIO_STATE_INACTIVE,
};

/* CURRENT_STATE of the R1 and R6 card status */
#define CARD_STATE_SHIFT        9
#define CARD_STATE_IDENT        2
#define CARD_STATE_STANDBY      3
#define CARD_STATE_TRANSFER     4

/* R4 (CMD5) */
#define R4_READY                (1u << 31)
#define R4_NUM_FUNCTIONS_SHIFT  28
#define R4_OCR_MASK             0x00ffffff

/* R5 flags (CMD52, CMD53) */
#define R5_COM_CRC_ERROR        0x80
#define R5_ILLEGAL_COMMAND      0x40
#define R5_STATE_SHIFT          4
#define R5_STATE_DIS            0
#define R5_STATE_CMD            1
#define R5_STATE_TRN            2
#define R5_ERROR                0x08
#define R5_FUNCTION_NUMBER      0x02
#define R5_OUT_OF_RANGE         0x01

/* CMD52 and CMD53 argument */
#define IO_RW                   (1u << 31)
#define IO_FN_SHIFT             28
#define IO_FN_MASK              0x7
#define IO_RAW                  (1u << 27)
#define IO_BLOCK_MODE           (1u << 27)
#define IO_INCR                 (1u << 26)
#define IO_ADDR_SHIFT           9
#define IO_DATA_MASK            0xff
#define IO_COUNT_MASK           0x1ff

/* OCR: 2.7-3.6 V */
#define SDIO_OCR                0x00ff8000

/* CCCR 3.00, SDIO 3.00; SD Physical 3.0x */
#define SDIO_CCCR_REV_VALUE     0x43
#define SDIO_SD_REV_VALUE       0x03

/* Card capability: SDC, SMB, S4MI */
#define SDIO_CAPS_VALUE         0x13

/* CISTPL_FUNCE TPLFE_MAX_TRAN_SPEED: 25 Mbit/s */
#define SDIO_MAX_TRAN_SPEED     0x32
/* CISTPL_FUNCE TPLFE_ENABLE_TIMEOUT_VAL, in 10 ms: 1 s */
#define SDIO_ENABLE_TIMEOUT     100

#define CISTPL_MANFID           0x20
#define CISTPL_FUNCID           0x21
#define CISTPL_FUNCE            0x22
#define CISTPL_END              0xff
#define CISTPL_FUNCID_SDIO      0x0c

static SDIOCardClass *sdio_card_get_class(SDIOCard *s)
{
    return SDIO_CARD_GET_CLASS(s);
}

static unsigned sdio_card_r5_state(SDIOCard *s)
{
    switch (s->state) {
    case SDIO_STATE_COMMAND:
        return R5_STATE_CMD;
    case SDIO_STATE_TRANSFER:
        return R5_STATE_TRN;
    default:
        return R5_STATE_DIS;
    }
}

static void sdio_card_update_irq(SDIOCard *s)
{
    bool level = (s->ien & 1) && (s->int_pending & s->ien & 0xfe);
    BusState *bus = qdev_get_parent_bus(DEVICE(s));

    if (level == s->irq) {
        return;
    }
    s->irq = level;
    trace_sdio_card_irq(level);
    if (bus) {
        sdbus_set_card_interrupt(SD_BUS(bus), level);
    }
}

void sdio_card_set_irq(SDIOCard *s, unsigned fn, bool level)
{
    assert(fn >= 1 && fn <= SDIO_MAX_FUNCTIONS);

    if (level) {
        s->int_pending |= 1 << fn;
    } else {
        s->int_pending &= ~(1 << fn);
    }
    sdio_card_update_irq(s);
}

static void sdio_card_transfer_end(SDIOCard *s)
{
    SDIOCardClass *sc = sdio_card_get_class(s);

    s->state = SDIO_STATE_COMMAND;
    s->xfer_left = 0;
    if (sc->transfer_done) {
        sc->transfer_done(s, s->xfer_fn, s->xfer_write);
    }
}

static void sdio_card_io_reset(SDIOCard *s)
{
    SDIOCardClass *sc = sdio_card_get_class(s);

    s->state = SDIO_STATE_INIT;
    s->rca = 0;
    s->ocr = 0;
    s->ioe = 0;
    s->ien = 0;
    s->bus_if = 0;
    s->speed = 0;
    s->int_ext = 0;
    memset(s->blksize, 0, sizeof(s->blksize));
    s->xfer_left = 0;
    if (sc->io_reset) {
        sc->io_reset(s);
    }
    sdio_card_update_irq(s);
}

static uint8_t sdio_card_ior(SDIOCard *s)
{
    SDIOCardClass *sc = sdio_card_get_class(s);
    uint8_t ior = 0;

    for (unsigned fn = 1; fn <= sc->num_functions; fn++) {
        if ((s->ioe & (1 << fn)) && (!sc->ready || sc->ready(s, fn))) {
            ior |= 1 << fn;
        }
    }
    return ior;
}

static uint8_t sdio_card_f0_read(SDIOCard *s, uint32_t addr)
{
    SDIOCardClass *sc = sdio_card_get_class(s);
    unsigned fn = addr >> 8;
    uint32_t cis_ptr;

    if (addr >= SDIO_CIS_BASE && addr < SDIO_CIS_BASE + sizeof(s->cis)) {
        return s->cis[addr - SDIO_CIS_BASE];
    }
    if (fn >= 1 && fn <= SDIO_MAX_FUNCTIONS) {
        /* FBR */
        if (fn > sc->num_functions) {
            return 0;
        }
        cis_ptr = SDIO_CIS_BASE + fn * SDIO_CIS_SIZE;
        switch (addr & 0xff) {
        case SDIO_FBR_STD_IF:
            return sc->fn_class[fn];
        case SDIO_FBR_CIS_PTR ... SDIO_FBR_CIS_PTR + 2:
            return cis_ptr >> (8 * ((addr & 0xff) - SDIO_FBR_CIS_PTR));
        case SDIO_FBR_BLKSIZE:
            return s->blksize[fn];
        case SDIO_FBR_BLKSIZE + 1:
            return s->blksize[fn] >> 8;
        default:
            return 0;
        }
    }
    if (fn) {
        return 0;
    }

    /* CCCR */
    switch (addr) {
    case SDIO_CCCR_REV:
        return SDIO_CCCR_REV_VALUE;
    case SDIO_CCCR_SD_REV:
        return SDIO_SD_REV_VALUE;
    case SDIO_CCCR_IOE:
        return s->ioe;
    case SDIO_CCCR_IOR:
        return sdio_card_ior(s);
    case SDIO_CCCR_IEN:
        return s->ien;
    case SDIO_CCCR_INT:
        return s->int_pending & s->ien & 0xfe;
    case SDIO_CCCR_BUS_IF:
        return s->bus_if;
    case SDIO_CCCR_CAPS:
        return SDIO_CAPS_VALUE;
    case SDIO_CCCR_CIS_PTR ... SDIO_CCCR_CIS_PTR + 2:
        return SDIO_CIS_BASE >> (8 * (addr - SDIO_CCCR_CIS_PTR));
    case SDIO_CCCR_BLKSIZE:
        return s->blksize[0];
    case SDIO_CCCR_BLKSIZE + 1:
        return s->blksize[0] >> 8;
    case SDIO_CCCR_SPEED:
        return SDIO_SPEED_SHS | (s->speed & SDIO_SPEED_EHS);
    case SDIO_CCCR_INT_EXT:
        return s->int_ext;
    case SDIO_CCCR_VENDOR ... 0xff:
        return sc->read_byte(s, 0, addr);
    default:
        return 0;
    }
}

static void sdio_card_f0_write(SDIOCard *s, uint32_t addr, uint8_t value)
{
    SDIOCardClass *sc = sdio_card_get_class(s);
    unsigned fn = addr >> 8;
    uint8_t fn_mask = ((2 << sc->num_functions) - 1) & 0xfe;
    uint8_t changed;

    if (fn >= 1 && fn <= sc->num_functions) {
        /* FBR */
        switch (addr & 0xff) {
        case SDIO_FBR_BLKSIZE:
            s->blksize[fn] = (s->blksize[fn] & 0xff00) | value;
            break;
        case SDIO_FBR_BLKSIZE + 1:
            s->blksize[fn] = (s->blksize[fn] & 0xff) | (value << 8);
            break;
        default:
            break;
        }
        return;
    }
    if (fn) {
        return;
    }

    /* CCCR */
    switch (addr) {
    case SDIO_CCCR_IOE:
        value &= fn_mask;
        changed = s->ioe ^ value;
        s->ioe = value;
        for (fn = 1; fn <= sc->num_functions; fn++) {
            if ((changed & (1 << fn)) && sc->enable) {
                sc->enable(s, fn, value & (1 << fn));
            }
        }
        break;
    case SDIO_CCCR_IEN:
        s->ien = value & (fn_mask | 1);
        sdio_card_update_irq(s);
        break;
    case SDIO_CCCR_ABORT:
        if (value & SDIO_ABORT_RES) {
            trace_sdio_card_reset();
            sdio_card_io_reset(s);
        } else if (s->state == SDIO_STATE_TRANSFER &&
                   (value & SDIO_ABORT_AS_MASK) == s->xfer_fn) {
            trace_sdio_card_abort(s->xfer_fn);
            sdio_card_transfer_end(s);
        }
        break;
    case SDIO_CCCR_BUS_IF:
        s->bus_if = value & (SDIO_BUS_CD_DISABLE | SDIO_BUS_WIDTH_MASK);
        break;
    case SDIO_CCCR_BLKSIZE:
        s->blksize[0] = (s->blksize[0] & 0xff00) | value;
        break;
    case SDIO_CCCR_BLKSIZE + 1:
        s->blksize[0] = (s->blksize[0] & 0xff) | (value << 8);
        break;
    case SDIO_CCCR_SPEED:
        s->speed = value & SDIO_SPEED_EHS;
        break;
    case SDIO_CCCR_VENDOR ... 0xff:
        sc->write_byte(s, 0, addr, value);
        break;
    default:
        break;
    }
}

static uint8_t sdio_card_read_byte(SDIOCard *s, unsigned fn, uint32_t addr)
{
    SDIOCardClass *sc = sdio_card_get_class(s);

    return fn ? sc->read_byte(s, fn, addr) : sdio_card_f0_read(s, addr);
}

static void sdio_card_write_byte(SDIOCard *s, unsigned fn, uint32_t addr,
                                 uint8_t value)
{
    SDIOCardClass *sc = sdio_card_get_class(s);

    if (fn) {
        sc->write_byte(s, fn, addr, value);
    } else {
        sdio_card_f0_write(s, addr, value);
    }
}

static size_t sdio_card_r5(SDIOCard *s, uint8_t flags, uint8_t data,
                           uint8_t *resp)
{
    flags |= sdio_card_r5_state(s) << R5_STATE_SHIFT;
    stl_be_p(resp, (flags << 8) | data);
    return 4;
}

static size_t sdio_card_cmd52(SDIOCard *s, uint32_t arg, uint8_t *resp)
{
    SDIOCardClass *sc = sdio_card_get_class(s);
    unsigned fn = (arg >> IO_FN_SHIFT) & IO_FN_MASK;
    uint32_t addr = (arg >> IO_ADDR_SHIFT) & SDIO_ADDR_MASK;
    uint8_t data = 0;

    if (fn > sc->num_functions) {
        return sdio_card_r5(s, R5_FUNCTION_NUMBER, 0, resp);
    }
    if (arg & IO_RW) {
        sdio_card_write_byte(s, fn, addr, arg & IO_DATA_MASK);
        if (arg & IO_RAW) {
            data = sdio_card_read_byte(s, fn, addr);
        }
    } else {
        data = sdio_card_read_byte(s, fn, addr);
    }
    trace_sdio_card_cmd52(arg & IO_RW ? "write" : "read", fn, addr,
                          arg & IO_RW ? arg & IO_DATA_MASK : data);
    return sdio_card_r5(s, 0, data, resp);
}

static size_t sdio_card_cmd53(SDIOCard *s, uint32_t arg, uint8_t *resp)
{
    SDIOCardClass *sc = sdio_card_get_class(s);
    unsigned fn = (arg >> IO_FN_SHIFT) & IO_FN_MASK;
    uint32_t count = arg & IO_COUNT_MASK;
    uint32_t len;

    if (fn > sc->num_functions) {
        return sdio_card_r5(s, R5_FUNCTION_NUMBER, 0, resp);
    }
    if (arg & IO_BLOCK_MODE) {
        if (!count || !s->blksize[fn]) {
            /* infinite block transfers are not implemented */
            qemu_log_mask(LOG_UNIMP, "sdio: CMD53 of %u blocks of %u bytes\n",
                          count, s->blksize[fn]);
            return sdio_card_r5(s, R5_ERROR, 0, resp);
        }
        len = count * s->blksize[fn];
    } else {
        len = count ? count : 512;
    }

    s->xfer_write = arg & IO_RW;
    s->xfer_fn = fn;
    s->xfer_incr = arg & IO_INCR;
    s->xfer_addr = (arg >> IO_ADDR_SHIFT) & SDIO_ADDR_MASK;
    s->xfer_left = len;
    s->state = SDIO_STATE_TRANSFER;
    trace_sdio_card_cmd53(s->xfer_write ? "write" : "read", fn, s->xfer_addr,
                          len, s->xfer_incr);
    return sdio_card_r5(s, 0, 0, resp);
}

static size_t sdio_card_do_command(SDState *sd, SDRequest *req,
                                   uint8_t *resp, size_t respsz)
{
    SDIOCard *s = SDIO_CARD(sd);
    SDIOCardClass *sc = sdio_card_get_class(s);
    uint32_t ocr;

    assert(respsz >= 4);
    trace_sdio_card_command(req->cmd, req->arg, s->state);

    if (s->state == SDIO_STATE_INACTIVE) {
        return 0;
    }

    switch (req->cmd) {
    case 5:     /* IO_SEND_OP_COND, R4 */
        ocr = req->arg & R4_OCR_MASK;
        if (s->state == SDIO_STATE_INIT && ocr) {
            if (!(ocr & SDIO_OCR)) {
                s->state = SDIO_STATE_INACTIVE;
                return 0;
            }
            s->ocr = ocr & SDIO_OCR;
            s->state = SDIO_STATE_READY;
        }
        stl_be_p(resp, (s->state != SDIO_STATE_INIT ? R4_READY : 0) |
                       (sc->num_functions << R4_NUM_FUNCTIONS_SHIFT) |
                       SDIO_OCR);
        return 4;

    case 3:     /* SEND_RELATIVE_ADDR, R6 */
        if (s->state != SDIO_STATE_READY && s->state != SDIO_STATE_STANDBY) {
            return 0;
        }
        s->rca = 1;
        s->state = SDIO_STATE_STANDBY;
        stl_be_p(resp, (s->rca << 16) | (CARD_STATE_IDENT << CARD_STATE_SHIFT));
        return 4;

    case 7:     /* SELECT/DESELECT_CARD, R1b */
        if (s->state != SDIO_STATE_STANDBY &&
            s->state != SDIO_STATE_COMMAND) {
            return 0;
        }
        if ((req->arg >> 16) != s->rca) {
            s->state = SDIO_STATE_STANDBY;
            return 0;
        }
        stl_be_p(resp, (s->state == SDIO_STATE_STANDBY ?
                        CARD_STATE_STANDBY : CARD_STATE_TRANSFER)
                       << CARD_STATE_SHIFT | READY_FOR_DATA);
        s->state = SDIO_STATE_COMMAND;
        return 4;

    case 15:    /* GO_INACTIVE_STATE */
        if ((req->arg >> 16) == s->rca && s->state >= SDIO_STATE_STANDBY) {
            s->state = SDIO_STATE_INACTIVE;
        }
        return 0;

    case 52:    /* IO_RW_DIRECT, R5 */
        if (s->state != SDIO_STATE_COMMAND &&
            s->state != SDIO_STATE_TRANSFER) {
            /* CCCR RES is accepted in any state */
            if ((req->arg & IO_RW) &&
                !((req->arg >> IO_FN_SHIFT) & IO_FN_MASK) &&
                ((req->arg >> IO_ADDR_SHIFT) & SDIO_ADDR_MASK) ==
                    SDIO_CCCR_ABORT &&
                (req->arg & SDIO_ABORT_RES)) {
                trace_sdio_card_reset();
                sdio_card_io_reset(s);
            }
            return 0;
        }
        return sdio_card_cmd52(s, req->arg, resp);

    case 53:    /* IO_RW_EXTENDED, R5 */
        if (s->state != SDIO_STATE_COMMAND) {
            return 0;
        }
        return sdio_card_cmd53(s, req->arg, resp);

    case 0:     /* GO_IDLE_STATE */
    case 8:     /* SEND_IF_COND */
        /* part of every card initialization; an I/O card ignores them */
        return 0;

    default:
        /* memory card commands: no response */
        qemu_log_mask(LOG_GUEST_ERROR, "sdio: CMD%d not supported\n",
                      req->cmd);
        return 0;
    }
}

static size_t sdio_card_read_data(SDState *sd, void *buf, size_t len)
{
    SDIOCard *s = SDIO_CARD(sd);
    SDIOCardClass *sc = sdio_card_get_class(s);
    size_t n;

    if (s->state != SDIO_STATE_TRANSFER || s->xfer_write) {
        qemu_log_mask(LOG_GUEST_ERROR, "sdio: read without a transfer\n");
        memset(buf, 0, len);
        return len;
    }

    n = MIN(len, s->xfer_left);
    if (!s->xfer_fn) {
        for (size_t i = 0; i < n; i++) {
            ((uint8_t *)buf)[i] =
                sdio_card_f0_read(s, s->xfer_addr + (s->xfer_incr ? i : 0));
        }
    } else if (sc->read) {
        sc->read(s, s->xfer_fn, s->xfer_addr, s->xfer_incr, buf, n);
    } else {
        for (size_t i = 0; i < n; i++) {
            ((uint8_t *)buf)[i] =
                sc->read_byte(s, s->xfer_fn,
                              s->xfer_addr + (s->xfer_incr ? i : 0));
        }
    }
    if (s->xfer_incr) {
        s->xfer_addr = (s->xfer_addr + n) & SDIO_ADDR_MASK;
    }
    s->xfer_left -= n;
    if (!s->xfer_left) {
        sdio_card_transfer_end(s);
    }
    memset(buf + n, 0, len - n);
    return len;
}

static size_t sdio_card_write_data(SDState *sd, const void *buf, size_t len)
{
    SDIOCard *s = SDIO_CARD(sd);
    SDIOCardClass *sc = sdio_card_get_class(s);
    size_t n;

    if (s->state != SDIO_STATE_TRANSFER || !s->xfer_write) {
        qemu_log_mask(LOG_GUEST_ERROR, "sdio: write without a transfer\n");
        return len;
    }

    n = MIN(len, s->xfer_left);
    if (!s->xfer_fn) {
        for (size_t i = 0; i < n; i++) {
            sdio_card_f0_write(s, s->xfer_addr + (s->xfer_incr ? i : 0),
                               ((const uint8_t *)buf)[i]);
        }
    } else if (sc->write) {
        sc->write(s, s->xfer_fn, s->xfer_addr, s->xfer_incr, buf, n);
    } else {
        for (size_t i = 0; i < n; i++) {
            sc->write_byte(s, s->xfer_fn,
                           s->xfer_addr + (s->xfer_incr ? i : 0),
                           ((const uint8_t *)buf)[i]);
        }
    }
    if (s->xfer_incr) {
        s->xfer_addr = (s->xfer_addr + n) & SDIO_ADDR_MASK;
    }
    s->xfer_left -= n;
    if (!s->xfer_left) {
        sdio_card_transfer_end(s);
    }
    return len;
}

static bool sdio_card_data_ready(SDState *sd)
{
    SDIOCard *s = SDIO_CARD(sd);

    return s->state == SDIO_STATE_TRANSFER && !s->xfer_write;
}

static bool sdio_card_receive_ready(SDState *sd)
{
    SDIOCard *s = SDIO_CARD(sd);

    return s->state == SDIO_STATE_TRANSFER && s->xfer_write;
}

static bool sdio_card_get_inserted(SDState *sd)
{
    return true;
}

static bool sdio_card_get_readonly(SDState *sd)
{
    return false;
}

static void sdio_card_set_voltage(SDState *sd, uint16_t millivolts)
{
    if (millivolts != SD_VOLTAGE_3_3V && millivolts != SD_VOLTAGE_3_0V) {
        qemu_log_mask(LOG_GUEST_ERROR, "sdio: %u mV not supported\n",
                      millivolts);
    }
}

static uint8_t sdio_card_get_dat_lines(SDState *sd)
{
    SDIOCard *s = SDIO_CARD(sd);

    /* DAT[1] low: interrupt */
    return s->irq ? 0b1101 : 0b1111;
}

static bool sdio_card_get_cmd_line(SDState *sd)
{
    return true;
}

static bool sdio_card_get_card_interrupt(SDState *sd)
{
    return SDIO_CARD(sd)->irq;
}

static size_t sdio_card_cis_append(uint8_t *cis, size_t pos, uint8_t code,
                                   const uint8_t *body, uint8_t len)
{
    cis[pos++] = code;
    cis[pos++] = len;
    memcpy(cis + pos, body, len);
    return pos + len;
}

static void sdio_card_build_cis(SDIOCard *s)
{
    SDIOCardClass *sc = sdio_card_get_class(s);
    uint8_t manfid[4], funcid[2] = { CISTPL_FUNCID_SDIO, 0 };
    uint8_t funce0[4], funce[42];
    size_t pos;

    stw_le_p(manfid, sc->vendor);
    stw_le_p(manfid + 2, sc->device);

    /* Common CIS */
    funce0[0] = 0x00;
    stw_le_p(funce0 + 1, sc->max_blksize[0]);
    funce0[3] = SDIO_MAX_TRAN_SPEED;
    pos = sdio_card_cis_append(s->cis, 0, CISTPL_MANFID, manfid,
                               sizeof(manfid));
    pos = sdio_card_cis_append(s->cis, pos, CISTPL_FUNCID, funcid,
                               sizeof(funcid));
    pos = sdio_card_cis_append(s->cis, pos, CISTPL_FUNCE, funce0,
                               sizeof(funce0));
    s->cis[pos] = CISTPL_END;

    /* Function CIS */
    for (unsigned fn = 1; fn <= sc->num_functions; fn++) {
        uint8_t *cis = s->cis + fn * SDIO_CIS_SIZE;

        memset(funce, 0, sizeof(funce));
        funce[0] = 0x01;
        stw_le_p(funce + 12, sc->max_blksize[fn]);
        stl_le_p(funce + 14, SDIO_OCR);
        stw_le_p(funce + 28, SDIO_ENABLE_TIMEOUT);
        pos = sdio_card_cis_append(cis, 0, CISTPL_MANFID, manfid,
                                   sizeof(manfid));
        pos = sdio_card_cis_append(cis, pos, CISTPL_FUNCID, funcid,
                                   sizeof(funcid));
        pos = sdio_card_cis_append(cis, pos, CISTPL_FUNCE, funce,
                                   sizeof(funce));
        cis[pos] = CISTPL_END;
    }
}

static void sdio_card_realize(DeviceState *dev, Error **errp)
{
    SDIOCard *s = SDIO_CARD(dev);
    SDIOCardClass *sc = sdio_card_get_class(s);

    assert(sc->num_functions >= 1 && sc->num_functions <= SDIO_MAX_FUNCTIONS);
    assert(sc->read_byte && sc->write_byte);

    if (s->parent_obj.blk) {
        error_setg(errp, "an SDIO card has no storage");
        return;
    }
    sdio_card_build_cis(s);
}

static void sdio_card_reset(DeviceState *dev)
{
    SDIOCard *s = SDIO_CARD(dev);

    sdio_card_io_reset(s);
}

const VMStateDescription vmstate_sdio_card = {
    .name = "sdio-card",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(state, SDIOCard),
        VMSTATE_UINT16(rca, SDIOCard),
        VMSTATE_UINT32(ocr, SDIOCard),
        VMSTATE_UINT8(ioe, SDIOCard),
        VMSTATE_UINT8(ien, SDIOCard),
        VMSTATE_UINT8(bus_if, SDIOCard),
        VMSTATE_UINT8(speed, SDIOCard),
        VMSTATE_UINT8(int_ext, SDIOCard),
        VMSTATE_UINT16_ARRAY(blksize, SDIOCard, SDIO_MAX_FUNCTIONS + 1),
        VMSTATE_UINT8(int_pending, SDIOCard),
        VMSTATE_BOOL(xfer_write, SDIOCard),
        VMSTATE_UINT8(xfer_fn, SDIOCard),
        VMSTATE_BOOL(xfer_incr, SDIOCard),
        VMSTATE_UINT32(xfer_addr, SDIOCard),
        VMSTATE_UINT32(xfer_left, SDIOCard),
        VMSTATE_BOOL(irq, SDIOCard),
        VMSTATE_END_OF_LIST()
    },
};

static void sdio_card_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SDCardClass *sc = SDMMC_COMMON_CLASS(klass);

    dc->realize = sdio_card_realize;
    dc->vmsd = &vmstate_sdio_card;
    device_class_set_legacy_reset(dc, sdio_card_reset);
    clear_bit(DEVICE_CATEGORY_STORAGE, dc->categories);

    sc->do_command = sdio_card_do_command;
    sc->read_data = sdio_card_read_data;
    sc->write_data = sdio_card_write_data;
    sc->data_ready = sdio_card_data_ready;
    sc->receive_ready = sdio_card_receive_ready;
    sc->get_inserted = sdio_card_get_inserted;
    sc->get_readonly = sdio_card_get_readonly;
    sc->set_voltage = sdio_card_set_voltage;
    sc->get_dat_lines = sdio_card_get_dat_lines;
    sc->get_cmd_line = sdio_card_get_cmd_line;
    sc->get_card_interrupt = sdio_card_get_card_interrupt;
}

static const TypeInfo sdio_card_types[] = {
    {
        .name           = TYPE_SDIO_CARD,
        .parent         = TYPE_SDMMC_COMMON,
        .abstract       = true,
        .instance_size  = sizeof(SDIOCard),
        .class_size     = sizeof(SDIOCardClass),
        .class_init     = sdio_card_class_init,
    },
};

DEFINE_TYPES(sdio_card_types)
