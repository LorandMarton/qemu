/*
 * Raspberry Pi (BCM2838) GPIO Controller
 * This implementation is based on bcm2835_gpio (hw/gpio/bcm2835_gpio.c)
 *
 * Copyright (c) 2022 Auriga LLC
 *
 * Authors:
 *  Lotosh, Aleksey <aleksey.lotosh@auriga.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "hw/sd/sd.h"
#include "hw/gpio/bcm2838_gpio.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "qemu/cutils.h"

#define GPFSEL0   0x00
#define GPFSEL1   0x04
#define GPFSEL2   0x08
#define GPFSEL3   0x0C
#define GPFSEL4   0x10
#define GPFSEL5   0x14
#define GPSET0    0x1C
#define GPSET1    0x20
#define GPCLR0    0x28
#define GPCLR1    0x2C
#define GPLEV0    0x34
#define GPLEV1    0x38
#define GPEDS0    0x40
#define GPEDS1    0x44
#define GPREN0    0x4C
#define GPREN1    0x50
#define GPFEN0    0x58
#define GPFEN1    0x5C
#define GPHEN0    0x64
#define GPHEN1    0x68
#define GPLEN0    0x70
#define GPLEN1    0x74
#define GPAREN0   0x7C
#define GPAREN1   0x80
#define GPAFEN0   0x88
#define GPAFEN1   0x8C

#define GPIO_PUP_PDN_CNTRL_REG0 0xE4
#define GPIO_PUP_PDN_CNTRL_REG1 0xE8
#define GPIO_PUP_PDN_CNTRL_REG2 0xEC
#define GPIO_PUP_PDN_CNTRL_REG3 0xF0

#define RESET_VAL_CNTRL_REG0 0xAAA95555
#define RESET_VAL_CNTRL_REG1 0xA0AAAAAA
#define RESET_VAL_CNTRL_REG2 0x50AAA95A
#define RESET_VAL_CNTRL_REG3 0x00055555

#define NUM_FSELN_IN_GPFSELN 10
#define NUM_BITS_FSELN       3
#define MASK_FSELN           0x7

#define BYTES_IN_WORD        4

/* bcm,function property */
#define BCM2838_FSEL_GPIO_IN    0
#define BCM2838_FSEL_GPIO_OUT   1
#define BCM2838_FSEL_ALT5       2
#define BCM2838_FSEL_ALT4       3
#define BCM2838_FSEL_ALT0       4
#define BCM2838_FSEL_ALT1       5
#define BCM2838_FSEL_ALT2       6
#define BCM2838_FSEL_ALT3       7

static uint32_t gpfsel_get(BCM2838GpioState *s, uint8_t reg)
{
    int i;
    uint32_t value = 0;
    for (i = 0; i < NUM_FSELN_IN_GPFSELN; i++) {
        uint32_t index = NUM_FSELN_IN_GPFSELN * reg + i;
        if (index < sizeof(s->fsel)) {
            value |= (s->fsel[index] & MASK_FSELN) << (NUM_BITS_FSELN * i);
        }
    }
    return value;
}

static void gpio_sd_route(BCM2838GpioState *s);

static void gpfsel_set(BCM2838GpioState *s, uint8_t reg, uint32_t value)
{
    int i;
    for (i = 0; i < NUM_FSELN_IN_GPFSELN; i++) {
        uint32_t index = NUM_FSELN_IN_GPFSELN * reg + i;
        if (index < sizeof(s->fsel)) {
            int fsel = (value >> (NUM_BITS_FSELN * i)) & MASK_FSELN;
            s->fsel[index] = fsel;
        }
    }
    gpio_sd_route(s);
}

/* sd_fsel: where the card of pins 48-53 is; not on a controller */
#define BCM2838_SD_UNROUTED     0xff

static SDBus *gpio_sd_bus(BCM2838GpioState *s, uint8_t sd_fsel)
{
    switch (sd_fsel) {
    case BCM2838_FSEL_GPIO_IN:
        return s->sdbus_sdhci;
    case BCM2838_FSEL_ALT0:
        return s->sdbus_sdhost;
    default:
        return &s->sdbus;
    }
}

static bool gpio_sd_pins(BCM2838GpioState *s, uint8_t fsel)
{
    int i;

    for (i = 48; i <= 53; i++) {
        if (s->fsel[i] != fsel) {
            return false;
        }
    }
    return true;
}

/*
 * The SD card on pins 48-53 is on the SDHost when they are in ALT0, on
 * the SDHCI when they are inputs, unless SD1 is on pins 34-39 (ALT3, the
 * wireless chip of the Raspberry Pi 4 and CM4) and so not on these pins.
 * Only this card moves: the controllers may have cards of their own.
 */
static void gpio_sd_route(BCM2838GpioState *s)
{
    uint8_t sd_fsel = s->sd_fsel;

    if (gpio_sd_pins(s, BCM2838_FSEL_GPIO_IN) &&
        s->fsel[34] != BCM2838_FSEL_ALT3) {
        sd_fsel = BCM2838_FSEL_GPIO_IN;
    } else if (gpio_sd_pins(s, BCM2838_FSEL_ALT0)) {
        sd_fsel = BCM2838_FSEL_ALT0;
    }
    if (sd_fsel != s->sd_fsel) {
        sdbus_reparent_card(gpio_sd_bus(s, s->sd_fsel),
                            gpio_sd_bus(s, sd_fsel));
        s->sd_fsel = sd_fsel;
    }
}

#define GPIO_EXT_LOW    0
#define GPIO_EXT_HIGH   1
#define GPIO_EXT_NONE   2

#define GPIO_PULL_NONE  0
#define GPIO_PULL_UP    1
#define GPIO_PULL_DOWN  2

#define GPIO_BANK0_MASK MAKE_64BIT_MASK(0, 28)
#define GPIO_BANK1_MASK MAKE_64BIT_MASK(28, 18)
#define GPIO_BANK2_MASK MAKE_64BIT_MASK(46, 12)
#define GPIO_ALL_MASK   MAKE_64BIT_MASK(0, BCM2838_GPIO_NUM)

static uint64_t gpio_word64(const uint32_t *w)
{
    return ((uint64_t)w[1] << 32 | w[0]) & GPIO_ALL_MASK;
}

static int gpio_pull(BCM2838GpioState *s, int pin)
{
    return extract32(s->pup_cntrl_reg[pin / 16], (pin % 16) * 2, 2);
}

/*
 * SPI chip enables (BCM2711 alternate functions), CE0 and CE1 of SPI0 and
 * SPI3..SPI6 in turn; the first pin of each is where the board's device is.
 */
static const struct {
    uint8_t cs, pin, fsel;
} gpio_spi_ce_pins[] = {
    { 0, 8, BCM2838_FSEL_ALT0 }, { 0, 36, BCM2838_FSEL_ALT0 },
    { 0, 43, BCM2838_FSEL_ALT4 }, { 0, 46, BCM2838_FSEL_ALT2 },
    { 1, 7, BCM2838_FSEL_ALT0 }, { 1, 35, BCM2838_FSEL_ALT0 },
    { 1, 44, BCM2838_FSEL_ALT4 },
    { 2, 0, BCM2838_FSEL_ALT3 }, { 3, 24, BCM2838_FSEL_ALT5 },
    { 4, 4, BCM2838_FSEL_ALT3 }, { 5, 25, BCM2838_FSEL_ALT5 },
    { 6, 12, BCM2838_FSEL_ALT3 }, { 7, 26, BCM2838_FSEL_ALT5 },
    { 8, 18, BCM2838_FSEL_ALT3 }, { 9, 27, BCM2838_FSEL_ALT5 },
};

/* the chip enable a pin outputs in its alternate function, or -1 */
static int gpio_spi_ce(BCM2838GpioState *s, int pin)
{
    for (int i = 0; i < ARRAY_SIZE(gpio_spi_ce_pins); i++) {
        if (gpio_spi_ce_pins[i].pin == pin &&
            gpio_spi_ce_pins[i].fsel == s->fsel[pin]) {
            return gpio_spi_ce_pins[i].cs;
        }
    }
    return -1;
}

/*
 * Pin level: the output latch for an output, the peripheral's output for
 * an SPI chip enable, otherwise what drives the pin from outside (a
 * peripheral's line, the board), otherwise its pull; an undriven pin
 * without pull reads low.
 */
static uint64_t gpio_levels(BCM2838GpioState *s)
{
    uint64_t latch = (uint64_t)s->lev1 << 32 | s->lev0;
    uint64_t level = 0;

    for (int i = 0; i < BCM2838_GPIO_NUM; i++) {
        int ce = gpio_spi_ce(s, i);
        bool hi;

        if (s->fsel[i] == BCM2838_FSEL_GPIO_OUT) {
            hi = latch & BIT_ULL(i);
        } else if (ce >= 0) {
            hi = s->spi_ce & BIT(ce);
        } else if (s->ext[i] != GPIO_EXT_NONE) {
            hi = s->ext[i] == GPIO_EXT_HIGH;
        } else {
            hi = gpio_pull(s, i) == GPIO_PULL_UP;
        }
        level |= (uint64_t)hi << i;
    }
    return level;
}

static void gpio_update_irq(BCM2838GpioState *s)
{
    uint64_t eds = gpio_word64(s->eds);

    qemu_set_irq(s->irq[0], !!(eds & GPIO_BANK0_MASK));
    qemu_set_irq(s->irq[1], !!(eds & GPIO_BANK1_MASK));
    qemu_set_irq(s->irq[2], !!(eds & GPIO_BANK2_MASK));
    qemu_set_irq(s->irq[3], !!eds);
}

/*
 * UART CTS inputs from the pin mux (BCM2711 alternate functions).
 * A UART whose CTS function is on no pin sees CTS asserted; otherwise CTS
 * is asserted while the pin is low (nCTS).
 */
static const struct {
    uint8_t uart, pin, fsel;
} gpio_cts_pins[] = {
    { 0, 16, BCM2838_FSEL_ALT3 }, { 0, 30, BCM2838_FSEL_ALT3 },
    { 0, 39, BCM2838_FSEL_ALT2 },
    { 1, 16, BCM2838_FSEL_ALT5 }, { 1, 30, BCM2838_FSEL_ALT5 },
    { 1, 43, BCM2838_FSEL_ALT5 },
    { 2, 2, BCM2838_FSEL_ALT4 }, { 3, 6, BCM2838_FSEL_ALT4 },
    { 4, 10, BCM2838_FSEL_ALT4 }, { 5, 14, BCM2838_FSEL_ALT4 },
};

static void gpio_update_uart_cts(BCM2838GpioState *s)
{
    bool cts[BCM2838_GPIO_NUM_UARTS];
    bool routed[BCM2838_GPIO_NUM_UARTS] = { false };

    for (int u = 0; u < BCM2838_GPIO_NUM_UARTS; u++) {
        cts[u] = true;
    }
    for (int i = 0; i < ARRAY_SIZE(gpio_cts_pins); i++) {
        int u = gpio_cts_pins[i].uart, pin = gpio_cts_pins[i].pin;

        if (s->fsel[pin] == gpio_cts_pins[i].fsel && !routed[u]) {
            routed[u] = true;
            cts[u] = !(s->level & BIT_ULL(pin));
        }
    }
    for (int u = 0; u < BCM2838_GPIO_NUM_UARTS; u++) {
        qemu_set_irq(s->uart_cts[u], cts[u]);
    }
}

/*
 * A device's chip select follows its CE pin: the pin in the CE function if
 * any, otherwise the first, as a GPIO (Linux "cs-gpios") or undriven.
 */
static void gpio_update_spi_cs(BCM2838GpioState *s)
{
    for (int cs = 0; cs < BCM2838_GPIO_NUM_SPI_CS; cs++) {
        int pin = -1;

        for (int i = 0; i < ARRAY_SIZE(gpio_spi_ce_pins); i++) {
            int p = gpio_spi_ce_pins[i].pin;

            if (gpio_spi_ce_pins[i].cs != cs) {
                continue;
            }
            if (pin < 0) {
                pin = p;
            }
            if (s->fsel[p] == gpio_spi_ce_pins[i].fsel) {
                pin = p;
                break;
            }
        }
        qemu_set_irq(s->spi_cs[cs], !!(s->level & BIT_ULL(pin)));
    }
}

static void gpio_chr_report(BCM2838GpioState *s, bool all);

/* Recompute the levels, latch edge and level events, update the IRQs */
static void gpio_update(BCM2838GpioState *s)
{
    uint64_t old = s->level;
    uint64_t new = gpio_levels(s);
    uint64_t rise = (old ^ new) & new;
    uint64_t fall = (old ^ new) & old;

    s->level = new;
    for (int w = 0; w < 2; w++) {
        uint32_t r = rise >> (32 * w), f = fall >> (32 * w);
        uint32_t l = new >> (32 * w);

        s->eds[w] |= (r & (s->ren[w] | s->aren[w])) |
                     (f & (s->fen[w] | s->afen[w])) |
                     (l & s->hen[w]) | (~l & s->len[w]);
    }
    s->eds[1] &= GPIO_ALL_MASK >> 32;
    gpio_update_irq(s);
    gpio_update_uart_cts(s);
    gpio_update_spi_cs(s);
    gpio_chr_report(s, false);
}

/* An SPI controller's chip enable output, for its pin's CE function */
static void gpio_spi_ce_set(void *opaque, int cs, int level)
{
    BCM2838GpioState *s = opaque;
    uint32_t ce = deposit32(s->spi_ce, cs, 1, level != 0);

    /* the controller drives its chip enables at every register access */
    if (ce != s->spi_ce) {
        s->spi_ce = ce;
        gpio_update(s);
    }
}

/* pinctrl level notation */
static int gpio_parse_level(const char *v)
{
    if (!strcmp(v, "hi")) {
        return GPIO_EXT_HIGH;
    }
    if (!strcmp(v, "lo")) {
        return GPIO_EXT_LOW;
    }
    return -1;
}

/*
 * "pins": the board state at reset, as "pinctrl get" prints it:
 * comma-separated PIN:FUNC:PULL:LEVEL, FUNC ip|op|a0..a5, PULL pu|pd|pn,
 * LEVEL hi|lo. For an output LEVEL is the latch; for anything else a
 * level its pull does not give means the board drives the pin. FUNC, PULL
 * and the latch are SoC state, restored by every reset; what the board
 * drives is set once, at power-on.
 */
static bool gpio_parse_pins(BCM2838GpioState *s, Error **errp)
{
    static const char *const funcs[] = {
        [BCM2838_FSEL_GPIO_IN] = "ip", [BCM2838_FSEL_GPIO_OUT] = "op",
        [BCM2838_FSEL_ALT0] = "a0", [BCM2838_FSEL_ALT1] = "a1",
        [BCM2838_FSEL_ALT2] = "a2", [BCM2838_FSEL_ALT3] = "a3",
        [BCM2838_FSEL_ALT4] = "a4", [BCM2838_FSEL_ALT5] = "a5",
    };
    g_auto(GStrv) items = g_strsplit_set(s->pins, ", ", -1);

    for (char **it = items; *it; it++) {
        g_auto(GStrv) f = NULL;
        uint64_t pin;
        int fsel = -1, pull, lvl;

        if (!**it) {
            continue;
        }
        f = g_strsplit(*it, ":", 4);
        if (g_strv_length(f) != 4 || qemu_strtou64(f[0], NULL, 10, &pin) ||
            pin >= BCM2838_GPIO_NUM) {
            goto bad;
        }
        for (int i = 0; i < ARRAY_SIZE(funcs); i++) {
            if (!strcmp(f[1], funcs[i])) {
                fsel = i;
            }
        }
        pull = !strcmp(f[2], "pu") ? GPIO_PULL_UP :
               !strcmp(f[2], "pd") ? GPIO_PULL_DOWN :
               !strcmp(f[2], "pn") ? GPIO_PULL_NONE : -1;
        lvl = gpio_parse_level(f[3]);
        if (fsel < 0 || pull < 0 || lvl < 0) {
            goto bad;
        }
        s->reset_fsel[pin] = fsel;
        s->reset_pup[pin / 16] = deposit32(s->reset_pup[pin / 16],
                                           (pin % 16) * 2, 2, pull);
        if (fsel == BCM2838_FSEL_GPIO_OUT) {
            s->reset_out = deposit64(s->reset_out, pin, 1, lvl);
            s->ext[pin] = GPIO_EXT_NONE;
        } else if ((pull == GPIO_PULL_UP && lvl == GPIO_EXT_HIGH) ||
                   (pull == GPIO_PULL_DOWN && lvl == GPIO_EXT_LOW)) {
            s->ext[pin] = GPIO_EXT_NONE;
        } else {
            s->ext[pin] = lvl;
        }
        continue;
bad:
        error_setg(errp, "bcm2838-gpio: bad pins entry '%s' "
                   "(expected PIN:ip|op|a0..a5:pu|pd|pn:hi|lo)", *it);
        return false;
    }
    return true;
}

/*
 * "gpioN" properties: what drives pin N from outside the SoC. "high" and
 * "low" drive it (it reads that level unless it is an output), "none"
 * leaves it to its pull.
 */
static const char *const gpio_drive_names[] = {
    [GPIO_EXT_LOW] = "low",
    [GPIO_EXT_HIGH] = "high",
    [GPIO_EXT_NONE] = "none",
};

static void gpio_get_pin(Object *obj, Visitor *v, const char *name,
                         void *opaque, Error **errp)
{
    BCM2838GpioState *s = BCM2838_GPIO(obj);
    char *value = g_strdup(gpio_drive_names[s->ext[(uintptr_t)opaque]]);

    visit_type_str(v, name, &value, errp);
    g_free(value);
}

static void gpio_set_pin(Object *obj, Visitor *v, const char *name,
                         void *opaque, Error **errp)
{
    BCM2838GpioState *s = BCM2838_GPIO(obj);
    g_autofree char *value = NULL;

    if (!visit_type_str(v, name, &value, errp)) {
        return;
    }
    for (int i = 0; i < ARRAY_SIZE(gpio_drive_names); i++) {
        if (!strcmp(value, gpio_drive_names[i])) {
            s->ext[(uintptr_t)opaque] = i;
            gpio_update(s);
            return;
        }
    }
    error_setg(errp, "%s: '%s' is not one of high, low, none", name, value);
}

/*
 * "chardev": the pins as a line protocol, PIN DRIVE per line, DRIVE one of
 * high, low, none. Received lines set the external drive, like the "gpioN"
 * properties. Sent lines tell how the SoC drives a pin: high or low for a
 * GPIO output, none when the pin is not one. All outputs are sent when the
 * other end connects, then every change.
 */
static uint64_t gpio_outputs(BCM2838GpioState *s)
{
    uint64_t out = 0;

    for (int i = 0; i < BCM2838_GPIO_NUM; i++) {
        if (s->fsel[i] == BCM2838_FSEL_GPIO_OUT) {
            out |= BIT_ULL(i);
        }
    }
    return out;
}

static void gpio_chr_report(BCM2838GpioState *s, bool all)
{
    uint64_t out = gpio_outputs(s);
    uint64_t latch = ((uint64_t)s->lev1 << 32 | s->lev0) & out;
    uint64_t changed = all ? out : (out ^ s->chr_out) | (latch ^ s->chr_latch);
    g_autoptr(GString) msg = NULL;

    s->chr_out = out;
    s->chr_latch = latch;
    if (!changed || !qemu_chr_fe_backend_connected(&s->chr)) {
        return;
    }
    msg = g_string_new(NULL);
    for (int i = 0; i < BCM2838_GPIO_NUM; i++) {
        if (changed & BIT_ULL(i)) {
            int drive = !(out & BIT_ULL(i)) ? GPIO_EXT_NONE :
                        latch & BIT_ULL(i) ? GPIO_EXT_HIGH : GPIO_EXT_LOW;

            g_string_append_printf(msg, "%d %s\n", i, gpio_drive_names[drive]);
        }
    }
    /* blocks while the other end does not read, rather than lose a change */
    qemu_chr_fe_write_all(&s->chr, (uint8_t *)msg->str, msg->len);
}

/* A received line: PIN DRIVE, or empty */
static bool gpio_chr_command(BCM2838GpioState *s, char *line)
{
    g_auto(GStrv) f = g_strsplit_set(g_strstrip(line), " \t", -1);
    uint64_t pin;

    if (!f[0] || !*f[0]) {
        return true;
    }
    if (g_strv_length(f) != 2 || qemu_strtou64(f[0], NULL, 10, &pin) ||
        pin >= BCM2838_GPIO_NUM) {
        return false;
    }
    for (int i = 0; i < ARRAY_SIZE(gpio_drive_names); i++) {
        if (!strcmp(f[1], gpio_drive_names[i])) {
            s->ext[pin] = i;
            gpio_update(s);
            return true;
        }
    }
    return false;
}

static int gpio_chr_can_receive(void *opaque)
{
    return sizeof(((BCM2838GpioState *)opaque)->chr_line);
}

static void gpio_chr_receive(void *opaque, const uint8_t *buf, int size)
{
    static const char err[] = "error: expected PIN high|low|none\n";
    BCM2838GpioState *s = opaque;

    for (int i = 0; i < size; i++) {
        if (buf[i] != '\n') {
            if (s->chr_len < sizeof(s->chr_line) - 1) {
                s->chr_line[s->chr_len++] = buf[i];
            } else {
                s->chr_len = sizeof(s->chr_line);
            }
            continue;
        }
        if (s->chr_len < sizeof(s->chr_line)) {
            s->chr_line[s->chr_len] = '\0';
        }
        if (s->chr_len == sizeof(s->chr_line) ||
            !gpio_chr_command(s, s->chr_line)) {
            qemu_chr_fe_write_all(&s->chr, (const uint8_t *)err,
                                  sizeof(err) - 1);
        }
        s->chr_len = 0;
    }
}

static void gpio_chr_event(void *opaque, QEMUChrEvent event)
{
    BCM2838GpioState *s = opaque;

    if (event == CHR_EVENT_OPENED) {
        s->chr_len = 0;
        gpio_chr_report(s, true);
    }
}

/* qdev GPIO inputs: other devices driving a pin (level 0/1) */
static void gpio_input_set(void *opaque, int pin, int level)
{
    BCM2838GpioState *s = opaque;

    s->ext[pin] = level ? GPIO_EXT_HIGH : GPIO_EXT_LOW;
    gpio_update(s);
}

static int gpfsel_is_out(BCM2838GpioState *s, int index)
{
    if (index >= 0 && index < BCM2838_GPIO_NUM) {
        return s->fsel[index] == 1;
    }
    return 0;
}

static void gpset(BCM2838GpioState *s, uint32_t val, uint8_t start,
                  uint8_t count, uint32_t *lev)
{
    uint32_t changes = val & ~*lev;
    uint32_t cur = 1;

    int i;
    for (i = 0; i < count; i++) {
        if ((changes & cur) && (gpfsel_is_out(s, start + i))) {
            qemu_set_irq(s->out[start + i], 1);
        }
        cur <<= 1;
    }

    *lev |= val;
}

static void gpclr(BCM2838GpioState *s, uint32_t val, uint8_t start,
                  uint8_t count, uint32_t *lev)
{
    uint32_t changes = val & *lev;
    uint32_t cur = 1;

    int i;
    for (i = 0; i < count; i++) {
        if ((changes & cur) && (gpfsel_is_out(s, start + i))) {
            qemu_set_irq(s->out[start + i], 0);
        }
        cur <<= 1;
    }

    *lev &= ~val;
}

static uint64_t bcm2838_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    BCM2838GpioState *s = (BCM2838GpioState *)opaque;
    uint64_t value = 0;

    switch (offset) {
    case GPFSEL0:
    case GPFSEL1:
    case GPFSEL2:
    case GPFSEL3:
    case GPFSEL4:
    case GPFSEL5:
        value = gpfsel_get(s, offset / BYTES_IN_WORD);
        break;
    case GPSET0:
    case GPSET1:
    case GPCLR0:
    case GPCLR1:
        /* Write Only */
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Attempt reading from write only"
                      " register. 0x%"PRIx64" will be returned."
                      " Address 0x%"HWADDR_PRIx", size %u\n",
                      TYPE_BCM2838_GPIO, __func__, value, offset, size);
        break;
    case GPLEV0:
        value = (uint32_t)s->level;
        break;
    case GPLEV1:
        value = s->level >> 32;
        break;
    case GPEDS0:
    case GPEDS1:
        value = s->eds[(offset - GPEDS0) / 4];
        break;
    case GPREN0:
    case GPREN1:
        value = s->ren[(offset - GPREN0) / 4];
        break;
    case GPFEN0:
    case GPFEN1:
        value = s->fen[(offset - GPFEN0) / 4];
        break;
    case GPHEN0:
    case GPHEN1:
        value = s->hen[(offset - GPHEN0) / 4];
        break;
    case GPLEN0:
    case GPLEN1:
        value = s->len[(offset - GPLEN0) / 4];
        break;
    case GPAREN0:
    case GPAREN1:
        value = s->aren[(offset - GPAREN0) / 4];
        break;
    case GPAFEN0:
    case GPAFEN1:
        value = s->afen[(offset - GPAFEN0) / 4];
        break;
    case GPIO_PUP_PDN_CNTRL_REG0:
    case GPIO_PUP_PDN_CNTRL_REG1:
    case GPIO_PUP_PDN_CNTRL_REG2:
    case GPIO_PUP_PDN_CNTRL_REG3:
        value = s->pup_cntrl_reg[(offset - GPIO_PUP_PDN_CNTRL_REG0)
                                 / sizeof(s->pup_cntrl_reg[0])];
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: bad offset %"HWADDR_PRIx"\n",
                      TYPE_BCM2838_GPIO, __func__, offset);
        break;
    }

    return value;
}

static void bcm2838_gpio_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    BCM2838GpioState *s = (BCM2838GpioState *)opaque;

    switch (offset) {
    case GPFSEL0:
    case GPFSEL1:
    case GPFSEL2:
    case GPFSEL3:
    case GPFSEL4:
    case GPFSEL5:
        gpfsel_set(s, offset / BYTES_IN_WORD, value);
        gpio_update(s);
        break;
    case GPSET0:
        gpset(s, value, 0, 32, &s->lev0);
        gpio_update(s);
        break;
    case GPSET1:
        gpset(s, value, 32, 26, &s->lev1);
        gpio_update(s);
        break;
    case GPCLR0:
        gpclr(s, value, 0, 32, &s->lev0);
        gpio_update(s);
        break;
    case GPCLR1:
        gpclr(s, value, 32, 26, &s->lev1);
        gpio_update(s);
        break;
    case GPLEV0:
    case GPLEV1:
        /* Read Only */
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: Attempt writing 0x%"PRIx64""
                      " to read only register. Ignored."
                      " Address 0x%"HWADDR_PRIx", size %u\n",
                      TYPE_BCM2838_GPIO, __func__, value, offset, size);
        break;
    case GPEDS0:
    case GPEDS1:
        /* Write 1 to clear; a level event still present sets it again */
        s->eds[(offset - GPEDS0) / 4] &= ~value;
        gpio_update(s);
        break;
    case GPREN0:
    case GPREN1:
        s->ren[(offset - GPREN0) / 4] = value;
        break;
    case GPFEN0:
    case GPFEN1:
        s->fen[(offset - GPFEN0) / 4] = value;
        break;
    case GPHEN0:
    case GPHEN1:
        s->hen[(offset - GPHEN0) / 4] = value;
        gpio_update(s);
        break;
    case GPLEN0:
    case GPLEN1:
        s->len[(offset - GPLEN0) / 4] = value;
        gpio_update(s);
        break;
    case GPAREN0:
    case GPAREN1:
        s->aren[(offset - GPAREN0) / 4] = value;
        break;
    case GPAFEN0:
    case GPAFEN1:
        s->afen[(offset - GPAFEN0) / 4] = value;
        break;
    case GPIO_PUP_PDN_CNTRL_REG0:
    case GPIO_PUP_PDN_CNTRL_REG1:
    case GPIO_PUP_PDN_CNTRL_REG2:
    case GPIO_PUP_PDN_CNTRL_REG3:
        s->pup_cntrl_reg[(offset - GPIO_PUP_PDN_CNTRL_REG0)
                         / sizeof(s->pup_cntrl_reg[0])] = value;
        gpio_update(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s: bad offset %"HWADDR_PRIx"\n",
                  TYPE_BCM2838_GPIO, __func__, offset);
    }
}

static void bcm2838_gpio_reset(DeviceState *dev)
{
    BCM2838GpioState *s = BCM2838_GPIO(dev);

    memset(s->fsel, 0, sizeof(s->fsel));

    /* the pins select the controller of the card again */
    sdbus_reparent_card(gpio_sd_bus(s, s->sd_fsel), &s->sdbus);
    s->sd_fsel = BCM2838_SD_UNROUTED;

    s->lev0 = (uint32_t)s->reset_out;
    s->lev1 = s->reset_out >> 32;

    memcpy(s->fsel, s->reset_fsel, sizeof(s->fsel));
    memcpy(s->pup_cntrl_reg, s->reset_pup, sizeof(s->pup_cntrl_reg));
    /* s->ext is outside the SoC: a SoC reset leaves the pins driven */
    gpio_sd_route(s);

    memset(s->eds, 0, sizeof(s->eds));
    memset(s->ren, 0, sizeof(s->ren));
    memset(s->fen, 0, sizeof(s->fen));
    memset(s->hen, 0, sizeof(s->hen));
    memset(s->len, 0, sizeof(s->len));
    memset(s->aren, 0, sizeof(s->aren));
    memset(s->afen, 0, sizeof(s->afen));
    s->level = gpio_levels(s);
    gpio_update_irq(s);
    gpio_update_uart_cts(s);
    gpio_update_spi_cs(s);
    gpio_chr_report(s, false);
}

static int bcm2838_gpio_post_load(void *opaque, int version_id)
{
    BCM2838GpioState *s = opaque;

    /* what the other end has seen is not migrated: report changes only */
    s->chr_out = gpio_outputs(s);
    s->chr_latch = ((uint64_t)s->lev1 << 32 | s->lev0) & s->chr_out;
    return 0;
}

static const MemoryRegionOps bcm2838_gpio_ops = {
    .read = bcm2838_gpio_read,
    .write = bcm2838_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static bool bcm2838_gpio_spi_ce_needed(void *opaque)
{
    BCM2838GpioState *s = opaque;

    return s->spi_ce != MAKE_64BIT_MASK(0, BCM2838_GPIO_NUM_SPI_CS);
}

static const VMStateDescription vmstate_bcm2838_gpio_spi_ce = {
    .name = "bcm2838_gpio/spi-ce",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = bcm2838_gpio_spi_ce_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(spi_ce, BCM2838GpioState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_bcm2838_gpio = {
    .name = "bcm2838_gpio",
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = bcm2838_gpio_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8_ARRAY(fsel, BCM2838GpioState, BCM2838_GPIO_NUM),
        VMSTATE_UINT32(lev0, BCM2838GpioState),
        VMSTATE_UINT32(lev1, BCM2838GpioState),
        VMSTATE_UINT8(sd_fsel, BCM2838GpioState),
        VMSTATE_UINT32_ARRAY(pup_cntrl_reg, BCM2838GpioState,
                             GPIO_PUP_PDN_CNTRL_NUM),
        VMSTATE_UINT32_ARRAY(eds, BCM2838GpioState, 2),
        VMSTATE_UINT32_ARRAY(ren, BCM2838GpioState, 2),
        VMSTATE_UINT32_ARRAY(fen, BCM2838GpioState, 2),
        VMSTATE_UINT32_ARRAY(hen, BCM2838GpioState, 2),
        VMSTATE_UINT32_ARRAY(len, BCM2838GpioState, 2),
        VMSTATE_UINT32_ARRAY(aren, BCM2838GpioState, 2),
        VMSTATE_UINT32_ARRAY(afen, BCM2838GpioState, 2),
        VMSTATE_UINT64(level, BCM2838GpioState),
        VMSTATE_UINT8_ARRAY(ext, BCM2838GpioState, BCM2838_GPIO_NUM),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_bcm2838_gpio_spi_ce,
        NULL
    }
};

static void bcm2838_gpio_init(Object *obj)
{
    BCM2838GpioState *s = BCM2838_GPIO(obj);
    DeviceState *dev = DEVICE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_SD_BUS, DEVICE(s), "sd-bus");

    memory_region_init_io(&s->iomem, obj, &bcm2838_gpio_ops, s,
                          "bcm2838_gpio", BCM2838_GPIO_REGS_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    qdev_init_gpio_out(dev, s->out, BCM2838_GPIO_NUM);
    qdev_init_gpio_in(dev, gpio_input_set, BCM2838_GPIO_NUM);
    for (int i = 0; i < BCM2838_GPIO_NUM_IRQS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
    qdev_init_gpio_out_named(dev, s->uart_cts, "uart-cts",
                             BCM2838_GPIO_NUM_UARTS);
    qdev_init_gpio_in_named(dev, gpio_spi_ce_set, "spi-ce",
                            BCM2838_GPIO_NUM_SPI_CS);
    qdev_init_gpio_out_named(dev, s->spi_cs, "spi-cs",
                             BCM2838_GPIO_NUM_SPI_CS);
    /* chip enables are active low: none selected */
    s->spi_ce = MAKE_64BIT_MASK(0, BCM2838_GPIO_NUM_SPI_CS);

    /* Defaults without a "pins" preset: all inputs, the SoC reset pulls */
    s->reset_pup[0] = RESET_VAL_CNTRL_REG0;
    s->reset_pup[1] = RESET_VAL_CNTRL_REG1;
    s->reset_pup[2] = RESET_VAL_CNTRL_REG2;
    s->reset_pup[3] = RESET_VAL_CNTRL_REG3;
    memset(s->ext, GPIO_EXT_NONE, sizeof(s->ext));

    for (uintptr_t i = 0; i < BCM2838_GPIO_NUM; i++) {
        g_autofree char *name = g_strdup_printf("gpio%" PRIuPTR, i);

        object_property_add(obj, name, "str", gpio_get_pin, gpio_set_pin,
                            NULL, (void *)i);
        object_property_set_description(obj, name,
            "external drive of the pin: high, low or none");
    }
    object_property_add_uint64_ptr(obj, "levels", &s->level,
                                   OBJ_PROP_FLAG_READ);
}

static void bcm2838_gpio_realize(DeviceState *dev, Error **errp)
{
    BCM2838GpioState *s = BCM2838_GPIO(dev);
    Object *obj;

    obj = object_property_get_link(OBJECT(dev), "sdbus-sdhci", &error_abort);
    s->sdbus_sdhci = SD_BUS(obj);

    obj = object_property_get_link(OBJECT(dev), "sdbus-sdhost", &error_abort);
    s->sdbus_sdhost = SD_BUS(obj);
    s->sd_fsel = BCM2838_SD_UNROUTED;

    if (s->pins && !gpio_parse_pins(s, errp)) {
        return;
    }
    qemu_chr_fe_set_handlers(&s->chr, gpio_chr_can_receive, gpio_chr_receive,
                             gpio_chr_event, NULL, s, NULL, true);
}

static const Property bcm2838_gpio_properties[] = {
    DEFINE_PROP_STRING("pins", BCM2838GpioState, pins),
    DEFINE_PROP_CHR("chardev", BCM2838GpioState, chr),
};

static void bcm2838_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_bcm2838_gpio;
    dc->realize = &bcm2838_gpio_realize;
    device_class_set_props(dc, bcm2838_gpio_properties);
    device_class_set_legacy_reset(dc, bcm2838_gpio_reset);
}

static const TypeInfo bcm2838_gpio_info = {
    .name          = TYPE_BCM2838_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2838GpioState),
    .instance_init = bcm2838_gpio_init,
    .class_init    = bcm2838_gpio_class_init,
};

static void bcm2838_gpio_register_types(void)
{
    type_register_static(&bcm2838_gpio_info);
}

type_init(bcm2838_gpio_register_types)
