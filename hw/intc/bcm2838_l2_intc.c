/*
 * BCM2838 (BCM2711) L2 interrupt controller
 *
 * The level-sensitive second-level controller of the always-on (AON)
 * block, "brcm,bcm2711-l2-intc": 32 inputs (the HDMI hot-plug, CEC and
 * wake-up interrupts) behind one GIC interrupt. CPU_STATUS shows the
 * inputs and the bits set through CPU_SET, which CPU_CLEAR clears; the
 * output is the OR of the status bits that are not masked. All inputs
 * are masked out of reset.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/intc/bcm2838_l2_intc.h"
#include "hw/core/irq.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "trace.h"

REG32(CPU_STATUS, 0x00)
REG32(CPU_SET, 0x04)
REG32(CPU_CLEAR, 0x08)
REG32(CPU_MASK_STATUS, 0x0c)
REG32(CPU_MASK_SET, 0x10)
REG32(CPU_MASK_CLEAR, 0x14)

#define BCM2838_L2_INTC_SIZE 0x30

static uint32_t bcm2838_l2_intc_status(BCM2838L2IntcState *s)
{
    return s->level | s->soft;
}

static void bcm2838_l2_intc_update(BCM2838L2IntcState *s)
{
    qemu_set_irq(s->irq, !!(bcm2838_l2_intc_status(s) & ~s->mask));
}

static void bcm2838_l2_intc_set_irq(void *opaque, int n, int level)
{
    BCM2838L2IntcState *s = opaque;

    trace_bcm2838_l2_intc_set_irq(n, level);
    s->level = deposit32(s->level, n, 1, !!level);
    bcm2838_l2_intc_update(s);
}

static uint64_t bcm2838_l2_intc_read(void *opaque, hwaddr addr, unsigned size)
{
    BCM2838L2IntcState *s = opaque;

    switch (addr) {
    case A_CPU_STATUS:
        return bcm2838_l2_intc_status(s);
    case A_CPU_MASK_STATUS:
        return s->mask;
    case A_CPU_SET:
    case A_CPU_CLEAR:
    case A_CPU_MASK_SET:
    case A_CPU_MASK_CLEAR:
        return 0;
    default:
        /* the PCIe copy of the registers: no PCIe interrupt output */
        qemu_log_mask(LOG_UNIMP, "%s: read 0x%" HWADDR_PRIx "\n", __func__,
                      addr);
        return 0;
    }
}

static void bcm2838_l2_intc_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    BCM2838L2IntcState *s = opaque;

    switch (addr) {
    case A_CPU_SET:
        s->soft |= value;
        break;
    case A_CPU_CLEAR:
        s->soft &= ~value;
        break;
    case A_CPU_MASK_SET:
        s->mask |= value;
        break;
    case A_CPU_MASK_CLEAR:
        s->mask &= ~value;
        break;
    case A_CPU_STATUS:
    case A_CPU_MASK_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: read-only register 0x%"
                      HWADDR_PRIx "\n", __func__, addr);
        return;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: write 0x%" HWADDR_PRIx " 0x%" PRIx64
                      "\n", __func__, addr, value);
        return;
    }
    bcm2838_l2_intc_update(s);
}

static const MemoryRegionOps bcm2838_l2_intc_ops = {
    .read = bcm2838_l2_intc_read,
    .write = bcm2838_l2_intc_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void bcm2838_l2_intc_reset_hold(Object *obj, ResetType type)
{
    BCM2838L2IntcState *s = BCM2838_L2_INTC(obj);

    s->soft = 0;
    s->mask = 0xffffffff;
    bcm2838_l2_intc_update(s);
}

static void bcm2838_l2_intc_init(Object *obj)
{
    BCM2838L2IntcState *s = BCM2838_L2_INTC(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2838_l2_intc_ops, s,
                          TYPE_BCM2838_L2_INTC, BCM2838_L2_INTC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    qdev_init_gpio_in(DEVICE(s), bcm2838_l2_intc_set_irq,
                      BCM2838_L2_INTC_NUM_IRQS);
}

static const VMStateDescription vmstate_bcm2838_l2_intc = {
    .name = TYPE_BCM2838_L2_INTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(level, BCM2838L2IntcState),
        VMSTATE_UINT32(soft, BCM2838L2IntcState),
        VMSTATE_UINT32(mask, BCM2838L2IntcState),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2838_l2_intc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = bcm2838_l2_intc_reset_hold;
    dc->vmsd = &vmstate_bcm2838_l2_intc;
}

static const TypeInfo bcm2838_l2_intc_info = {
    .name = TYPE_BCM2838_L2_INTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2838L2IntcState),
    .instance_init = bcm2838_l2_intc_init,
    .class_init = bcm2838_l2_intc_class_init,
};

static void bcm2838_l2_intc_register_types(void)
{
    type_register_static(&bcm2838_l2_intc_info);
}

type_init(bcm2838_l2_intc_register_types)
