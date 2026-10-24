/*
 * BCM2838 (BCM2711) asynchronous AXI bridge (ASB) controls
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef BCM2838_ASB_H
#define BCM2838_ASB_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BCM2838_ASB "bcm2838-asb"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2838AsbState, BCM2838_ASB)

/* BRDG_VERSION, CPR_CTRL and the six V3D/ISP/H264 slave/master controls */
#define BCM2838_ASB_NREGS 8

struct BCM2838AsbState {
    SysBusDevice busdev;
    MemoryRegion iomem;
    uint32_t regs[BCM2838_ASB_NREGS];
};

#endif /* BCM2838_ASB_H */
