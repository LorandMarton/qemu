/*
 * Raspberry Pi 4B emulation
 *
 * Copyright (C) 2022 Ovchinnikov Vitalii <vitalii.ovchinnikov@auriga.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/arm/raspi_platform.h"
#include "hw/display/bcm2835_fb.h"
#include "hw/core/registerfields.h"
#include "qemu/error-report.h"
#include "system/device_tree.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/arm/boot.h"
#include "qom/object.h"
#include "hw/arm/bcm2838.h"
#include "hw/sd/bcm4345.h"
#include "net/net.h"
#include "qemu/guest-random.h"
#include <libfdt.h>

#define TYPE_RASPI4B_MACHINE MACHINE_TYPE_NAME("raspi4b")
OBJECT_DECLARE_SIMPLE_TYPE(Raspi4bMachineState, RASPI4B_MACHINE)

struct Raspi4bMachineState {
    RaspiBaseMachineState parent_obj;
    BCM2838State soc;
};

/*
 * Add second memory region if board RAM amount exceeds VC base address
 * (see https://datasheets.raspberrypi.com/bcm2711/bcm2711-peripherals.pdf
 * 1.2 Address Map)
 */
static void raspi_add_memory_node(void *fdt, hwaddr mem_base, hwaddr mem_len)
{
    uint32_t acells, scells;
    char *nodename = g_strdup_printf("/memory@%" PRIx64, mem_base);

    acells = qemu_fdt_getprop_cell(fdt, "/", "#address-cells",
                                   NULL, &error_fatal);
    scells = qemu_fdt_getprop_cell(fdt, "/", "#size-cells",
                                   NULL, &error_fatal);
    /* validated by arm_load_dtb */
    g_assert(acells && scells);

    qemu_fdt_add_subnode(fdt, nodename);
    qemu_fdt_setprop_string(fdt, nodename, "device_type", "memory");
    qemu_fdt_setprop_sized_cells(fdt, nodename, "reg",
                                        acells, mem_base,
                                        scells, mem_len);

    g_free(nodename);
}

static void raspi4_modify_dtb(const struct arm_boot_info *info, void *fdt)
{
    uint64_t ram_size;

    ram_size = board_ram_size(info->board_id);

    if (ram_size > UPPER_RAM_BASE) {
        raspi_add_memory_node(fdt, UPPER_RAM_BASE, ram_size - UPPER_RAM_BASE);
    }
}

/*
 * Memory as the BCM2711 firmware describes it: one node, ARM low memory
 * below the VideoCore carve-out, the rest of the first 4 GiB up to the
 * peripherals, and everything above 4 GiB in 2 GiB ranges (#size-cells is 1).
 */
static void raspi4_fw_modify_dtb(const struct arm_boot_info *info, void *fdt)
{
    uint64_t ram_size = board_ram_size(info->board_id);
    uint64_t ranges[8][2];
    uint32_t acells, scells;
    g_autofree uint32_t *cells = NULL;
    int n = 0, c = 0;

    acells = qemu_fdt_getprop_cell(fdt, "/", "#address-cells",
                                   NULL, &error_fatal);
    scells = qemu_fdt_getprop_cell(fdt, "/", "#size-cells",
                                   NULL, &error_fatal);

    ranges[n][0] = 0;
    ranges[n++][1] = info->ram_size;
    ranges[n][0] = UPPER_RAM_BASE;
    ranges[n++][1] = MIN(ram_size, BCM2838_PERI_LOW_BASE) - UPPER_RAM_BASE;
    for (uint64_t base = 4 * GiB; base < ram_size && n < ARRAY_SIZE(ranges);
         base += 2 * GiB) {
        ranges[n][0] = base;
        ranges[n++][1] = MIN(ram_size - base, 2 * GiB);
    }

    cells = g_new0(uint32_t, n * (acells + scells));
    for (int i = 0; i < n; i++) {
        if (acells == 2) {
            cells[c++] = cpu_to_be32(ranges[i][0] >> 32);
        }
        cells[c++] = cpu_to_be32(ranges[i][0]);
        if (scells == 2) {
            cells[c++] = cpu_to_be32(ranges[i][1] >> 32);
        }
        cells[c++] = cpu_to_be32(ranges[i][1]);
    }
    qemu_fdt_setprop(fdt, "/memory@0", "reg", cells, c * sizeof(uint32_t));

    /*
     * The firmware seeds KASLR and the CRNG ("KASLR enabled",
     * "crng init done" at 0 s); Linux zeroes kaslr-seed and removes
     * rng-seed after use. rng-seed is refreshed on each reset.
     */
    {
        struct {
            uint64_t kaslr;
            uint8_t rng[32];
        } seed;

        if (!qemu_guest_getrandom(&seed, sizeof(seed), NULL)) {
            qemu_fdt_setprop_u64(fdt, "/chosen", "kaslr-seed", seed.kaslr);
            qemu_fdt_setprop(fdt, "/chosen", "rng-seed", seed.rng,
                             sizeof(seed.rng));
        }
    }

    /*
     * The firmware passes the tree without free space; Linux keeps
     * totalsize bytes of it reserved.
     */
    fdt_pack(fdt);
}

static void raspi4b_machine_init(MachineState *machine)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(machine);
    RaspiBaseMachineState *s_base = RASPI_BASE_MACHINE(machine);
    RaspiBaseMachineClass *mc = RASPI_BASE_MACHINE_GET_CLASS(machine);
    BCM2838State *soc = &s->soc;

    s_base->binfo.modify_dtb = raspi4_modify_dtb;
    s_base->binfo.board_id = mc->board_rev;

    object_initialize_child(OBJECT(machine), "soc", soc,
                            board_soc_type(mc->board_rev));

    raspi_base_machine_init(machine, &soc->parent_obj);
}

/*
 * Compute Module 4: as the 4B, with the RAM size of the board revision
 * and the 76 MiB VideoCore carve-out of the default firmware config.
 */
/*
 * GPIO state as the CM4 firmware leaves it and the board holds it ("pinctrl
 * get" on a CM4 on the CM4 IO Board): functions, pulls, and the
 * levels the module's own parts drive (BT UART on 30-33, RGMII on 46/47,
 * WiFi SDIO on 34-39, ACT LED on 42).
 */
static const char raspicm4_pins[] =
    "0:ip:pu:hi,1:ip:pu:hi,2:ip:pu:hi,3:ip:pu:hi,4:a4:pn:hi,5:a4:pu:hi,"
    "6:a4:pu:hi,7:a4:pn:hi,8:ip:pu:hi,9:ip:pd:lo,10:ip:pd:lo,11:ip:pd:lo,"
    "12:ip:pd:lo,13:ip:pd:lo,14:a5:pn:hi,15:a5:pu:hi,16:ip:pd:lo,17:ip:pd:lo,"
    "18:ip:pd:lo,19:ip:pd:lo,20:ip:pd:lo,21:ip:pd:lo,22:ip:pd:lo,23:ip:pd:lo,"
    "24:ip:pd:lo,25:ip:pd:lo,26:ip:pn:hi,27:ip:pd:lo,28:a5:pu:hi,29:a5:pd:lo,"
    "30:ip:pu:hi,31:ip:pn:hi,32:ip:pd:hi,33:ip:pd:hi,34:a3:pn:lo,35:a3:pu:hi,"
    "36:a3:pu:hi,37:a3:pu:hi,38:a3:pu:hi,39:a3:pu:hi,40:ip:pn:hi,41:ip:pn:lo,"
    "42:op:pn:lo,43:ip:pn:hi,44:a1:pn:hi,45:a1:pn:hi,46:ip:pu:lo,47:ip:pu:lo,"
    "48:a0:pd:lo,49:a0:pd:lo,50:a0:pd:lo,51:a0:pd:lo,52:a0:pd:lo,53:a0:pd:lo,"
    "54:ip:pd:lo,55:ip:pd:lo,56:ip:pd:lo,57:ip:pd:lo";

static void raspicm4_machine_init(MachineState *machine)
{
    Raspi4bMachineState *s = RASPI4B_MACHINE(machine);
    RaspiBaseMachineState *s_base = RASPI_BASE_MACHINE(machine);
    RaspiBaseMachineClass *mc = RASPI_BASE_MACHINE_GET_CLASS(machine);
    BCM2838State *soc = &s->soc;
    g_autofree char *user_pins = NULL, *pins = NULL;
    DeviceState *wlan;
    Object *gpio;

    s_base->binfo.modify_dtb = raspi4_fw_modify_dtb;
    s_base->binfo.dtb_below_initrd = true;
    s_base->binfo.board_id = mc->board_rev;

    object_initialize_child(OBJECT(machine), "soc", soc,
                            board_soc_type(mc->board_rev));
    object_property_set_uint(OBJECT(soc), "vcram-size", 76 * MiB,
                             &error_abort);
    /* The board's pins, then the user's: later entries win */
    gpio = OBJECT(&soc->peripherals.gpio);
    user_pins = object_property_get_str(gpio, "pins", &error_abort);
    pins = g_strjoin(",", raspicm4_pins, user_pins, NULL);
    object_property_set_str(gpio, "pins", pins, &error_abort);

    raspi_base_machine_init(machine, &soc->parent_obj);

    /* The wireless chip, on SD1 (pins 34-39), its radio a NIC */
    wlan = qdev_new(TYPE_BCM4345);
    object_property_add_child(OBJECT(machine), "wlan", OBJECT(wlan));
    qemu_configure_nic_device(wlan, false, NULL);
    qdev_realize_and_unref(wlan, BUS(&soc->peripherals.parent_obj.sdhci.sdbus),
                           &error_fatal);
}

/* No monitor on the module: the firmware reports no display */
static GlobalProperty raspicm4_compat_defaults[] = {
    { TYPE_BCM2835_PROPERTY, "num-displays", "0" },
};

static void raspicm4_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    RaspiBaseMachineClass *rmc = RASPI_BASE_MACHINE_CLASS(oc);

    rmc->board_rev = 0xd03140; /* Revision 1.0, 8 GiB RAM */
    raspi_machine_class_common_init(mc, rmc->board_rev);
    mc->auto_create_sdcard = true;
    mc->init = raspicm4_machine_init;
    compat_props_add(mc->compat_props, raspicm4_compat_defaults,
                     G_N_ELEMENTS(raspicm4_compat_defaults));
}

static void raspi4b_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    RaspiBaseMachineClass *rmc = RASPI_BASE_MACHINE_CLASS(oc);

    rmc->board_rev = 0xb03115; /* Revision 1.5, 2 Gb RAM */
    raspi_machine_class_common_init(mc, rmc->board_rev);
    mc->auto_create_sdcard = true;
    mc->init = raspi4b_machine_init;
}

static const TypeInfo raspi4b_machine_type = {
    .name           = TYPE_RASPI4B_MACHINE,
    .parent         = TYPE_RASPI_BASE_MACHINE,
    .instance_size  = sizeof(Raspi4bMachineState),
    .class_init     = raspi4b_machine_class_init,
    .is_available   = target_aarch64,
};

static const TypeInfo raspicm4_machine_type = {
    .name           = MACHINE_TYPE_NAME("raspicm4"),
    .parent         = TYPE_RASPI4B_MACHINE,
    .class_init     = raspicm4_machine_class_init,
    .is_available   = target_aarch64,
};

static void raspi4b_machine_register_type(void)
{
    type_register_static(&raspi4b_machine_type);
    type_register_static(&raspicm4_machine_type);
}

type_init(raspi4b_machine_register_type)
