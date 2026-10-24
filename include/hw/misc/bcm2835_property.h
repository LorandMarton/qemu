/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef BCM2835_PROPERTY_H
#define BCM2835_PROPERTY_H

#include "hw/core/sysbus.h"
#include "net/net.h"
#include "hw/display/bcm2835_fb.h"
#include "hw/nvram/bcm2835_otp.h"
#include "hw/misc/bcm2838_thermal.h"
#include "qom/object.h"

#define TYPE_BCM2835_PROPERTY "bcm2835-property"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835PropertyState, BCM2835_PROPERTY)

struct BCM2835PropertyState {
    /*< private >*/
    SysBusDevice busdev;
    /*< public >*/

    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    MemoryRegion iomem;
    qemu_irq mbox_irq;
    BCM2835FBState *fbdev;
    BCM2835OTPState *otp;
    Bcm2838ThermalState *thermal;   /* BCM2711 only */

    MACAddr macaddr;
    uint32_t board_rev;
    uint32_t num_displays; /* 0 or 1: the framebuffer, on HDMI0 */
    uint64_t board_serial;
    /* VideoCore firmware build: time (Unix), variant, version (git hash) */
    uint32_t fw_revision;
    uint32_t fw_variant;
    char *fw_version;
    uint32_t fw_hash[5];
    uint32_t throttled;
    uint32_t clk_rate[16]; /* BCM2711 firmware clocks, by clock ID */
    /* firmware-controlled GPIO expander (Linux gpio-raspberrypi-exp) */
    uint32_t exp_gpio_dir[8];
    uint32_t exp_gpio_pol[8];
    uint32_t exp_gpio_term_en[8];
    uint32_t exp_gpio_pull_up[8];
    uint32_t exp_gpio_state[8];
    uint32_t domain_on; /* firmware power domains, bit = RPI_POWER_DOMAIN_* */
    uint32_t addr;
    char *command_line;
    char *vc_config_file;
    char *vc_config; /* "name=value" lines for vcgencmd get_config */
    bool pending;
};

#endif
