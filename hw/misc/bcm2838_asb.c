/*
 * BCM2838 (BCM2711) asynchronous AXI bridge (ASB) controls
 *
 * The bridges between the power domains and the AXI bus. Linux
 * (bcm2835-power) checks the bridge ID before it registers the PM power
 * domains, and stops/starts a bridge around a domain power change by
 * setting REQ_STOP and polling ACK. The bridges stop at once here: ACK
 * follows REQ_STOP.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/bcm2838_asb.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"

REG32(BRDG_VERSION, 0x00)
REG32(AXI_BRDG_ID, 0x20)

#define ASB_REQ_STOP    BIT(0)
#define ASB_ACK         BIT(1)
#define ASB_EMPTY       BIT(2)

#define BCM2838_BRDG_ID 0x62726467 /* "brdg" */
#define PM_PASSWORD     0x5a000000
#define PM_PASSWORD_MASK 0xff000000

#define BCM2838_ASB_SIZE 0x24

static uint64_t bcm2838_asb_read(void *opaque, hwaddr addr, unsigned size)
{
    BCM2838AsbState *s = BCM2838_ASB(opaque);

    if (addr == A_AXI_BRDG_ID) {
        return BCM2838_BRDG_ID;
    }
    if (addr == A_BRDG_VERSION) {
        return s->regs[0];
    }
    /* Bridge controls: stopped as soon as requested, never holding data */
    if (s->regs[addr / 4] & ASB_REQ_STOP) {
        return ASB_REQ_STOP | ASB_ACK | ASB_EMPTY;
    }
    return ASB_EMPTY;
}

static void bcm2838_asb_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    BCM2838AsbState *s = BCM2838_ASB(opaque);

    if ((value & PM_PASSWORD_MASK) != PM_PASSWORD) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad password 0x%" PRIx64 " at 0x%" HWADDR_PRIx "\n",
                      __func__, value, addr);
        return;
    }
    if (addr == A_AXI_BRDG_ID || addr == A_BRDG_VERSION) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: read-only register 0x%"
                      HWADDR_PRIx "\n", __func__, addr);
        return;
    }
    s->regs[addr / 4] = value & ASB_REQ_STOP;
}

static const MemoryRegionOps bcm2838_asb_ops = {
    .read = bcm2838_asb_read,
    .write = bcm2838_asb_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void bcm2838_asb_reset(DeviceState *dev)
{
    BCM2838AsbState *s = BCM2838_ASB(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void bcm2838_asb_realize(DeviceState *dev, Error **errp)
{
    BCM2838AsbState *s = BCM2838_ASB(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &bcm2838_asb_ops, s,
                          TYPE_BCM2838_ASB, BCM2838_ASB_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const VMStateDescription vmstate_bcm2838_asb = {
    .name = TYPE_BCM2838_ASB,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, BCM2838AsbState, BCM2838_ASB_NREGS),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2838_asb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bcm2838_asb_realize;
    device_class_set_legacy_reset(dc, bcm2838_asb_reset);
    dc->vmsd = &vmstate_bcm2838_asb;
}

static const TypeInfo bcm2838_asb_info = {
    .name = TYPE_BCM2838_ASB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2838AsbState),
    .class_init = bcm2838_asb_class_init,
};

static void bcm2838_asb_register_types(void)
{
    type_register_static(&bcm2838_asb_info);
}

type_init(bcm2838_asb_register_types)
