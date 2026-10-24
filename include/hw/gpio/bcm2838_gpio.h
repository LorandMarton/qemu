/*
 * Raspberry Pi (BCM2838) GPIO Controller
 * This implementation is based on bcm2835_gpio (hw/gpio/bcm2835_gpio.c)
 *
 * Copyright (c) 2022 Auriga LLC
 *
 * Authors:
 *  Lotosh, Aleksey <aleksey.lotosh@auriga.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef BCM2838_GPIO_H
#define BCM2838_GPIO_H

#include "chardev/char-fe.h"
#include "hw/sd/sd.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BCM2838_GPIO "bcm2838-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2838GpioState, BCM2838_GPIO)

#define BCM2838_GPIO_REGS_SIZE 0x1000
#define BCM2838_GPIO_NUM       58
#define GPIO_PUP_PDN_CNTRL_NUM 4
/* Interrupt lines: banks 0 (GPIO 0-27), 1 (28-45), 2 (46-57), any bank */
#define BCM2838_GPIO_NUM_IRQS  4
/* "uart-cts" outputs: UART0, mini UART (UART1), UART2..UART5 */
#define BCM2838_GPIO_NUM_UARTS 6
/* CE0 and CE1 of SPI0, SPI3, SPI4, SPI5, SPI6 */
#define BCM2838_GPIO_NUM_SPI_CS 10

struct BCM2838GpioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    /* SDBus selector */
    SDBus sdbus;
    SDBus *sdbus_sdhci;
    SDBus *sdbus_sdhost;

    uint8_t fsel[BCM2838_GPIO_NUM];
    uint32_t lev0, lev1;    /* output latch (GPSET/GPCLR) */
    uint8_t sd_fsel;
    qemu_irq out[BCM2838_GPIO_NUM];
    uint32_t pup_cntrl_reg[GPIO_PUP_PDN_CNTRL_NUM];

    /* Event detect: status and enables, two words each (GPIO 0-31, 32-57) */
    uint32_t eds[2], ren[2], fen[2], hen[2], len[2], aren[2], afen[2];
    uint64_t level;         /* pin levels as GPLEV reads them */
    /* External drive of each pin: 0 low, 1 high, 2 none ("gpioN" properties) */
    uint8_t ext[BCM2838_GPIO_NUM];
    qemu_irq irq[BCM2838_GPIO_NUM_IRQS];
    qemu_irq uart_cts[BCM2838_GPIO_NUM_UARTS];  /* 1 = CTS asserted */
    /* SPI chip selects: the controllers' CE outputs, the CE pin levels */
    uint32_t spi_ce;
    qemu_irq spi_cs[BCM2838_GPIO_NUM_SPI_CS];

    /* Board state at reset ("pins" property, pinctrl notation) */
    char *pins;
    uint8_t reset_fsel[BCM2838_GPIO_NUM];
    uint64_t reset_out;
    uint32_t reset_pup[GPIO_PUP_PDN_CNTRL_NUM];

    /* Pin line protocol on "chardev" */
    CharFrontend chr;
    uint64_t chr_out;       /* outputs last reported */
    uint64_t chr_latch;     /* and their levels */
    char chr_line[32];      /* command being received */
    uint32_t chr_len;       /* its length, sizeof(chr_line): too long */
};

#endif
