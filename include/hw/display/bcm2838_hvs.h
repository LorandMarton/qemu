/*
 * BCM2838 (BCM2711) Hardware Video Scaler (HVS)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_BCM2838_HVS_H
#define HW_DISPLAY_BCM2838_HVS_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BCM2838_HVS "bcm2838-hvs"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2838HvsState, BCM2838_HVS)

#define BCM2838_HVS_REGS_SIZE  0x4000
#define BCM2838_HVS_DLIST_SIZE 0x4000

struct BCM2838HvsState {
    SysBusDevice parent_obj;

    MemoryRegion container;
    MemoryRegion iomem;
    MemoryRegion dlist;
    qemu_irq irq;

    uint32_t regs[BCM2838_HVS_REGS_SIZE / 4];
};

#endif
