/*
 * I2C target backed by a character device
 *
 * The bus events at the device's address go to the chardev as text lines,
 * and the program at the other end (a device model, or a bridge to a real
 * device) answers those that need an answer:
 *
 *   start AA write      ack | nack     controller addresses the target
 *   start AA read       ack | nack
 *   send DD             ack | nack     controller writes a byte
 *   recv                DD             controller reads a byte
 *   nack                               controller NACKed the byte it read
 *   stop                               end of the transfer
 *
 * AA (7-bit address) and DD are hexadecimal; a repeated start is another
 * "start" without a "stop" before it. The guest waits for each answer, as
 * a controller waits for a target that stretches the clock. With nothing
 * connected, or after the other end went away, the target does not
 * acknowledge and reads 0xff.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TYPE_I2C_CHARDEV "i2c-chardev"
OBJECT_DECLARE_SIMPLE_TYPE(I2CChardevState, I2C_CHARDEV)

/* Longest answer line, without the newline */
#define I2C_CHARDEV_ANSWER_MAX  15

struct I2CChardevState {
    I2CSlave parent_obj;

    CharFrontend chr;
    bool open;
};

/*
 * Send @req; with @answer, wait for the answer line. Returns false when
 * the other end is not there.
 */
static bool i2c_chardev_request(I2CChardevState *s, const char *req,
                                char *answer)
{
    g_autofree char *line = g_strdup_printf("%s\n", req);
    int len = 0;

    trace_i2c_chardev_request(s->parent_obj.address, req);
    if (!s->open ||
        qemu_chr_fe_write_all(&s->chr, (uint8_t *)line, strlen(line)) < 0) {
        return false;
    }
    if (!answer) {
        return true;
    }
    for (;;) {
        uint8_t c;

        if (qemu_chr_fe_read_all(&s->chr, &c, 1) != 1) {
            return false;
        }
        if (c == '\n') {
            break;
        }
        if (len < I2C_CHARDEV_ANSWER_MAX) {
            answer[len++] = c;
        }
    }
    answer[len] = '\0';
    g_strstrip(answer);
    trace_i2c_chardev_answer(s->parent_obj.address, answer);
    return true;
}

/* 0: ack, 1: nack (also for no answer) */
static int i2c_chardev_ack(I2CChardevState *s, const char *req)
{
    char answer[I2C_CHARDEV_ANSWER_MAX + 1];

    if (!i2c_chardev_request(s, req, answer)) {
        return 1;
    }
    if (!strcmp(answer, "ack")) {
        return 0;
    }
    if (strcmp(answer, "nack")) {
        warn_report_once("i2c-chardev: answer '%s' to '%s' is not ack or "
                         "nack, taken as nack", answer, req);
    }
    return 1;
}

static int i2c_chardev_event(I2CSlave *i2c, enum i2c_event event)
{
    I2CChardevState *s = I2C_CHARDEV(i2c);
    g_autofree char *req = NULL;

    switch (event) {
    case I2C_START_SEND:
    case I2C_START_RECV:
        req = g_strdup_printf("start %02x %s", i2c->address,
                              event == I2C_START_SEND ? "write" : "read");
        return i2c_chardev_ack(s, req);
    case I2C_FINISH:
        i2c_chardev_request(s, "stop", NULL);
        return 0;
    case I2C_NACK:
        i2c_chardev_request(s, "nack", NULL);
        return 0;
    default:
        /* no asynchronous sends */
        return 1;
    }
}

static int i2c_chardev_send(I2CSlave *i2c, uint8_t data)
{
    g_autofree char *req = g_strdup_printf("send %02x", data);

    return i2c_chardev_ack(I2C_CHARDEV(i2c), req);
}

static uint8_t i2c_chardev_recv(I2CSlave *i2c)
{
    I2CChardevState *s = I2C_CHARDEV(i2c);
    char answer[I2C_CHARDEV_ANSWER_MAX + 1];
    uint64_t data;

    if (!i2c_chardev_request(s, "recv", answer)) {
        return 0xff;
    }
    if (qemu_strtou64(answer, NULL, 16, &data) || data > 0xff) {
        warn_report_once("i2c-chardev: answer '%s' to 'recv' is not a byte "
                         "in hexadecimal, taken as ff", answer);
        return 0xff;
    }
    return data;
}

static void i2c_chardev_chr_event(void *opaque, QEMUChrEvent event)
{
    I2CChardevState *s = opaque;

    switch (event) {
    case CHR_EVENT_OPENED:
        s->open = true;
        break;
    case CHR_EVENT_CLOSED:
        s->open = false;
        break;
    default:
        break;
    }
}

static void i2c_chardev_realize(DeviceState *dev, Error **errp)
{
    I2CChardevState *s = I2C_CHARDEV(dev);

    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        error_setg(errp, "i2c-chardev: 'chardev' property is required");
        return;
    }
    /* answers are read synchronously: no read handler */
    qemu_chr_fe_set_handlers(&s->chr, NULL, NULL, i2c_chardev_chr_event,
                             NULL, s, NULL, true);
}

static const VMStateDescription vmstate_i2c_chardev = {
    .name = TYPE_I2C_CHARDEV,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, I2CChardevState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property i2c_chardev_properties[] = {
    DEFINE_PROP_CHR("chardev", I2CChardevState, chr),
};

static void i2c_chardev_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->desc = "I2C target backed by a character device";
    dc->realize = i2c_chardev_realize;
    dc->vmsd = &vmstate_i2c_chardev;
    device_class_set_props(dc, i2c_chardev_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    k->event = i2c_chardev_event;
    k->send = i2c_chardev_send;
    k->recv = i2c_chardev_recv;
}

static const TypeInfo i2c_chardev_types[] = {
    {
        .name          = TYPE_I2C_CHARDEV,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(I2CChardevState),
        .class_init    = i2c_chardev_class_init,
    },
};

DEFINE_TYPES(i2c_chardev_types)
