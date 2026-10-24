/*
 * QTest testcase for the NXP PCF85063A real-time clock
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bcd.h"
#include "libqos/i2c.h"

#define PCF85063_ADDR   0x51

#define REG_CTRL1       0x00
#define REG_SC          0x04
#define REG_DW          0x08
#define REG_YR          0x0a
#define REG_RAM         0x03

#define CTRL1_STOP      0x20
#define CTRL1_SR        0x58
#define SC_OS           0x80

static void pcf85063_read_date(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = obj;
    uint8_t resp[7];
    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);

    i2c_read_block(i2cdev, REG_SC, resp, sizeof(resp));

    /* Without power-lost the clock runs on the host time, OS clear */
    g_assert_cmphex(resp[0] & SC_OS, ==, 0);
    g_assert_cmpuint(from_bcd(resp[3]), ==, utc->tm_mday);
    g_assert_cmpuint(from_bcd(resp[5]), ==, 1 + utc->tm_mon);
    g_assert_cmpuint(2000 + from_bcd(resp[6]), ==, 1900 + utc->tm_year);
}

static void pcf85063_software_reset(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = obj;
    uint8_t resp[7];

    i2c_set8(i2cdev, REG_RAM, 0xa5);
    i2c_set8(i2cdev, REG_CTRL1, CTRL1_SR);

    /* Registers back to their reset values, time invalid (OS) */
    g_assert_cmphex(i2c_get8(i2cdev, REG_RAM), ==, 0);
    g_assert_cmphex(i2c_get8(i2cdev, REG_CTRL1), ==, 0);
    i2c_read_block(i2cdev, REG_SC, resp, sizeof(resp));
    g_assert_cmphex(resp[0] & SC_OS, ==, SC_OS);
    g_assert_cmphex(resp[3], ==, 0x01);     /* day 1 */
    g_assert_cmphex(resp[4], ==, 0x06);     /* Saturday */
    g_assert_cmphex(resp[5], ==, 0x01);     /* January */
    g_assert_cmphex(resp[6], ==, 0x00);     /* 2000 */

    /* Writing the seconds register clears OS */
    i2c_set8(i2cdev, REG_SC, 0x10);
    g_assert_cmphex(i2c_get8(i2cdev, REG_SC) & SC_OS, ==, 0);
}

static void pcf85063_stop(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = obj;
    /* 2031-12-31 (Wednesday) 23:59:58 */
    const uint8_t set[7] = { 0x58, 0x59, 0x23, 0x31, 0x03, 0x12, 0x31 };
    uint8_t resp[7];

    /* A stopped clock keeps the time written to it */
    i2c_set8(i2cdev, REG_CTRL1, CTRL1_STOP);
    i2c_write_block(i2cdev, REG_SC, set, sizeof(set));
    i2c_read_block(i2cdev, REG_SC, resp, sizeof(resp));
    g_assert_cmpmem(resp, sizeof(resp), set, sizeof(set));

    /* Restarted, it runs from there: still 2031 or just into 2032 */
    i2c_set8(i2cdev, REG_CTRL1, 0);
    i2c_read_block(i2cdev, REG_SC, resp, sizeof(resp));
    g_assert(resp[6] == 0x31 || resp[6] == 0x32);
}

static void pcf85063_register_nodes(void)
{
    QOSGraphEdgeOptions opts = { };

    add_qi2c_address(&opts, &(QI2CAddress) { PCF85063_ADDR });

    qos_node_create_driver("pcf85063", i2c_device_create);
    qos_node_consumes("pcf85063", "i2c-bus", &opts);
    qos_add_test("read_date", "pcf85063", pcf85063_read_date, NULL);
    qos_add_test("software_reset", "pcf85063", pcf85063_software_reset,
                 NULL);
    qos_add_test("stop", "pcf85063", pcf85063_stop, NULL);
}

libqos_init(pcf85063_register_nodes);
