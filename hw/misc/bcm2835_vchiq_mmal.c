/*
 * BCM2835 VCHIQ MMAL service
 *
 * MMAL is the VideoCore multimedia framework. Its VCHIQ service ("mmal",
 * Linux vchiq-mmal) creates components, describes and configures their
 * ports and passes buffers through them. The bcm2835-codec driver uses
 * ril.video_decode, ril.video_encode, ril.isp, ril.image_fx and
 * ril.image_encode, the bcm2835-isp driver ril.isp, the legacy camera
 * driver bcm2835-camera camera_info.
 *
 * The model has these components with the ports and the encodings of the
 * Raspberry Pi 4 firmware: the encodings, in the firmware order, are those
 * that the drivers list on a CM4 (bcm2835-codec and bcm2835-isp of Linux
 * 6.6, firmware of 2026-09-14), mapped back with the drivers' format
 * tables. A port keeps the format the ARM sets, and the parameters that the
 * ARM reads back (supported encodings, crop, interlacing) answer as the
 * firmware does. The components process nothing: a port cannot be
 * enabled, so no buffer is ever passed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/bcm2835_vchiq.h"
#include "trace.h"

#define MMAL_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | (b) << 8 | (c) << 16 | (uint32_t)(d) << 24)

#define MMAL_MAGIC              MMAL_FOURCC('m', 'm', 'a', 'l')
#define MMAL_VERSION            15
#define MMAL_VERSION_MIN        10

enum {
    MMAL_MSG_COMPONENT_CREATE = 4,
    MMAL_MSG_COMPONENT_DESTROY,
    MMAL_MSG_COMPONENT_ENABLE,
    MMAL_MSG_COMPONENT_DISABLE,
    MMAL_MSG_PORT_INFO_GET,
    MMAL_MSG_PORT_INFO_SET,
    MMAL_MSG_PORT_ACTION,
    MMAL_MSG_BUFFER_FROM_HOST,
    MMAL_MSG_PORT_PARAMETER_SET = 14,
    MMAL_MSG_PORT_PARAMETER_GET,
};

enum {
    MMAL_SUCCESS,
    MMAL_ENOMEM,
    MMAL_ENOSPC,
    MMAL_EINVAL,
    MMAL_ENOSYS,
};

enum {
    MMAL_PORT_CONTROL = 1,
    MMAL_PORT_INPUT,
    MMAL_PORT_OUTPUT,
    MMAL_PORT_CLOCK,
};

enum {
    MMAL_PORT_ACTION_ENABLE = 1,
    MMAL_PORT_ACTION_DISABLE,
    MMAL_PORT_ACTION_FLUSH,
    MMAL_PORT_ACTION_CONNECT,
    MMAL_PORT_ACTION_DISCONNECT,
    MMAL_PORT_ACTION_SET_REQUIREMENTS,
};

#define MMAL_ES_TYPE_CONTROL    1
#define MMAL_ES_TYPE_VIDEO      3

#define MMAL_PARAMETER_SUPPORTED_ENCODINGS      0x00001
#define MMAL_PARAMETER_CAMERA_INFO              0x1001c
#define MMAL_PARAMETER_CROP                     0x10055
#define MMAL_PARAMETER_VIDEO_INTERLACE_TYPE     0x2002e

/* struct mmal_msg_header, then the payload */
#define MSG_HEADER_SIZE         24
#define MSG_TYPE                4
#define MSG_STATUS              16

/* struct mmal_port */
#define PORT_TYPE               8
#define PORT_INDEX              12      /* and index_all, 16 bits each */
#define PORT_BUFFER_NUM_MIN     24
#define PORT_BUFFER_SIZE_MIN    28
#define PORT_BUFFER_NUM_RECOMMENDED 36
#define PORT_BUFFER_SIZE_RECOMMENDED 40
#define PORT_BUFFER_NUM         44
#define PORT_BUFFER_SIZE        48
#define PORT_SIZE               64

/* struct mmal_es_format */
#define FORMAT_TYPE             0
#define FORMAT_ENCODING         4
#define FORMAT_ENCODING_VARIANT 8
#define FORMAT_BITRATE          16
#define FORMAT_FLAGS            20
#define FORMAT_EXTRADATA_SIZE   24
#define FORMAT_SIZE             32

#define ES_SIZE                 (BCM2835_MMAL_ES_WORDS * 4)
/* video width and height, the first two words of the ES format */
#define ES_WIDTH                0
#define ES_HEIGHT               1

/* port, format, ES format and extradata, as port info get and set have */
#define PORT_INFO_SIZE \
    (PORT_SIZE + FORMAT_SIZE + ES_SIZE + BCM2835_MMAL_EXTRADATA_SIZE)

#define PARAMETER_SPACE         (96 * 4)

/* struct mmal_parameter_camera_info: 4 cameras, 2 flashes */
#define CAMERA_INFO_SIZE        (8 + 4 * 32 + 2 * 4)

/* Encodings of the ports, in the firmware order */
static const uint32_t mmal_enc_decode_in[] = {
    MMAL_FOURCC('H', '2', '6', '4'), MMAL_FOURCC('M', 'J', 'P', 'G'),
};

static const uint32_t mmal_enc_decode_out[] = {
    MMAL_FOURCC('I', '4', '2', '0'), MMAL_FOURCC('Y', 'V', '1', '2'),
    MMAL_FOURCC('N', 'V', '1', '2'), MMAL_FOURCC('N', 'V', '2', '1'),
    MMAL_FOURCC('S', 'A', 'N', 'D'), MMAL_FOURCC('R', 'G', 'B', '2'),
    MMAL_FOURCC('R', 'G', 'B', 'A'), MMAL_FOURCC('B', 'G', 'R', 'A'),
};

static const uint32_t mmal_enc_encode_in[] = {
    MMAL_FOURCC('I', '4', '2', '0'), MMAL_FOURCC('Y', 'V', '1', '2'),
    MMAL_FOURCC('N', 'V', '1', '2'), MMAL_FOURCC('N', 'V', '2', '1'),
    MMAL_FOURCC('R', 'G', 'B', '2'), MMAL_FOURCC('R', 'G', 'B', '3'),
    MMAL_FOURCC('B', 'G', 'R', '3'), MMAL_FOURCC('R', 'G', 'B', 'A'),
    MMAL_FOURCC('B', 'G', 'R', 'A'), MMAL_FOURCC('Y', 'U', 'Y', 'V'),
    MMAL_FOURCC('Y', 'V', 'Y', 'U'), MMAL_FOURCC('U', 'Y', 'V', 'Y'),
    MMAL_FOURCC('V', 'Y', 'U', 'Y'), MMAL_FOURCC('S', 'A', 'N', 'D'),
};

static const uint32_t mmal_enc_encode_out[] = {
    MMAL_FOURCC('H', '2', '6', '4'), MMAL_FOURCC('M', 'J', 'P', 'G'),
};

static const uint32_t mmal_enc_isp_in[] = {
    MMAL_FOURCC('B', 'A', '8', '1'), MMAL_FOURCC('R', 'G', 'G', 'B'),
    MMAL_FOURCC('G', 'B', 'R', 'G'), MMAL_FOURCC('G', 'R', 'B', 'G'),
    MMAL_FOURCC('p', 'B', 'A', 'A'), MMAL_FOURCC('p', 'R', 'A', 'A'),
    MMAL_FOURCC('p', 'G', 'A', 'A'), MMAL_FOURCC('p', 'g', 'A', 'A'),
    MMAL_FOURCC('p', 'B', '1', '2'), MMAL_FOURCC('p', 'R', '1', '2'),
    MMAL_FOURCC('p', 'G', '1', '2'), MMAL_FOURCC('p', 'g', '1', '2'),
    MMAL_FOURCC('p', 'B', 'E', 'E'), MMAL_FOURCC('p', 'R', 'E', 'E'),
    MMAL_FOURCC('p', 'G', 'E', 'E'), MMAL_FOURCC('p', 'g', 'E', 'E'),
    MMAL_FOURCC('B', 'G', '1', '6'), MMAL_FOURCC('R', 'G', '1', '6'),
    MMAL_FOURCC('G', 'B', '1', '6'), MMAL_FOURCC('G', 'R', '1', '6'),
    MMAL_FOURCC('G', 'R', 'E', 'Y'), MMAL_FOURCC('Y', '1', '0', 'P'),
    MMAL_FOURCC('Y', '1', '2', 'P'), MMAL_FOURCC('Y', '1', '4', 'P'),
    MMAL_FOURCC('Y', '1', '6', ' '), MMAL_FOURCC('Y', 'U', 'Y', 'V'),
    MMAL_FOURCC('Y', 'V', 'Y', 'U'), MMAL_FOURCC('V', 'Y', 'U', 'Y'),
    MMAL_FOURCC('U', 'Y', 'V', 'Y'), MMAL_FOURCC('I', '4', '2', '0'),
    MMAL_FOURCC('Y', 'V', '1', '2'), MMAL_FOURCC('R', 'G', 'B', '3'),
    MMAL_FOURCC('B', 'G', 'R', '3'), MMAL_FOURCC('R', 'G', 'B', 'A'),
    MMAL_FOURCC('B', 'G', 'R', 'A'), MMAL_FOURCC('R', 'G', 'B', '2'),
    MMAL_FOURCC('S', 'A', 'N', 'D'), MMAL_FOURCC('N', 'V', '1', '2'),
    MMAL_FOURCC('N', 'V', '2', '1'), MMAL_FOURCC('B', 'G', '1', '0'),
    MMAL_FOURCC('R', 'G', '1', '0'), MMAL_FOURCC('G', 'B', '1', '0'),
    MMAL_FOURCC('B', 'A', '1', '0'), MMAL_FOURCC('B', 'G', '1', '2'),
    MMAL_FOURCC('R', 'G', '1', '2'), MMAL_FOURCC('G', 'B', '1', '2'),
    MMAL_FOURCC('B', 'A', '1', '2'), MMAL_FOURCC('B', 'G', '1', '4'),
    MMAL_FOURCC('R', 'G', '1', '4'), MMAL_FOURCC('G', 'B', '1', '4'),
    MMAL_FOURCC('G', 'R', '1', '4'), MMAL_FOURCC('Y', '1', '0', ' '),
    MMAL_FOURCC('Y', '1', '2', ' '), MMAL_FOURCC('Y', '1', '4', ' '),
};

static const uint32_t mmal_enc_isp_out0[] = {
    MMAL_FOURCC('Y', 'U', 'Y', 'V'), MMAL_FOURCC('Y', 'V', 'Y', 'U'),
    MMAL_FOURCC('V', 'Y', 'U', 'Y'), MMAL_FOURCC('U', 'Y', 'V', 'Y'),
    MMAL_FOURCC('I', '4', '2', '0'), MMAL_FOURCC('Y', 'V', '1', '2'),
    MMAL_FOURCC('R', 'G', 'B', '3'), MMAL_FOURCC('B', 'G', 'R', '3'),
    MMAL_FOURCC('R', 'G', 'B', 'A'), MMAL_FOURCC('B', 'G', 'R', 'A'),
    MMAL_FOURCC('R', 'G', 'B', '2'), MMAL_FOURCC('S', 'A', 'N', 'D'),
    MMAL_FOURCC('N', 'V', '1', '2'), MMAL_FOURCC('N', 'V', '2', '1'),
};

static const uint32_t mmal_enc_isp_out1[] = {
    MMAL_FOURCC('Y', 'U', 'Y', 'V'), MMAL_FOURCC('Y', 'V', 'Y', 'U'),
    MMAL_FOURCC('V', 'Y', 'U', 'Y'), MMAL_FOURCC('U', 'Y', 'V', 'Y'),
    MMAL_FOURCC('I', '4', '2', '0'), MMAL_FOURCC('Y', 'V', '1', '2'),
    MMAL_FOURCC('N', 'V', '1', '2'), MMAL_FOURCC('N', 'V', '2', '1'),
};

static const uint32_t mmal_enc_image_fx[] = {
    MMAL_FOURCC('I', '4', '2', '0'), MMAL_FOURCC('S', 'A', 'N', 'D'),
    MMAL_FOURCC('R', 'G', 'B', 'A'), MMAL_FOURCC('R', 'G', 'B', '2'),
};

static const uint32_t mmal_enc_image_encode_in[] = {
    MMAL_FOURCC('R', 'G', 'B', '2'), MMAL_FOURCC('R', 'G', 'B', '3'),
    MMAL_FOURCC('B', 'G', 'R', '3'), MMAL_FOURCC('R', 'G', 'B', 'A'),
    MMAL_FOURCC('B', 'G', 'R', 'A'), MMAL_FOURCC('I', '4', '2', '0'),
    MMAL_FOURCC('Y', 'U', 'Y', 'V'), MMAL_FOURCC('Y', 'V', 'Y', 'U'),
    MMAL_FOURCC('V', 'Y', 'U', 'Y'), MMAL_FOURCC('U', 'Y', 'V', 'Y'),
};

static const uint32_t mmal_enc_image_encode_out[] = {
    MMAL_FOURCC('J', 'P', 'E', 'G'),
};

static const uint32_t mmal_enc_isp_stats[] = {
    MMAL_FOURCC('S', 'T', 'A', 'T'),
};

typedef struct MmalPortDef {
    const uint32_t *encodings;
    unsigned int num_encodings;
    uint32_t buffer_size_min;
} MmalPortDef;

typedef struct MmalComponentDef {
    const char *name;
    unsigned int inputs;
    unsigned int outputs;
    MmalPortDef port[BCM2835_MMAL_MAX_PORTS - 1];    /* inputs, outputs */
} MmalComponentDef;

#define MMAL_PORT_DEF(enc)      { enc, ARRAY_SIZE(enc), 0 }

static const MmalComponentDef mmal_components[] = {
    {
        "ril.video_decode", 1, 1, {
            MMAL_PORT_DEF(mmal_enc_decode_in),
            MMAL_PORT_DEF(mmal_enc_decode_out),
        }
    }, {
        "ril.video_encode", 1, 1, {
            MMAL_PORT_DEF(mmal_enc_encode_in),
            MMAL_PORT_DEF(mmal_enc_encode_out),
        }
    }, {
        /* outputs: image, low resolution image, statistics */
        "ril.isp", 1, 3, {
            MMAL_PORT_DEF(mmal_enc_isp_in),
            MMAL_PORT_DEF(mmal_enc_isp_out0),
            MMAL_PORT_DEF(mmal_enc_isp_out1),
            /* the statistics buffer size as the CM4 firmware gives it */
            { mmal_enc_isp_stats, ARRAY_SIZE(mmal_enc_isp_stats), 10824 },
        }
    }, {
        "ril.image_fx", 1, 1, {
            MMAL_PORT_DEF(mmal_enc_image_fx),
            MMAL_PORT_DEF(mmal_enc_image_fx),
        }
    }, {
        "ril.image_encode", 1, 1, {
            MMAL_PORT_DEF(mmal_enc_image_encode_in),
            MMAL_PORT_DEF(mmal_enc_image_encode_out),
        }
    }, {
        /* the cameras of the legacy camera stack (bcm2835-camera) */
        "camera_info", 0, 0,
    },
};

static const MmalComponentDef *mmal_def(BCM2835MmalComponent *c)
{
    return &mmal_components[c->def - 1];
}

static uint32_t mmal_handle(BCM2835VchiqState *s, BCM2835MmalComponent *c)
{
    return c - s->component + 1;
}

static unsigned int mmal_num_ports(BCM2835MmalComponent *c)
{
    return 1 + mmal_def(c)->inputs + mmal_def(c)->outputs;
}

/* The component of @handle, NULL if there is none */
static BCM2835MmalComponent *mmal_component(BCM2835VchiqState *s,
                                            unsigned int svc, uint32_t handle)
{
    BCM2835MmalComponent *c;

    if (handle < 1 || handle > BCM2835_MMAL_MAX_COMPONENTS) {
        return NULL;
    }
    c = &s->component[handle - 1];
    return c->def && c->service == svc ? c : NULL;
}

/* The port number (control, inputs, outputs) of a port type and index */
static int mmal_port_num(BCM2835MmalComponent *c, uint32_t type,
                         uint32_t index)
{
    const MmalComponentDef *def = mmal_def(c);

    switch (type) {
    case MMAL_PORT_CONTROL:
        return index == 0 ? 0 : -1;
    case MMAL_PORT_INPUT:
        return index < def->inputs ? 1 + index : -1;
    case MMAL_PORT_OUTPUT:
        return index < def->outputs ? 1 + def->inputs + index : -1;
    default:
        return -1;
    }
}

static const MmalPortDef *mmal_port_def(BCM2835MmalComponent *c,
                                        unsigned int num)
{
    return num ? &mmal_def(c)->port[num - 1] : NULL;
}

static void mmal_port_reset(BCM2835MmalComponent *c, unsigned int num)
{
    BCM2835MmalPort *p = &c->port[num];
    const MmalPortDef *pd = mmal_port_def(c, num);

    memset(p, 0, sizeof(*p));
    p->buffer_num = 1;
    if (pd) {
        p->es_type = MMAL_ES_TYPE_VIDEO;
        p->encoding = pd->encodings[0];
        p->buffer_size = pd->buffer_size_min;
    } else {
        p->es_type = MMAL_ES_TYPE_CONTROL;
    }
}

/* struct mmal_port, mmal_es_format, the ES format and the extradata */
static void mmal_port_info(BCM2835MmalComponent *c, unsigned int num,
                           uint8_t *buf)
{
    const MmalComponentDef *def = mmal_def(c);
    const MmalPortDef *pd = mmal_port_def(c, num);
    BCM2835MmalPort *p = &c->port[num];
    uint8_t *format = buf + PORT_SIZE;
    uint8_t *es = format + FORMAT_SIZE;
    uint32_t size_min = pd ? pd->buffer_size_min : 0;
    uint32_t type, index;

    if (num == 0) {
        type = MMAL_PORT_CONTROL;
        index = 0;
    } else if (num <= def->inputs) {
        type = MMAL_PORT_INPUT;
        index = num - 1;
    } else {
        type = MMAL_PORT_OUTPUT;
        index = num - 1 - def->inputs;
    }

    memset(buf, 0, PORT_INFO_SIZE);
    stl_le_p(buf + PORT_TYPE, type);
    stw_le_p(buf + PORT_INDEX, index);
    stw_le_p(buf + PORT_INDEX + 2, num);
    stl_le_p(buf + PORT_BUFFER_NUM_MIN, 1);
    stl_le_p(buf + PORT_BUFFER_SIZE_MIN, size_min);
    stl_le_p(buf + PORT_BUFFER_NUM_RECOMMENDED, 1);
    stl_le_p(buf + PORT_BUFFER_SIZE_RECOMMENDED, size_min);
    stl_le_p(buf + PORT_BUFFER_NUM, p->buffer_num);
    stl_le_p(buf + PORT_BUFFER_SIZE, p->buffer_size);

    stl_le_p(format + FORMAT_TYPE, p->es_type);
    stl_le_p(format + FORMAT_ENCODING, p->encoding);
    stl_le_p(format + FORMAT_ENCODING_VARIANT, p->encoding_variant);
    stl_le_p(format + FORMAT_BITRATE, p->bitrate);
    stl_le_p(format + FORMAT_FLAGS, p->flags);
    stl_le_p(format + FORMAT_EXTRADATA_SIZE, p->extradata_size);
    for (int i = 0; i < BCM2835_MMAL_ES_WORDS; i++) {
        stl_le_p(es + 4 * i, p->es[i]);
    }
    memcpy(es + ES_SIZE, p->extradata, p->extradata_size);
}

/* Set a port format from struct mmal_port and the formats in @buf */
static uint32_t mmal_port_set(BCM2835MmalComponent *c, unsigned int num,
                              const uint8_t *buf)
{
    const MmalPortDef *pd = mmal_port_def(c, num);
    BCM2835MmalPort *p = &c->port[num];
    const uint8_t *format = buf + PORT_SIZE;
    const uint8_t *es = format + FORMAT_SIZE;
    uint32_t encoding = ldl_le_p(format + FORMAT_ENCODING);
    uint32_t extradata_size = ldl_le_p(format + FORMAT_EXTRADATA_SIZE);
    unsigned int i;

    if (!pd) {
        return MMAL_EINVAL;
    }
    for (i = 0; i < pd->num_encodings && pd->encodings[i] != encoding; i++) {
        continue;
    }
    if (i == pd->num_encodings ||
        extradata_size > BCM2835_MMAL_EXTRADATA_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: %s port %u: unsupported "
                      "format %08x\n", __func__, mmal_def(c)->name, num,
                      encoding);
        return MMAL_EINVAL;
    }

    p->buffer_num = ldl_le_p(buf + PORT_BUFFER_NUM);
    p->buffer_size = ldl_le_p(buf + PORT_BUFFER_SIZE);
    p->es_type = ldl_le_p(format + FORMAT_TYPE);
    p->encoding = encoding;
    p->encoding_variant = ldl_le_p(format + FORMAT_ENCODING_VARIANT);
    p->bitrate = ldl_le_p(format + FORMAT_BITRATE);
    p->flags = ldl_le_p(format + FORMAT_FLAGS);
    for (i = 0; i < BCM2835_MMAL_ES_WORDS; i++) {
        p->es[i] = ldl_le_p(es + 4 * i);
    }
    p->extradata_size = extradata_size;
    memcpy(p->extradata, es + ES_SIZE, extradata_size);
    return MMAL_SUCCESS;
}

static uint32_t mmal_create(BCM2835VchiqState *s, unsigned int svc,
                            const uint8_t *in, uint8_t *out)
{
    /* client_component, name[128], pid */
    g_autofree char *name = g_strndup((const char *)in + 4, 128);
    BCM2835MmalComponent *c;
    unsigned int def, i;

    for (def = 0; def < ARRAY_SIZE(mmal_components); def++) {
        if (!strcmp(name, mmal_components[def].name)) {
            break;
        }
    }
    if (def == ARRAY_SIZE(mmal_components)) {
        qemu_log_mask(LOG_UNIMP, "%s: component %s\n", __func__, name);
        return MMAL_ENOSYS;
    }
    for (i = 0; i < BCM2835_MMAL_MAX_COMPONENTS && s->component[i].def; i++) {
        continue;
    }
    if (i == BCM2835_MMAL_MAX_COMPONENTS) {
        return MMAL_ENOMEM;
    }

    c = &s->component[i];
    c->def = def + 1;
    c->service = svc;
    c->enabled = 0;
    for (unsigned int n = 0; n < mmal_num_ports(c); n++) {
        mmal_port_reset(c, n);
    }
    trace_bcm2835_vchiq_mmal_create(name, i + 1);
    /* component_handle, input_num, output_num, clock_num */
    stl_le_p(out, i + 1);
    stl_le_p(out + 4, mmal_def(c)->inputs);
    stl_le_p(out + 8, mmal_def(c)->outputs);
    stl_le_p(out + 12, 0);
    return MMAL_SUCCESS;
}

static uint32_t mmal_parameter_get(BCM2835MmalComponent *c, unsigned int num,
                                   uint32_t id, uint32_t space, uint8_t *value,
                                   uint32_t *size)
{
    const MmalPortDef *pd = mmal_port_def(c, num);
    BCM2835MmalPort *p = &c->port[num];

    switch (id) {
    case MMAL_PARAMETER_CAMERA_INFO:
        if (strcmp(mmal_def(c)->name, "camera_info")) {
            return MMAL_ENOSYS;
        }
        /*
         * None, as on a CM4 whose camera libcamera drives through unicam:
         * the firmware does not detect cameras for the legacy stack
         */
        memset(value, 0, CAMERA_INFO_SIZE);
        *size = CAMERA_INFO_SIZE;
        return *size > space ? MMAL_ENOSPC : MMAL_SUCCESS;
    case MMAL_PARAMETER_SUPPORTED_ENCODINGS:
        if (!pd) {
            return MMAL_ENOSYS;
        }
        for (unsigned int i = 0; i < pd->num_encodings &&
             i < PARAMETER_SPACE / 4; i++) {
            stl_le_p(value + 4 * i, pd->encodings[i]);
        }
        *size = 4 * pd->num_encodings;
        return *size > space ? MMAL_ENOSPC : MMAL_SUCCESS;
    case MMAL_PARAMETER_CROP:
        if (!pd) {
            return MMAL_ENOSYS;
        }
        /* struct mmal_parameter_crop, by default the whole picture */
        stl_le_p(value, p->crop[0]);
        stl_le_p(value + 4, p->crop[1]);
        stl_le_p(value + 8, p->crop[2] ? p->crop[2] : p->es[ES_WIDTH]);
        stl_le_p(value + 12, p->crop[3] ? p->crop[3] : p->es[ES_HEIGHT]);
        *size = 16;
        return MMAL_SUCCESS;
    case MMAL_PARAMETER_VIDEO_INTERLACE_TYPE:
        /* progressive, no repeated first field */
        if (!pd) {
            return MMAL_ENOSYS;
        }
        stl_le_p(value, 0);
        stl_le_p(value + 4, 0);
        *size = 8;
        return MMAL_SUCCESS;
    default:
        return MMAL_ENOSYS;
    }
}

static void mmal_data(BCM2835VchiqState *s, unsigned int svc,
                      const uint8_t *buf, uint32_t len)
{
    uint8_t reply[BCM2835_VCHIQ_MAX_REPLY] = { 0 };
    const uint8_t *in = buf + MSG_HEADER_SIZE;
    uint8_t *out = reply + MSG_HEADER_SIZE;
    uint32_t in_len = len - MSG_HEADER_SIZE;
    uint32_t out_len = 4, status = MMAL_SUCCESS;
    BCM2835MmalComponent *c = NULL;
    uint32_t type, id, size;
    int num = -1;

    if (len < MSG_HEADER_SIZE || ldl_le_p(buf) != MMAL_MAGIC) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: not an MMAL message\n", __func__);
        return;
    }
    type = ldl_le_p(buf + MSG_TYPE);
    trace_bcm2835_vchiq_mmal(type, in_len >= 4 ? ldl_le_p(in) : 0);

    /* every message but create starts with a component handle */
    if (type >= MMAL_MSG_COMPONENT_DESTROY &&
        type <= MMAL_MSG_PORT_PARAMETER_GET && in_len >= 4) {
        c = mmal_component(s, svc, ldl_le_p(in));
    }

    switch (type) {
    case MMAL_MSG_COMPONENT_CREATE:
        if (in_len < 4 + 128) {
            goto short_message;
        }
        status = mmal_create(s, svc, in, out + 4);
        out_len = 4 + 16;
        break;
    case MMAL_MSG_COMPONENT_DESTROY:
        if (!c) {
            status = MMAL_EINVAL;
            break;
        }
        memset(c, 0, sizeof(*c));
        break;
    case MMAL_MSG_COMPONENT_ENABLE:
    case MMAL_MSG_COMPONENT_DISABLE:
        if (!c) {
            status = MMAL_EINVAL;
            break;
        }
        c->enabled = type == MMAL_MSG_COMPONENT_ENABLE;
        break;
    case MMAL_MSG_PORT_INFO_GET:
    case MMAL_MSG_PORT_INFO_SET:
        /* component_handle, port_type, index, [port info] */
        if (in_len < 12 ||
            (type == MMAL_MSG_PORT_INFO_SET && in_len < 12 + PORT_INFO_SIZE -
             BCM2835_MMAL_EXTRADATA_SIZE)) {
            goto short_message;
        }
        if (c) {
            num = mmal_port_num(c, ldl_le_p(in + 4), ldl_le_p(in + 8));
        }
        if (num < 0) {
            status = MMAL_EINVAL;
            break;
        }
        if (type == MMAL_MSG_PORT_INFO_SET) {
            uint8_t info[PORT_INFO_SIZE] = { 0 };

            memcpy(info, in + 12, MIN(in_len - 12, PORT_INFO_SIZE));
            status = mmal_port_set(c, num, info);
        }
        /* component_handle, port_type, port_index, found, port_handle */
        memcpy(out + 4, in, 12);
        stl_le_p(out + 16, 1);
        stl_le_p(out + 20, num);
        mmal_port_info(c, num, out + 24);
        out_len = 24 + PORT_INFO_SIZE;
        break;
    case MMAL_MSG_PORT_ACTION:
        /* component_handle, port_handle, action, port */
        if (in_len < 12) {
            goto short_message;
        }
        if (!c || ldl_le_p(in + 4) >= mmal_num_ports(c)) {
            status = MMAL_EINVAL;
            break;
        }
        num = ldl_le_p(in + 4);
        switch (ldl_le_p(in + 8)) {
        case MMAL_PORT_ACTION_DISABLE:
        case MMAL_PORT_ACTION_FLUSH:
            break;
        case MMAL_PORT_ACTION_SET_REQUIREMENTS:
            if (in_len < 12 + PORT_SIZE) {
                goto short_message;
            }
            c->port[num].buffer_num = ldl_le_p(in + 12 + PORT_BUFFER_NUM);
            c->port[num].buffer_size = ldl_le_p(in + 12 + PORT_BUFFER_SIZE);
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "%s: %s port %d: action %u: buffer "
                          "processing is not emulated\n", __func__,
                          mmal_def(c)->name, num, ldl_le_p(in + 8));
            status = MMAL_ENOSYS;
            break;
        }
        break;
    case MMAL_MSG_PORT_PARAMETER_SET:
        /* component_handle, port_handle, id, size, value */
        if (in_len < 16) {
            goto short_message;
        }
        id = ldl_le_p(in + 8);
        size = ldl_le_p(in + 12);
        if (!c || ldl_le_p(in + 4) >= mmal_num_ports(c) || size < 8 ||
            size - 8 > in_len - 16) {
            status = MMAL_EINVAL;
            break;
        }
        num = ldl_le_p(in + 4);
        trace_bcm2835_vchiq_mmal_parameter_set(mmal_handle(s, c), num, id,
                                               size - 8);
        if (id == MMAL_PARAMETER_CROP && size - 8 >= 16) {
            for (int i = 0; i < 4; i++) {
                c->port[num].crop[i] = ldl_le_p(in + 16 + 4 * i);
            }
        }
        /* the other parameters change how buffers are processed */
        break;
    case MMAL_MSG_PORT_PARAMETER_GET:
        /* component_handle, port_handle, id, size */
        if (in_len < 16) {
            goto short_message;
        }
        id = ldl_le_p(in + 8);
        size = ldl_le_p(in + 12);
        if (!c || ldl_le_p(in + 4) >= mmal_num_ports(c) || size < 8) {
            status = MMAL_EINVAL;
            break;
        }
        num = ldl_le_p(in + 4);
        size = MIN(size - 8, PARAMETER_SPACE);
        {
            uint32_t value_size = 0;

            status = mmal_parameter_get(c, num, id, size, out + 12,
                                        &value_size);
            /* id, size including id and size, value */
            stl_le_p(out + 4, id);
            stl_le_p(out + 8, 8 + value_size);
            out_len = 12 + MAX(MIN(value_size, PARAMETER_SPACE), size);
        }
        break;
    case MMAL_MSG_BUFFER_FROM_HOST:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: buffer for a disabled port\n",
                      __func__);
        return;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: message type %u\n", __func__, type);
        status = MMAL_ENOSYS;
        break;
    }

    if (status != MMAL_SUCCESS) {
        trace_bcm2835_vchiq_mmal_status(type, status);
    }
    /* the header with its context, then the status and the reply */
    memcpy(reply, buf, MSG_HEADER_SIZE);
    stl_le_p(reply + MSG_STATUS, 0);
    stl_le_p(out, status);
    bcm2835_vchiq_send_data(s, svc, reply, MSG_HEADER_SIZE + out_len);
    return;

short_message:
    qemu_log_mask(LOG_GUEST_ERROR, "%s: short message of type %u\n", __func__,
                  type);
}

/* The client closed the service: destroy its components */
static void mmal_close(BCM2835VchiqState *s, unsigned int svc)
{
    for (int i = 0; i < BCM2835_MMAL_MAX_COMPONENTS; i++) {
        if (s->component[i].def && s->component[i].service == svc) {
            memset(&s->component[i], 0, sizeof(s->component[i]));
        }
    }
}

const BCM2835VchiqServiceDef bcm2835_vchiq_mmal_service = {
    .fourcc = 'm' << 24 | 'm' << 16 | 'a' << 8 | 'l',
    .version = MMAL_VERSION,
    .version_min = MMAL_VERSION_MIN,
    .close = mmal_close,
    .data = mmal_data,
};
