/*
 * BCM2838 (BCM2711) HDMI controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_BCM2838_HDMI_H
#define HW_DISPLAY_BCM2838_HDMI_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_BCM2838_HDMI "bcm2838-hdmi"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2838HdmiState, BCM2838_HDMI)

#define TYPE_BCM2838_HDMI_HD "bcm2838-hdmi-hd"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2838HdmiHdState, BCM2838_HDMI_HD)

/* MMIO regions, in the order of the Linux "reg-names" */
enum {
    BCM2838_HDMI_REGION_HDMI,
    BCM2838_HDMI_REGION_DVP,
    BCM2838_HDMI_REGION_PHY,
    BCM2838_HDMI_REGION_RM,
    BCM2838_HDMI_REGION_PACKET,
    BCM2838_HDMI_REGION_METADATA,
    BCM2838_HDMI_REGION_CSC,
    BCM2838_HDMI_REGION_CEC,
    BCM2838_HDMI_NUM_REGIONS
};

/* Interrupts, in the order of the Linux "interrupt-names" */
enum {
    BCM2838_HDMI_IRQ_CEC_TX,
    BCM2838_HDMI_IRQ_CEC_RX,
    BCM2838_HDMI_IRQ_CEC_LOW,
    BCM2838_HDMI_IRQ_WAKEUP,
    BCM2838_HDMI_IRQ_HPD_CONNECTED,
    BCM2838_HDMI_IRQ_HPD_REMOVED,
    BCM2838_HDMI_NUM_IRQS
};

#define BCM2838_HDMI_HDMI_SIZE      0x300
#define BCM2838_HDMI_DVP_SIZE       0x200
#define BCM2838_HDMI_PHY_SIZE       0x80
#define BCM2838_HDMI_RM_SIZE        0x80
#define BCM2838_HDMI_PACKET_SIZE    0x200
#define BCM2838_HDMI_METADATA_SIZE  0x400
#define BCM2838_HDMI_CSC_SIZE       0x80
#define BCM2838_HDMI_CEC_SIZE       0x100
#define BCM2838_HDMI_HD_SIZE        0x100

struct BCM2838HdmiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem[BCM2838_HDMI_NUM_REGIONS];
    qemu_irq irq[BCM2838_HDMI_NUM_IRQS];
    QEMUTimer *cec_timer;

    uint32_t hdmi[BCM2838_HDMI_HDMI_SIZE / 4];
    uint32_t dvp[BCM2838_HDMI_DVP_SIZE / 4];
    uint32_t phy[BCM2838_HDMI_PHY_SIZE / 4];
    uint32_t rm[BCM2838_HDMI_RM_SIZE / 4];
    uint32_t packet[BCM2838_HDMI_PACKET_SIZE / 4];
    uint32_t metadata[BCM2838_HDMI_METADATA_SIZE / 4];
    uint32_t csc[BCM2838_HDMI_CSC_SIZE / 4];
    uint32_t cec[BCM2838_HDMI_CEC_SIZE / 4];
};

/* The HD block, shared by both HDMI controllers (MAI, video control) */
struct BCM2838HdmiHdState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[BCM2838_HDMI_HD_SIZE / 4];
};

#endif
