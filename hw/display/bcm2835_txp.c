/*
 * BCM2835 transposer (TXP)
 *
 * "brcm,bcm2835-txp": the writeback engine that stores the output of an
 * HVS channel in memory (the DRM writeback connector). The model keeps
 * the registers for the guest but writes nothing back: the HVS model
 * composes no frames, so a frame started with GO never completes and
 * the end-of-frame interrupt is never raised.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/display/bcm2835_txp.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"

REG32(DST_PTR, 0x00)
REG32(DST_PITCH, 0x04)
REG32(DIM, 0x08)
REG32(DST_CTRL, 0x0c)
    FIELD(DST_CTRL, GO, 0, 1)
    FIELD(DST_CTRL, BUSY, 1, 1)
    FIELD(DST_CTRL, ABORT, 14, 1)
    FIELD(DST_CTRL, VERSION, 22, 2)
REG32(PROGRESS, 0x10)

/* GO, ABORT: commands; BUSY, VERSION: status */
#define DST_CTRL_RO_MASK (R_DST_CTRL_BUSY_MASK | R_DST_CTRL_VERSION_MASK)
#define DST_CTRL_CMD_MASK (R_DST_CTRL_GO_MASK | R_DST_CTRL_ABORT_MASK)

static uint64_t bcm2835_txp_read(void *opaque, hwaddr addr, unsigned size)
{
    BCM2835TxpState *s = opaque;

    return s->regs[addr >> 2];
}

static void bcm2835_txp_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    BCM2835TxpState *s = opaque;

    switch (addr) {
    case A_DST_CTRL:
        if (value & R_DST_CTRL_GO_MASK) {
            qemu_log_mask(LOG_UNIMP, "%s: writeback is not emulated\n",
                          __func__);
        }
        s->regs[R_DST_CTRL] = (s->regs[R_DST_CTRL] & DST_CTRL_RO_MASK) |
                              (value & ~(DST_CTRL_RO_MASK |
                                         DST_CTRL_CMD_MASK));
        break;
    case A_PROGRESS:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: PROGRESS is read-only\n",
                      __func__);
        break;
    default:
        s->regs[addr >> 2] = value;
        break;
    }
}

static const MemoryRegionOps bcm2835_txp_ops = {
    .read = bcm2835_txp_read,
    .write = bcm2835_txp_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void bcm2835_txp_reset_hold(Object *obj, ResetType type)
{
    BCM2835TxpState *s = BCM2835_TXP(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void bcm2835_txp_init(Object *obj)
{
    BCM2835TxpState *s = BCM2835_TXP(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2835_txp_ops, s,
                          TYPE_BCM2835_TXP, BCM2835_TXP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static const VMStateDescription vmstate_bcm2835_txp = {
    .name = TYPE_BCM2835_TXP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, BCM2835TxpState, BCM2835_TXP_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2835_txp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = bcm2835_txp_reset_hold;
    dc->vmsd = &vmstate_bcm2835_txp;
}

static const TypeInfo bcm2835_txp_info = {
    .name = TYPE_BCM2835_TXP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835TxpState),
    .instance_init = bcm2835_txp_init,
    .class_init = bcm2835_txp_class_init,
};

static void bcm2835_txp_register_types(void)
{
    type_register_static(&bcm2835_txp_info);
}

type_init(bcm2835_txp_register_types)
