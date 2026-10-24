/*
 * BCM2835 transposer (TXP)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_BCM2835_TXP_H
#define HW_DISPLAY_BCM2835_TXP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BCM2835_TXP "bcm2835-txp"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835TxpState, BCM2835_TXP)

#define BCM2835_TXP_SIZE 0x20

struct BCM2835TxpState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[BCM2835_TXP_SIZE / 4];
};

#endif
