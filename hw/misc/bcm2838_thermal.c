/*
 * BCM2838 dummy thermal sensor
 *
 * Copyright (C) 2022 Maksim Kopusov <maksim.kopusov@auriga.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/misc/bcm2838_thermal.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "qemu/error-report.h"

REG32(STAT, 0x200)
FIELD(STAT, DATA, 0, 10)
FIELD(STAT, VALID_1, 10, 1)
FIELD(STAT, VALID_2, 16, 1)

#define BCM2838_THERMAL_SIZE 0xf00

/* temperature = THERMAL_OFFSET_MC - THERMAL_SLOPE_MC * code, millidegrees */
#define THERMAL_OFFSET_MC   410040
#define THERMAL_SLOPE_MC    487
#define THERMAL_CODE_MAX    0x3ff
#define THERMAL_DEFAULT_MC  25000

/* The nearest code; the temperature is within what the 10 bits can tell */
uint16_t bcm2838_thermal_get_code(Bcm2838ThermalState *s)
{
    int code = (THERMAL_OFFSET_MC - s->temperature + THERMAL_SLOPE_MC / 2) /
               THERMAL_SLOPE_MC;

    return MIN(code, THERMAL_CODE_MAX);
}

static uint64_t bcm2838_thermal_read(void *opaque, hwaddr addr, unsigned size)
{
    Bcm2838ThermalState *s = opaque;
    uint32_t val = 0;

    switch (addr) {
    case A_STAT:
        val = FIELD_DP32(val, STAT, DATA, bcm2838_thermal_get_code(s));
        val = FIELD_DP32(val, STAT, VALID_1, 1);
        val = FIELD_DP32(val, STAT, VALID_2, 1);

        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s can't access addr: 0x%"HWADDR_PRIx,
                     TYPE_BCM2838_THERMAL, addr);
    }
    return val;
}

static void bcm2838_thermal_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: write 0x%" PRIx64
                                " to 0x%" HWADDR_PRIx "\n",
                __func__, value, addr);
}

static const MemoryRegionOps bcm2838_thermal_ops = {
    .read = bcm2838_thermal_read,
    .write = bcm2838_thermal_write,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* "temperature": millidegrees Celsius, as for the other sensors */
static void bcm2838_thermal_get_temperature(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    int64_t value = BCM2838_THERMAL(obj)->temperature;

    visit_type_int(v, name, &value, errp);
}

static void bcm2838_thermal_set_temperature(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    int64_t value;

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }
    if (value < THERMAL_OFFSET_MC - THERMAL_SLOPE_MC * THERMAL_CODE_MAX ||
        value > THERMAL_OFFSET_MC) {
        error_setg(errp, "%s: %" PRId64 " is out of the sensor range %d..%d",
                   name, value,
                   THERMAL_OFFSET_MC - THERMAL_SLOPE_MC * THERMAL_CODE_MAX,
                   THERMAL_OFFSET_MC);
        return;
    }
    BCM2838_THERMAL(obj)->temperature = value;
}

static void bcm2838_thermal_init(Object *obj)
{
    BCM2838_THERMAL(obj)->temperature = THERMAL_DEFAULT_MC;
    object_property_add(obj, "temperature", "int",
                        bcm2838_thermal_get_temperature,
                        bcm2838_thermal_set_temperature, NULL, NULL);
    object_property_set_description(obj, "temperature",
                                    "die temperature in millidegrees Celsius");
}

static void bcm2838_thermal_realize(DeviceState *dev, Error **errp)
{
    Bcm2838ThermalState *s = BCM2838_THERMAL(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &bcm2838_thermal_ops,
                          s, TYPE_BCM2838_THERMAL, BCM2838_THERMAL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

/* The temperature is the environment's: kept across a reset, migrated */
static const VMStateDescription vmstate_bcm2838_thermal = {
    .name = TYPE_BCM2838_THERMAL,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(temperature, Bcm2838ThermalState),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2838_thermal_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bcm2838_thermal_realize;
    dc->vmsd = &vmstate_bcm2838_thermal;
}

static const TypeInfo bcm2838_thermal_info = {
    .name = TYPE_BCM2838_THERMAL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Bcm2838ThermalState),
    .instance_init = bcm2838_thermal_init,
    .class_init = bcm2838_thermal_class_init,
};

static void bcm2838_thermal_register_types(void)
{
    type_register_static(&bcm2838_thermal_info);
}

type_init(bcm2838_thermal_register_types)
