/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef BCM2835_DMA_H
#define BCM2835_DMA_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

typedef struct {
    uint32_t cs;
    uint32_t conblk_ad;
    uint32_t ti;
    uint32_t source_ad;
    uint32_t dest_ad;
    uint32_t txfr_len;
    uint32_t stride;
    uint32_t nextconbk;
    uint32_t debug;

    /* the control block in progress: none when ylen is 0 */
    uint32_t xlen;      /* bytes left in the row */
    uint32_t ylen;      /* rows left, the current one included */
    uint32_t row_len;   /* XLENGTH, in 2D mode */

    qemu_irq irq;
} BCM2835DMAChan;

#define TYPE_BCM2835_DMA "bcm2835-dma"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835DMAState, BCM2835_DMA)

#define BCM2835_DMA_NCHANS 16
#define BCM2835_DMA_NDREQS 32

struct BCM2835DMAState {
    /*< private >*/
    SysBusDevice busdev;
    /*< public >*/

    MemoryRegion iomem0, iomem15;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;

    BCM2835DMAChan chan[BCM2835_DMA_NCHANS];
    uint32_t int_status;
    uint32_t enable;
    uint32_t dreq;      /* the peripherals' DREQ lines (PERMAP), bit 0 unused */
    uint32_t dreq_inputs; /* the lines a peripheral model drives */
    QEMUBH *bh;
};

#endif
