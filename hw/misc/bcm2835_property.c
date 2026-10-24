/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/misc/bcm2835_property.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "hw/core/irq.h"
#include "hw/misc/bcm2835_mbox_defs.h"
#include "hw/arm/raspberrypi-fw-defs.h"
#include "system/dma.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"
#include "hw/arm/raspi_platform.h"

#define VCHI_BUSADDR_SIZE       sizeof(uint32_t)

/*
 * Firmware clocks of the BCM2711 as listed by GET_CLOCKS, with the boot
 * rate and the limits the firmware reports (arm_freq=1500, core_freq=500,
 * gpu_freq=500 defaults); from a CM4's /sys/kernel/debug/clk/fw-clk-*.
 */
static const struct {
    uint32_t id, rate, min, max;
} bcm2711_fw_clocks[] = {
    { RPI_FIRMWARE_ARM_CLK_ID,       1500000000, 600000000, 1500000000 },
    { RPI_FIRMWARE_CORE_CLK_ID,       500000000, 200000000,  500000000 },
    { RPI_FIRMWARE_V3D_CLK_ID,        250000000, 250000000,  500000000 },
    { RPI_FIRMWARE_ISP_CLK_ID,        250000000, 250000000,  500000000 },
    { RPI_FIRMWARE_PIXEL_CLK_ID,              0,         0, 2400000000 },
    { RPI_FIRMWARE_HEVC_CLK_ID,       250000000, 250000000,  500000000 },
    { RPI_FIRMWARE_M2MC_CLK_ID,       120000000, 120000000,  600000000 },
    { RPI_FIRMWARE_PIXEL_BVB_CLK_ID,   75000000,  75000000,  324000000 },
    { RPI_FIRMWARE_VEC_CLK_ID,                0,         0,  108000000 },
};

/* Whether the firmware configuration (vc-config) has the line @line */
static bool bcm2835_property_config_has(BCM2835PropertyState *s,
                                        const char *line)
{
    g_auto(GStrv) lines = s->vc_config ?
                          g_strsplit(s->vc_config, "\n", -1) : NULL;

    return lines && g_strv_contains((const char * const *)lines, line);
}

static int bcm2711_fw_clock(BCM2835PropertyState *s, uint32_t id)
{
    /* PROCESSOR field of the revision code: 3 is the BCM2711 */
    if (((s->board_rev >> 12) & 0xf) != 3) {
        return -1;
    }
    for (int i = 0; i < ARRAY_SIZE(bcm2711_fw_clocks); i++) {
        if (bcm2711_fw_clocks[i].id == id) {
            return i;
        }
    }
    return -1;
}

/* https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface */

static void bcm2835_property_mbox_push(BCM2835PropertyState *s, uint32_t value)
{
    uint32_t tot_len;

    /*
     * Copy the current state of the framebuffer config; we will update
     * this copy as we process tags and then ask the framebuffer to use
     * it at the end.
     */
    BCM2835FBConfig fbconfig = s->fbdev->config;
    bool fbconfig_updated = false;

    value &= ~0xf;

    s->addr = value;

    tot_len = ldl_le_phys(&s->dma_as, value);

    /* @(addr + 4) : Buffer response code */
    value = s->addr + 8;
    while (value + 8 <= s->addr + tot_len) {
        uint32_t tag = ldl_le_phys(&s->dma_as, value);
        uint32_t bufsize = ldl_le_phys(&s->dma_as, value + 4);
        /* @(value + 8) : Request/response indicator */
        size_t resplen = 0;
        switch (tag) {
        case RPI_FWREQ_PROPERTY_END:
            break;
        case RPI_FWREQ_GET_FIRMWARE_REVISION:
            stl_le_phys(&s->dma_as, value + 12, s->fw_revision);
            resplen = 4;
            break;
        case RPI_FWREQ_GET_FIRMWARE_VARIANT:
            stl_le_phys(&s->dma_as, value + 12, s->fw_variant);
            resplen = 4;
            break;
        case RPI_FWREQ_GET_FIRMWARE_HASH:
            for (int i = 0; i < ARRAY_SIZE(s->fw_hash); i++) {
                stl_le_phys(&s->dma_as, value + 12 + 4 * i, s->fw_hash[i]);
            }
            resplen = sizeof(s->fw_hash);
            break;
        case RPI_FWREQ_GET_BOARD_MODEL:
            qemu_log_mask(LOG_UNIMP,
                          "bcm2835_property: 0x%08x get board model NYI\n",
                          tag);
            resplen = 4;
            break;
        case RPI_FWREQ_GET_BOARD_REVISION:
            stl_le_phys(&s->dma_as, value + 12, s->board_rev);
            resplen = 4;
            break;
        case RPI_FWREQ_GET_BOARD_MAC_ADDRESS:
            resplen = sizeof(s->macaddr.a);
            dma_memory_write(&s->dma_as, value + 12, s->macaddr.a, resplen,
                             MEMTXATTRS_UNSPECIFIED);
            break;
        case RPI_FWREQ_GET_BOARD_SERIAL:
            stq_le_phys(&s->dma_as, value + 12, s->board_serial);
            resplen = 8;
            break;
        case RPI_FWREQ_GET_ARM_MEMORY:
            /* base */
            stl_le_phys(&s->dma_as, value + 12, 0);
            /* size */
            stl_le_phys(&s->dma_as, value + 16, s->fbdev->vcram_base);
            resplen = 8;
            break;
        case RPI_FWREQ_GET_VC_MEMORY:
            /* base */
            stl_le_phys(&s->dma_as, value + 12, s->fbdev->vcram_base);
            /* size */
            stl_le_phys(&s->dma_as, value + 16, s->fbdev->vcram_size);
            resplen = 8;
            break;
        case RPI_FWREQ_SET_POWER_STATE:
        {
            /*
             * Assume that whatever device they asked for exists,
             * and we'll just claim we set it to the desired state.
             */
            uint32_t state = ldl_le_phys(&s->dma_as, value + 16);
            stl_le_phys(&s->dma_as, value + 16, (state & 1));
            resplen = 8;
            break;
        }

        /* Clocks */

        case RPI_FWREQ_GET_CLOCK_STATE:
            stl_le_phys(&s->dma_as, value + 16, 0x1);
            resplen = 8;
            break;

        case RPI_FWREQ_SET_CLOCK_STATE:
            qemu_log_mask(LOG_UNIMP,
                          "bcm2835_property: 0x%08x set clock state NYI\n",
                          tag);
            resplen = 8;
            break;

        case RPI_FWREQ_GET_CLOCK_RATE:
        case RPI_FWREQ_GET_MAX_CLOCK_RATE:
        case RPI_FWREQ_GET_MIN_CLOCK_RATE:
        {
            uint32_t id = ldl_le_phys(&s->dma_as, value + 12);
            int fw = bcm2711_fw_clock(s, id);

            if (fw >= 0) {
                stl_le_phys(&s->dma_as, value + 16,
                            tag == RPI_FWREQ_GET_MIN_CLOCK_RATE ?
                            bcm2711_fw_clocks[fw].min :
                            tag == RPI_FWREQ_GET_MAX_CLOCK_RATE ?
                            bcm2711_fw_clocks[fw].max : s->clk_rate[id]);
                resplen = 8;
                break;
            }
            switch (id) {
            case RPI_FIRMWARE_EMMC_CLK_ID:
                stl_le_phys(&s->dma_as, value + 16, RPI_FIRMWARE_EMMC_CLK_RATE);
                break;
            case RPI_FIRMWARE_UART_CLK_ID:
                stl_le_phys(&s->dma_as, value + 16, RPI_FIRMWARE_UART_CLK_RATE);
                break;
            case RPI_FIRMWARE_CORE_CLK_ID:
                stl_le_phys(&s->dma_as, value + 16, RPI_FIRMWARE_CORE_CLK_RATE);
                break;
            default:
                stl_le_phys(&s->dma_as, value + 16,
                            RPI_FIRMWARE_DEFAULT_CLK_RATE);
                break;
            }
            resplen = 8;
            break;
        }

        case RPI_FWREQ_GET_CLOCKS:
            if (bcm2711_fw_clock(s, RPI_FIRMWARE_ARM_CLK_ID) >= 0) {
                /* (parent, id) pairs, zero-terminated by the guest buffer */
                for (int i = 0; i < ARRAY_SIZE(bcm2711_fw_clocks) &&
                     8 * (i + 1) <= bufsize; i++) {
                    stl_le_phys(&s->dma_as, value + 12 + 8 * i, 0);
                    stl_le_phys(&s->dma_as, value + 16 + 8 * i,
                                bcm2711_fw_clocks[i].id);
                    resplen = 8 * (i + 1);
                }
                break;
            }
            /* TODO: add more clock IDs if needed */
            stl_le_phys(&s->dma_as, value + 12, 0);
            stl_le_phys(&s->dma_as, value + 16, RPI_FIRMWARE_ARM_CLK_ID);
            resplen = 8;
            break;

        case RPI_FWREQ_SET_CLOCK_RATE:
        {
            uint32_t id = ldl_le_phys(&s->dma_as, value + 12);
            int fw = bcm2711_fw_clock(s, id);

            if (fw >= 0) {
                /*
                 * Rate only: guest-visible, no effect on emulation speed.
                 * With enable_uart=1 the firmware keeps the core clock, which
                 * the mini UART's baud rate derives from, at its boot rate.
                 */
                if (id != RPI_FIRMWARE_CORE_CLK_ID ||
                    !bcm2835_property_config_has(s, "enable_uart=1")) {
                    s->clk_rate[id] =
                        MIN(MAX(ldl_le_phys(&s->dma_as, value + 16),
                                bcm2711_fw_clocks[fw].min),
                            bcm2711_fw_clocks[fw].max);
                }
                stl_le_phys(&s->dma_as, value + 16, s->clk_rate[id]);
                resplen = 8;
                break;
            }
        }
            /* fall through */
        case RPI_FWREQ_SET_MAX_CLOCK_RATE:
        case RPI_FWREQ_SET_MIN_CLOCK_RATE:
            qemu_log_mask(LOG_UNIMP,
                          "bcm2835_property: 0x%08x set clock rate NYI\n",
                          tag);
            resplen = 8;
            break;

        /* Temperature */

        case RPI_FWREQ_GET_TEMPERATURE:
            stl_le_phys(&s->dma_as, value + 16, 25000);
            resplen = 8;
            break;

        case RPI_FWREQ_GET_MAX_TEMPERATURE:
            stl_le_phys(&s->dma_as, value + 16, 99000);
            resplen = 8;
            break;

        /* Frame buffer */

        case RPI_FWREQ_FRAMEBUFFER_ALLOCATE:
            stl_le_phys(&s->dma_as, value + 12, fbconfig.base);
            stl_le_phys(&s->dma_as, value + 16,
                        bcm2835_fb_get_size(&fbconfig));
            resplen = 8;
            break;
        case RPI_FWREQ_FRAMEBUFFER_RELEASE:
            resplen = 0;
            break;
        case RPI_FWREQ_FRAMEBUFFER_BLANK:
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_PHYSICAL_WIDTH_HEIGHT:
        case RPI_FWREQ_FRAMEBUFFER_TEST_VIRTUAL_WIDTH_HEIGHT:
            resplen = 8;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_PHYSICAL_WIDTH_HEIGHT:
            fbconfig.xres = ldl_le_phys(&s->dma_as, value + 12);
            fbconfig.yres = ldl_le_phys(&s->dma_as, value + 16);
            bcm2835_fb_validate_config(&fbconfig);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_PHYSICAL_WIDTH_HEIGHT:
            stl_le_phys(&s->dma_as, value + 12, fbconfig.xres);
            stl_le_phys(&s->dma_as, value + 16, fbconfig.yres);
            resplen = 8;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_WIDTH_HEIGHT:
            fbconfig.xres_virtual = ldl_le_phys(&s->dma_as, value + 12);
            fbconfig.yres_virtual = ldl_le_phys(&s->dma_as, value + 16);
            bcm2835_fb_validate_config(&fbconfig);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_VIRTUAL_WIDTH_HEIGHT:
            stl_le_phys(&s->dma_as, value + 12, fbconfig.xres_virtual);
            stl_le_phys(&s->dma_as, value + 16, fbconfig.yres_virtual);
            resplen = 8;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_DEPTH:
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_DEPTH:
            fbconfig.bpp = ldl_le_phys(&s->dma_as, value + 12);
            bcm2835_fb_validate_config(&fbconfig);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_DEPTH:
            stl_le_phys(&s->dma_as, value + 12, fbconfig.bpp);
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_PIXEL_ORDER:
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_PIXEL_ORDER:
            fbconfig.pixo = ldl_le_phys(&s->dma_as, value + 12);
            bcm2835_fb_validate_config(&fbconfig);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_PIXEL_ORDER:
            stl_le_phys(&s->dma_as, value + 12, fbconfig.pixo);
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_ALPHA_MODE:
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_ALPHA_MODE:
            fbconfig.alpha = ldl_le_phys(&s->dma_as, value + 12);
            bcm2835_fb_validate_config(&fbconfig);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_ALPHA_MODE:
            stl_le_phys(&s->dma_as, value + 12, fbconfig.alpha);
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_GET_PITCH:
            stl_le_phys(&s->dma_as, value + 12,
                        bcm2835_fb_get_pitch(&fbconfig));
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_VIRTUAL_OFFSET:
            resplen = 8;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_OFFSET:
            fbconfig.xoffset = ldl_le_phys(&s->dma_as, value + 12);
            fbconfig.yoffset = ldl_le_phys(&s->dma_as, value + 16);
            bcm2835_fb_validate_config(&fbconfig);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_VIRTUAL_OFFSET:
            stl_le_phys(&s->dma_as, value + 12, fbconfig.xoffset);
            stl_le_phys(&s->dma_as, value + 16, fbconfig.yoffset);
            resplen = 8;
            break;
        case RPI_FWREQ_FRAMEBUFFER_GET_OVERSCAN:
        case RPI_FWREQ_FRAMEBUFFER_TEST_OVERSCAN:
        case RPI_FWREQ_FRAMEBUFFER_SET_OVERSCAN:
            stl_le_phys(&s->dma_as, value + 12, 0);
            stl_le_phys(&s->dma_as, value + 16, 0);
            stl_le_phys(&s->dma_as, value + 20, 0);
            stl_le_phys(&s->dma_as, value + 24, 0);
            resplen = 16;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_PALETTE:
        {
            uint32_t offset = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t length = ldl_le_phys(&s->dma_as, value + 16);
            int resp;

            if (offset > 255 || length < 1 || length > 256) {
                resp = 1; /* invalid request */
            } else {
                for (uint32_t e = 0; e < length; e++) {
                    uint32_t color = ldl_le_phys(&s->dma_as, value + 20 + (e << 2));
                    stl_le_phys(&s->dma_as,
                                s->fbdev->vcram_base + ((offset + e) << 2), color);
                }
                resp = 0;
            }
            stl_le_phys(&s->dma_as, value + 12, resp);
            resplen = 4;
            break;
        }
        case RPI_FWREQ_FRAMEBUFFER_GET_NUM_DISPLAYS:
            stl_le_phys(&s->dma_as, value + 12, 1);
            resplen = 4;
            break;

        case RPI_FWREQ_GET_DMA_CHANNELS:
            /* channels 2-5 */
            stl_le_phys(&s->dma_as, value + 12, 0x003C);
            resplen = 4;
            break;

        case RPI_FWREQ_GET_COMMAND_LINE:
            /*
             * We follow the firmware behaviour: no NUL terminator is
             * written to the buffer, and if the buffer is too short
             * we report the required length in the response header
             * and copy nothing to the buffer.
             */
            resplen = strlen(s->command_line);
            if (bufsize >= resplen)
                address_space_write(&s->dma_as, value + 12,
                                    MEMTXATTRS_UNSPECIFIED, s->command_line,
                                    resplen);
            break;

        case RPI_FWREQ_GET_THROTTLED:
            stl_le_phys(&s->dma_as, value + 12, 0);
            resplen = 4;
            break;

        /*
         * Expander GPIOs 128..135 (BT_ON, WL_ON, PWR_LED_OFF, ANT1,
         * VDD_SD_IO_SEL, CAM_GPIO, SD_PWR_ON, ANT2 on the CM4). The first
         * word returns 0 on success, the GPIO number is rejected otherwise.
         */
        case RPI_FWREQ_GET_GPIO_CONFIG:
        case RPI_FWREQ_SET_GPIO_CONFIG:
        case RPI_FWREQ_GET_GPIO_STATE:
        case RPI_FWREQ_SET_GPIO_STATE:
        {
            uint32_t gpio = ldl_le_phys(&s->dma_as, value + 12) - 128;

            resplen = tag == RPI_FWREQ_GET_GPIO_CONFIG ? 20 :
                      tag == RPI_FWREQ_SET_GPIO_CONFIG ? 24 : 8;
            if (gpio >= ARRAY_SIZE(s->exp_gpio_state)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "bcm2835_property: bad expander GPIO %u\n",
                              gpio + 128);
                break;
            }
            switch (tag) {
            case RPI_FWREQ_GET_GPIO_CONFIG:
                stl_le_phys(&s->dma_as, value + 16, s->exp_gpio_dir[gpio]);
                stl_le_phys(&s->dma_as, value + 20, s->exp_gpio_pol[gpio]);
                stl_le_phys(&s->dma_as, value + 24,
                            s->exp_gpio_term_en[gpio]);
                stl_le_phys(&s->dma_as, value + 28,
                            s->exp_gpio_pull_up[gpio]);
                break;
            case RPI_FWREQ_SET_GPIO_CONFIG:
                s->exp_gpio_dir[gpio] = ldl_le_phys(&s->dma_as, value + 16);
                s->exp_gpio_pol[gpio] = ldl_le_phys(&s->dma_as, value + 20);
                s->exp_gpio_term_en[gpio] =
                    ldl_le_phys(&s->dma_as, value + 24);
                s->exp_gpio_pull_up[gpio] =
                    ldl_le_phys(&s->dma_as, value + 28);
                if (s->exp_gpio_dir[gpio]) {
                    s->exp_gpio_state[gpio] =
                        !!ldl_le_phys(&s->dma_as, value + 32);
                }
                break;
            case RPI_FWREQ_GET_GPIO_STATE:
                stl_le_phys(&s->dma_as, value + 16, s->exp_gpio_state[gpio]);
                break;
            case RPI_FWREQ_SET_GPIO_STATE:
                s->exp_gpio_state[gpio] = !!ldl_le_phys(&s->dma_as, value + 16);
                break;
            }
            stl_le_phys(&s->dma_as, value + 12, 0);
            break;
        }

        case RPI_FWREQ_VCHIQ_INIT:
            stl_le_phys(&s->dma_as,
                        value + offsetof(rpi_firmware_prop_request_t, payload),
                        0);
            resplen = VCHI_BUSADDR_SIZE;
            break;

        /* Customer OTP */

        case RPI_FWREQ_GET_CUSTOMER_OTP:
        {
            uint32_t start_num = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t number = ldl_le_phys(&s->dma_as, value + 16);

            resplen = 8 + 4 * number;

            for (uint32_t n = start_num; n < start_num + number &&
                 n < BCM2835_OTP_CUSTOMER_OTP_LEN; n++) {
                uint32_t otp_row = bcm2835_otp_get_row(s->otp,
                                              BCM2835_OTP_CUSTOMER_OTP + n);
                stl_le_phys(&s->dma_as,
                            value + 20 + ((n - start_num) << 2), otp_row);
            }
            break;
        }
        case RPI_FWREQ_SET_CUSTOMER_OTP:
        {
            uint32_t start_num = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t number = ldl_le_phys(&s->dma_as, value + 16);

            resplen = 4;

            /* Magic numbers to permanently lock customer OTP */
            if (start_num == BCM2835_OTP_LOCK_NUM1 &&
                number == BCM2835_OTP_LOCK_NUM2) {
                bcm2835_otp_set_row(s->otp,
                                    BCM2835_OTP_ROW_32,
                                    BCM2835_OTP_ROW_32_LOCK);
                break;
            }

            /* If row 32 has the lock bit, don't allow further writes */
            if (bcm2835_otp_get_row(s->otp, BCM2835_OTP_ROW_32) &
                                    BCM2835_OTP_ROW_32_LOCK) {
                break;
            }

            for (uint32_t n = start_num; n < start_num + number &&
                 n < BCM2835_OTP_CUSTOMER_OTP_LEN; n++) {
                uint32_t otp_row = ldl_le_phys(&s->dma_as,
                                      value + 20 + ((n - start_num) << 2));
                bcm2835_otp_set_row(s->otp,
                                    BCM2835_OTP_CUSTOMER_OTP + n, otp_row);
            }
            break;
        }

        /* Device-specific private key */
        case RPI_FWREQ_GET_PRIVATE_KEY:
        {
            uint32_t start_num = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t number = ldl_le_phys(&s->dma_as, value + 16);

            resplen = 8 + 4 * number;

            for (uint32_t n = start_num; n < start_num + number &&
                 n < BCM2835_OTP_PRIVATE_KEY_LEN; n++) {
                uint32_t otp_row = bcm2835_otp_get_row(s->otp,
                                              BCM2835_OTP_PRIVATE_KEY + n);
                stl_le_phys(&s->dma_as,
                            value + 20 + ((n - start_num) << 2), otp_row);
            }
            break;
        }
        case RPI_FWREQ_SET_PRIVATE_KEY:
        {
            uint32_t start_num = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t number = ldl_le_phys(&s->dma_as, value + 16);

            resplen = 4;

            /* If row 32 has the lock bit, don't allow further writes */
            if (bcm2835_otp_get_row(s->otp, BCM2835_OTP_ROW_32) &
                                    BCM2835_OTP_ROW_32_LOCK) {
                break;
            }

            for (uint32_t n = start_num; n < start_num + number &&
                 n < BCM2835_OTP_PRIVATE_KEY_LEN; n++) {
                uint32_t otp_row = ldl_le_phys(&s->dma_as,
                                      value + 20 + ((n - start_num) << 2));
                bcm2835_otp_set_row(s->otp,
                                    BCM2835_OTP_PRIVATE_KEY + n, otp_row);
            }
            break;
        }
        default:
            qemu_log_mask(LOG_UNIMP,
                          "bcm2835_property: unhandled tag 0x%08x\n", tag);
            break;
        }

        trace_bcm2835_mbox_property(tag, bufsize, resplen);
        if (tag == 0) {
            break;
        }

        stl_le_phys(&s->dma_as, value + 8, (1 << 31) | resplen);
        value += bufsize + 12;
    }

    /* Reconfigure framebuffer if required */
    if (fbconfig_updated) {
        bcm2835_fb_reconfigure(s->fbdev, &fbconfig);
    }

    /* Buffer response code */
    stl_le_phys(&s->dma_as, s->addr + 4, (1 << 31));
}

static uint64_t bcm2835_property_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    BCM2835PropertyState *s = opaque;
    uint32_t res = 0;

    switch (offset) {
    case MBOX_AS_DATA:
        res = MBOX_CHAN_PROPERTY | s->addr;
        s->pending = false;
        qemu_set_irq(s->mbox_irq, 0);
        break;

    case MBOX_AS_PENDING:
        res = s->pending;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        return 0;
    }

    return res;
}

static void bcm2835_property_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    BCM2835PropertyState *s = opaque;

    switch (offset) {
    case MBOX_AS_DATA:
        /* bcm2835_mbox should check our pending status before pushing */
        assert(!s->pending);
        s->pending = true;
        bcm2835_property_mbox_push(s, value);
        qemu_set_irq(s->mbox_irq, 1);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        return;
    }
}

static const MemoryRegionOps bcm2835_property_ops = {
    .read = bcm2835_property_read,
    .write = bcm2835_property_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const VMStateDescription vmstate_bcm2835_property = {
    .name = TYPE_BCM2835_PROPERTY,
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_MACADDR(macaddr, BCM2835PropertyState),
        VMSTATE_UINT32(addr, BCM2835PropertyState),
        VMSTATE_BOOL(pending, BCM2835PropertyState),
        VMSTATE_UINT32_ARRAY(clk_rate, BCM2835PropertyState, 16),
        VMSTATE_UINT32_ARRAY(exp_gpio_dir, BCM2835PropertyState, 8),
        VMSTATE_UINT32_ARRAY(exp_gpio_pol, BCM2835PropertyState, 8),
        VMSTATE_UINT32_ARRAY(exp_gpio_term_en, BCM2835PropertyState, 8),
        VMSTATE_UINT32_ARRAY(exp_gpio_pull_up, BCM2835PropertyState, 8),
        VMSTATE_UINT32_ARRAY(exp_gpio_state, BCM2835PropertyState, 8),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2835_property_init(Object *obj)
{
    BCM2835PropertyState *s = BCM2835_PROPERTY(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &bcm2835_property_ops, s,
                          TYPE_BCM2835_PROPERTY, 0x10);

    /*
     * bcm2835_property_ops call into bcm2835_mbox, which in-turn reads from
     * iomem. As such, mark iomem as re-entracy safe.
     */
    s->iomem.disable_reentrancy_guard = true;

    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->mbox_irq);
}

static void bcm2835_property_reset(DeviceState *dev)
{
    BCM2835PropertyState *s = BCM2835_PROPERTY(dev);

    s->pending = false;
    for (int i = 0; i < ARRAY_SIZE(bcm2711_fw_clocks); i++) {
        s->clk_rate[bcm2711_fw_clocks[i].id] = bcm2711_fw_clocks[i].rate;
    }
}

static void bcm2835_property_realize(DeviceState *dev, Error **errp)
{
    BCM2835PropertyState *s = BCM2835_PROPERTY(dev);
    Object *obj;

    obj = object_property_get_link(OBJECT(dev), "fb", &error_abort);
    s->fbdev = BCM2835_FB(obj);

    obj = object_property_get_link(OBJECT(dev), "dma-mr", &error_abort);
    s->dma_mr = MEMORY_REGION(obj);
    address_space_init(&s->dma_as, s->dma_mr, TYPE_BCM2835_PROPERTY "-memory");

    obj = object_property_get_link(OBJECT(dev), "otp", &error_abort);
    s->otp = BCM2835_OTP(obj);

    /* TODO: connect to MAC address of USB NIC device, once we emulate it */
    qemu_macaddr_default_if_unset(&s->macaddr);

    /*
     * Board identity rows as programmed at the factory, so that otp_dump
     * matches the mailbox: serial and its complement, revision, MAC.
     */
    if (s->board_serial) {
        bcm2835_otp_set_row(s->otp, 28, (uint32_t)s->board_serial);
        bcm2835_otp_set_row(s->otp, 29, ~(uint32_t)s->board_serial);
    }
    bcm2835_otp_set_row(s->otp, 30, s->board_rev);
    bcm2835_otp_set_row(s->otp, 64, s->macaddr.a[4] << 24 |
                        s->macaddr.a[5] << 16);
    bcm2835_otp_set_row(s->otp, 65, s->macaddr.a[0] << 24 |
                        s->macaddr.a[1] << 16 | s->macaddr.a[2] << 8 |
                        s->macaddr.a[3]);

    /*
     * The version begins with the 40 hex digits of the git hash, which the
     * hash tag gives as five words, first first
     */
    memset(s->fw_hash, 0, sizeof(s->fw_hash));
    if (s->fw_version) {
        size_t n = strspn(s->fw_version, "0123456789abcdef");

        if (n != 40 || (s->fw_version[n] && s->fw_version[n] != ' ')) {
            error_setg(errp, "bcm2835_property: firmware-version must begin "
                       "with a git hash of 40 hex digits");
            return;
        }
        for (int i = 0; i < 40; i++) {
            s->fw_hash[i / 8] = s->fw_hash[i / 8] << 4 |
                                g_ascii_xdigit_value(s->fw_version[i]);
        }
    }

    if (s->vc_config_file &&
        !g_file_get_contents(s->vc_config_file, &s->vc_config, NULL, NULL)) {
        error_setg(errp, "bcm2835_property: cannot read vc-config '%s'",
                   s->vc_config_file);
        return;
    }

    bcm2835_property_reset(dev);
}

static const Property bcm2835_property_props[] = {
    DEFINE_PROP_UINT32("board-rev", BCM2835PropertyState, board_rev, 0),
    DEFINE_PROP_UINT64("board-serial", BCM2835PropertyState, board_serial, 0),
    DEFINE_PROP_MACADDR("mac", BCM2835PropertyState, macaddr),
    /*
     * The firmware build (VC_BUILD_ID_* of start4.elf): its time, the
     * variant (1 start, 2 start_x, 3 start_db, 4 start_cd) and the version,
     * "<git hash> (clean)"
     */
    DEFINE_PROP_UINT32("firmware-revision", BCM2835PropertyState, fw_revision,
                       346337),
    DEFINE_PROP_UINT32("firmware-variant", BCM2835PropertyState, fw_variant,
                       0),
    DEFINE_PROP_STRING("firmware-version", BCM2835PropertyState, fw_version),
    /* text file of "name=value" lines, as "vcgencmd get_config int" prints */
    DEFINE_PROP_STRING("vc-config", BCM2835PropertyState, vc_config_file),
    DEFINE_PROP_STRING("command-line", BCM2835PropertyState, command_line),
};

static void bcm2835_property_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, bcm2835_property_props);
    dc->realize = bcm2835_property_realize;
    dc->vmsd = &vmstate_bcm2835_property;
}

static const TypeInfo bcm2835_property_info = {
    .name          = TYPE_BCM2835_PROPERTY,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835PropertyState),
    .class_init    = bcm2835_property_class_init,
    .instance_init = bcm2835_property_init,
};

static void bcm2835_property_register_types(void)
{
    type_register_static(&bcm2835_property_info);
}

type_init(bcm2835_property_register_types)
