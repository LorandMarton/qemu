/*
 * SPI peripheral backed by a character device
 *
 * The bus traffic of the device goes to the chardev as text lines, and the
 * program at the other end (a device model, or a bridge to a real device)
 * answers each byte with the byte the device shifts back:
 *
 *   select              chip select asserted
 *   xfer DD [DD...]     DD [DD...]  controller shifts the bytes out and reads
 *                                   as many
 *   deselect            chip select released
 *
 * DD is hexadecimal. A line carries the bytes the controller has queued (its
 * FIFO); the answer has a byte for each, the one the device shifts back while
 * it receives that byte, so it may depend on the bytes before it only. The
 * guest waits for the answer. The chip select is active low;
 * with nothing connected, or after the other end went away, the device
 * answers 0x00, as a bus with no device.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/ssi/ssi.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TYPE_SPI_CHARDEV "spi-chardev"
OBJECT_DECLARE_SIMPLE_TYPE(SPIChardevState, SPI_CHARDEV)

struct SPIChardevState {
    SSIPeripheral parent_obj;

    CharFrontend chr;
    bool open;
    GString *in;            /* received, not yet taken as an answer */
};

/*
 * Send @req; with @answer, wait for the answer line. Returns false when
 * the other end is not there.
 */
static bool spi_chardev_request(SPIChardevState *s, const char *req,
                                GString *answer)
{
    g_autofree char *line = g_strdup_printf("%s\n", req);

    trace_spi_chardev_request(s->parent_obj.cs_index, req);
    if (!s->open ||
        qemu_chr_fe_write_all(&s->chr, (uint8_t *)line, strlen(line)) < 0) {
        return false;
    }
    if (!answer) {
        return true;
    }
    for (;;) {
        char *nl = memchr(s->in->str, '\n', s->in->len);
        uint8_t buf[256];
        int n;

        if (nl) {
            g_string_append_len(answer, s->in->str, nl - s->in->str);
            g_string_erase(s->in, 0, nl - s->in->str + 1);
            break;
        }
        n = qemu_chr_fe_read(&s->chr, buf, sizeof(buf));
        if (n <= 0) {
            return false;
        }
        g_string_append_len(s->in, (char *)buf, n);
    }
    trace_spi_chardev_answer(s->parent_obj.cs_index, answer->str);
    return true;
}

static void spi_chardev_transfer_buf(SSIPeripheral *ssi, const uint32_t *tx,
                                     uint32_t *rx, size_t len)
{
    SPIChardevState *s = SPI_CHARDEV(ssi);
    g_autoptr(GString) req = g_string_new("xfer");
    g_autoptr(GString) answer = g_string_new(NULL);
    g_auto(GStrv) words = NULL;
    size_t i, n = 0;

    memset(rx, 0, len * sizeof(*rx));
    for (i = 0; i < len; i++) {
        g_string_append_printf(req, " %02x", tx[i] & 0xff);
    }
    if (!spi_chardev_request(s, req->str, answer)) {
        return;
    }
    words = g_strsplit_set(answer->str, " \t\r", -1);
    for (i = 0; words[i]; i++) {
        uint64_t data;

        if (!*words[i]) {
            continue;
        }
        if (n == len || qemu_strtou64(words[i], NULL, 16, &data) ||
            data > 0xff) {
            break;
        }
        rx[n++] = data;
    }
    if (words[i] || n != len) {
        warn_report_once("spi-chardev: answer '%s' to '%s' is not %zu "
                         "bytes in hexadecimal, taken as 00", answer->str,
                         req->str, len);
        memset(rx, 0, len * sizeof(*rx));
    }
}

static uint32_t spi_chardev_transfer(SSIPeripheral *ssi, uint32_t val)
{
    uint32_t rx;

    spi_chardev_transfer_buf(ssi, &val, &rx, 1);
    return rx;
}

/* @level: the chip select line, active low */
static int spi_chardev_set_cs(SSIPeripheral *ssi, bool level)
{
    spi_chardev_request(SPI_CHARDEV(ssi), level ? "deselect" : "select",
                        NULL);
    return 0;
}

static void spi_chardev_chr_event(void *opaque, QEMUChrEvent event)
{
    SPIChardevState *s = opaque;

    switch (event) {
    case CHR_EVENT_OPENED:
        s->open = true;
        break;
    case CHR_EVENT_CLOSED:
        s->open = false;
        g_string_truncate(s->in, 0);
        break;
    default:
        break;
    }
}

static void spi_chardev_realize(SSIPeripheral *ssi, Error **errp)
{
    SPIChardevState *s = SPI_CHARDEV(ssi);

    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        error_setg(errp, "spi-chardev: 'chardev' property is required");
        return;
    }
    /* answers are read synchronously: no read handler */
    qemu_chr_fe_set_handlers(&s->chr, NULL, NULL, spi_chardev_chr_event,
                             NULL, s, NULL, true);
}

static const VMStateDescription vmstate_spi_chardev = {
    .name = TYPE_SPI_CHARDEV,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, SPIChardevState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property spi_chardev_properties[] = {
    DEFINE_PROP_CHR("chardev", SPIChardevState, chr),
};

static void spi_chardev_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    dc->desc = "SPI peripheral backed by a character device";
    dc->vmsd = &vmstate_spi_chardev;
    device_class_set_props(dc, spi_chardev_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    k->realize = spi_chardev_realize;
    k->transfer = spi_chardev_transfer;
    k->transfer_buf = spi_chardev_transfer_buf;
    k->set_cs = spi_chardev_set_cs;
    k->cs_polarity = SSI_CS_LOW;
}

static void spi_chardev_init(Object *obj)
{
    SPI_CHARDEV(obj)->in = g_string_new(NULL);
}

static void spi_chardev_finalize(Object *obj)
{
    g_string_free(SPI_CHARDEV(obj)->in, true);
}

static const TypeInfo spi_chardev_types[] = {
    {
        .name          = TYPE_SPI_CHARDEV,
        .parent        = TYPE_SSI_PERIPHERAL,
        .instance_size = sizeof(SPIChardevState),
        .instance_init = spi_chardev_init,
        .instance_finalize = spi_chardev_finalize,
        .class_init    = spi_chardev_class_init,
    },
};

DEFINE_TYPES(spi_chardev_types)
