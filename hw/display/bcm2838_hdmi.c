/*
 * BCM2838 (BCM2711) HDMI controller
 *
 * "brcm,bcm2711-hdmi0" and "brcm,bcm2711-hdmi1": the HDMI core, its DVP
 * (clock stop, video crossbar), PHY, rate manager, packet RAM, metadata,
 * colour space converter and CEC registers, one controller per HDMI
 * port. The DDC bus is a separate controller (brcmstb-i2c) and the
 * interrupts go to the always-on L2 interrupt controller.
 *
 * No display is connected: the hot-plug status reads disconnected and
 * the hot-plug interrupts are never raised. The model keeps the video,
 * PHY and audio registers for the guest but sends no TMDS signal.
 *
 * CEC: the bus has no other device. A transmission takes the nominal
 * frame time (start bit 4.5 ms, 10 bit periods of 2.4 ms per block). A
 * directed message is not acknowledged and stops after the header block;
 * a broadcast message is sent completely and reported good, since a
 * broadcast is only refused by a follower driving the ACK bit. At the end
 * TX_EOM is set and the cec-tx interrupt is raised until the guest clears
 * START_XMIT_BEGIN. Nothing is ever received.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/display/bcm2838_hdmi.h"
#include "hw/core/irq.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "trace.h"

/* HDMI core */
REG32(HDMI_HOTPLUG, 0x1a8)
    FIELD(HDMI_HOTPLUG, CONNECTED, 0, 1)

/* CEC */
REG32(CEC_CNTRL_1, 0x10)
    FIELD(CEC_CNTRL_1, DIV_CLK_CNT, 0, 12)
    FIELD(CEC_CNTRL_1, ADDR, 12, 4)
    FIELD(CEC_CNTRL_1, MESSAGE_LENGTH, 16, 4)
    FIELD(CEC_CNTRL_1, START_XMIT_BEGIN, 20, 1)
    FIELD(CEC_CNTRL_1, CLEAR_RECEIVE_OFF, 21, 1)
    FIELD(CEC_CNTRL_1, REC_WRD_CNT, 24, 4)
    FIELD(CEC_CNTRL_1, RX_STATUS_GOOD, 28, 1)
    FIELD(CEC_CNTRL_1, RX_EOM, 29, 1)
    FIELD(CEC_CNTRL_1, TX_STATUS_GOOD, 30, 1)
    FIELD(CEC_CNTRL_1, TX_EOM, 31, 1)
REG32(CEC_CNTRL_5, 0x20)
    FIELD(CEC_CNTRL_5, RX_SW_RESET, 26, 1)
    FIELD(CEC_CNTRL_5, TX_SW_RESET, 27, 1)
REG32(CEC_TX_DATA_1, 0x28)

#define CEC_CNTRL_1_STATUS_MASK (R_CEC_CNTRL_1_REC_WRD_CNT_MASK | \
                                 R_CEC_CNTRL_1_RX_STATUS_GOOD_MASK | \
                                 R_CEC_CNTRL_1_RX_EOM_MASK | \
                                 R_CEC_CNTRL_1_TX_STATUS_GOOD_MASK | \
                                 R_CEC_CNTRL_1_TX_EOM_MASK)

/* CEC nominal timing (HDMI 1.4 CEC 5.2.1) */
#define CEC_START_BIT_US        4500
#define CEC_BIT_US              2400
#define CEC_BITS_PER_BLOCK      10
#define CEC_BROADCAST           0xf

static const struct {
    const char *name;
    hwaddr size;
} bcm2838_hdmi_regions[BCM2838_HDMI_NUM_REGIONS] = {
    [BCM2838_HDMI_REGION_HDMI] = { "hdmi", BCM2838_HDMI_HDMI_SIZE },
    [BCM2838_HDMI_REGION_DVP] = { "dvp", BCM2838_HDMI_DVP_SIZE },
    [BCM2838_HDMI_REGION_PHY] = { "phy", BCM2838_HDMI_PHY_SIZE },
    [BCM2838_HDMI_REGION_RM] = { "rm", BCM2838_HDMI_RM_SIZE },
    [BCM2838_HDMI_REGION_PACKET] = { "packet", BCM2838_HDMI_PACKET_SIZE },
    [BCM2838_HDMI_REGION_METADATA] = { "metadata",
                                       BCM2838_HDMI_METADATA_SIZE },
    [BCM2838_HDMI_REGION_CSC] = { "csc", BCM2838_HDMI_CSC_SIZE },
    [BCM2838_HDMI_REGION_CEC] = { "cec", BCM2838_HDMI_CEC_SIZE },
};

/* Register blocks without side effects; the opaque is the register array */

static uint64_t bcm2838_hdmi_regs_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    uint32_t *regs = opaque;

    return regs[addr >> 2];
}

static void bcm2838_hdmi_regs_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    uint32_t *regs = opaque;

    regs[addr >> 2] = value;
}

static const MemoryRegionOps bcm2838_hdmi_regs_ops = {
    .read = bcm2838_hdmi_regs_read,
    .write = bcm2838_hdmi_regs_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* HDMI core */

static uint64_t bcm2838_hdmi_core_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    BCM2838HdmiState *s = opaque;

    if (addr == A_HDMI_HOTPLUG) {
        /* no display */
        return 0;
    }
    return s->hdmi[addr >> 2];
}

static void bcm2838_hdmi_core_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    BCM2838HdmiState *s = opaque;

    if (addr == A_HDMI_HOTPLUG) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: HOTPLUG is read-only\n",
                      __func__);
        return;
    }
    s->hdmi[addr >> 2] = value;
}

static const MemoryRegionOps bcm2838_hdmi_core_ops = {
    .read = bcm2838_hdmi_core_read,
    .write = bcm2838_hdmi_core_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* CEC */

static void bcm2838_hdmi_cec_update_irq(BCM2838HdmiState *s)
{
    uint32_t cntrl1 = s->cec[R_CEC_CNTRL_1];

    qemu_set_irq(s->irq[BCM2838_HDMI_IRQ_CEC_TX],
                 FIELD_EX32(cntrl1, CEC_CNTRL_1, START_XMIT_BEGIN) &&
                 FIELD_EX32(cntrl1, CEC_CNTRL_1, TX_EOM));
}

static void bcm2838_hdmi_cec_abort(BCM2838HdmiState *s)
{
    timer_del(s->cec_timer);
    s->cec[R_CEC_CNTRL_1] &= ~(R_CEC_CNTRL_1_TX_EOM_MASK |
                               R_CEC_CNTRL_1_TX_STATUS_GOOD_MASK);
}

static bool bcm2838_hdmi_cec_broadcast(BCM2838HdmiState *s)
{
    return extract32(s->cec[R_CEC_TX_DATA_1], 0, 4) == CEC_BROADCAST;
}

static void bcm2838_hdmi_cec_start(BCM2838HdmiState *s)
{
    unsigned len = FIELD_EX32(s->cec[R_CEC_CNTRL_1], CEC_CNTRL_1,
                              MESSAGE_LENGTH) + 1;
    bool broadcast = bcm2838_hdmi_cec_broadcast(s);
    /* a directed message ends with the unacknowledged header block */
    unsigned blocks = broadcast ? len : 1;
    int64_t us = CEC_START_BIT_US + blocks * CEC_BITS_PER_BLOCK * CEC_BIT_US;

    trace_bcm2838_hdmi_cec_tx(len, extract32(s->cec[R_CEC_TX_DATA_1], 0, 8));
    timer_mod(s->cec_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              us * SCALE_US);
}

static void bcm2838_hdmi_cec_tx_done(void *opaque)
{
    BCM2838HdmiState *s = opaque;
    bool ack = bcm2838_hdmi_cec_broadcast(s);

    trace_bcm2838_hdmi_cec_tx_done(ack);
    s->cec[R_CEC_CNTRL_1] = FIELD_DP32(s->cec[R_CEC_CNTRL_1], CEC_CNTRL_1,
                                       TX_EOM, 1);
    s->cec[R_CEC_CNTRL_1] = FIELD_DP32(s->cec[R_CEC_CNTRL_1], CEC_CNTRL_1,
                                       TX_STATUS_GOOD, ack);
    bcm2838_hdmi_cec_update_irq(s);
}

static uint64_t bcm2838_hdmi_cec_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    BCM2838HdmiState *s = opaque;

    return s->cec[addr >> 2];
}

static void bcm2838_hdmi_cec_write(void *opaque, hwaddr addr,
                                   uint64_t value, unsigned size)
{
    BCM2838HdmiState *s = opaque;
    uint32_t old = s->cec[addr >> 2];

    switch (addr) {
    case A_CEC_CNTRL_1:
        /* nothing is received: CLEAR_RECEIVE_OFF has no effect */
        s->cec[R_CEC_CNTRL_1] = (old & CEC_CNTRL_1_STATUS_MASK) |
                                (value & ~CEC_CNTRL_1_STATUS_MASK);
        if (!FIELD_EX32(value, CEC_CNTRL_1, START_XMIT_BEGIN)) {
            if (FIELD_EX32(old, CEC_CNTRL_1, START_XMIT_BEGIN)) {
                /* stops a transmission in progress */
                timer_del(s->cec_timer);
            }
        } else if (!FIELD_EX32(old, CEC_CNTRL_1, START_XMIT_BEGIN) &&
                   !FIELD_EX32(s->cec[R_CEC_CNTRL_5], CEC_CNTRL_5,
                               TX_SW_RESET)) {
            bcm2838_hdmi_cec_abort(s);
            bcm2838_hdmi_cec_start(s);
        }
        break;
    case A_CEC_CNTRL_5:
        s->cec[R_CEC_CNTRL_5] = value;
        if (FIELD_EX32(value, CEC_CNTRL_5, TX_SW_RESET)) {
            bcm2838_hdmi_cec_abort(s);
        }
        break;
    default:
        s->cec[addr >> 2] = value;
        return;
    }
    bcm2838_hdmi_cec_update_irq(s);
}

static const MemoryRegionOps bcm2838_hdmi_cec_ops = {
    .read = bcm2838_hdmi_cec_read,
    .write = bcm2838_hdmi_cec_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void bcm2838_hdmi_reset_hold(Object *obj, ResetType type)
{
    BCM2838HdmiState *s = BCM2838_HDMI(obj);

    timer_del(s->cec_timer);
    memset(s->hdmi, 0, sizeof(s->hdmi));
    memset(s->dvp, 0, sizeof(s->dvp));
    memset(s->phy, 0, sizeof(s->phy));
    memset(s->rm, 0, sizeof(s->rm));
    memset(s->packet, 0, sizeof(s->packet));
    memset(s->metadata, 0, sizeof(s->metadata));
    memset(s->csc, 0, sizeof(s->csc));
    memset(s->cec, 0, sizeof(s->cec));
    bcm2838_hdmi_cec_update_irq(s);
}

static void bcm2838_hdmi_init(Object *obj)
{
    BCM2838HdmiState *s = BCM2838_HDMI(obj);
    uint32_t *regs[BCM2838_HDMI_NUM_REGIONS] = {
        [BCM2838_HDMI_REGION_DVP] = s->dvp,
        [BCM2838_HDMI_REGION_PHY] = s->phy,
        [BCM2838_HDMI_REGION_RM] = s->rm,
        [BCM2838_HDMI_REGION_PACKET] = s->packet,
        [BCM2838_HDMI_REGION_METADATA] = s->metadata,
        [BCM2838_HDMI_REGION_CSC] = s->csc,
    };

    for (int i = 0; i < BCM2838_HDMI_NUM_REGIONS; i++) {
        g_autofree char *name = g_strdup_printf(TYPE_BCM2838_HDMI "-%s",
                                                bcm2838_hdmi_regions[i].name);
        const MemoryRegionOps *ops = &bcm2838_hdmi_regs_ops;
        void *opaque = regs[i];

        if (i == BCM2838_HDMI_REGION_HDMI) {
            ops = &bcm2838_hdmi_core_ops;
            opaque = s;
        } else if (i == BCM2838_HDMI_REGION_CEC) {
            ops = &bcm2838_hdmi_cec_ops;
            opaque = s;
        }
        memory_region_init_io(&s->iomem[i], obj, ops, opaque, name,
                              bcm2838_hdmi_regions[i].size);
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem[i]);
    }
    for (int i = 0; i < BCM2838_HDMI_NUM_IRQS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq[i]);
    }
    s->cec_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bcm2838_hdmi_cec_tx_done,
                                s);
}

static void bcm2838_hdmi_finalize(Object *obj)
{
    BCM2838HdmiState *s = BCM2838_HDMI(obj);

    timer_free(s->cec_timer);
}

static const VMStateDescription vmstate_bcm2838_hdmi = {
    .name = TYPE_BCM2838_HDMI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(hdmi, BCM2838HdmiState,
                             BCM2838_HDMI_HDMI_SIZE / 4),
        VMSTATE_UINT32_ARRAY(dvp, BCM2838HdmiState,
                             BCM2838_HDMI_DVP_SIZE / 4),
        VMSTATE_UINT32_ARRAY(phy, BCM2838HdmiState,
                             BCM2838_HDMI_PHY_SIZE / 4),
        VMSTATE_UINT32_ARRAY(rm, BCM2838HdmiState, BCM2838_HDMI_RM_SIZE / 4),
        VMSTATE_UINT32_ARRAY(packet, BCM2838HdmiState,
                             BCM2838_HDMI_PACKET_SIZE / 4),
        VMSTATE_UINT32_ARRAY(metadata, BCM2838HdmiState,
                             BCM2838_HDMI_METADATA_SIZE / 4),
        VMSTATE_UINT32_ARRAY(csc, BCM2838HdmiState,
                             BCM2838_HDMI_CSC_SIZE / 4),
        VMSTATE_UINT32_ARRAY(cec, BCM2838HdmiState,
                             BCM2838_HDMI_CEC_SIZE / 4),
        VMSTATE_TIMER_PTR(cec_timer, BCM2838HdmiState),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2838_hdmi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = bcm2838_hdmi_reset_hold;
    dc->vmsd = &vmstate_bcm2838_hdmi;
}

/* HD block */

static void bcm2838_hdmi_hd_reset_hold(Object *obj, ResetType type)
{
    BCM2838HdmiHdState *s = BCM2838_HDMI_HD(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void bcm2838_hdmi_hd_init(Object *obj)
{
    BCM2838HdmiHdState *s = BCM2838_HDMI_HD(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2838_hdmi_regs_ops, s->regs,
                          TYPE_BCM2838_HDMI_HD, BCM2838_HDMI_HD_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const VMStateDescription vmstate_bcm2838_hdmi_hd = {
    .name = TYPE_BCM2838_HDMI_HD,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, BCM2838HdmiHdState,
                             BCM2838_HDMI_HD_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2838_hdmi_hd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = bcm2838_hdmi_hd_reset_hold;
    dc->vmsd = &vmstate_bcm2838_hdmi_hd;
}

static const TypeInfo bcm2838_hdmi_types[] = {
    {
        .name = TYPE_BCM2838_HDMI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(BCM2838HdmiState),
        .instance_init = bcm2838_hdmi_init,
        .instance_finalize = bcm2838_hdmi_finalize,
        .class_init = bcm2838_hdmi_class_init,
    }, {
        .name = TYPE_BCM2838_HDMI_HD,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(BCM2838HdmiHdState),
        .instance_init = bcm2838_hdmi_hd_init,
        .class_init = bcm2838_hdmi_hd_class_init,
    },
};

DEFINE_TYPES(bcm2838_hdmi_types)
