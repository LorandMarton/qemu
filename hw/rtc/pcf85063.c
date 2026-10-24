/*
 * NXP PCF85063A I2C real-time clock
 *
 * Time and date in BCD, 24-hour mode, register pointer auto-increment
 * (0x11 wraps to 0x00), STOP bit, the oscillator-stop (OS) flag, RAM byte,
 * offset, alarm and timer registers as plain storage (no alarm/timer
 * interrupt, no CLKOUT).
 *
 * The OS flag is the chip's "time invalid" indication: it is set at power-on
 * when the backup supply was lost, and cleared by the guest writing the
 * seconds register. "power-lost=on" models a board without backup supply,
 * "power-lost=off" (default) one whose RTC kept running on the host clock.
 * Like on hardware, a system reset does not reset the time keeping.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bcd.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "system/rtc.h"

#define PCF85063_REG_CTRL1      0x00
#define PCF85063_REG_CTRL2      0x01
#define PCF85063_REG_SC         0x04
#define PCF85063_REG_MN         0x05
#define PCF85063_REG_HR         0x06
#define PCF85063_REG_DM         0x07
#define PCF85063_REG_DW         0x08
#define PCF85063_REG_MO         0x09
#define PCF85063_REG_YR         0x0a
#define PCF85063_NUM_REGS       0x12

#define CTRL1_12_24             BIT(1)
#define CTRL1_STOP              BIT(5)
#define CTRL1_SR_MAGIC          0x58
#define CTRL2_AF                BIT(6)
#define SC_OS                   BIT(7)

#define TYPE_PCF85063 "pcf85063"
OBJECT_DECLARE_SIMPLE_TYPE(PCF85063State, PCF85063)

struct PCF85063State {
    I2CSlave parent_obj;

    bool power_lost;

    /* RTC time = host RTC time + offset, or stop_time while stopped */
    int64_t offset;
    int64_t stop_time;
    uint8_t wday_offset;
    bool os;
    uint8_t regs[PCF85063_NUM_REGS];
    uint8_t ptr;
    bool addr_byte;
    bool time_written;      /* time registers written in this access */
};

static void pcf85063_get_time(PCF85063State *s, struct tm *tm)
{
    if (s->regs[PCF85063_REG_CTRL1] & CTRL1_STOP) {
        time_t t = s->stop_time;

        gmtime_r(&t, tm);
    } else {
        qemu_get_timedate(tm, s->offset);
    }
}

static void pcf85063_set_time(PCF85063State *s, struct tm *tm)
{
    if (s->regs[PCF85063_REG_CTRL1] & CTRL1_STOP) {
        s->stop_time = mktimegm(tm);
    } else {
        s->offset = qemu_timedate_diff(tm);
    }
}

static void pcf85063_capture_time(PCF85063State *s)
{
    struct tm now;

    pcf85063_get_time(s, &now);
    s->regs[PCF85063_REG_SC] = to_bcd(now.tm_sec) | (s->os ? SC_OS : 0);
    s->regs[PCF85063_REG_MN] = to_bcd(now.tm_min);
    s->regs[PCF85063_REG_HR] = to_bcd(now.tm_hour);
    s->regs[PCF85063_REG_DM] = to_bcd(now.tm_mday);
    s->regs[PCF85063_REG_DW] = (now.tm_wday + s->wday_offset) % 7;
    s->regs[PCF85063_REG_MO] = to_bcd(now.tm_mon + 1);
    s->regs[PCF85063_REG_YR] = to_bcd(now.tm_year - 100);
}

/*
 * A write access to the time registers takes effect as a whole when it ends,
 * as the time counters are frozen during the access. The weekday register is
 * an independent counter: it is kept as an offset to the date's weekday.
 */
static void pcf85063_commit_time(PCF85063State *s)
{
    struct tm tm = {
        .tm_sec = from_bcd(s->regs[PCF85063_REG_SC] & 0x7f),
        .tm_min = from_bcd(s->regs[PCF85063_REG_MN] & 0x7f),
        .tm_hour = from_bcd(s->regs[PCF85063_REG_HR] & 0x3f),
        .tm_mday = from_bcd(s->regs[PCF85063_REG_DM] & 0x3f),
        .tm_mon = from_bcd(s->regs[PCF85063_REG_MO] & 0x1f) - 1,
        .tm_year = from_bcd(s->regs[PCF85063_REG_YR]) + 100,
    };
    time_t t = mktimegm(&tm);
    struct tm norm;

    gmtime_r(&t, &norm);
    s->wday_offset = ((s->regs[PCF85063_REG_DW] & 7) - norm.tm_wday + 7) % 7;
    pcf85063_set_time(s, &norm);
}

static void pcf85063_inc_ptr(PCF85063State *s)
{
    s->ptr = (s->ptr + 1) % PCF85063_NUM_REGS;
}

static int pcf85063_event(I2CSlave *i2c, enum i2c_event event)
{
    PCF85063State *s = PCF85063(i2c);

    switch (event) {
    case I2C_START_RECV:
        /* The time registers are latched when a read access starts */
        pcf85063_capture_time(s);
        break;
    case I2C_START_SEND:
        /* registers the access does not write keep the current time */
        pcf85063_capture_time(s);
        s->addr_byte = true;
        s->time_written = false;
        break;
    case I2C_FINISH:
        if (s->time_written) {
            pcf85063_commit_time(s);
            s->time_written = false;
        }
        break;
    default:
        break;
    }

    return 0;
}

static uint8_t pcf85063_recv(I2CSlave *i2c)
{
    PCF85063State *s = PCF85063(i2c);
    uint8_t res = s->regs[s->ptr];

    pcf85063_inc_ptr(s);
    return res;
}

/*
 * Register reset values (datasheet section 7, Table 6), also applied by a
 * software reset. "lost" restarts the time at 2000-01-01 00:00:00 with OS set.
 */
static void pcf85063_reset_regs(PCF85063State *s, bool lost)
{
    struct tm epoch = { .tm_year = 100, .tm_mon = 0, .tm_mday = 1 };

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0x0b] = s->regs[0x0c] = s->regs[0x0d] = 0x80;
    s->regs[0x0e] = s->regs[0x0f] = 0x80;
    s->regs[0x11] = 0x18;
    s->stop_time = 0;
    if (lost) {
        /* weekday register resets to 6, and 2000-01-01 was a Saturday */
        s->os = true;
        s->offset = qemu_timedate_diff(&epoch);
        s->wday_offset = 0;
    }
}

static int pcf85063_send(I2CSlave *i2c, uint8_t data)
{
    PCF85063State *s = PCF85063(i2c);
    struct tm now;

    if (s->addr_byte) {
        s->ptr = data % PCF85063_NUM_REGS;
        s->addr_byte = false;
        return 0;
    }

    switch (s->ptr) {
    case PCF85063_REG_CTRL1:
        if (data == CTRL1_SR_MAGIC) {
            pcf85063_reset_regs(s, true);
            break;
        }
        if (data & CTRL1_12_24) {
            qemu_log_mask(LOG_UNIMP, "pcf85063: 12-hour mode\n");
        }
        if ((data ^ s->regs[s->ptr]) & CTRL1_STOP) {
            /* freeze or restart the clock at the current time */
            pcf85063_get_time(s, &now);
            s->regs[s->ptr] = data;
            if (data & CTRL1_STOP) {
                s->stop_time = mktimegm(&now);
            } else {
                s->offset = qemu_timedate_diff(&now);
            }
        }
        s->regs[s->ptr] = data;
        break;
    case PCF85063_REG_CTRL2:
        /* AF can only be cleared */
        data = (data & ~CTRL2_AF) | (data & s->regs[s->ptr] & CTRL2_AF);
        s->regs[s->ptr] = data;
        break;
    case PCF85063_REG_SC ... PCF85063_REG_YR:
        if (s->ptr == PCF85063_REG_SC) {
            s->os = data & SC_OS;
        }
        s->regs[s->ptr] = data;
        s->time_written = true;
        break;
    default:
        s->regs[s->ptr] = data;
        break;
    }
    pcf85063_inc_ptr(s);
    return 0;
}

static void pcf85063_realize(DeviceState *dev, Error **errp)
{
    PCF85063State *s = PCF85063(dev);

    /* power-on: with a backup supply the clock kept the host time */
    s->offset = 0;
    s->wday_offset = 0;
    s->os = false;
    pcf85063_reset_regs(s, s->power_lost);
}

static void pcf85063_reset(DeviceState *dev)
{
    PCF85063State *s = PCF85063(dev);

    /* The RTC is not reset with the SoC: only the I2C interface is */
    s->ptr = 0;
    s->addr_byte = false;
    s->time_written = false;
}

static const VMStateDescription vmstate_pcf85063 = {
    .name = TYPE_PCF85063,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, PCF85063State),
        VMSTATE_INT64(offset, PCF85063State),
        VMSTATE_INT64(stop_time, PCF85063State),
        VMSTATE_UINT8(wday_offset, PCF85063State),
        VMSTATE_BOOL(os, PCF85063State),
        VMSTATE_UINT8_ARRAY(regs, PCF85063State, PCF85063_NUM_REGS),
        VMSTATE_UINT8(ptr, PCF85063State),
        VMSTATE_BOOL(addr_byte, PCF85063State),
        VMSTATE_BOOL(time_written, PCF85063State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property pcf85063_props[] = {
    DEFINE_PROP_BOOL("power-lost", PCF85063State, power_lost, false),
};

static void pcf85063_init(Object *obj)
{
    /* The PCF85063A has the fixed address 0x51 */
    qdev_prop_set_uint8(DEVICE(obj), "address", 0x51);
}

static void pcf85063_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = pcf85063_event;
    k->recv = pcf85063_recv;
    k->send = pcf85063_send;
    dc->realize = pcf85063_realize;
    device_class_set_legacy_reset(dc, pcf85063_reset);
    device_class_set_props(dc, pcf85063_props);
    dc->vmsd = &vmstate_pcf85063;
    dc->desc = "NXP PCF85063A real-time clock";
}

static const TypeInfo pcf85063_types[] = {
    {
        .name          = TYPE_PCF85063,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(PCF85063State),
        .instance_init = pcf85063_init,
        .class_init    = pcf85063_class_init,
    },
};

DEFINE_TYPES(pcf85063_types)
