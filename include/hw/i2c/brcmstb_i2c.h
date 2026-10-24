/*
 * Broadcom STB BSC I2C controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I2C_BRCMSTB_I2C_H
#define HW_I2C_BRCMSTB_I2C_H

#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_BRCMSTB_I2C "brcmstb-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(BrcmstbI2CState, BRCMSTB_I2C)

#define BRCMSTB_I2C_DATA_REGS 8

struct BrcmstbI2CState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion autoi2c_iomem;
    qemu_irq irq;
    I2CBus *bus;

    uint32_t chip_address;
    uint32_t data_in[BRCMSTB_I2C_DATA_REGS];
    uint32_t cnt;
    uint32_t ctl;
    uint32_t iic_enable;
    uint32_t data_out[BRCMSTB_I2C_DATA_REGS];
    uint32_t ctlhi;
    uint32_t scl_param;
    bool active;            /* a transfer without its stop condition yet */
    uint32_t autoi2c_ctrl0;

    bool has_autoi2c;
    char *bus_name;
};

#endif
