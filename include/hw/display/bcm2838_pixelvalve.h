/*
 * BCM2838 (BCM2711) pixel valve
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_BCM2838_PIXELVALVE_H
#define HW_DISPLAY_BCM2838_PIXELVALVE_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BCM2838_PIXELVALVE "bcm2838-pixelvalve"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2838PixelValveState, BCM2838_PIXELVALVE)

#define BCM2838_PIXELVALVE_SIZE 0x100

struct BCM2838PixelValveState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[BCM2838_PIXELVALVE_SIZE / 4];
};

#endif
