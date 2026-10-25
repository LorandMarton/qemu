/*
 * QTest testcase for the BCM2838 (BCM2711) SPI0 controller
 *
 * A transfer with no device on the bus completes, and its interrupt
 * reaches the GIC on SPI 118 as the BCM2711 device tree has it. In DMA
 * mode, DREQ 6 (TX) and 7 (RX) pace the DMA channels as spi-bcm2835 sets
 * them up: a W25Q32 flash is programmed and read back. Its chip select is
 * GPIO 8, as a GPIO (Linux "cs-gpios") or as SPI0 CE0. SPI3 has its own
 * bus, its chip select on GPIO 0, and shares the interrupt.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "libqtest.h"

#define SPI0_BASE       0xfe204000
#define SPI3_BASE       0xfe204600
#define SPI_CS          0x00
#define SPI_FIFO        0x04
#define SPI_DLEN        0x0c

#define CS_CLEAR_RXTX   (3 << 4)
#define CS_TA           BIT(7)
#define CS_INTD         BIT(9)
#define CS_DMAEN        BIT(8)
#define CS_DONE         BIT(16)
#define CS_RXD          BIT(17)

#define GICD_BASE       0xff841000
#define GICD_ISENABLER  0x100
#define GICD_ISPENDR    0x200
#define GICD_ICPENDR    0x280
#define SPI0_IRQ        (32 + 118)

#define DMA_BASE        0xfe007000
#define DMA_CS          0x00
#define DMA_ADDR        0x04
#define DMA_TXFR_LEN    0x14
#define DMA_NEXTCB      0x1c
#define DMA_CHAN(n)     (DMA_BASE + (n) * 0x100)
#define DMA_TX          1
#define DMA_RX          2

#define DMA_CS_ACTIVE   BIT(0)
#define DMA_CS_END      BIT(1)
#define DMA_CS_INT      BIT(2)
#define DMA_CS_ISHELD   BIT(5)
#define DMA_CS_ABORT    BIT(30)
#define DMA_CS_RESET    BIT(31)
#define DMA_TI_INT_EN   BIT(0)
#define DMA_TI_D_INC    BIT(4)
#define DMA_TI_D_DREQ   BIT(6)
#define DMA_TI_S_INC    BIT(8)
#define DMA_TI_S_DREQ   BIT(10)
#define DMA_TI_PERMAP(n) ((n) << 16)

/* the FIFO as the DMA engine sees it, on the VideoCore bus */
#define SPI0_FIFO_BUS   0x7e204004

/* RAM, the same address on the VideoCore bus */
#define CB_TX           0x100000
#define CB_RX           0x100020
#define TX_BUF          0x101000
#define RX_BUF          0x102000
#define ZERO_BUF        0x103000

#define GPIO_BASE       0xfe200000
#define GPFSEL0         0x00
#define GPSET0          0x1c
#define GPCLR0          0x28
#define CS_PIN          8
#define FSEL_OUT        1
#define FSEL_ALT0       4
#define FLASH_ADDR      0x1000
#define XFER_LEN        101     /* not a multiple of 4: a short last word */

static uint32_t gicd_bit(QTestState *qts, uint32_t reg)
{
    return qtest_readl(qts, GICD_BASE + reg + SPI0_IRQ / 32 * 4) &
           BIT(SPI0_IRQ % 32);
}

static void test_transfer_irq(void)
{
    QTestState *qts = qtest_init("-M raspi4b");

    qtest_writel(qts, GICD_BASE + GICD_ISENABLER + SPI0_IRQ / 32 * 4,
                 BIT(SPI0_IRQ % 32));
    g_assert_false(gicd_bit(qts, GICD_ISPENDR));

    /* two bytes out, interrupt on DONE */
    qtest_writel(qts, SPI0_BASE + SPI_CS, CS_CLEAR_RXTX | CS_TA);
    qtest_writel(qts, SPI0_BASE + SPI_FIFO, 0xa5);
    qtest_writel(qts, SPI0_BASE + SPI_FIFO, 0x5a);
    qtest_writel(qts, SPI0_BASE + SPI_CS, CS_TA | CS_INTD);

    g_assert_cmphex(qtest_readl(qts, SPI0_BASE + SPI_CS) & (CS_DONE | CS_RXD),
                    ==, CS_DONE | CS_RXD);
    g_assert_true(gicd_bit(qts, GICD_ISPENDR));

    /* nothing on the bus: zeroes back */
    g_assert_cmphex(qtest_readl(qts, SPI0_BASE + SPI_FIFO), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SPI0_BASE + SPI_FIFO), ==, 0);
    g_assert_false(qtest_readl(qts, SPI0_BASE + SPI_CS) & CS_RXD);

    /* the end of the transfer drops the interrupt */
    qtest_writel(qts, SPI0_BASE + SPI_CS, 0);
    qtest_writel(qts, GICD_BASE + GICD_ICPENDR + SPI0_IRQ / 32 * 4,
                 BIT(SPI0_IRQ % 32));
    g_assert_false(gicd_bit(qts, GICD_ISPENDR));

    qtest_quit(qts);
}

/* A reset in the middle of a transfer drops the interrupt */
static void test_reset_irq(void)
{
    QTestState *qts = qtest_init("-M raspi4b");

    qtest_irq_intercept_out_named(qts, "/machine/soc/peripherals/bcm2835-spi0",
                                  "sysbus-irq");
    qtest_writel(qts, SPI0_BASE + SPI_CS, CS_CLEAR_RXTX | CS_TA);
    qtest_writel(qts, SPI0_BASE + SPI_FIFO, 0xa5);
    qtest_writel(qts, SPI0_BASE + SPI_CS, CS_TA | CS_INTD);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_system_reset(qts);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void cs_pin_function(QTestState *qts, uint32_t fsel)
{
    uint32_t v = qtest_readl(qts, GPIO_BASE + GPFSEL0);

    v = deposit32(v, CS_PIN * 3, 3, fsel);
    qtest_writel(qts, GPIO_BASE + GPFSEL0, v);
}

/* chip select by GPIO, active low */
static void flash_select(QTestState *qts, bool on)
{
    qtest_writel(qts, GPIO_BASE + (on ? GPCLR0 : GPSET0), BIT(CS_PIN));
}

static void write_cb(QTestState *qts, uint64_t cb, uint32_t ti, uint32_t src,
                     uint32_t dst, uint32_t len, uint32_t next)
{
    uint32_t words[8] = { ti, src, dst, len, 0, next, 0, 0 };

    for (int i = 0; i < 8; i++) {
        qtest_writel(qts, cb + i * 4, words[i]);
    }
}

static void dma_start(QTestState *qts, int c, uint64_t cb)
{
    qtest_writel(qts, DMA_CHAN(c) + DMA_CS, DMA_CS_RESET);
    qtest_writel(qts, DMA_CHAN(c) + DMA_ADDR, cb);
    qtest_writel(qts, DMA_CHAN(c) + DMA_CS, DMA_CS_ACTIVE);
}

/* the DMA engine runs DREQ-paced work from a bottom half */
static void dma_wait_end(QTestState *qts, int c)
{
    for (int i = 0; i < 1000; i++) {
        if (qtest_readl(qts, DMA_CHAN(c) + DMA_CS) & DMA_CS_END) {
            return;
        }
        qtest_clock_step(qts, 1000);
    }
    g_assert_not_reached();
}

/*
 * One transfer of XFER_LEN bytes in DMA mode, as spi-bcm2835 does it:
 * TX DMA issued first, RX DMA after it; RX completion ends the transfer.
 */
static void dma_transfer(QTestState *qts, uint32_t tx_ti, uint32_t tx_src,
                         uint32_t tx_next)
{
    write_cb(qts, CB_TX, tx_ti | DMA_TI_D_DREQ | DMA_TI_PERMAP(6),
             tx_src, SPI0_FIFO_BUS, XFER_LEN, tx_next);
    write_cb(qts, CB_RX, DMA_TI_S_DREQ | DMA_TI_PERMAP(7) | DMA_TI_D_INC |
             DMA_TI_INT_EN, SPI0_FIFO_BUS, RX_BUF, XFER_LEN, 0);

    qtest_writel(qts, SPI0_BASE + SPI_DLEN, XFER_LEN);
    qtest_writel(qts, SPI0_BASE + SPI_CS, CS_CLEAR_RXTX | CS_TA | CS_DMAEN);
    dma_start(qts, DMA_TX, CB_TX);
    dma_start(qts, DMA_RX, CB_RX);
    dma_wait_end(qts, DMA_RX);
    g_assert_cmphex(qtest_readl(qts, DMA_CHAN(DMA_RX) + DMA_TXFR_LEN), ==, 0);
    g_assert_true(qtest_readl(qts, SPI0_BASE + SPI_CS) & CS_DONE);
}

static void spi_reset(QTestState *qts)
{
    qtest_writel(qts, SPI0_BASE + SPI_CS, CS_CLEAR_RXTX);
    qtest_writel(qts, SPI0_BASE + SPI_DLEN, 0);
}

static void test_dma(void)
{
    QTestState *qts = qtest_init("-M raspi4b "
                                 "-device w25q32,id=flash,bus=spi");
    uint8_t tx[XFER_LEN], rx[XFER_LEN];
    uint32_t cs;

    qtest_writel(qts, GPIO_BASE + GPSET0, BIT(CS_PIN));
    cs_pin_function(qts, FSEL_OUT);

    /* write enable, without DMA */
    flash_select(qts, true);
    qtest_writel(qts, GICD_BASE + GICD_ISENABLER + SPI0_IRQ / 32 * 4,
                 BIT(SPI0_IRQ % 32));
    qtest_writel(qts, SPI0_BASE + SPI_CS, CS_CLEAR_RXTX | CS_TA | CS_INTD);
    qtest_writel(qts, SPI0_BASE + SPI_FIFO, 0x06);
    /* DONE was set until the write */
    qtest_writel(qts, GICD_BASE + GICD_ICPENDR + SPI0_IRQ / 32 * 4,
                 BIT(SPI0_IRQ % 32));
    /* the byte goes out in 8 SPI clocks: 8 * 65536 (reset CDIV) / 500 MHz */
    qtest_clock_step(qts, 1048576 - 1);
    g_assert_false(gicd_bit(qts, GICD_ISPENDR));
    qtest_clock_step(qts, 1);
    g_assert_true(gicd_bit(qts, GICD_ISPENDR));
    g_assert_true(qtest_readl(qts, SPI0_BASE + SPI_CS) & CS_DONE);
    spi_reset(qts);
    flash_select(qts, false);

    /* page program: command, 24-bit address, data */
    tx[0] = 0x02;
    tx[1] = (FLASH_ADDR >> 16) & 0xff;
    tx[2] = (FLASH_ADDR >> 8) & 0xff;
    tx[3] = FLASH_ADDR & 0xff;
    for (int i = 4; i < XFER_LEN; i++) {
        tx[i] = i * 7 + 3;
    }
    qtest_memwrite(qts, TX_BUF, tx, sizeof(tx));
    flash_select(qts, true);
    dma_transfer(qts, DMA_TI_S_INC, TX_BUF, 0);
    dma_wait_end(qts, DMA_TX);
    spi_reset(qts);
    flash_select(qts, false);

    /* read back */
    memset(tx + 4, 0, XFER_LEN - 4);
    tx[0] = 0x03;
    qtest_memwrite(qts, TX_BUF, tx, sizeof(tx));
    qtest_memset(qts, RX_BUF, 0xaa, XFER_LEN + 3);
    flash_select(qts, true);
    dma_transfer(qts, DMA_TI_S_INC, TX_BUF, 0);
    spi_reset(qts);
    flash_select(qts, false);
    qtest_memread(qts, RX_BUF, rx, sizeof(rx));
    for (int i = 4; i < XFER_LEN; i++) {
        g_assert_cmphex(rx[i], ==, (uint8_t)(i * 7 + 3));
    }
    /* the short last word wrote its byte only */
    g_assert_cmphex(qtest_readb(qts, RX_BUF + XFER_LEN), ==, 0xaa);

    /*
     * RX only: TX zero-filled by a cyclic control block. DREQ stays up
     * after DLEN has counted down (an erratum), so TX DMA goes on until the
     * FIFO is half full, then is held until aborted.
     */
    qtest_memset(qts, ZERO_BUF, 0, 4096);
    dma_transfer(qts, DMA_TI_S_INC, ZERO_BUF, CB_TX);
    cs = qtest_readl(qts, DMA_CHAN(DMA_TX) + DMA_CS);
    g_assert_cmphex(cs & (DMA_CS_ACTIVE | DMA_CS_ISHELD), ==,
                    DMA_CS_ACTIVE | DMA_CS_ISHELD);
    qtest_writel(qts, DMA_CHAN(DMA_TX) + DMA_NEXTCB, 0);
    qtest_writel(qts, DMA_CHAN(DMA_TX) + DMA_CS,
                 cs | DMA_CS_ABORT | DMA_CS_ACTIVE);
    cs = qtest_readl(qts, DMA_CHAN(DMA_TX) + DMA_CS);
    g_assert_false(cs & (DMA_CS_ACTIVE | DMA_CS_ABORT));
    g_assert_cmphex(qtest_readl(qts, DMA_CHAN(DMA_TX) + DMA_ADDR), ==, 0);
    spi_reset(qts);

    /* the RX channel's interrupt */
    g_assert_true(qtest_readl(qts, DMA_CHAN(DMA_RX) + DMA_CS) & DMA_CS_INT);

    qtest_quit(qts);
}

/* GPIO 8 as SPI0 CE0: TA with chip select 0 selects the flash */
static void test_native_ce(void)
{
    QTestState *qts = qtest_init("-M raspi4b "
                                 "-device w25q32,id=flash,bus=spi");
    uint8_t id[3];

    cs_pin_function(qts, FSEL_ALT0);
    for (int pass = 0; pass < 2; pass++) {
        /* JEDEC ID: the command is complete when CE0 goes high again */
        qtest_writel(qts, SPI0_BASE + SPI_CS, CS_CLEAR_RXTX | CS_TA);
        for (int i = 0; i < 4; i++) {
            qtest_writel(qts, SPI0_BASE + SPI_FIFO, i ? 0 : 0x9f);
        }
        qtest_readl(qts, SPI0_BASE + SPI_FIFO);
        for (int i = 0; i < 3; i++) {
            id[i] = qtest_readl(qts, SPI0_BASE + SPI_FIFO);
        }
        qtest_writel(qts, SPI0_BASE + SPI_CS, CS_CLEAR_RXTX);
        g_assert_cmphex(id[0] << 16 | id[1] << 8 | id[2], ==, 0xef4016);
    }

    /* chip select 1 leaves CE0 high: the flash does not answer */
    qtest_writel(qts, SPI0_BASE + SPI_CS, CS_CLEAR_RXTX | CS_TA | 1);
    qtest_writel(qts, SPI0_BASE + SPI_FIFO, 0x9f);
    qtest_writel(qts, SPI0_BASE + SPI_FIFO, 0);
    qtest_readl(qts, SPI0_BASE + SPI_FIFO);
    g_assert_cmphex(qtest_readl(qts, SPI0_BASE + SPI_FIFO), ==, 0);
    qtest_writel(qts, SPI0_BASE + SPI_CS, CS_CLEAR_RXTX);

    qtest_quit(qts);
}

static void test_spi3(void)
{
    QTestState *qts = qtest_init("-M raspi4b -device w25q32,bus=spi3");
    uint32_t fsel;
    uint8_t id[3];

    qtest_writel(qts, GICD_BASE + GICD_ISENABLER + SPI0_IRQ / 32 * 4,
                 BIT(SPI0_IRQ % 32));
    /* GPIO 0 as output, low: SPI3 CE0 selected (spi3-1cs) */
    fsel = qtest_readl(qts, GPIO_BASE + GPFSEL0);
    qtest_writel(qts, GPIO_BASE + GPFSEL0, deposit32(fsel, 0, 3, FSEL_OUT));
    qtest_writel(qts, GPIO_BASE + GPCLR0, BIT(0));

    qtest_writel(qts, SPI3_BASE + SPI_CS, CS_CLEAR_RXTX | CS_TA);
    for (int i = 0; i < 4; i++) {
        qtest_writel(qts, SPI3_BASE + SPI_FIFO, i ? 0 : 0x9f);
    }
    qtest_writel(qts, SPI3_BASE + SPI_CS, CS_TA | CS_INTD);
    g_assert_true(gicd_bit(qts, GICD_ISPENDR));
    qtest_readl(qts, SPI3_BASE + SPI_FIFO);
    for (int i = 0; i < 3; i++) {
        id[i] = qtest_readl(qts, SPI3_BASE + SPI_FIFO);
    }
    g_assert_cmphex(id[0] << 16 | id[1] << 8 | id[2], ==, 0xef4016);

    qtest_writel(qts, SPI3_BASE + SPI_CS, CS_CLEAR_RXTX);
    qtest_writel(qts, GPIO_BASE + GPSET0, BIT(0));
    qtest_writel(qts, GICD_BASE + GICD_ICPENDR + SPI0_IRQ / 32 * 4,
                 BIT(SPI0_IRQ % 32));
    g_assert_false(gicd_bit(qts, GICD_ISPENDR));

    /* nothing on SPI0 */
    qtest_writel(qts, SPI0_BASE + SPI_CS, CS_CLEAR_RXTX | CS_TA);
    qtest_writel(qts, SPI0_BASE + SPI_FIFO, 0x9f);
    qtest_writel(qts, SPI0_BASE + SPI_FIFO, 0);
    qtest_readl(qts, SPI0_BASE + SPI_FIFO);
    g_assert_cmphex(qtest_readl(qts, SPI0_BASE + SPI_FIFO), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/bcm2838-spi/transfer-irq", test_transfer_irq);
    qtest_add_func("/bcm2838-spi/reset-irq", test_reset_irq);
    qtest_add_func("/bcm2838-spi/dma", test_dma);
    qtest_add_func("/bcm2838-spi/native-ce", test_native_ce);
    qtest_add_func("/bcm2838-spi/spi3", test_spi3);

    return g_test_run();
}
