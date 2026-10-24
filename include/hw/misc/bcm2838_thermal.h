/*
 * BCM2838 dummy thermal sensor
 *
 * Copyright (C) 2022 Maksim Kopusov <maksim.kopusov@auriga.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef BCM2838_THERMAL_H
#define BCM2838_THERMAL_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BCM2838_THERMAL "bcm2838-thermal"
OBJECT_DECLARE_SIMPLE_TYPE(Bcm2838ThermalState, BCM2838_THERMAL)

struct Bcm2838ThermalState {
    SysBusDevice busdev;
    MemoryRegion iomem;
    int32_t temperature;    /* die temperature, millidegrees Celsius */
};

/* The sensor's 10-bit code for the current temperature */
uint16_t bcm2838_thermal_get_code(Bcm2838ThermalState *s);

#endif /* BCM2838_THERMAL_H */
