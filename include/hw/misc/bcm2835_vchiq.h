/*
 * BCM2835 VCHIQ, the VideoCore side of the message queue to the ARM
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_BCM2835_VCHIQ_H
#define HW_MISC_BCM2835_VCHIQ_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BCM2835_VCHIQ "bcm2835-vchiq"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835VchiqState, BCM2835_VCHIQ)

/* Doorbell registers, from 0x7e00b840 */
#define BCM2835_VCHIQ_REGS_SIZE         0x40

#define BCM2835_VCHIQ_MAX_SERVICES      32
#define BCM2835_VCHIQ_MAX_IMPORTS       256
#define BCM2835_MMAL_MAX_COMPONENTS     32
/* control port, inputs and outputs: ril.isp has one input, three outputs */
#define BCM2835_MMAL_MAX_PORTS          5
#define BCM2835_MMAL_ES_WORDS           11
#define BCM2835_MMAL_EXTRADATA_SIZE     128

/* A service the ARM opened; vc_port is the index + 1 */
typedef struct BCM2835VchiqService {
    uint32_t fourcc;                    /* 0: free */
    uint32_t arm_port;
} BCM2835VchiqService;

/* A vc-sm-cma (SMEM) import of ARM memory */
typedef struct BCM2835VchiqImport {
    uint32_t handle;                    /* 0: free */
    uint32_t addr;
    uint32_t size;
    uint32_t kernel_id;
} BCM2835VchiqImport;

/* An MMAL port: its format and buffer configuration */
typedef struct BCM2835MmalPort {
    uint32_t buffer_num;
    uint32_t buffer_size;
    uint32_t es_type;
    uint32_t encoding;
    uint32_t encoding_variant;
    uint32_t bitrate;
    uint32_t flags;
    uint32_t es[BCM2835_MMAL_ES_WORDS]; /* union mmal_es_specific_format */
    uint32_t extradata_size;
    uint8_t extradata[BCM2835_MMAL_EXTRADATA_SIZE];
    int32_t crop[4];                    /* MMAL_PARAMETER_CROP, 0 x 0: none */
} BCM2835MmalPort;

/* An MMAL component that an ARM client created */
typedef struct BCM2835MmalComponent {
    uint32_t def;                       /* catalogue index + 1, 0: free */
    uint32_t service;                   /* index of the owning service */
    uint32_t enabled;
    BCM2835MmalPort port[BCM2835_MMAL_MAX_PORTS];
} BCM2835MmalComponent;

struct BCM2835VchiqState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    QEMUBH *bh;
    qemu_irq irq;

    uint32_t bell0;                     /* the VideoCore rang the ARM */
    uint32_t channelbase;               /* slot zero bus address, 0: none */
    uint32_t rx_pos;                    /* read position in the ARM stream */
    uint32_t tx_pos;                    /* write position in our stream */
    uint32_t import_handle;             /* last SMEM handle given out */
    BCM2835VchiqService service[BCM2835_VCHIQ_MAX_SERVICES];
    BCM2835VchiqImport import[BCM2835_VCHIQ_MAX_IMPORTS];
    BCM2835MmalComponent component[BCM2835_MMAL_MAX_COMPONENTS];
};

/*
 * RPI_FIRMWARE_VCHIQ_INIT: the ARM hands over its slot memory at the bus
 * address @channelbase. Returns the firmware response, 0 on success.
 */
uint32_t bcm2835_vchiq_init(BCM2835VchiqState *s, uint32_t channelbase);

/* Service interface, for the services in other files */
typedef struct BCM2835VchiqServiceDef {
    uint32_t fourcc;
    int16_t version;
    int16_t version_min;
    void (*close)(BCM2835VchiqState *s, unsigned int svc);
    void (*data)(BCM2835VchiqState *s, unsigned int svc,
                 const uint8_t *buf, uint32_t len);
} BCM2835VchiqServiceDef;

extern const BCM2835VchiqServiceDef bcm2835_vchiq_mmal_service;

/*
 * Queue a DATA message to the ARM client of @svc. Returns false if there
 * is no room, which a service avoids by sending at most one reply of up to
 * BCM2835_VCHIQ_MAX_REPLY bytes per message it receives.
 */
#define BCM2835_VCHIQ_MAX_REPLY         512
bool bcm2835_vchiq_send_data(BCM2835VchiqState *s, unsigned int svc,
                             const void *buf, uint32_t len);

#endif
