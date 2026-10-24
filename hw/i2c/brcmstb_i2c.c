/*
 * Broadcom STB BSC I2C controller
 *
 * The BSC master of Broadcom set-top box chips, "brcm,brcmstb-i2c"; on the
 * BCM2711 the two HDMI DDC controllers ("brcm,bcm2711-hdmi-i2c"), whose
 * auto-I2C block (EDID and SCDC reads by the HDMI core) owns the bus until
 * the host releases it.
 *
 * A transfer is one command: the host sets the start/stop conditions and
 * ENABLE in IIC_ENABLE, the controller sends a start (or repeated start) and
 * the address in CHIP_ADDRESS unless NOSTART, moves up to CNT bytes through
 * DATA_IN or DATA_OUT (one or four bytes per register, CTLHI DATAREG_SIZE),
 * ends with a stop unless NOSTOP, and sets INTRP, with NOACK when the
 * address or a written byte was not acknowledged. Transfers complete at
 * once.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/i2c/brcmstb_i2c.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "trace.h"

REG32(CHIP_ADDRESS, 0x00)
REG32(DATA_IN0, 0x04)
REG32(CNT, 0x24)
    FIELD(CNT, CNT1, 0, 6)
    FIELD(CNT, CNT2, 6, 6)
REG32(CTL, 0x28)
    FIELD(CTL, DTF, 0, 2)
    FIELD(CTL, SCL_SEL, 4, 2)
    FIELD(CTL, INT_EN, 6, 1)
    FIELD(CTL, DIV_CLK, 7, 1)
REG32(IIC_ENABLE, 0x2c)
    FIELD(IIC_ENABLE, ENABLE, 0, 1)
    FIELD(IIC_ENABLE, INTRP, 1, 1)
    FIELD(IIC_ENABLE, NOACK, 2, 1)
    FIELD(IIC_ENABLE, NOSTOP, 4, 1)
    FIELD(IIC_ENABLE, NOSTART, 5, 1)
    FIELD(IIC_ENABLE, RESTART, 6, 1)
REG32(DATA_OUT0, 0x30)
REG32(CTLHI, 0x50)
    FIELD(CTLHI, WAIT_DIS, 0, 1)
    FIELD(CTLHI, IGNORE_ACK, 1, 1)
    FIELD(CTLHI, DATAREG_SIZE, 6, 1)
REG32(SCL_PARAM, 0x54)

#define BRCMSTB_I2C_SIZE        0x58

#define DTF_WRITE               0
#define DTF_READ                1

/* auto-I2C (BCM2711 HDMI) */
#define AUTOI2C_SIZE            0x300
#define AUTOI2C_CTRL0           0x26c
#define AUTOI2C_CTRL0_RELEASE_BSC BIT(1)

#define IIC_ENABLE_COND_MASK    (R_IIC_ENABLE_NOSTOP_MASK | \
                                 R_IIC_ENABLE_NOSTART_MASK | \
                                 R_IIC_ENABLE_RESTART_MASK)

static void brcmstb_i2c_update_irq(BrcmstbI2CState *s)
{
    qemu_set_irq(s->irq, FIELD_EX32(s->ctl, CTL, INT_EN) &&
                         FIELD_EX32(s->iic_enable, IIC_ENABLE, INTRP));
}

static unsigned brcmstb_i2c_bytes_per_reg(BrcmstbI2CState *s)
{
    return FIELD_EX32(s->ctlhi, CTLHI, DATAREG_SIZE) ? 4 : 1;
}

/* The command IIC_ENABLE started; returns false on a NACK */
static bool brcmstb_i2c_command(BrcmstbI2CState *s)
{
    unsigned dtf = FIELD_EX32(s->ctl, CTL, DTF);
    unsigned bpr = brcmstb_i2c_bytes_per_reg(s);
    unsigned len = MIN(FIELD_EX32(s->cnt, CNT, CNT1),
                       BRCMSTB_I2C_DATA_REGS * bpr);
    bool ignore_ack = FIELD_EX32(s->ctlhi, CTLHI, IGNORE_ACK);
    uint8_t addr = s->chip_address >> 1;

    if (dtf != DTF_WRITE && dtf != DTF_READ) {
        qemu_log_mask(LOG_UNIMP, "%s: combined transfer (DTF %u)\n",
                      __func__, dtf);
        return false;
    }
    trace_brcmstb_i2c_command(addr, dtf == DTF_READ ? "read" : "write",
                              len, s->iic_enable & IIC_ENABLE_COND_MASK);

    if (!FIELD_EX32(s->iic_enable, IIC_ENABLE, NOSTART) || !s->active) {
        /* start, or a repeated start while the bus is held */
        if (i2c_start_transfer(s->bus, addr, dtf == DTF_READ) &&
            !ignore_ack) {
            i2c_end_transfer(s->bus);
            s->active = false;
            return false;
        }
        s->active = true;
    }

    if (dtf == DTF_WRITE) {
        for (unsigned i = 0; i < len; i++) {
            uint8_t b = s->data_in[i / bpr] >> (8 * (i % bpr));

            if (i2c_send(s->bus, b) && !ignore_ack) {
                i2c_end_transfer(s->bus);
                s->active = false;
                return false;
            }
        }
    } else {
        memset(s->data_out, 0, sizeof(s->data_out));
        for (unsigned i = 0; i < len; i++) {
            s->data_out[i / bpr] |= (uint32_t)i2c_recv(s->bus) <<
                                    (8 * (i % bpr));
        }
    }

    if (!FIELD_EX32(s->iic_enable, IIC_ENABLE, NOSTOP)) {
        i2c_end_transfer(s->bus);
        s->active = false;
    }
    return true;
}

static uint64_t brcmstb_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    BrcmstbI2CState *s = opaque;
    uint64_t val = 0;

    switch (addr) {
    case A_CHIP_ADDRESS:
        val = s->chip_address;
        break;
    case A_DATA_IN0 ... A_DATA_IN0 + 4 * (BRCMSTB_I2C_DATA_REGS - 1):
        val = s->data_in[(addr - A_DATA_IN0) / 4];
        break;
    case A_CNT:
        val = s->cnt;
        break;
    case A_CTL:
        val = s->ctl;
        break;
    case A_IIC_ENABLE:
        val = s->iic_enable;
        break;
    case A_DATA_OUT0 ... A_DATA_OUT0 + 4 * (BRCMSTB_I2C_DATA_REGS - 1):
        val = s->data_out[(addr - A_DATA_OUT0) / 4];
        break;
    case A_CTLHI:
        val = s->ctlhi;
        break;
    case A_SCL_PARAM:
        val = s->scl_param;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
    trace_brcmstb_i2c_read(addr, val);
    return val;
}

static void brcmstb_i2c_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    BrcmstbI2CState *s = opaque;

    trace_brcmstb_i2c_write(addr, value);
    switch (addr) {
    case A_CHIP_ADDRESS:
        s->chip_address = value & 0xff;
        break;
    case A_DATA_IN0 ... A_DATA_IN0 + 4 * (BRCMSTB_I2C_DATA_REGS - 1):
        s->data_in[(addr - A_DATA_IN0) / 4] = value;
        break;
    case A_CNT:
        s->cnt = value & (R_CNT_CNT1_MASK | R_CNT_CNT2_MASK);
        break;
    case A_CTL:
        s->ctl = value & 0xff;
        brcmstb_i2c_update_irq(s);
        break;
    case A_IIC_ENABLE:
        if (s->has_autoi2c &&
            !(s->autoi2c_ctrl0 & AUTOI2C_CTRL0_RELEASE_BSC)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: BSC not released by the "
                          "auto-I2C block\n", __func__);
        }
        /* writing 0 clears the status; INTRP and NOACK are read-only */
        s->iic_enable = value & (R_IIC_ENABLE_ENABLE_MASK |
                                 IIC_ENABLE_COND_MASK);
        if (value & R_IIC_ENABLE_ENABLE_MASK) {
            if (!brcmstb_i2c_command(s)) {
                s->iic_enable |= R_IIC_ENABLE_NOACK_MASK;
            }
            s->iic_enable |= R_IIC_ENABLE_INTRP_MASK;
        }
        brcmstb_i2c_update_irq(s);
        break;
    case A_DATA_OUT0 ... A_DATA_OUT0 + 4 * (BRCMSTB_I2C_DATA_REGS - 1):
        qemu_log_mask(LOG_GUEST_ERROR, "%s: read-only register 0x%"
                      HWADDR_PRIx "\n", __func__, addr);
        break;
    case A_CTLHI:
        s->ctlhi = value & (R_CTLHI_WAIT_DIS_MASK | R_CTLHI_IGNORE_ACK_MASK |
                            R_CTLHI_DATAREG_SIZE_MASK | 0x80);
        break;
    case A_SCL_PARAM:
        s->scl_param = value;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps brcmstb_i2c_ops = {
    .read = brcmstb_i2c_read,
    .write = brcmstb_i2c_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t brcmstb_autoi2c_read(void *opaque, hwaddr addr, unsigned size)
{
    BrcmstbI2CState *s = opaque;

    if (addr == AUTOI2C_CTRL0) {
        return s->autoi2c_ctrl0;
    }
    qemu_log_mask(LOG_UNIMP, "%s: read 0x%" HWADDR_PRIx "\n", __func__, addr);
    return 0;
}

static void brcmstb_autoi2c_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    BrcmstbI2CState *s = opaque;

    if (addr == AUTOI2C_CTRL0) {
        s->autoi2c_ctrl0 = value;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "%s: write 0x%" HWADDR_PRIx " 0x%" PRIx64 "\n",
                  __func__, addr, value);
}

static const MemoryRegionOps brcmstb_autoi2c_ops = {
    .read = brcmstb_autoi2c_read,
    .write = brcmstb_autoi2c_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void brcmstb_i2c_reset_hold(Object *obj, ResetType type)
{
    BrcmstbI2CState *s = BRCMSTB_I2C(obj);

    if (s->active) {
        i2c_end_transfer(s->bus);
        s->active = false;
    }
    s->chip_address = 0;
    memset(s->data_in, 0, sizeof(s->data_in));
    memset(s->data_out, 0, sizeof(s->data_out));
    s->cnt = 0;
    s->ctl = 0;
    s->iic_enable = 0;
    s->ctlhi = 0;
    s->scl_param = 0;
    s->autoi2c_ctrl0 = 0;
    brcmstb_i2c_update_irq(s);
}

static void brcmstb_i2c_realize(DeviceState *dev, Error **errp)
{
    BrcmstbI2CState *s = BRCMSTB_I2C(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &brcmstb_i2c_ops, s,
                          TYPE_BRCMSTB_I2C, BRCMSTB_I2C_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    if (s->has_autoi2c) {
        /* region 1: the BCM2711 HDMI auto-I2C block */
        memory_region_init_io(&s->autoi2c_iomem, OBJECT(dev),
                              &brcmstb_autoi2c_ops, s,
                              TYPE_BRCMSTB_I2C "-auto", AUTOI2C_SIZE);
        sysbus_init_mmio(sbd, &s->autoi2c_iomem);
    }
    sysbus_init_irq(sbd, &s->irq);
    s->bus = i2c_init_bus(dev, s->bus_name);
}

static const Property brcmstb_i2c_properties[] = {
    DEFINE_PROP_BOOL("auto-i2c", BrcmstbI2CState, has_autoi2c, false),
    /* the name of the I2C bus (-device ...,bus=NAME); default i2c-bus.N */
    DEFINE_PROP_STRING("bus-name", BrcmstbI2CState, bus_name),
};

static const VMStateDescription vmstate_brcmstb_i2c = {
    .name = TYPE_BRCMSTB_I2C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(chip_address, BrcmstbI2CState),
        VMSTATE_UINT32_ARRAY(data_in, BrcmstbI2CState, BRCMSTB_I2C_DATA_REGS),
        VMSTATE_UINT32(cnt, BrcmstbI2CState),
        VMSTATE_UINT32(ctl, BrcmstbI2CState),
        VMSTATE_UINT32(iic_enable, BrcmstbI2CState),
        VMSTATE_UINT32_ARRAY(data_out, BrcmstbI2CState,
                             BRCMSTB_I2C_DATA_REGS),
        VMSTATE_UINT32(ctlhi, BrcmstbI2CState),
        VMSTATE_UINT32(scl_param, BrcmstbI2CState),
        VMSTATE_BOOL(active, BrcmstbI2CState),
        VMSTATE_UINT32(autoi2c_ctrl0, BrcmstbI2CState),
        VMSTATE_END_OF_LIST()
    }
};

static void brcmstb_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = brcmstb_i2c_realize;
    rc->phases.hold = brcmstb_i2c_reset_hold;
    dc->vmsd = &vmstate_brcmstb_i2c;
    device_class_set_props(dc, brcmstb_i2c_properties);
    dc->desc = "Broadcom STB BSC I2C controller";
}

static const TypeInfo brcmstb_i2c_info = {
    .name = TYPE_BRCMSTB_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BrcmstbI2CState),
    .class_init = brcmstb_i2c_class_init,
};

static void brcmstb_i2c_register_types(void)
{
    type_register_static(&brcmstb_i2c_info);
}

type_init(brcmstb_i2c_register_types)
