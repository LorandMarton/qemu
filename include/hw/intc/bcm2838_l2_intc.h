/*
 * BCM2838 (BCM2711) L2 interrupt controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_INTC_BCM2838_L2_INTC_H
#define HW_INTC_BCM2838_L2_INTC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BCM2838_L2_INTC "bcm2838-l2-intc"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2838L2IntcState, BCM2838_L2_INTC)

#define BCM2838_L2_INTC_NUM_IRQS 32

struct BCM2838L2IntcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t level;     /* the inputs */
    uint32_t soft;      /* set through CPU_SET */
    uint32_t mask;
};

#endif
