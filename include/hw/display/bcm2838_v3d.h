/*
 * BCM2838 (BCM2711) V3D 4.2 GPU
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_BCM2838_V3D_H
#define HW_DISPLAY_BCM2838_V3D_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BCM2838_V3D "bcm2838-v3d"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2838V3dState, BCM2838_V3D)

#define BCM2838_V3D_HUB_SIZE  0x4000
#define BCM2838_V3D_CORE_SIZE 0x4000

struct BCM2838V3dState {
    SysBusDevice parent_obj;

    MemoryRegion hub_iomem;
    MemoryRegion core_iomem;
    qemu_irq irq;

    uint32_t hub[BCM2838_V3D_HUB_SIZE / 4];
    uint32_t core[BCM2838_V3D_CORE_SIZE / 4];
};

#endif
