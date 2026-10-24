/*
 * QTest testcase for the chardev-backed SPI peripheral
 *
 * The test is the program at the other end of the chardev, in a thread: it
 * answers each byte with its complement. The controller is SPI0 of
 * raspi4b; the device's chip select is GPIO 8 (CE0) or GPIO 7 (CE1). A
 * DMA mode word goes out as one line of four bytes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/cutils.h"
#include "qemu/thread.h"
#include "libqtest.h"

#define SPI0_BASE       0xfe204000
#define SPI_CS          0x00
#define SPI_FIFO        0x04
#define CS_CLEAR_RXTX   (3 << 4)
#define CS_TA           BIT(7)
#define CS_DMAEN        BIT(8)

#define GPIO_BASE       0xfe200000
#define GPFSEL0         0x00
#define GPSET0          0x1c
#define GPCLR0          0x28
#define FSEL_OUT        1
#define FSEL_ALT0       4

typedef struct Model {
    int fd;
    QemuThread thread;
    QemuMutex lock;
    GString *log;               /* lines received, one per line */
    bool bad_answer;            /* answer xfer with something else */
} Model;

static void model_line(Model *m, const char *line)
{
    g_autofree char *answer = NULL;

    qemu_mutex_lock(&m->lock);
    g_string_append_printf(m->log, "%s\n", line);
    if (g_str_has_prefix(line, "xfer ") && m->bad_answer) {
        answer = g_strdup("maybe\n");
    } else if (g_str_has_prefix(line, "xfer ")) {
        g_auto(GStrv) bytes = g_strsplit(line + 5, " ", -1);
        GString *a = g_string_new(NULL);

        for (int i = 0; bytes[i]; i++) {
            unsigned long v;

            g_assert_cmpint(qemu_strtoul(bytes[i], NULL, 16, &v), ==, 0);
            g_string_append_printf(a, "%s%02lx", i ? " " : "", ~v & 0xff);
        }
        g_string_append_c(a, '\n');
        answer = g_string_free(a, false);
    }
    qemu_mutex_unlock(&m->lock);
    if (answer) {
        g_assert_cmpint(write(m->fd, answer, strlen(answer)), ==,
                        strlen(answer));
    }
}

static void *model_run(void *opaque)
{
    Model *m = opaque;
    g_autoptr(GString) buf = g_string_new(NULL);
    char c;

    while (read(m->fd, &c, 1) == 1) {
        if (c == '\n') {
            model_line(m, buf->str);
            g_string_truncate(buf, 0);
        } else {
            g_string_append_c(buf, c);
        }
    }
    return NULL;
}

static QTestState *model_start(Model *m, int cs)
{
    int sv[2];
    QTestState *qts;

    memset(m, 0, sizeof(*m));
    m->log = g_string_new(NULL);
    qemu_mutex_init(&m->lock);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    m->fd = sv[0];
    qts = qtest_initf("-M raspi4b -chardev socket,id=spi,fd=%d "
                      "-device spi-chardev,bus=spi,cs=%d,chardev=spi",
                      sv[1], cs);
    close(sv[1]);
    qemu_thread_create(&m->thread, "model", model_run, m,
                       QEMU_THREAD_JOINABLE);
    return qts;
}

static void model_stop(Model *m, QTestState *qts)
{
    shutdown(m->fd, SHUT_RDWR);
    qemu_thread_join(&m->thread);
    close(m->fd);
    qtest_quit(qts);
    g_string_free(m->log, true);
    qemu_mutex_destroy(&m->lock);
}

/*
 * Wait for the lines the model received to be @expected, then forget
 * them. The lines that need no answer may still be on their way.
 */
static void model_expect(Model *m, const char *expected)
{
    for (int i = 0; i < 10000; i++) {
        bool done;

        qemu_mutex_lock(&m->lock);
        done = !strcmp(m->log->str, expected);
        qemu_mutex_unlock(&m->lock);
        if (done) {
            break;
        }
        g_usleep(1000);
    }
    qemu_mutex_lock(&m->lock);
    g_assert_cmpstr(m->log->str, ==, expected);
    g_string_truncate(m->log, 0);
    qemu_mutex_unlock(&m->lock);
}

static void pin_function(QTestState *qts, int pin, uint32_t fsel)
{
    uint32_t v = qtest_readl(qts, GPIO_BASE + GPFSEL0);

    qtest_writel(qts, GPIO_BASE + GPFSEL0, deposit32(v, pin * 3, 3, fsel));
}

/* Two bytes out with chip select @cs (native CE), the answers back */
static void transfer(QTestState *qts, int cs, uint8_t a, uint8_t b,
                     uint32_t *ra, uint32_t *rb)
{
    qtest_writel(qts, SPI0_BASE + SPI_CS, CS_CLEAR_RXTX | CS_TA | cs);
    qtest_writel(qts, SPI0_BASE + SPI_FIFO, a);
    qtest_writel(qts, SPI0_BASE + SPI_FIFO, b);
    *ra = qtest_readl(qts, SPI0_BASE + SPI_FIFO);
    *rb = qtest_readl(qts, SPI0_BASE + SPI_FIFO);
    qtest_writel(qts, SPI0_BASE + SPI_CS, CS_CLEAR_RXTX | cs);
}

static void test_gpio_cs(void)
{
    Model m;
    QTestState *qts = model_start(&m, 0);
    uint32_t a, b;

    /* reset: GPIO 8 is an input with its pull-up, the device released */
    model_expect(&m, "deselect\n");

    /* GPIO 8 as the chip select (Linux "cs-gpios") */
    qtest_writel(qts, GPIO_BASE + GPSET0, BIT(8));
    pin_function(qts, 8, FSEL_OUT);
    qtest_writel(qts, GPIO_BASE + GPCLR0, BIT(8));
    transfer(qts, 3, 0x12, 0xa0, &a, &b);
    qtest_writel(qts, GPIO_BASE + GPSET0, BIT(8));
    g_assert_cmphex(a, ==, 0xed);
    g_assert_cmphex(b, ==, 0x5f);
    model_expect(&m, "select\nxfer 12 a0\ndeselect\n");

    /* released: nothing reaches the device, the bus reads 0 */
    transfer(qts, 3, 0x34, 0x56, &a, &b);
    g_assert_cmphex(a | b, ==, 0);
    model_expect(&m, "");

    model_stop(&m, qts);
}

static void test_native_ce(void)
{
    Model m;
    QTestState *qts = model_start(&m, 1);
    uint32_t a, b;

    model_expect(&m, "deselect\n");

    /* GPIO 7 as SPI0 CE1: TA with chip select 1 selects the device */
    pin_function(qts, 7, FSEL_ALT0);
    transfer(qts, 1, 0x00, 0xff, &a, &b);
    g_assert_cmphex(a, ==, 0xff);
    g_assert_cmphex(b, ==, 0x00);
    model_expect(&m, "select\nxfer 00 ff\ndeselect\n");

    /* chip select 0 is another device */
    transfer(qts, 0, 0x01, 0x02, &a, &b);
    g_assert_cmphex(a | b, ==, 0);
    model_expect(&m, "");

    /* an answer that is not a byte reads 0 */
    qemu_mutex_lock(&m.lock);
    m.bad_answer = true;
    qemu_mutex_unlock(&m.lock);
    transfer(qts, 1, 0x01, 0x02, &a, &b);
    g_assert_cmphex(a | b, ==, 0);
    model_expect(&m, "select\nxfer 01 02\ndeselect\n");

    model_stop(&m, qts);
}

static void test_disconnect(void)
{
    Model m;
    QTestState *qts = model_start(&m, 0);
    uint32_t a, b;

    model_expect(&m, "deselect\n");
    /* The other end goes away: zeroes, and no hang */
    shutdown(m.fd, SHUT_RDWR);
    qemu_thread_join(&m.thread);
    pin_function(qts, 8, FSEL_ALT0);
    transfer(qts, 0, 0x12, 0x34, &a, &b);
    g_assert_cmphex(a | b, ==, 0);

    close(m.fd);
    qtest_quit(qts);
    g_string_free(m.log, true);
    qemu_mutex_destroy(&m.lock);
}

static void test_dma_word(void)
{
    Model m;
    QTestState *qts = model_start(&m, 0);

    model_expect(&m, "deselect\n");
    pin_function(qts, 8, FSEL_ALT0);

    /* DLEN 4 and TA from the FIFO, as a DMA control block starts it */
    qtest_writel(qts, SPI0_BASE + SPI_CS, CS_CLEAR_RXTX | CS_DMAEN);
    qtest_writel(qts, SPI0_BASE + SPI_FIFO, 4 << 16 | CS_TA);
    qtest_writel(qts, SPI0_BASE + SPI_FIFO, 0x04030201);
    g_assert_cmphex(qtest_readl(qts, SPI0_BASE + SPI_FIFO), ==, 0xfbfcfdfe);
    qtest_writel(qts, SPI0_BASE + SPI_CS, CS_CLEAR_RXTX);
    model_expect(&m, "select\nxfer 01 02 03 04\ndeselect\n");

    model_stop(&m, qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/spi-chardev/gpio-cs", test_gpio_cs);
    qtest_add_func("/spi-chardev/native-ce", test_native_ce);
    qtest_add_func("/spi-chardev/disconnect", test_disconnect);
    qtest_add_func("/spi-chardev/dma-word", test_dma_word);

    return g_test_run();
}
