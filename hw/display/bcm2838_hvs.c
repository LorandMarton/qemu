/*
 * BCM2838 (BCM2711) Hardware Video Scaler (HVS)
 *
 * "brcm,bcm2711-hvs": the compositor in front of the pixel valves. The
 * first 16 KiB hold the registers, the second 16 KiB the display list
 * memory. The model keeps the registers and the display list memory for
 * the guest but composes nothing: a display channel that the guest
 * enables never runs, so its end-of-frame and underrun interrupts are
 * never raised. This is what Linux (vc4) sees on a board without a
 * display.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/display/bcm2838_hvs.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"

REG32(DISPCTRL, 0x00)
REG32(DISPSTAT, 0x04)
REG32(DISPID, 0x08)

/* Channel registers: DISPCTRLX, DISPBKGNDX, DISPSTATX, DISPBASEX */
#define HVS_CHANNEL_BASE        0x40
#define HVS_CHANNEL_STRIDE      0x10
#define HVS_NUM_CHANNELS        3
#define HVS_DISPCTRLX           0x0
#define HVS_DISPSTATX           0x8

#define DISPCTRLX_ENABLE        BIT(31)
#define DISPCTRLX_RESET         BIT(30)
#define DISPSTATX_EMPTY         BIT(28)

/* Interrupt status bits of DISPSTAT, write 1 to clear; IRQSCL clears all */
#define DISPSTAT_IRQ_MASK       0x3f3f3fff
#define DISPSTAT_IRQSCL         BIT(0)

static int bcm2838_hvs_channel(hwaddr addr, hwaddr *reg)
{
    if (addr < HVS_CHANNEL_BASE ||
        addr >= HVS_CHANNEL_BASE + HVS_NUM_CHANNELS * HVS_CHANNEL_STRIDE) {
        return -1;
    }
    *reg = (addr - HVS_CHANNEL_BASE) % HVS_CHANNEL_STRIDE;
    return (addr - HVS_CHANNEL_BASE) / HVS_CHANNEL_STRIDE;
}

static uint64_t bcm2838_hvs_read(void *opaque, hwaddr addr, unsigned size)
{
    BCM2838HvsState *s = opaque;
    hwaddr reg;
    int chan = bcm2838_hvs_channel(addr, &reg);

    if (chan >= 0 && reg == HVS_DISPSTATX) {
        /* the channel never leaves the DISABLED mode, its FIFO is empty */
        return DISPSTATX_EMPTY;
    }
    return s->regs[addr >> 2];
}

static void bcm2838_hvs_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    BCM2838HvsState *s = opaque;
    hwaddr reg;
    int chan = bcm2838_hvs_channel(addr, &reg);

    switch (addr) {
    case A_DISPSTAT:
        if (value & DISPSTAT_IRQSCL) {
            value = DISPSTAT_IRQ_MASK;
        }
        s->regs[R_DISPSTAT] &= ~(value & DISPSTAT_IRQ_MASK);
        return;
    case A_DISPID:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DISPID is read-only\n",
                      __func__);
        return;
    }

    if (chan >= 0 && reg == HVS_DISPSTATX) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DISPSTAT%d is read-only\n",
                      __func__, chan);
        return;
    }
    if (chan >= 0 && reg == HVS_DISPCTRLX) {
        /* the channel reset completes at once */
        value &= ~DISPCTRLX_RESET;
        if ((value & DISPCTRLX_ENABLE) &&
            !(s->regs[addr >> 2] & DISPCTRLX_ENABLE)) {
            qemu_log_mask(LOG_UNIMP, "%s: channel %d: composition is not "
                          "emulated\n", __func__, chan);
        }
    }
    s->regs[addr >> 2] = value;
}

static const MemoryRegionOps bcm2838_hvs_ops = {
    .read = bcm2838_hvs_read,
    .write = bcm2838_hvs_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void bcm2838_hvs_reset_hold(Object *obj, ResetType type)
{
    BCM2838HvsState *s = BCM2838_HVS(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void bcm2838_hvs_init(Object *obj)
{
    BCM2838HvsState *s = BCM2838_HVS(obj);

    memory_region_init(&s->container, obj, TYPE_BCM2838_HVS,
                       BCM2838_HVS_REGS_SIZE + BCM2838_HVS_DLIST_SIZE);
    memory_region_init_io(&s->iomem, obj, &bcm2838_hvs_ops, s,
                          TYPE_BCM2838_HVS "-regs", BCM2838_HVS_REGS_SIZE);
    memory_region_add_subregion(&s->container, 0, &s->iomem);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->container);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static void bcm2838_hvs_realize(DeviceState *dev, Error **errp)
{
    BCM2838HvsState *s = BCM2838_HVS(dev);

    if (!memory_region_init_ram(&s->dlist, OBJECT(dev),
                                TYPE_BCM2838_HVS "-dlist",
                                BCM2838_HVS_DLIST_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(&s->container, BCM2838_HVS_REGS_SIZE,
                                &s->dlist);
}

static const VMStateDescription vmstate_bcm2838_hvs = {
    .name = TYPE_BCM2838_HVS,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, BCM2838HvsState,
                             BCM2838_HVS_REGS_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2838_hvs_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = bcm2838_hvs_realize;
    rc->phases.hold = bcm2838_hvs_reset_hold;
    dc->vmsd = &vmstate_bcm2838_hvs;
}

static const TypeInfo bcm2838_hvs_info = {
    .name = TYPE_BCM2838_HVS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2838HvsState),
    .instance_init = bcm2838_hvs_init,
    .class_init = bcm2838_hvs_class_init,
};

static void bcm2838_hvs_register_types(void)
{
    type_register_static(&bcm2838_hvs_info);
}

type_init(bcm2838_hvs_register_types)
