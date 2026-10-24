/*
 * QTest testcase for the chardev-backed I2C target
 *
 * The test is the program at the other end of the chardev: a register
 * file with an auto-incrementing pointer, in a thread. The controller is
 * the BCM2835 BSC0 of raspi3b, which reports NACKs in its status register.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "libqtest.h"
#include "hw/i2c/bcm2835_i2c.h"

#define BSC0            0x3f205000
#define TARGET          0x50

typedef struct Model {
    int fd;
    QemuThread thread;
    QemuMutex lock;
    GString *log;               /* lines received, one per line */
    uint8_t reg[256];
    uint8_t ptr;
    bool first;                 /* next byte written is the pointer */
    bool absent;                /* NACK the address */
    int nack_byte;              /* NACK this byte, -1: none */
    bool bad_recv;              /* answer recv with something else */
} Model;

static void model_answer(Model *m, const char *line)
{
    g_assert_cmpint(write(m->fd, line, strlen(line)), ==, strlen(line));
}

static void model_line(Model *m, const char *line)
{
    unsigned int v;
    g_autofree char *answer = NULL;

    qemu_mutex_lock(&m->lock);
    g_string_append_printf(m->log, "%s\n", line);
    if (g_str_has_prefix(line, "start ")) {
        m->first = g_str_has_suffix(line, "write");
        answer = g_strdup(m->absent ? "nack\n" : "ack\n");
    } else if (sscanf(line, "send %x", &v) == 1) {
        if (v == m->nack_byte) {
            answer = g_strdup("nack\n");
        } else {
            if (m->first) {
                m->ptr = v;
                m->first = false;
            } else {
                m->reg[m->ptr++] = v;
            }
            answer = g_strdup("ack\n");
        }
    } else if (!strcmp(line, "recv")) {
        answer = m->bad_recv ? g_strdup("maybe\n") :
                 g_strdup_printf("%02x\n", m->reg[m->ptr++]);
    }
    qemu_mutex_unlock(&m->lock);
    if (answer) {
        model_answer(m, answer);
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

static QTestState *model_start(Model *m)
{
    int sv[2];
    QTestState *qts;

    memset(m, 0, sizeof(*m));
    m->nack_byte = -1;
    m->log = g_string_new(NULL);
    qemu_mutex_init(&m->lock);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    m->fd = sv[0];
    qts = qtest_initf("-M raspi3b -chardev socket,id=i2c,fd=%d "
                      "-device i2c-chardev,bus=i2c-bus.0,address=0x%x,"
                      "chardev=i2c", sv[1], TARGET);
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

#define MODEL_SET(m, field, value) do {     \
        qemu_mutex_lock(&(m)->lock);        \
        (m)->field = (value);               \
        qemu_mutex_unlock(&(m)->lock);      \
    } while (0)

static void bsc_start(QTestState *qts, uint8_t addr, uint32_t len, bool read)
{
    qtest_writel(qts, BSC0 + BCM2835_I2C_S, BCM2835_I2C_S_DONE |
                 BCM2835_I2C_S_ERR | BCM2835_I2C_S_CLKT);
    qtest_writel(qts, BSC0 + BCM2835_I2C_A, addr);
    qtest_writel(qts, BSC0 + BCM2835_I2C_DLEN, len);
    qtest_writel(qts, BSC0 + BCM2835_I2C_C, BCM2835_I2C_C_I2CEN |
                 BCM2835_I2C_C_ST | BCM2835_I2C_C_CLEAR |
                 (read ? BCM2835_I2C_C_READ : 0));
}

static uint32_t bsc_status(QTestState *qts)
{
    return qtest_readl(qts, BSC0 + BCM2835_I2C_S) &
           (BCM2835_I2C_S_ERR | BCM2835_I2C_S_DONE | BCM2835_I2C_S_TA);
}

static void test_transfers(void)
{
    Model m;
    QTestState *qts = model_start(&m);

    /* Write: pointer, then two registers */
    bsc_start(qts, TARGET, 3, false);
    qtest_writel(qts, BSC0 + BCM2835_I2C_FIFO, 0x10);
    qtest_writel(qts, BSC0 + BCM2835_I2C_FIFO, 0xaa);
    qtest_writel(qts, BSC0 + BCM2835_I2C_FIFO, 0xbb);
    g_assert_cmphex(bsc_status(qts), ==, BCM2835_I2C_S_DONE);
    model_expect(&m, "start 50 write\nsend 10\nsend aa\nsend bb\nstop\n");
    g_assert_cmphex(m.reg[0x10], ==, 0xaa);
    g_assert_cmphex(m.reg[0x11], ==, 0xbb);

    /* Read them back */
    bsc_start(qts, TARGET, 1, false);
    qtest_writel(qts, BSC0 + BCM2835_I2C_FIFO, 0x10);
    bsc_start(qts, TARGET, 2, true);
    g_assert_cmphex(qtest_readl(qts, BSC0 + BCM2835_I2C_FIFO), ==, 0xaa);
    g_assert_cmphex(qtest_readl(qts, BSC0 + BCM2835_I2C_FIFO), ==, 0xbb);
    g_assert_cmphex(bsc_status(qts), ==, BCM2835_I2C_S_DONE);
    model_expect(&m, "start 50 write\nsend 10\nstop\n"
                 "start 50 read\nrecv\nrecv\nstop\n");

    /* Another address is not ours */
    bsc_start(qts, TARGET + 1, 1, false);
    g_assert_cmphex(bsc_status(qts), ==,
                    BCM2835_I2C_S_ERR | BCM2835_I2C_S_DONE);
    model_expect(&m, "");

    model_stop(&m, qts);
}

static void test_nack(void)
{
    Model m;
    QTestState *qts = model_start(&m);

    /* The model NACKs a data byte: the transfer ends there */
    MODEL_SET(&m, nack_byte, 0xee);
    bsc_start(qts, TARGET, 3, false);
    qtest_writel(qts, BSC0 + BCM2835_I2C_FIFO, 0x20);
    qtest_writel(qts, BSC0 + BCM2835_I2C_FIFO, 0xee);
    g_assert_cmphex(bsc_status(qts), ==,
                    BCM2835_I2C_S_ERR | BCM2835_I2C_S_DONE);
    model_expect(&m, "start 50 write\nsend 20\nsend ee\nstop\n");

    /* ... or its address */
    MODEL_SET(&m, absent, true);
    bsc_start(qts, TARGET, 1, true);
    g_assert_cmphex(bsc_status(qts), ==,
                    BCM2835_I2C_S_ERR | BCM2835_I2C_S_DONE);
    model_expect(&m, "start 50 read\nstop\n");
    MODEL_SET(&m, absent, false);

    /* An answer that is not a byte reads 0xff */
    MODEL_SET(&m, bad_recv, true);
    bsc_start(qts, TARGET, 1, true);
    g_assert_cmphex(qtest_readl(qts, BSC0 + BCM2835_I2C_FIFO), ==, 0xff);
    model_expect(&m, "start 50 read\nrecv\nstop\n");

    model_stop(&m, qts);
}

static void test_disconnect(void)
{
    Model m;
    QTestState *qts = model_start(&m);

    /* The other end goes away: no ACK any more, and no hang */
    shutdown(m.fd, SHUT_RDWR);
    qemu_thread_join(&m.thread);
    for (int read = 0; read < 2; read++) {
        bsc_start(qts, TARGET, 1, read);
        g_assert_cmphex(bsc_status(qts), ==,
                        BCM2835_I2C_S_ERR | BCM2835_I2C_S_DONE);
    }

    close(m.fd);
    qtest_quit(qts);
    g_string_free(m.log, true);
    qemu_mutex_destroy(&m.lock);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/i2c-chardev/transfers", test_transfers);
    qtest_add_func("/i2c-chardev/nack", test_nack);
    qtest_add_func("/i2c-chardev/disconnect", test_disconnect);

    return g_test_run();
}
