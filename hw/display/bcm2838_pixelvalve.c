/*
 * BCM2838 (BCM2711) pixel valve
 *
 * "brcm,bcm2711-pixelvalveN": the video timing generator between an HVS
 * channel and an encoder (HDMI, DSI, DPI, VEC). The model keeps the
 * timing registers for the guest but generates no video timing: an
 * enabled pixel valve never reaches a vertical or horizontal event, so
 * none of the interrupt status bits is ever set, and disabling the video
 * (clearing VIDEN) takes effect at once.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/display/bcm2838_pixelvalve.h"
#include "hw/core/irq.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"

REG32(CONTROL, 0x00)
    FIELD(CONTROL, EN, 0, 1)
    FIELD(CONTROL, FIFO_CLR, 1, 1)
REG32(V_CONTROL, 0x04)
    FIELD(V_CONTROL, VIDEN, 0, 1)
REG32(INTEN, 0x24)
REG32(INTSTAT, 0x28)
REG32(STAT, 0x2c)

/* HSYNC_START .. VID_IDLE */
#define PV_INT_MASK 0x3ff

static void bcm2838_pixelvalve_update_irq(BCM2838PixelValveState *s)
{
    qemu_set_irq(s->irq, !!(s->regs[R_INTEN] & s->regs[R_INTSTAT]));
}

static uint64_t bcm2838_pixelvalve_read(void *opaque, hwaddr addr,
                                        unsigned size)
{
    BCM2838PixelValveState *s = opaque;

    return s->regs[addr >> 2];
}

static void bcm2838_pixelvalve_write(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned size)
{
    BCM2838PixelValveState *s = opaque;
    uint32_t old = s->regs[addr >> 2];

    switch (addr) {
    case A_CONTROL:
        /* the FIFO clears at once */
        s->regs[R_CONTROL] = value & ~R_CONTROL_FIFO_CLR_MASK;
        break;
    case A_V_CONTROL:
        s->regs[R_V_CONTROL] = value;
        if ((value & R_V_CONTROL_VIDEN_MASK) &&
            !(old & R_V_CONTROL_VIDEN_MASK)) {
            qemu_log_mask(LOG_UNIMP, "%s: video timing is not emulated\n",
                          __func__);
        }
        break;
    case A_INTEN:
        s->regs[R_INTEN] = value & PV_INT_MASK;
        break;
    case A_INTSTAT:
        s->regs[R_INTSTAT] &= ~value;
        break;
    case A_STAT:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: STAT is read-only\n", __func__);
        return;
    default:
        s->regs[addr >> 2] = value;
        return;
    }
    bcm2838_pixelvalve_update_irq(s);
}

static const MemoryRegionOps bcm2838_pixelvalve_ops = {
    .read = bcm2838_pixelvalve_read,
    .write = bcm2838_pixelvalve_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void bcm2838_pixelvalve_reset_hold(Object *obj, ResetType type)
{
    BCM2838PixelValveState *s = BCM2838_PIXELVALVE(obj);

    memset(s->regs, 0, sizeof(s->regs));
    bcm2838_pixelvalve_update_irq(s);
}

static void bcm2838_pixelvalve_init(Object *obj)
{
    BCM2838PixelValveState *s = BCM2838_PIXELVALVE(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2838_pixelvalve_ops, s,
                          TYPE_BCM2838_PIXELVALVE, BCM2838_PIXELVALVE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static const VMStateDescription vmstate_bcm2838_pixelvalve = {
    .name = TYPE_BCM2838_PIXELVALVE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, BCM2838PixelValveState,
                             BCM2838_PIXELVALVE_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2838_pixelvalve_class_init(ObjectClass *klass,
                                          const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = bcm2838_pixelvalve_reset_hold;
    dc->vmsd = &vmstate_bcm2838_pixelvalve;
}

static const TypeInfo bcm2838_pixelvalve_info = {
    .name = TYPE_BCM2838_PIXELVALVE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2838PixelValveState),
    .instance_init = bcm2838_pixelvalve_init,
    .class_init = bcm2838_pixelvalve_class_init,
};

static void bcm2838_pixelvalve_register_types(void)
{
    type_register_static(&bcm2838_pixelvalve_info);
}

type_init(bcm2838_pixelvalve_register_types)
