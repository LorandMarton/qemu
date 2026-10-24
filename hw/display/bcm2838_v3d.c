/*
 * BCM2838 (BCM2711) V3D 4.2 GPU
 *
 * "brcm,2711-v3d": the hub (TFU, MMU, cache control) and one core with
 * two slices of four QPUs. The identification registers hold the values
 * of a CM4 (Linux v3d_ident: V3D 4.2.14.0 with MMU, TFU, TSY and MSO,
 * 2 slices, 2 TMUs, 8 QPUs). The model keeps the registers for the guest
 * and completes the MMU TLB, MMU cache and L2T cache flushes at once, but
 * it executes no jobs: a binning, rendering, TFU or compute job that the
 * guest starts never completes and its interrupt is never raised.
 *
 * Both blocks raise the one V3D interrupt: the core (CTL_INT_*) and the
 * hub (HUB_INT_*) status bits that are not masked.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/display/bcm2838_v3d.h"
#include "hw/core/irq.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"

/* Interrupts, hub (HUB_INT_*) and core (CTL_INT_*) */
REG32(INT_STS, 0x0050)
REG32(INT_SET, 0x0054)
REG32(INT_CLR, 0x0058)
REG32(INT_MSK_STS, 0x005c)
REG32(INT_MSK_SET, 0x0060)
REG32(INT_MSK_CLR, 0x0064)

/* Hub */
REG32(HUB_UIFCFG, 0x0004)
REG32(HUB_IDENT0, 0x0008)
REG32(HUB_IDENT1, 0x000c)
REG32(HUB_IDENT2, 0x0010)
REG32(HUB_IDENT3, 0x0014)
REG32(TFU_ICFG, 0x0408)
REG32(MMUC_CONTROL, 0x1000)
    FIELD(MMUC_CONTROL, FLUSH, 1, 1)
    FIELD(MMUC_CONTROL, FLUSHING, 2, 1)
    FIELD(MMUC_CONTROL, CLEAR, 3, 1)
REG32(MMU_CTL, 0x1200)
    FIELD(MMU_CTL, TLB_CLEAR, 2, 1)
    FIELD(MMU_CTL, TLB_STATS_CLEAR, 3, 1)
    FIELD(MMU_CTL, TLB_CLEARING, 7, 1)
    FIELD(MMU_CTL, WRITE_VIOLATION, 12, 1)
    FIELD(MMU_CTL, PT_INVALID, 20, 1)
    FIELD(MMU_CTL, CAP_EXCEEDED, 27, 1)
REG32(MMU_SHOOT_DOWN, 0x1218)
    FIELD(MMU_SHOOT_DOWN, SHOOT, 28, 1)
    FIELD(MMU_SHOOT_DOWN, SHOOTING, 29, 1)
REG32(MMU_DEBUG_INFO, 0x1238)
    FIELD(MMU_DEBUG_INFO, VA_WIDTH, 4, 4)
    FIELD(MMU_DEBUG_INFO, PA_WIDTH, 8, 4)

/* Core */
REG32(CTL_IDENT0, 0x0000)
REG32(CTL_IDENT1, 0x0004)
REG32(CTL_IDENT2, 0x0008)
REG32(CTL_L2TCACTL, 0x0030)
    FIELD(CTL_L2TCACTL, L2TFLS, 0, 1)
REG32(CLE_CT0QEA, 0x0168)
REG32(CLE_CT1QEA, 0x016c)
REG32(GMP_STATUS, 0x0800)
REG32(CSD_QUEUED_CFG0, 0x0904)

/* CM4: Linux DRM_V3D_PARAM_* and v3d_ident */
#define V3D_HUB_UIFCFG_RESET  0x00000045
#define V3D_HUB_IDENT1_VALUE  0x000e1124 /* 4.2, 1 core, TFU TSY MSO */
#define V3D_HUB_IDENT2_VALUE  0x00000100 /* MMU, no L3C */
#define V3D_HUB_IDENT3_VALUE  0x00000e00 /* IPREV 14, IPIDX 0 */
#define V3D_CTL_IDENT0_VALUE  0x04443356 /* "V3D", version 4 */
#define V3D_CTL_IDENT1_VALUE  0x81001422 /* rev 2, 2 slices, 4 QPUs/slice */
#define V3D_CTL_IDENT2_VALUE  0x40078121

/*
 * The MMU page table entries hold 24-bit page numbers of 4 KiB pages
 * (36-bit physical addresses); the virtual address space is 4 GiB.
 */
#define V3D_MMU_PA_WIDTH      6
#define V3D_MMU_VA_WIDTH      2

/* Status bits, write 1 to clear */
#define MMU_CTL_W1C_MASK (R_MMU_CTL_WRITE_VIOLATION_MASK | \
                          R_MMU_CTL_PT_INVALID_MASK | \
                          R_MMU_CTL_CAP_EXCEEDED_MASK)

static void bcm2838_v3d_update_irq(BCM2838V3dState *s)
{
    qemu_set_irq(s->irq,
                 (s->hub[R_INT_STS] & ~s->hub[R_INT_MSK_STS]) ||
                 (s->core[R_INT_STS] & ~s->core[R_INT_MSK_STS]));
}

/*
 * The interrupt registers of the hub and of the core share one layout:
 * STS, SET, CLR, MSK_STS, MSK_SET, MSK_CLR from 0x50. Returns false for
 * other offsets.
 */
static bool bcm2838_v3d_int_write(BCM2838V3dState *s, uint32_t *regs,
                                  hwaddr addr, uint32_t value)
{
    switch (addr) {
    case A_INT_STS:
    case A_INT_MSK_STS:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: read-only register 0x%"
                      HWADDR_PRIx "\n", __func__, addr);
        return true;
    case A_INT_SET:
        regs[R_INT_STS] |= value;
        break;
    case A_INT_CLR:
        regs[R_INT_STS] &= ~value;
        break;
    case A_INT_MSK_SET:
        regs[R_INT_MSK_STS] |= value;
        break;
    case A_INT_MSK_CLR:
        regs[R_INT_MSK_STS] &= ~value;
        break;
    default:
        return false;
    }
    bcm2838_v3d_update_irq(s);
    return true;
}

static uint64_t bcm2838_v3d_hub_read(void *opaque, hwaddr addr,
                                     unsigned size)
{
    BCM2838V3dState *s = opaque;

    switch (addr) {
    case A_INT_SET:
    case A_INT_CLR:
    case A_INT_MSK_SET:
    case A_INT_MSK_CLR:
        return 0;
    default:
        return s->hub[addr >> 2];
    }
}

static void bcm2838_v3d_hub_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    BCM2838V3dState *s = opaque;

    if (bcm2838_v3d_int_write(s, s->hub, addr, value)) {
        return;
    }

    switch (addr) {
    case A_HUB_IDENT0:
    case A_HUB_IDENT1:
    case A_HUB_IDENT2:
    case A_HUB_IDENT3:
    case A_MMU_DEBUG_INFO:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: read-only register 0x%"
                      HWADDR_PRIx "\n", __func__, addr);
        return;
    case A_TFU_ICFG:
        qemu_log_mask(LOG_UNIMP, "%s: TFU jobs are not executed\n",
                      __func__);
        break;
    case A_MMUC_CONTROL:
        /* the MMU cache flushes and clears at once */
        value &= ~(R_MMUC_CONTROL_FLUSH_MASK | R_MMUC_CONTROL_FLUSHING_MASK |
                   R_MMUC_CONTROL_CLEAR_MASK);
        break;
    case A_MMU_CTL:
        /* the TLB clears at once */
        value = (value & ~(MMU_CTL_W1C_MASK | R_MMU_CTL_TLB_CLEAR_MASK |
                           R_MMU_CTL_TLB_STATS_CLEAR_MASK |
                           R_MMU_CTL_TLB_CLEARING_MASK)) |
                (s->hub[R_MMU_CTL] & MMU_CTL_W1C_MASK & ~value);
        break;
    case A_MMU_SHOOT_DOWN:
        value &= ~(R_MMU_SHOOT_DOWN_SHOOT_MASK |
                   R_MMU_SHOOT_DOWN_SHOOTING_MASK);
        break;
    }
    s->hub[addr >> 2] = value;
}

static uint64_t bcm2838_v3d_core_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    BCM2838V3dState *s = opaque;

    switch (addr) {
    case A_INT_SET:
    case A_INT_CLR:
    case A_INT_MSK_SET:
    case A_INT_MSK_CLR:
        return 0;
    default:
        return s->core[addr >> 2];
    }
}

static void bcm2838_v3d_core_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size)
{
    BCM2838V3dState *s = opaque;

    if (bcm2838_v3d_int_write(s, s->core, addr, value)) {
        return;
    }

    switch (addr) {
    case A_CTL_IDENT0:
    case A_CTL_IDENT1:
    case A_CTL_IDENT2:
    case A_GMP_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: read-only register 0x%"
                      HWADDR_PRIx "\n", __func__, addr);
        return;
    case A_CTL_L2TCACTL:
        /* the L2T cache flushes at once */
        value &= ~R_CTL_L2TCACTL_L2TFLS_MASK;
        break;
    case A_CLE_CT0QEA:
    case A_CLE_CT1QEA:
        qemu_log_mask(LOG_UNIMP, "%s: control lists are not executed\n",
                      __func__);
        break;
    case A_CSD_QUEUED_CFG0:
        qemu_log_mask(LOG_UNIMP, "%s: compute jobs are not executed\n",
                      __func__);
        break;
    }
    s->core[addr >> 2] = value;
}

static const MemoryRegionOps bcm2838_v3d_hub_ops = {
    .read = bcm2838_v3d_hub_read,
    .write = bcm2838_v3d_hub_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps bcm2838_v3d_core_ops = {
    .read = bcm2838_v3d_core_read,
    .write = bcm2838_v3d_core_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void bcm2838_v3d_reset_hold(Object *obj, ResetType type)
{
    BCM2838V3dState *s = BCM2838_V3D(obj);

    memset(s->hub, 0, sizeof(s->hub));
    memset(s->core, 0, sizeof(s->core));

    s->hub[R_HUB_UIFCFG] = V3D_HUB_UIFCFG_RESET;
    s->hub[R_HUB_IDENT1] = V3D_HUB_IDENT1_VALUE;
    s->hub[R_HUB_IDENT2] = V3D_HUB_IDENT2_VALUE;
    s->hub[R_HUB_IDENT3] = V3D_HUB_IDENT3_VALUE;
    s->hub[R_INT_MSK_STS] = 0xffffffff;
    s->hub[R_MMU_DEBUG_INFO] =
        FIELD_DP32(0, MMU_DEBUG_INFO, PA_WIDTH, V3D_MMU_PA_WIDTH) |
        FIELD_DP32(0, MMU_DEBUG_INFO, VA_WIDTH, V3D_MMU_VA_WIDTH);

    s->core[R_CTL_IDENT0] = V3D_CTL_IDENT0_VALUE;
    s->core[R_CTL_IDENT1] = V3D_CTL_IDENT1_VALUE;
    s->core[R_CTL_IDENT2] = V3D_CTL_IDENT2_VALUE;
    s->core[R_INT_MSK_STS] = 0xffffffff;

    bcm2838_v3d_update_irq(s);
}

static void bcm2838_v3d_init(Object *obj)
{
    BCM2838V3dState *s = BCM2838_V3D(obj);

    memory_region_init_io(&s->hub_iomem, obj, &bcm2838_v3d_hub_ops, s,
                          TYPE_BCM2838_V3D "-hub", BCM2838_V3D_HUB_SIZE);
    memory_region_init_io(&s->core_iomem, obj, &bcm2838_v3d_core_ops, s,
                          TYPE_BCM2838_V3D "-core0", BCM2838_V3D_CORE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->hub_iomem);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->core_iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static const VMStateDescription vmstate_bcm2838_v3d = {
    .name = TYPE_BCM2838_V3D,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(hub, BCM2838V3dState, BCM2838_V3D_HUB_SIZE / 4),
        VMSTATE_UINT32_ARRAY(core, BCM2838V3dState,
                             BCM2838_V3D_CORE_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2838_v3d_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = bcm2838_v3d_reset_hold;
    dc->vmsd = &vmstate_bcm2838_v3d;
}

static const TypeInfo bcm2838_v3d_info = {
    .name = TYPE_BCM2838_V3D,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2838V3dState),
    .instance_init = bcm2838_v3d_init,
    .class_init = bcm2838_v3d_class_init,
};

static void bcm2838_v3d_register_types(void)
{
    type_register_static(&bcm2838_v3d_info);
}

type_init(bcm2838_v3d_register_types)
