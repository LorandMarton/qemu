/*
 * BCM2838 (BCM2711) HDMI DVP clocks and resets
 *
 * The DVP block at the start of the always-on (AON) block,
 * "brcm,brcm2711-dvp": SW_INIT holds the resets of the two HDMI cores
 * (one bit each, set = in reset), MISC_CONFIG gates their 108 MHz clocks
 * (bits 3 and 4, set = gated). The settings take no effect in the model;
 * they are kept for the guest.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/bcm2838_dvp.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "trace.h"

REG32(SW_INIT, 0x04)
REG32(MISC_CONFIG, 0x08)

#define BCM2838_DVP_SIZE 0x10

/* six resets, two clock gates (Linux clk-bcm2711-dvp) */
#define SW_INIT_MASK     0x3f
#define MISC_CONFIG_MASK 0x18

static uint64_t bcm2838_dvp_read(void *opaque, hwaddr addr, unsigned size)
{
    BCM2838DvpState *s = opaque;

    switch (addr) {
    case A_SW_INIT:
        return s->sw_init;
    case A_MISC_CONFIG:
        return s->misc_config;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: read 0x%" HWADDR_PRIx "\n", __func__,
                      addr);
        return 0;
    }
}

static void bcm2838_dvp_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    BCM2838DvpState *s = opaque;

    trace_bcm2838_dvp_write(addr, value);
    switch (addr) {
    case A_SW_INIT:
        s->sw_init = value & SW_INIT_MASK;
        break;
    case A_MISC_CONFIG:
        s->misc_config = value & MISC_CONFIG_MASK;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: write 0x%" HWADDR_PRIx " 0x%" PRIx64
                      "\n", __func__, addr, value);
        break;
    }
}

static const MemoryRegionOps bcm2838_dvp_ops = {
    .read = bcm2838_dvp_read,
    .write = bcm2838_dvp_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void bcm2838_dvp_reset_hold(Object *obj, ResetType type)
{
    BCM2838DvpState *s = BCM2838_DVP(obj);

    s->sw_init = 0;
    s->misc_config = 0;
}

static void bcm2838_dvp_init(Object *obj)
{
    BCM2838DvpState *s = BCM2838_DVP(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2838_dvp_ops, s,
                          TYPE_BCM2838_DVP, BCM2838_DVP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const VMStateDescription vmstate_bcm2838_dvp = {
    .name = TYPE_BCM2838_DVP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(sw_init, BCM2838DvpState),
        VMSTATE_UINT32(misc_config, BCM2838DvpState),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2838_dvp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = bcm2838_dvp_reset_hold;
    dc->vmsd = &vmstate_bcm2838_dvp;
}

static const TypeInfo bcm2838_dvp_info = {
    .name = TYPE_BCM2838_DVP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2838DvpState),
    .instance_init = bcm2838_dvp_init,
    .class_init = bcm2838_dvp_class_init,
};

static void bcm2838_dvp_register_types(void)
{
    type_register_static(&bcm2838_dvp_info);
}

type_init(bcm2838_dvp_register_types)
