/*
 * BCM2838 (BCM2711) HDMI DVP clocks and resets
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_BCM2838_DVP_H
#define HW_MISC_BCM2838_DVP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BCM2838_DVP "bcm2838-dvp"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2838DvpState, BCM2838_DVP)

struct BCM2838DvpState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t sw_init;
    uint32_t misc_config;
};

#endif
