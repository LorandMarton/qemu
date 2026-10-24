/*
 * BCM2835 SPI Master Controller
 *
 * Copyright (c) 2024 Rayhan Faizel <rayhan.faizel@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/fifo8.h"
#include "qemu/timer.h"
#include "hw/ssi/bcm2835_spi.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

static void bcm2835_spi_update_int(BCM2835SPIState *s)
{
    int do_interrupt = 0;

    /* Interrupt on DONE */
    if (s->cs & BCM2835_SPI_CS_INTD && s->cs & BCM2835_SPI_CS_DONE) {
        do_interrupt = 1;
    }
    /* Interrupt on RXR */
    if (s->cs & BCM2835_SPI_CS_INTR && s->cs & BCM2835_SPI_CS_RXR) {
        do_interrupt = 1;
    }
    qemu_set_irq(s->irq, do_interrupt);
}

static void bcm2835_spi_update_rx_flags(BCM2835SPIState *s)
{
    /* Set RXD if RX FIFO is non empty */
    if (!fifo8_is_empty(&s->rx_fifo)) {
        s->cs |= BCM2835_SPI_CS_RXD;
    } else {
        s->cs &= ~BCM2835_SPI_CS_RXD;
    }

    /* Set RXF if RX FIFO is full */
    if (fifo8_is_full(&s->rx_fifo)) {
        s->cs |= BCM2835_SPI_CS_RXF;
    } else {
        s->cs &= ~BCM2835_SPI_CS_RXF;
    }

    /* Set RXR if RX FIFO is 3/4th used or above */
    if (fifo8_num_used(&s->rx_fifo) >= FIFO_SIZE_3_4) {
        s->cs |= BCM2835_SPI_CS_RXR;
    } else {
        s->cs &= ~BCM2835_SPI_CS_RXR;
    }
}

static void bcm2835_spi_update_tx_flags(BCM2835SPIState *s)
{
    bool done;

    /* Set TXD if TX FIFO is not full */
    if (fifo8_is_full(&s->tx_fifo)) {
        s->cs &= ~BCM2835_SPI_CS_TXD;
    } else {
        s->cs |= BCM2835_SPI_CS_TXD;
    }

    /*
     * Set DONE if in TA mode and TX FIFO is empty; in DMA mode also once
     * DLEN bytes are out, with words written past the end left in the FIFO.
     */
    done = fifo8_is_empty(&s->tx_fifo) ||
           (s->cs & BCM2835_SPI_CS_DMAEN && s->dlen == 0);
    if (done && s->cs & BCM2835_SPI_CS_TA) {
        s->cs |= BCM2835_SPI_CS_DONE;
    } else {
        s->cs &= ~BCM2835_SPI_CS_DONE;
    }
}

/*
 * DREQ thresholds of the DC register, in bytes. TX DREQ stays asserted after
 * DLEN has reached 0 (an erratum spi-bcm2835 works around); RX DREQ is also
 * asserted for the rest of the RX FIFO once the transfer has finished.
 */
static void bcm2835_spi_update_dreq(BCM2835SPIState *s)
{
    bool dma = s->cs & BCM2835_SPI_CS_DMAEN;
    uint32_t tx_used = fifo8_num_used(&s->tx_fifo);
    uint32_t rx_used = fifo8_num_used(&s->rx_fifo);

    qemu_set_irq(s->dreq[BCM2835_SPI_DREQ_TX],
                 dma && tx_used <= extract32(s->dc, 0, 8));
    qemu_set_irq(s->dreq[BCM2835_SPI_DREQ_RX],
                 dma && (rx_used > extract32(s->dc, 16, 8) ||
                         (rx_used && s->dlen == 0)));
}

/*
 * The TX FIFO goes out in batches: on the hardware it drains at the SPI
 * clock while the guest fills it, so the guest sees it fill and the RX
 * bytes come later; a device whose transfers are costly (an exchange with
 * another process) gets a FIFO's worth at once. The controller shifts when
 * the guest could otherwise not go on: the TX FIFO full with the RX FIFO
 * read empty (it would otherwise shift a byte per RX read), in DMA mode the
 * TX DREQ about to drop or all DLEN bytes queued, a CS write or chip select
 * change, a CS read after one without FIFO accesses in between, a FIFO read
 * with no RX byte (or word, in DMA mode) left; and from a timer, after the
 * time the bus takes for the bytes that can go (a guest waiting for the
 * interrupt or for DREQ). The timer waits at least SHIFT_MIN_NS, as waking
 * the main loop for a few bytes costs more than the bytes: spi-bcm2835 polls
 * transfers shorter than that (polling_limit_us), so they shift at once.
 */
#define SHIFT_MIN_NS    (30 * SCALE_US)

/* the bus time of @n bytes: CDIV divides the core clock, 0 by 65536 */
static int64_t bcm2835_spi_bus_ns(BCM2835SPIState *s, uint32_t n)
{
    uint64_t cdiv = (s->clk & 0xfffe) ?: 0x10000;

    return muldiv64(8 * n * cdiv, NANOSECONDS_PER_SECOND, s->clock_hz);
}

static bool bcm2835_spi_can_shift(BCM2835SPIState *s)
{
    return s->cs & BCM2835_SPI_CS_TA && !fifo8_is_empty(&s->tx_fifo) &&
           !fifo8_is_full(&s->rx_fifo) &&
           !(s->cs & BCM2835_SPI_CS_DMAEN && s->dlen == 0);
}

static void bcm2835_spi_shift(BCM2835SPIState *s)
{
    uint32_t tx[FIFO_SIZE], rx[FIFO_SIZE];
    size_t n = 0;

    while (s->cs & BCM2835_SPI_CS_TA &&
           !fifo8_is_empty(&s->tx_fifo) &&
           n < fifo8_num_free(&s->rx_fifo)) {
        if (s->cs & BCM2835_SPI_CS_DMAEN) {
            /* in DMA mode, DLEN counts the bytes to transfer */
            if (s->dlen == 0) {
                break;
            }
            s->dlen--;
        }
        tx[n++] = fifo8_pop(&s->tx_fifo);
    }
    if (n) {
        ssi_transfer_buf(s->bus, tx, rx, n);
        for (size_t i = 0; i < n; i++) {
            fifo8_push(&s->rx_fifo, rx[i]);
        }
    }

    bcm2835_spi_update_tx_flags(s);
    bcm2835_spi_update_rx_flags(s);
}

static bool bcm2835_spi_must_shift(BCM2835SPIState *s)
{
    uint32_t used = fifo8_num_used(&s->tx_fifo);

    if (!bcm2835_spi_can_shift(s)) {
        return false;
    }
    if (s->cs & BCM2835_SPI_CS_DMAEN) {
        return used > extract32(s->dc, 0, 8) || used >= s->dlen;
    }
    /* a full TX FIFO waits while the guest still has RX bytes to read */
    return fifo8_is_full(&s->tx_fifo) && fifo8_is_empty(&s->rx_fifo);
}

/* chip enable n: the CS field selects it while TA is set, CSPOLn high */
static void bcm2835_spi_update_ce(BCM2835SPIState *s)
{
    for (int n = 0; n < BCM2835_SPI_NUM_CE; n++) {
        bool active = s->cs & BCM2835_SPI_CS_TA && extract32(s->cs, 0, 2) == n;
        bool active_high = s->cs & (BCM2835_SPI_CS_CSPOL0 << n);

        qemu_set_irq(s->ce[n], active == active_high);
    }
}

static void bcm2835_spi_update(BCM2835SPIState *s)
{
    bcm2835_spi_update_ce(s);
    if (bcm2835_spi_must_shift(s)) {
        bcm2835_spi_shift(s);
    }
    bcm2835_spi_update_tx_flags(s);
    bcm2835_spi_update_rx_flags(s);
    bcm2835_spi_update_int(s);
    bcm2835_spi_update_dreq(s);
    if (bcm2835_spi_can_shift(s) && !timer_pending(&s->shift_timer)) {
        uint32_t n = MIN(fifo8_num_used(&s->tx_fifo),
                         fifo8_num_free(&s->rx_fifo));

        timer_mod(&s->shift_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  MAX(bcm2835_spi_bus_ns(s, n), SHIFT_MIN_NS));
    }
}

static void bcm2835_spi_shift_timer(void *opaque)
{
    BCM2835SPIState *s = opaque;

    bcm2835_spi_shift(s);
    bcm2835_spi_update(s);
}

/*
 * The level of chip select pin n (the pin mux puts the controller's chip
 * enable or a GPIO on it) to the device with chip select index n.
 */
static void bcm2835_spi_cs_set(void *opaque, int n, int level)
{
    BCM2835SPIState *s = opaque;
    DeviceState *dev = ssi_get_cs(s->bus, n);

    /* the queued bytes go out before the device sees a change */
    if (dev && SSI_PERIPHERAL(dev)->cs != !!level) {
        bcm2835_spi_shift(s);
    }
    if (dev) {
        qemu_set_irq(qdev_get_gpio_in_named(dev, SSI_GPIO_CS, 0), level);
    }
}

/*
 * In DMA mode the FIFO is accessed in 32 bits, 4 bytes LSB first. The last
 * write of a transfer may hold fewer: the controller takes as many bytes as
 * DLEN still needs.
 */
static void bcm2835_spi_push_word(BCM2835SPIState *s, uint32_t value)
{
    uint32_t used = fifo8_num_used(&s->tx_fifo);
    uint32_t n = s->dlen > used ? MIN(s->dlen - used, 4) : 4;

    for (n = MIN(n, fifo8_num_free(&s->tx_fifo)); n; n--, value >>= 8) {
        fifo8_push(&s->tx_fifo, value & 0xff);
    }
}

static uint32_t bcm2835_spi_pop_word(BCM2835SPIState *s)
{
    uint32_t value = 0;
    unsigned i;

    for (i = 0; i < 4 && !fifo8_is_empty(&s->rx_fifo); i++) {
        value |= fifo8_pop(&s->rx_fifo) << (8 * i);
    }
    return value;
}

static uint64_t bcm2835_spi_read(void *opaque, hwaddr addr, unsigned size)
{
    BCM2835SPIState *s = opaque;
    uint32_t readval = 0;

    switch (addr) {
    case BCM2835_SPI_CS:
        if (!s->fifo_accessed) {
            bcm2835_spi_shift(s);
            bcm2835_spi_update(s);
        }
        s->fifo_accessed = false;
        readval = s->cs & 0xffffffff;
        break;
    case BCM2835_SPI_FIFO:
        s->fifo_accessed = true;
        if (fifo8_num_used(&s->rx_fifo) <
            (s->cs & BCM2835_SPI_CS_DMAEN ? 4 : 1)) {
            bcm2835_spi_shift(s);
        }
        if (s->cs & BCM2835_SPI_CS_DMAEN) {
            readval = bcm2835_spi_pop_word(s);
        } else if (s->cs & BCM2835_SPI_CS_RXD) {
            readval = fifo8_pop(&s->rx_fifo);
        }
        bcm2835_spi_update(s);
        break;
    case BCM2835_SPI_CLK:
        readval = s->clk & 0xffff;
        break;
    case BCM2835_SPI_DLEN:
        readval = s->dlen & 0xffff;
        break;
    case BCM2835_SPI_LTOH:
        readval = s->ltoh & 0xf;
        break;
    case BCM2835_SPI_DC:
        readval = s->dc & 0xffffffff;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
    }
    return readval;
}

static void bcm2835_spi_write_cs(BCM2835SPIState *s, uint32_t value)
{
    s->cs = (value & ~RO_MASK) | (s->cs & RO_MASK);
    if (!(s->cs & BCM2835_SPI_CS_TA)) {
        /* Clear DONE and RXR if TA is off */
        s->cs &= ~(BCM2835_SPI_CS_DONE);
        s->cs &= ~(BCM2835_SPI_CS_RXR);
    }

    /* Clear RX FIFO */
    if (s->cs & BCM2835_SPI_CLEAR_RX) {
        fifo8_reset(&s->rx_fifo);
    }

    /* Clear TX FIFO*/
    if (s->cs & BCM2835_SPI_CLEAR_TX) {
        fifo8_reset(&s->tx_fifo);
    }

    /* one-shot, read back as 0 */
    s->cs &= ~(BCM2835_SPI_CLEAR_RX | BCM2835_SPI_CLEAR_TX);

    if (s->cs & BCM2835_SPI_CS_LEN) {
        qemu_log_mask(LOG_UNIMP, "%s: " \
                      "LoSSI not supported\n", __func__);
    }
}

static void bcm2835_spi_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned int size)
{
    BCM2835SPIState *s = opaque;

    switch (addr) {
    case BCM2835_SPI_CS:
        bcm2835_spi_shift(s);
        bcm2835_spi_write_cs(s, value);
        bcm2835_spi_update(s);
        break;
    case BCM2835_SPI_FIFO:
        s->fifo_accessed = true;
        if (s->cs & BCM2835_SPI_CS_TA) {
            if (s->cs & BCM2835_SPI_CS_DMAEN) {
                bcm2835_spi_push_word(s, value);
            } else if (s->cs & BCM2835_SPI_CS_TXD) {
                fifo8_push(&s->tx_fifo, value & 0xff);
            }
        } else if (s->cs & BCM2835_SPI_CS_DMAEN) {
            /*
             * In DMA mode a write with TA clear sets DLEN and CS[7:0]:
             * a DMA control block can start a transfer this way.
             */
            bcm2835_spi_shift(s);
            s->dlen = value >> 16;
            bcm2835_spi_write_cs(s, (s->cs & ~0xff) | (value & 0xff));
        }
        /* Writes without TA outside DMA mode are ignored. */
        bcm2835_spi_update(s);
        break;
    case BCM2835_SPI_CLK:
        s->clk = value & 0xffff;
        break;
    case BCM2835_SPI_DLEN:
        s->dlen = value & 0xffff;
        bcm2835_spi_update(s);
        break;
    case BCM2835_SPI_LTOH:
        s->ltoh = value & 0xf;
        break;
    case BCM2835_SPI_DC:
        s->dc = value & 0xffffffff;
        bcm2835_spi_update_dreq(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
    }
}

static const MemoryRegionOps bcm2835_spi_ops = {
    .read = bcm2835_spi_read,
    .write = bcm2835_spi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void bcm2835_spi_realize(DeviceState *dev, Error **errp)
{
    BCM2835SPIState *s = BCM2835_SPI(dev);

    if (!s->clock_hz) {
        error_setg(errp, "bcm2835-spi: 'clock-hz' must not be 0");
        return;
    }
    s->bus = ssi_create_bus(dev, s->bus_name ?: "spi");

    memory_region_init_io(&s->iomem, OBJECT(dev), &bcm2835_spi_ops, s,
                          TYPE_BCM2835_SPI, 0x18);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    qdev_init_gpio_out_named(dev, s->dreq, "dreq", ARRAY_SIZE(s->dreq));
    qdev_init_gpio_out_named(dev, s->ce, "ce", BCM2835_SPI_NUM_CE);
    qdev_init_gpio_in_named(dev, bcm2835_spi_cs_set, "cs", BCM2835_SPI_NUM_CE);

    fifo8_create(&s->tx_fifo, FIFO_SIZE);
    fifo8_create(&s->rx_fifo, FIFO_SIZE);
    timer_init_ns(&s->shift_timer, QEMU_CLOCK_VIRTUAL,
                  bcm2835_spi_shift_timer, s);
}
static void bcm2835_spi_reset(DeviceState *dev)
{
    BCM2835SPIState *s = BCM2835_SPI(dev);

    timer_del(&s->shift_timer);
    s->fifo_accessed = false;
    fifo8_reset(&s->tx_fifo);
    fifo8_reset(&s->rx_fifo);

    /* Reset values according to BCM2835 Peripheral Documentation */
    s->cs = BCM2835_SPI_CS_TXD | BCM2835_SPI_CS_REN;
    s->clk = 0;
    s->dlen = 0;
    s->ltoh = 0x1;
    s->dc = 0x30201020;

    bcm2835_spi_update_ce(s);
    bcm2835_spi_update_dreq(s);
}

static int bcm2835_spi_post_load(void *opaque, int version_id)
{
    BCM2835SPIState *s = opaque;

    if (bcm2835_spi_can_shift(s)) {
        timer_mod(&s->shift_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    return 0;
}

static const VMStateDescription vmstate_bcm2835_spi = {
    .name = TYPE_BCM2835_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = bcm2835_spi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_FIFO8(tx_fifo, BCM2835SPIState),
        VMSTATE_FIFO8(rx_fifo, BCM2835SPIState),
        VMSTATE_UINT32(cs, BCM2835SPIState),
        VMSTATE_UINT32(clk, BCM2835SPIState),
        VMSTATE_UINT32(dlen, BCM2835SPIState),
        VMSTATE_UINT32(ltoh, BCM2835SPIState),
        VMSTATE_UINT32(dc, BCM2835SPIState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property bcm2835_spi_properties[] = {
    /* "spi" by default; SoCs with several controllers name them apart */
    DEFINE_PROP_STRING("bus-name", BCM2835SPIState, bus_name),
    /* VPU core clock, which CDIV divides into the SPI clock */
    DEFINE_PROP_UINT32("clock-hz", BCM2835SPIState, clock_hz, 500000000),
};

static void bcm2835_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, bcm2835_spi_reset);
    dc->realize = bcm2835_spi_realize;
    dc->vmsd = &vmstate_bcm2835_spi;
    device_class_set_props(dc, bcm2835_spi_properties);
}

static const TypeInfo bcm2835_spi_info = {
    .name = TYPE_BCM2835_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835SPIState),
    .class_init = bcm2835_spi_class_init,
};

static void bcm2835_spi_register_types(void)
{
    type_register_static(&bcm2835_spi_info);
}

type_init(bcm2835_spi_register_types)
