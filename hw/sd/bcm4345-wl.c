/*
 * Broadcom BCM4345 (Cypress CYW43455) firmware model
 *
 * Models the FullMAC firmware (wl) the host downloads into the chip
 * (7.45.241, the Raspberry Pi build), as the host sees it over the BCDC
 * protocol: firmware commands (dcmds) and variables (iovars) on the
 * SDPCM control channel, events on the event channel, Ethernet frames
 * on the data channel.  Identity and capabilities are those of the chip
 * on a Raspberry Pi Compute Module 4; the version strings come from the
 * firmware image and the CLM blob the host downloads, as on the chip.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "net/checksum.h"
#include "net/eth.h"
#include "bcm4345-internal.h"
#include "trace.h"

/* Firmware commands */
#define WLC_GET_VERSION                 1
#define WLC_UP                          2
#define WLC_DOWN                        3
#define WLC_SET_PROMISC                 10
#define WLC_GET_INFRA                   19
#define WLC_SET_INFRA                   20
#define WLC_GET_AUTH                    21
#define WLC_SET_AUTH                    22
#define WLC_GET_BSSID                   23
#define WLC_GET_SSID                    25
#define WLC_SET_SSID                    26
#define WLC_GET_CHANNEL                 29
#define WLC_GET_SRL                     31
#define WLC_SET_SRL                     32
#define WLC_GET_LRL                     33
#define WLC_SET_LRL                     34
#define WLC_GET_RADIO                   37
#define WLC_SET_RADIO                   38
#define WLC_GET_PHYTYPE                 39
#define WLC_SET_KEY                     45
#define WLC_GET_REGULATORY              46
#define WLC_SET_REGULATORY              47
#define WLC_SET_PASSIVE_SCAN            49
#define WLC_SCAN                        50
#define WLC_DISASSOC                    52
#define WLC_SET_ROAM_TRIGGER            55
#define WLC_SET_ROAM_DELTA              57
#define WLC_GET_BCNPRD                  75
#define WLC_SET_BCNPRD                  76
#define WLC_GET_DTIMPRD                 77
#define WLC_SET_DTIMPRD                 78
#define WLC_SET_COUNTRY                 84
#define WLC_GET_PM                      85
#define WLC_SET_PM                      86
#define WLC_GET_REVINFO                 98
#define WLC_GET_MONITOR                 107
#define WLC_SET_MONITOR                 108
#define WLC_GET_AP                      117
#define WLC_SET_AP                      118
#define WLC_SET_SCB_AUTHORIZE           121
#define WLC_SET_SCB_DEAUTHORIZE         122
#define WLC_GET_RSSI                    127
#define WLC_GET_WSEC                    133
#define WLC_SET_WSEC                    134
#define WLC_GET_BSS_INFO                136
#define WLC_GET_BANDLIST                140
#define WLC_SET_SCB_TIMEOUT             158
#define WLC_GET_ASSOCLIST               159
#define WLC_SET_SCAN_CHANNEL_TIME       185
#define WLC_SET_SCAN_UNASSOC_TIME       187
#define WLC_SCB_DEAUTHENTICATE_FOR_REASON 201
#define WLC_SET_ASSOC_PREFER            205
#define WLC_SET_FAKEFRAG                219
#define WLC_GET_KEY_PRIMARY             235
#define WLC_SET_KEY_PRIMARY             236
#define WLC_SET_SCAN_PASSIVE_TIME       258
#define WLC_GET_VAR                     262
#define WLC_SET_VAR                     263

/* Firmware error codes */
#define BCME_OK                         0
#define BCME_ERROR                      -1
#define BCME_BADARG                     -2
#define BCME_NOTUP                      -4
#define BCME_NOTSTA                     -7
#define BCME_BUFTOOSHORT                -14
#define BCME_BUSY                       -16
#define BCME_NOTASSOCIATED              -17
#define BCME_BADADDR                    -21
#define BCME_NORESOURCE                 -22
#define BCME_UNSUPPORTED                -23
#define BCME_BADLEN                     -24
#define BCME_NOMEM                      -27
#define BCME_RANGE                      -29
#define BCME_VERSION                    -37

/* BCDC control message header (struct brcmf_proto_bcdc_dcmd) */
#define BCDC_DCMD_LEN                   16
#define BCDC_DCMD_ERROR                 0x01
#define BCDC_DCMD_SET                   0x02
#define BCDC_DCMD_IF_SHIFT              12
#define BCDC_DCMD_IF_MASK               0xf
#define BCDC_PROTO_VER                  2
#define BCDC_FLAG_VER_SHIFT             4

/* Events */
#define WLC_E_SET_SSID                  0
#define WLC_E_AUTH                      3
#define WLC_E_DEAUTH                    5
#define WLC_E_DEAUTH_IND                6
#define WLC_E_ASSOC                     7
#define WLC_E_ASSOC_IND                 8
#define WLC_E_REASSOC                   9
#define WLC_E_REASSOC_IND               10
#define WLC_E_DISASSOC                  11
#define WLC_E_DISASSOC_IND              12
#define WLC_E_LINK                      16
#define WLC_E_IF                        54
#define WLC_E_P2P_DISC_LISTEN_COMPLETE  55
#define WLC_E_RSSI                      56
#define WLC_E_ESCAN_RESULT              69
#define WLC_E_STATUS_SUCCESS            0
#define WLC_E_STATUS_FAIL               1
#define WLC_E_STATUS_TIMEOUT            2
#define WLC_E_STATUS_NO_NETWORKS        3
#define WLC_E_STATUS_ABORT              4
#define WLC_E_STATUS_PARTIAL            8
#define WLC_E_LINK_BCN_LOSS             1
#define WLC_E_LINK_DISASSOC             2
#define WLC_EVENT_MSG_LINK              0x01
#define WLC_E_IF_ADD                    1
#define WLC_E_IF_DEL                    2
#define WLC_E_IF_FLAG_NOIF              1
#define WLC_E_IF_ROLE_STA               0
#define WL_EVENT_MSG_VERSION            2
#define WL_EVENT_MASK_LEN               24
#define EVENTMSGS_VER                   1
#define EVENTMSGS_SET_BIT               1
#define EVENTMSGS_RESET_BIT             2
#define EVENTMSGS_SET_MASK              3
#define EVENTMSGS_HDR_LEN               4
#define ETH_P_BRCM                      0x886c
#define BCMILCP_SUBTYPE_VENDOR_LONG     0x8001
#define BCMILCP_BCM_SUBTYPE_EVENT       1
/* Ethernet, BCM and event headers of an event packet */
#define WL_EVENT_HDR_LEN                (14 + 10 + 48)

/* Bands, bandwidths, chanspecs (D11AC encoding) */
#define WLC_BAND_5G                     1
#define WLC_BAND_2G                     2
#define WLC_BW_CAP_20MHZ                0x1
#define WLC_BW_CAP_40MHZ                0x3
#define WLC_BW_CAP_80MHZ                0x7
#define WL_CHANSPEC_BW_20               0x1000
#define WL_CHANSPEC_BW_40               0x1800
#define WL_CHANSPEC_BW_80               0x2000
#define WL_CHANSPEC_BW_MASK             0x3800
#define WL_CHANSPEC_BAND_2G             0x0000
#define WL_CHANSPEC_BAND_5G             0xc000
#define WL_CHANSPEC_BAND_MASK           0xc000
#define WL_CHANSPEC_SB_SHIFT            8
#define WL_CHAN_VALID_HW                (1 << 0)
#define WL_CHAN_VALID_SW                (1 << 1)
#define WL_CHAN_BAND_5G                 (1 << 2)
#define WL_CHAN_RADAR                   (1 << 3)
#define WL_CHAN_PASSIVE                 (1 << 5)
#define WLC_IOCTL_VERSION               2
#define WLC_PHY_TYPE_AC                 11

/* Scans */
#define WL_ESCAN_ACTION_START           1
#define WL_ESCAN_ACTION_ABORT           3
#define WL_SCAN_PARAMS_FIXED_LEN        64
#define WL_ESCAN_PARAMS_FIXED_LEN       (8 + WL_SCAN_PARAMS_FIXED_LEN)
#define WL_ESCAN_RESULTS_FIXED_LEN      12
#define WL_BSS_INFO_VERSION             109
#define WL_SCANTYPE_PASSIVE             1
#define WL_SCAN_ACTIVE_TIME             40
#define WL_SCAN_PASSIVE_TIME            110
#define WL_MAX_SCAN_CHANS               64

/* 802.11 frames (IEEE Std 802.11-2020, clause 9), without FCS */
#define DOT11_FC(type, subtype)         ((type) << 2 | (subtype) << 4)
#define DOT11_FC_TYPE(fc)               (((fc) >> 2) & 0x3)
#define DOT11_FC_SUBTYPE(fc)            (((fc) >> 4) & 0xf)
#define DOT11_TYPE_MGMT                 0
#define DOT11_TYPE_DATA                 2
#define DOT11_ST_ASSOC_REQ              0
#define DOT11_ST_ASSOC_RESP             1
#define DOT11_ST_REASSOC_REQ            2
#define DOT11_ST_REASSOC_RESP           3
#define DOT11_ST_PROBE_REQ              4
#define DOT11_ST_PROBE_RESP             5
#define DOT11_ST_BEACON                 8
#define DOT11_ST_DISASSOC               10
#define DOT11_ST_AUTH                   11
#define DOT11_ST_DEAUTH                 12
#define DOT11_ST_DATA                   0
#define DOT11_ST_QOS_DATA               8
#define DOT11_ST_NODATA                 0x4
#define DOT11_FC_TODS                   0x0100
#define DOT11_FC_FROMDS                 0x0200
#define DOT11_FC_PROTECTED              0x4000
#define DOT11_HDR_LEN                   24
#define DOT11_QOS_LEN                   2
#define DOT11_MAX_MPDU                  2346
#define DOT11_MAX_MSDU                  2304
#define DOT11_BCN_FIXED_LEN             12
#define DOT11_LLC_SNAP_LEN              8
#define DOT11_CAP_ESS                   0x0001
#define DOT11_CAP_PRIVACY               0x0010
#define DOT11_CAP_SHORT_PREAMBLE        0x0020
#define DOT11_CAP_SHORT_SLOT            0x0400
#define DOT11_AUTH_OPEN                 0
#define DOT11_SC_SUCCESS                0
#define DOT11_SC_FAILURE                1
#define DOT11_SC_AUTH_ALG               13
#define DOT11_SC_ASSOC_BUSY             17
#define DOT11_SC_INVALID_IE             40
#define DOT11_RC_UNSPECIFIED            1
#define DOT11_RC_DEAUTH_LEAVING         3
#define DOT11_RC_NOT_AUTH               6
#define DOT11_RC_NOT_ASSOC              7
#define DOT11_AID_BITS                  0xc000
#define DOT11_IE_SSID                   0
#define DOT11_IE_RATES                  1
#define DOT11_IE_DS                     3
#define DOT11_IE_TIM                    5
#define DOT11_IE_COUNTRY                7
#define DOT11_IE_ERP                    42
#define DOT11_IE_HT_CAP                 45
#define DOT11_IE_RSN                    48
#define DOT11_IE_EXT_RATES              50
#define DOT11_IE_HT_OP                  61
#define DOT11_IE_VHT_CAP                191
#define DOT11_IE_VHT_OP                 192
#define DOT11_IE_VENDOR                 221
#define DOT11_MAX_SSID                  32
/* microseconds in a time unit */
#define DOT11_TU                        1024
/* Wi-Fi Alliance vendor IEs, cipher suites */
#define WFA_TYPE_WPA                    1
#define WFA_TYPE_WMM                    2
#define WMM_SUBTYPE_INFO                0
#define WMM_SUBTYPE_PARAM               1
#define WPA_CIPHER_TKIP                 2
#define WPA_CIPHER_CCMP                 4
#define WPA_CIPHER_BIP                  6
#define WL_ETH_P_802_3_MIN              0x0600
#define WL_ETH_P_AARP                   0x80f3
#define WL_ETH_P_IPX                    0x8137
#define WL_ETH_P_EAPOL                  0x888e

/* ARP and neighbour discovery offload: modes, host addresses */
#define WL_ARP_OL_AGENT                 0x01
#define WL_ARP_OL_PEER_AUTO_REPLY       0x08
#define WL_MAX_HOSTIP                   8
#define WL_ARP_LEN                      28
#define WL_ARP_REQUEST                  1
#define WL_ARP_REPLY                    2
#define WL_IP6_HDR_LEN                  40
#define WL_IP6_NEXT_ICMP6               58
#define WL_ICMP6_NS                     135
#define WL_ICMP6_NA                     136
#define WL_ICMP6_NA_LEN                 32
#define WL_ICMP6_NA_SOLICITED           0x40
#define WL_ICMP6_NA_OVERRIDE            0x20
#define WL_ND_OPT_TARGET_LLADDR         2

/* Preferred network offload: parameter, channel, network sizes */
#define WL_PFN_VERSION                  2
#define WL_PFN_PARAM_LEN                24
#define WL_PFN_MAX_CHANS                64
#define WL_PFN_CFG_LEN                  (12 + 2 * WL_PFN_MAX_CHANS)
#define WL_PFN_NET_LEN                  56
#define WL_PFN_BSSID_LEN                8

/* RSSI events (struct wl_rssi_event): rate limit, number, levels */
#define WL_MAX_RSSI_LEVELS              8
#define WL_RSSI_EVENT_LEN               (5 + WL_MAX_RSSI_LEVELS)
#define WL_RSSI_LEVEL_NONE              0xff
/* HT and VHT capabilities of the chip: one stream, short GI, STBC rx */
#define WL_HT_CAP_40MHZ                 0x0002
#define WL_HT_CAP_SGI20                 0x0020
#define WL_HT_CAP_SGI40                 0x0040
#define WL_HT_CAP_RX_STBC1              0x0100
#define WL_VHT_CAP_INFO                 0x00001020

/* Radiotap header (https://www.radiotap.org) of the frames on the NIC */
#define RADIOTAP_FLAGS                  1
#define RADIOTAP_CHANNEL                3
#define RADIOTAP_DBM_ANTSIGNAL          5
#define RADIOTAP_DBM_TX_POWER           10
#define RADIOTAP_EXT                    31
#define RADIOTAP_F_FCS                  0x10
#define RADIOTAP_CHAN_OFDM              0x0040
#define RADIOTAP_CHAN_2GHZ              0x0080
#define RADIOTAP_CHAN_5GHZ              0x0100
#define WL_RADIOTAP_LEN                 16

/* CLM blob download */
#define DL_BEGIN                        0x0002
#define DL_END                          0x0004
#define DL_TYPE_CLM                     2
#define WL_DLOAD_HDR_LEN                12
#define WL_CLM_MAX                      (16 * KiB)

/* Security settings (wsec, wpa_auth) */
#define WL_WSEC_WEP                     0x0001
#define WL_WSEC_TKIP                    0x0002
#define WL_WSEC_AES                     0x0004
#define WL_WPA_AUTH_UNSPECIFIED         0x0002
#define WL_WPA_AUTH_PSK                 0x0004
#define WL_WPA2_AUTH_UNSPECIFIED        0x0040
#define WL_WPA2_AUTH_PSK                0x0080
#define WL_WPA2_AUTH_1X_SHA256          0x1000
#define WL_WPA2_AUTH_PSK_SHA256         0x8000
#define WL_WPA3_AUTH_SAE_PSK            0x40000
#define WL_WPA_AUTH_WPA1                (WL_WPA_AUTH_UNSPECIFIED | \
                                         WL_WPA_AUTH_PSK)
#define WL_WPA_AUTH_RSN                 (WL_WPA2_AUTH_UNSPECIFIED | \
                                         WL_WPA2_AUTH_PSK | \
                                         WL_WPA2_AUTH_1X_SHA256 | \
                                         WL_WPA2_AUTH_PSK_SHA256 | \
                                         WL_WPA3_AUTH_SAE_PSK)
#define WL_MFP_CAPABLE                  1
#define WL_MFP_REQUIRED                 2

/* bss_info flags */
#define WL_BSS_FLAGS_FROM_BEACON        0x01
#define WL_BSS_FLAGS_RSSI_ONCHANNEL     0x04
#define WL_BSS_INFO_FIXED_LEN           128

/* Station flags (sta_info) */
#define WL_STA_WME                      0x00000002
#define WL_STA_AUTHE                    0x00000008
#define WL_STA_ASSOC                    0x00000010
#define WL_STA_AUTHO                    0x00000020
#define WL_STA_N_CAP                    0x00002000
#define WL_STA_SCBSTATS                 0x00004000
#define WL_STA_INFO_VER                 4
#define WL_STA_INFO_LEN                 172

/* Vendor IEs of the host, by frame (vndr_ie) */
#define WL_VNDR_IE_BEACON               0x01
#define WL_VNDR_IE_PRBRSP               0x02
#define WL_VNDR_IE_ASSOCRSP             0x04
#define WL_VNDR_IE_PRBREQ               0x10
#define WL_VNDR_IE_ASSOCREQ             0x20
#define WL_VNDR_IE_HDR_LEN              12
/* join parameters (struct wl_extjoin_params), wsec_key (struct wl_wsec_key) */
#define WL_EXT_JOIN_FIXED_LEN           68
#define WL_JOIN_FIXED_LEN               48
#define WL_WSEC_KEY_LEN                 164
#define WL_MAX_VNDR_IE                  8

#define WL_MAX_BSS                      4
/* Stations of the access point */
#define WL_MAX_SCB                      10
#define WL_MAX_WPAIE                    128
#define WL_MAX_BSS_IES                  1024
#define WL_MAX_ASSOC_IES                512
#define WL_MAX_SCAN_SSIDS               4
#define WL_MAX_SCAN_SEEN                32
/* Join: tries and wait of authentication and association */
#define WL_JOIN_TRIES                   3
#define WL_JOIN_TIMEOUT                 300
#define WL_JOIN_ACTIVE_TIME             320
#define WL_LISTEN_INTERVAL              10
/* Radio: noise floor, sensitivity, power of senders that do not say */
#define WL_NOISE                        -92
/* 5 GHz subbands of the NVRAM power limits: UNII-1, -2A, -2C, -3 */
#define WL_NUM_5G_SUBBANDS              4
#define WL_MAXP_NONE                    0xff
#define WL_SENSITIVITY                  -90
#define WL_TX_POWER_UNKNOWN             20
/* P2P discovery states */
#define WL_P2P_DISC_ST_SCAN             0
#define WL_P2P_DISC_ST_LISTEN           1
#define WL_P2P_DISC_ST_SEARCH           2
#define WL_MAX_PMKID                    16
#define WL_PMKSA_LEN                    (6 + 16)
#define WL_STR_LEN                      160
#define WL_MAX_DCMD_LEN                 8192
#define WL_MAX_VAR_NAME                 64
/* events a command causes, sent after its reply */
#define WL_MAX_EVENTS                   (WL_MAX_SCB + 4)
#define WL_MAX_EVENT_DATA               64

/* The firmware's capabilities ("cap"), as the CM4's firmware reports them */
static const char wl_cap[] =
    "ap sta wme 802.11d 802.11h rm cqa cac dualband ampdu ampdu_tx "
    "ampdu_rx amsdurx radio_pwrsave btamp p2p proptxstatus mchan p2po "
    "anqpo vht-prop-rates dfrts txpwrcache stbc-tx stbc-rx-1ss epno pfnx "
    "wnm bsstrans mfp idauth ";

/*
 * Build time of the wl part of known firmware images, which the "ver"
 * string gives and the image trailer does not; other images give the
 * trailer's date.
 */
static const struct {
    const char *fwid;
    const char *date;
} wl_fw_dates[] = {
    /* cyfmac43455-sdio-minimal.bin 7.45.241 (Raspberry Pi OS bookworm) */
    { "01-5209c09", "Nov  1 2021 00:37:12" },
};

/* Integer variables of the firmware */
enum {
    WL_IV_MPC,
    WL_IV_BCN_TIMEOUT,
    WL_IV_ROAM_OFF,
    WL_IV_TXBF,
    WL_IV_PFN,
    WL_IV_ARPOE,
    WL_IV_ARP_OL,
    WL_IV_NDOE,
    WL_IV_OBSS_COEX,
    WL_IV_APSTA,
    WL_IV_ALLMULTI,
    WL_IV_TXGLOM,
    WL_IV_TXGLOMALIGN,
    WL_IV_RXGLOM,
    WL_IV_AMPDU_HOSTREORDER,
    WL_IV_TLV,
    WL_IV_PM2_SLEEP_RET,
    WL_IV_QTXPOWER,
    WL_IV_FRAGTHRESH,
    WL_IV_RTSTHRESH,
    WL_IV_PFNMEM,
    WL_IV_MCHAN,
    WL_IV_P2P_DISC,
    WL_NUM_IVARS,
};

/* Integer variables of each BSS configuration */
enum {
    WL_BV_WSEC,
    WL_BV_WPA_AUTH,
    WL_BV_AUTH,
    WL_BV_MFP,
    WL_BV_CLOSEDNET,
    WL_BV_WME_BSS_DISABLE,
    WL_NUM_BVARS,
};

/* Integer state set and read by firmware commands */
enum {
    WL_CV_INFRA,
    WL_CV_SRL,
    WL_CV_LRL,
    WL_CV_RADIO,
    WL_CV_BCNPRD,
    WL_CV_DTIMPRD,
    WL_CV_PM,
    WL_CV_MONITOR,
    WL_CV_AP,
    WL_CV_PROMISC,
    WL_CV_PASSIVE_SCAN,
    WL_CV_SCAN_CHANNEL_TIME,
    WL_CV_SCAN_UNASSOC_TIME,
    WL_CV_SCAN_PASSIVE_TIME,
    WL_CV_FAKEFRAG,
    WL_CV_ASSOC_PREFER,
    WL_CV_SCB_TIMEOUT,
    WL_CV_REGULATORY,
    WL_CV_KEY_PRIMARY,
    WL_NUM_CVARS,
};

/* A vendor IE the host adds to the frames in flags */
typedef struct WlVndrIe {
    uint32_t flags;
    uint8_t ie[2 + 255];
} WlVndrIe;

typedef struct WlBss {
    bool enabled;
    uint8_t ifidx;
    uint8_t mac[ETH_ALEN];
    int32_t var[WL_NUM_BVARS];
    WlVndrIe vndr[WL_MAX_VNDR_IE];
    /* WPA or RSN IE of association requests */
    uint8_t wpaie[WL_MAX_WPAIE];
    uint32_t wpaie_len;
} WlBss;

/* Join and link states of a station */
enum {
    WL_JOIN_IDLE,
    WL_JOIN_SCAN,
    WL_JOIN_AUTH,
    WL_JOIN_ASSOC,
    WL_JOIN_ASSOCIATED,
};

/* A BSS: the one a station joins, or the access point's own */
typedef struct WlPeer {
    uint8_t bssid[ETH_ALEN];
    uint8_t ssid[DOT11_MAX_SSID];
    uint8_t ssid_len;
    uint16_t chanspec;
    uint16_t capability;
    uint16_t beacon_period;
    uint8_t dtim_period;
    int8_t rssi;
    bool wme;
    bool from_beacon;
    uint16_t aid;
    /* IEs of its beacon or probe response */
    uint8_t ies[WL_MAX_BSS_IES];
    uint32_t ies_len;
} WlPeer;

/* A station of the access point */
enum {
    WL_SCB_NONE,
    WL_SCB_AUTH,
    WL_SCB_ASSOC,
};

typedef struct WlScb {
    uint8_t state;
    bool authorized;
    bool wme;
    bool ht;
    uint8_t mac[ETH_ALEN];
    uint16_t aid;
    uint16_t capability;
    int8_t rssi;
    int64_t assoc_time;
    int64_t last_rx;
    uint32_t tx_pkts;
    uint32_t rx_pkts;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
} WlScb;

struct BCM4345Wl {
    BCM4345State *s;

    /* From the downloaded firmware image, NVRAM and CLM blob */
    char ver[WL_STR_LEN];
    char clmver[WL_STR_LEN];
    uint32_t nvramrev;
    /* the board's power limits, quarter dBm: 2.4 GHz, the 5 GHz subbands */
    uint8_t maxp2g;
    uint8_t maxp5g[WL_NUM_5G_SUBBANDS];

    bool up;
    int32_t ivar[WL_NUM_IVARS];
    int32_t cvar[WL_NUM_CVARS];
    uint8_t event_mask[WL_EVENT_MASK_LEN];
    uint32_t bw_cap_2g;
    uint32_t bw_cap_5g;
    uint16_t chanspec;
    uint8_t join_pref[16];
    uint32_t join_pref_len;
    uint8_t pfn_mac[ETH_ALEN];
    uint8_t p2p_da[ETH_ALEN];
    /* PMKSA cache of the host: count, BSSID and PMKID of each */
    uint8_t pmkid_info[4 + WL_MAX_PMKID * WL_PMKSA_LEN];

    /* CLM blob download */
    uint8_t clm[WL_CLM_MAX];
    uint32_t clm_len;
    int32_t clm_status;

    /* P2P discovery: state, listen channel and time */
    QEMUTimer *listen_timer;
    uint8_t p2p_state;
    uint16_t p2p_chspec;
    uint16_t p2p_dwell;

    /* Scan */
    QEMUTimer *scan_timer;
    bool scanning;
    uint8_t scan_bss;
    uint16_t scan_sync_id;
    uint8_t scan_type;
    int32_t scan_active_time;
    int32_t scan_passive_time;
    uint32_t scan_nchan;
    uint32_t scan_idx;
    uint16_t scan_chan[WL_MAX_SCAN_CHANS];
    uint32_t scan_nssid;
    uint8_t scan_ssid[WL_MAX_SCAN_SSIDS][1 + DOT11_MAX_SSID];
    /* BSSs reported so far */
    uint32_t scan_nseen;
    uint8_t scan_seen[WL_MAX_SCAN_SEEN][ETH_ALEN];

    WlBss bss[WL_MAX_BSS];

    /* Radio */
    uint16_t tx_seq;

    /* Station: join, then the link to the access point */
    QEMUTimer *join_timer;
    uint8_t join_state;
    uint8_t join_tries;
    bool join_any_bssid;
    uint32_t join_nchan;
    uint32_t join_idx;
    uint16_t join_chan[WL_MAX_SCAN_CHANS];
    int32_t join_active_time;
    int32_t join_passive_time;
    WlPeer cur;
    int64_t assoc_time;
    int64_t last_beacon;
    uint8_t assoc_req_ies[WL_MAX_ASSOC_IES];
    uint32_t assoc_req_len;
    uint8_t assoc_resp_ies[WL_MAX_ASSOC_IES];
    uint32_t assoc_resp_len;
    uint16_t assoc_resp_cap;
    uint16_t assoc_status;
    uint32_t sta_tx_pkts;
    uint32_t sta_rx_pkts;
    uint64_t sta_tx_bytes;
    uint64_t sta_rx_bytes;
    QEMUTimer *link_timer;
    /* RSSI events: rate limit, levels, the level and time last reported */
    uint32_t rssi_ev_rate;
    uint8_t rssi_ev_num;
    int8_t rssi_ev_level[WL_MAX_RSSI_LEVELS];
    uint8_t rssi_ev_idx;
    int64_t rssi_ev_time;

    /* ARP and neighbour solicitation offload: the host's addresses */
    uint8_t arp_hostip[WL_MAX_HOSTIP][4];
    uint8_t nd_hostip[WL_MAX_HOSTIP][16];

    /* Access point: its BSS, beacons and stations */
    bool ap_up;
    QEMUTimer *bcn_timer;
    uint8_t dtim_count;
    WlScb scb[WL_MAX_SCB];

    /* Events of the command in progress */
    bool in_control;
    unsigned num_events;
    struct {
        uint8_t hdr[BDC_HEADER_LEN + WL_EVENT_HDR_LEN];
        uint8_t data[WL_MAX_EVENT_DATA];
        size_t len;
    } events[WL_MAX_EVENTS];
};

/* Locale: the channels of the firmware's country (DE/0) */

typedef struct WlLocale {
    char ccode[3];
    int32_t rev;
    uint8_t chan_2g[14];
    uint8_t chan_5g[32];
    /* radar detection (DFS) on 5 GHz channels in these ranges */
    uint8_t radar_lo[2];
    uint8_t radar_hi[2];
    /* subbands of the country IE: first channel, channels, dBm */
    uint8_t country_2g[1][3];
    uint8_t country_5g[3][3];
} WlLocale;

static const WlLocale wl_locales[] = {
    {
        .ccode = "DE",
        .chan_2g = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 },
        .chan_5g = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112,
                     116, 120, 124, 128, 132, 136, 140 },
        .radar_lo = { 52, 100 },
        .radar_hi = { 64, 140 },
        .country_2g = { { 1, 13, 20 } },
        .country_5g = { { 36, 4, 23 }, { 52, 4, 20 }, { 100, 11, 27 } },
    },
};

static const WlLocale *wl_locale(BCM4345Wl *wl)
{
    return &wl_locales[0];
}

static bool wl_chan_valid(const WlLocale *loc, bool is5g, unsigned ch)
{
    const uint8_t *list = is5g ? loc->chan_5g : loc->chan_2g;
    size_t n = is5g ? sizeof(loc->chan_5g) : sizeof(loc->chan_2g);
    size_t i;

    for (i = 0; i < n && list[i]; i++) {
        if (list[i] == ch) {
            return true;
        }
    }
    return false;
}

static bool wl_chan_radar(const WlLocale *loc, unsigned ch)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(loc->radar_lo); i++) {
        if (ch >= loc->radar_lo[i] && ch <= loc->radar_hi[i]) {
            return true;
        }
    }
    return false;
}

/*
 * The 40 and 80 MHz channels of the 5 GHz band are aligned blocks of 2
 * and 4 channels starting at 36, 52, 100, 116, ...; on 2.4 GHz any two
 * channels 4 apart form a 40 MHz channel.
 */
static int wl_block_start(bool is5g, unsigned ch, unsigned width)
{
    unsigned span = width / 5;

    if (!is5g) {
        return -1;
    }
    if (ch >= 36 && ch <= 64) {
        return 36 + (ch - 36) / span * span;
    }
    if (ch >= 100 && ch <= 144) {
        return 100 + (ch - 100) / span * span;
    }
    if (ch >= 149 && ch <= 165) {
        return 149 + (ch - 149) / span * span;
    }
    return -1;
}

static bool wl_block_valid(const WlLocale *loc, unsigned start,
                           unsigned width)
{
    unsigned ch;

    for (ch = start; ch < start + width / 5; ch += 4) {
        if (!wl_chan_valid(loc, true, ch)) {
            return false;
        }
    }
    return true;
}

/* Append the chanspecs of a band and bandwidth to a chanspec list */
static void wl_add_chanspecs(BCM4345Wl *wl, bool is5g, unsigned bw,
                             uint8_t *buf, size_t len, uint32_t *count)
{
    const WlLocale *loc = wl_locale(wl);
    const uint8_t *list = is5g ? loc->chan_5g : loc->chan_2g;
    size_t n = is5g ? sizeof(loc->chan_5g) : sizeof(loc->chan_2g);
    uint16_t band = is5g ? WL_CHANSPEC_BAND_5G : WL_CHANSPEC_BAND_2G;
    size_t i;

    for (i = 0; i < n && list[i]; i++) {
        unsigned ch = list[i];
        uint16_t cs;
        int start;

        switch (bw) {
        case WL_CHANSPEC_BW_20:
            cs = band | WL_CHANSPEC_BW_20 | ch;
            break;
        case WL_CHANSPEC_BW_40:
            if (is5g) {
                start = wl_block_start(true, ch, 40);
                if (start < 0 || !wl_block_valid(loc, start, 40)) {
                    continue;
                }
                cs = band | WL_CHANSPEC_BW_40 | (start + 2) |
                     ((ch - start) / 4) << WL_CHANSPEC_SB_SHIFT;
            } else if (wl_chan_valid(loc, false, ch + 4)) {
                /* control channel lower, then (below) upper */
                cs = band | WL_CHANSPEC_BW_40 | (ch + 2);
            } else {
                continue;
            }
            break;
        case WL_CHANSPEC_BW_80:
            start = wl_block_start(is5g, ch, 80);
            if (start < 0 || !wl_block_valid(loc, start, 80)) {
                continue;
            }
            cs = band | WL_CHANSPEC_BW_80 | (start + 6) |
                 ((ch - start) / 4) << WL_CHANSPEC_SB_SHIFT;
            break;
        default:
            g_assert_not_reached();
        }
        if (4 + 4 * (*count + 1) > len) {
            return;
        }
        stl_le_p(buf + 4 + 4 * (*count)++, cs);
        if (!is5g && bw == WL_CHANSPEC_BW_40 && ch > 4 &&
            wl_chan_valid(loc, false, ch - 4)) {
            if (4 + 4 * (*count + 1) > len) {
                return;
            }
            stl_le_p(buf + 4 + 4 * (*count)++,
                     band | WL_CHANSPEC_BW_40 | (ch - 2) |
                     1 << WL_CHANSPEC_SB_SHIFT);
        }
    }
}

/* Control channel of a chanspec */
static unsigned wl_chanspec_ctl(uint16_t cs)
{
    unsigned center = cs & 0xff;
    unsigned sb = (cs >> WL_CHANSPEC_SB_SHIFT) & 0x7;

    switch (cs & WL_CHANSPEC_BW_MASK) {
    case WL_CHANSPEC_BW_40:
        return center - 2 + sb * 4;
    case WL_CHANSPEC_BW_80:
        return center - 6 + sb * 4;
    default:
        return center;
    }
}

static bool wl_chanspec_is5g(uint16_t cs)
{
    return (cs & WL_CHANSPEC_BAND_MASK) == WL_CHANSPEC_BAND_5G;
}

/* Events */

static void wl_event_flags(BCM4345Wl *wl, WlBss *bss, uint32_t type,
                           uint16_t flags, uint32_t status, uint32_t reason,
                           const uint8_t *addr, const void *data, size_t len)
{
    uint8_t hdr[BDC_HEADER_LEN + WL_EVENT_HDR_LEN];
    uint8_t *e = hdr + BDC_HEADER_LEN;
    uint8_t *m = e + 24;
    unsigned bssidx = bss - wl->bss;

    if (type >= WL_EVENT_MASK_LEN * 8 ||
        !(wl->event_mask[type / 8] & (1 << (type % 8)))) {
        return;
    }

    memset(hdr, 0, sizeof(hdr));
    /*
     * BDC header: the host interface of the BSS; interface events on the
     * primary one, as the host has no interface for a new BSS yet
     */
    hdr[0] = BCDC_PROTO_VER << BCDC_FLAG_VER_SHIFT;
    hdr[2] = type == WLC_E_IF ? 0 : bss->ifidx;
    /* Ethernet header */
    memcpy(e, bss->mac, ETH_ALEN);
    memcpy(e + 6, bss->mac, ETH_ALEN);
    stw_be_p(e + 12, ETH_P_BRCM);
    /* BCM header: subtype, length, version, OUI, user subtype */
    stw_be_p(e + 14, BCMILCP_SUBTYPE_VENDOR_LONG);
    stw_be_p(e + 16, 6 + 48 + len);
    e[18] = 0;
    e[19] = 0x00;
    e[20] = 0x10;
    e[21] = 0x18;
    stw_be_p(e + 22, BCMILCP_BCM_SUBTYPE_EVENT);
    /* event message */
    stw_be_p(m, WL_EVENT_MSG_VERSION);
    stw_be_p(m + 2, flags);
    stl_be_p(m + 4, type);
    stl_be_p(m + 8, status);
    stl_be_p(m + 12, reason);
    stl_be_p(m + 16, 0);
    stl_be_p(m + 20, len);
    if (addr) {
        memcpy(m + 24, addr, ETH_ALEN);
    }
    snprintf((char *)m + 30, 16, bssidx ? "wl0.%u" : "wl0", bssidx);
    m[46] = bss->ifidx;
    m[47] = bssidx;

    trace_bcm4345_wl_event(type, status, bssidx);
    if (wl->in_control) {
        assert(wl->num_events < WL_MAX_EVENTS && len <= WL_MAX_EVENT_DATA);
        memcpy(wl->events[wl->num_events].hdr, hdr, sizeof(hdr));
        memcpy(wl->events[wl->num_events].data, data, len);
        wl->events[wl->num_events++].len = len;
        return;
    }
    bcm4345_send(wl->s, SDPCM_EVENT_CHANNEL, hdr, sizeof(hdr), data, len);
}

static void wl_event(BCM4345Wl *wl, WlBss *bss, uint32_t type,
                     uint32_t status, uint32_t reason, const uint8_t *addr,
                     const void *data, size_t len)
{
    wl_event_flags(wl, bss, type, 0, status, reason, addr, data, len);
}

static void wl_flush_events(BCM4345Wl *wl)
{
    unsigned i;

    for (i = 0; i < wl->num_events; i++) {
        bcm4345_send(wl->s, SDPCM_EVENT_CHANNEL, wl->events[i].hdr,
                     sizeof(wl->events[i].hdr), wl->events[i].data,
                     wl->events[i].len);
    }
    wl->num_events = 0;
}

/* Firmware image, NVRAM and CLM blob */

/* "Mon 2021-11-01 00:40:07" -> "Nov  1 2021 00:40:07" */
static bool wl_parse_date(const char *p, char *out, size_t len)
{
    static const char months[12][4] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    unsigned y, mo, d, h, mi, sec;

    if (sscanf(p, "%*3s %4u-%2u-%2u %2u:%2u:%2u", &y, &mo, &d, &h, &mi,
               &sec) != 6 || mo < 1 || mo > 12) {
        return false;
    }
    snprintf(out, len, "%s %2u %u %02u:%02u:%02u", months[mo - 1], d, y,
             h, mi, sec);
    return true;
}

/*
 * The image ends with its identification, for example
 * "43455c0-roml/43455_sdio-... Version: 7.45.241 (1a2f2fa CY) CRC: 349757d5
 * Date: Mon 2021-11-01 00:40:07 PDT Ucode Ver: 1043.2164 FWID 01-5209c09".
 */
static bool wl_parse_id(BCM4345Wl *wl, const char *id)
{
    const char *v, *crc, *date, *fwid;
    char version[64], when[32], fwidstr[16];
    size_t i;

    v = strstr(id, " Version: ");
    crc = v ? strstr(v, " CRC: ") : NULL;
    date = crc ? strstr(crc, " Date: ") : NULL;
    fwid = date ? strstr(date, " FWID 01-") : NULL;
    if (!fwid || crc - v - 10 >= sizeof(version) ||
        !wl_parse_date(date + 7, when, sizeof(when))) {
        return false;
    }
    pstrcpy(version, crc - v - 10 + 1, v + 10);
    pstrcpy(fwidstr, sizeof(fwidstr), fwid + 6);
    fwidstr[strcspn(fwidstr, "\n ")] = '\0';
    for (i = 0; i < ARRAY_SIZE(wl_fw_dates); i++) {
        if (!strcmp(fwidstr, wl_fw_dates[i].fwid)) {
            pstrcpy(when, sizeof(when), wl_fw_dates[i].date);
        }
    }
    snprintf(wl->ver, sizeof(wl->ver), "wl0: %s version %s FWID %s\n",
             when, version, fwidstr);
    return true;
}

static void wl_parse_image(BCM4345Wl *wl)
{
    const uint8_t *ram = wl->s->ram;
    const uint8_t *end = ram + BCM4345_RAM_SIZE;
    const uint8_t *p = ram;

    pstrcpy(wl->ver, sizeof(wl->ver),
            "wl0: Nov  1 2021 00:37:12 version 7.45.241 (1a2f2fa CY) "
            "FWID 01-5209c09\n");
    /* the first string with a version, date and FWID */
    while ((p = memmem(p, end - p, "FWID 01-", 8))) {
        const uint8_t *id = p;

        while (id > ram && id[-1]) {
            id--;
        }
        if (memchr(p, 0, end - p) && wl_parse_id(wl, (const char *)id)) {
            return;
        }
        p += 8;
    }
}

/*
 * The host writes the NVRAM at the end of the RAM, followed by a token
 * with its length in words in the last word; "NVRAMRev=$Rev: 498373 $"
 * gives the revision the firmware reports, maxp2ga0 and maxp5ga0 the
 * board's transmit power limits.
 */
static void wl_parse_nvram(BCM4345Wl *wl)
{
    const uint8_t *ram = wl->s->ram;
    uint32_t token = ldl_le_p(ram + BCM4345_RAM_SIZE - 4);
    uint32_t words = token & 0xffff;
    const char *p, *end;

    wl->nvramrev = 0;
    wl->maxp2g = WL_MAXP_NONE;
    memset(wl->maxp5g, WL_MAXP_NONE, sizeof(wl->maxp5g));
    if (((token >> 16) ^ words) != 0xffff || !words ||
        words * 4 + 4 > BCM4345_RAM_SIZE) {
        return;
    }
    end = (const char *)ram + BCM4345_RAM_SIZE - 4;
    p = end - words * 4;
    while (p < end && *p) {
        unsigned v[WL_NUM_5G_SUBBANDS];
        int n;

        if (sscanf(p, "maxp2ga0=%u", &v[0]) == 1) {
            wl->maxp2g = MIN(v[0], WL_MAXP_NONE);
        }
        /* one value for all subbands, or one each */
        n = sscanf(p, "maxp5ga0=%u,%u,%u,%u", &v[0], &v[1], &v[2], &v[3]);
        for (int i = 0; n > 0 && i < WL_NUM_5G_SUBBANDS; i++) {
            wl->maxp5g[i] = MIN(v[MIN(i, n - 1)], WL_MAXP_NONE);
        }
        sscanf(p, "NVRAMRev=$Rev: %u", &wl->nvramrev);
        p += strnlen(p, end - p) + 1;
    }
}

/*
 * CLM blob: "BLOB" container, the CLM data at the offset in its first
 * entry: "CLM DATA", API version, data name, compiler, importer and
 * customization strings; the creation date is the first date in it.
 */
static int wl_parse_clm(BCM4345Wl *wl)
{
    const uint8_t *b = wl->clm;
    uint32_t len = wl->clm_len;
    uint32_t off;
    const uint8_t *c, *p;
    char name[21], compiler[15], import[31], custom[23], created[20];

    if (len < 28 || memcmp(b, "BLOB", 4)) {
        return BCME_BADARG;
    }
    off = ldl_le_p(b + 24);
    if (off > len || len - off < 104 || memcmp(b + off, "CLM DATA", 8)) {
        return BCME_BADARG;
    }
    c = b + off;
    pstrcpy(name, sizeof(name), (const char *)c + 14);
    pstrcpy(compiler, sizeof(compiler), (const char *)c + 34);
    pstrcpy(import, sizeof(import), (const char *)c + 52);
    pstrcpy(custom, sizeof(custom), (const char *)c + 82);
    created[0] = '\0';
    for (p = c + 104; p + 19 <= b + len; p++) {
        unsigned y, mo, d, h, mi, s;
        char t[20];

        if (p[4] != '-' || p[10] != ' ') {
            continue;
        }
        memcpy(t, p, 19);
        t[19] = '\0';
        if (sscanf(t, "%4u-%2u-%2u %2u:%2u:%2u", &y, &mo, &d, &h, &mi,
                   &s) == 6) {
            pstrcpy(created, sizeof(created), t);
            break;
        }
    }
    snprintf(wl->clmver, sizeof(wl->clmver),
             "API: %u.%u\nData: %s\nCompiler: %s\n%s\nCustomization: %s\n"
             "Creation: %s\n", lduw_le_p(c + 10), lduw_le_p(c + 12), name,
             compiler, import, custom, created);
    return BCME_OK;
}

/* Radio: 802.11 frames with a radiotap header on the NIC */

static unsigned wl_chan_freq(uint16_t cs)
{
    unsigned ch = wl_chanspec_ctl(cs);

    if (wl_chanspec_is5g(cs)) {
        return 5000 + 5 * ch;
    }
    return ch == 14 ? 2484 : 2407 + 5 * ch;
}

static uint16_t wl_chanspec_20(bool is5g, unsigned ch)
{
    return (is5g ? WL_CHANSPEC_BAND_5G : WL_CHANSPEC_BAND_2G) |
           WL_CHANSPEC_BW_20 | ch;
}

/* The channel the radio is on */
static uint16_t wl_radio_chanspec(BCM4345Wl *wl)
{
    if (wl->scanning) {
        return wl->scan_chan[wl->scan_idx];
    }
    if (wl->join_state == WL_JOIN_SCAN) {
        return wl->join_chan[wl->join_idx];
    }
    if (wl->p2p_state == WL_P2P_DISC_ST_LISTEN &&
        timer_pending(wl->listen_timer)) {
        return wl->p2p_chspec;
    }
    return wl->chanspec;
}

/*
 * The power on channel CS, dBm: qtxpower (quarter dBm; the host reads it
 * back as is, 31 dBm by default) within the board's limit for the band
 */
static int wl_tx_power(BCM4345Wl *wl, uint16_t cs)
{
    unsigned q = wl->ivar[WL_IV_QTXPOWER] & 0xff;
    unsigned ch = wl_chanspec_ctl(cs);
    unsigned limit;

    if (!wl_chanspec_is5g(cs)) {
        limit = wl->maxp2g;
    } else {
        limit = wl->maxp5g[ch <= 48 ? 0 : ch <= 64 ? 1 : ch <= 144 ? 2 : 3];
    }
    return MIN(q, limit) / 4;
}

typedef struct WlFrame {
    uint8_t buf[WL_RADIOTAP_LEN + DOT11_MAX_MPDU];
    size_t len;
} WlFrame;

static void wl_put(WlFrame *f, const void *data, size_t len)
{
    assert(f->len + len <= sizeof(f->buf));
    memcpy(f->buf + f->len, data, len);
    f->len += len;
}

static void wl_put_le16(WlFrame *f, uint16_t v)
{
    uint8_t b[2];

    stw_le_p(b, v);
    wl_put(f, b, sizeof(b));
}

static void wl_put_ie(WlFrame *f, uint8_t id, const void *data, size_t len)
{
    uint8_t h[2] = { id, len };

    assert(len <= UINT8_MAX);
    wl_put(f, h, sizeof(h));
    wl_put(f, data, len);
}

static void wl_frame_init(BCM4345Wl *wl, WlFrame *f, uint16_t fc,
                          const uint8_t *a1, const uint8_t *a2,
                          const uint8_t *a3)
{
    f->len = WL_RADIOTAP_LEN;
    wl_put_le16(f, fc);
    wl_put_le16(f, 0);
    wl_put(f, a1, ETH_ALEN);
    wl_put(f, a2, ETH_ALEN);
    wl_put(f, a3, ETH_ALEN);
    wl_put_le16(f, (wl->tx_seq++ & 0xfff) << 4);
}

static void wl_air_send(BCM4345Wl *wl, WlFrame *f, uint16_t cs)
{
    uint8_t *rt = f->buf;

    /* flags, channel, transmit power */
    memset(rt, 0, WL_RADIOTAP_LEN);
    stw_le_p(rt + 2, WL_RADIOTAP_LEN);
    stl_le_p(rt + 4, BIT(RADIOTAP_FLAGS) | BIT(RADIOTAP_CHANNEL) |
             BIT(RADIOTAP_DBM_TX_POWER));
    stw_le_p(rt + 10, wl_chan_freq(cs));
    stw_le_p(rt + 12, RADIOTAP_CHAN_OFDM |
             (wl_chanspec_is5g(cs) ? RADIOTAP_CHAN_5GHZ : RADIOTAP_CHAN_2GHZ));
    rt[14] = wl_tx_power(wl, cs);
    trace_bcm4345_wl_air_tx(lduw_le_p(rt + WL_RADIOTAP_LEN), wl_chan_freq(cs),
                            f->len - WL_RADIOTAP_LEN);
    qemu_send_packet(qemu_get_queue(wl->s->nic), f->buf, f->len);
}

/* A frame from the air */
typedef struct WlRx {
    unsigned freq;
    int rssi;
    uint16_t fc;
    uint8_t tid;
    const uint8_t *a1;
    const uint8_t *a2;
    const uint8_t *a3;
    const uint8_t *body;
    size_t len;
} WlRx;

static bool wl_rx_parse(BCM4345Wl *wl, const uint8_t *buf, size_t len,
                        WlRx *rx)
{
    /* size and alignment of the radiotap fields up to the TX power */
    static const uint8_t fields[][2] = {
        { 8, 8 }, { 1, 1 }, { 1, 1 }, { 4, 2 }, { 2, 1 }, { 1, 1 },
        { 1, 1 }, { 2, 2 }, { 2, 2 }, { 2, 2 }, { 1, 1 },
    };
    uint32_t present, p;
    size_t hlen, off, hdr;
    uint8_t flags = 0;
    int signal = INT_MIN, txpower = WL_TX_POWER_UNKNOWN;
    unsigned i;

    if (len < 8 || buf[0] != 0) {
        return false;
    }
    hlen = lduw_le_p(buf + 2);
    if (hlen < 8 || hlen > len) {
        return false;
    }
    present = p = ldl_le_p(buf + 4);
    off = 8;
    while (p & BIT(RADIOTAP_EXT)) {
        if (off + 4 > hlen) {
            return false;
        }
        p = ldl_le_p(buf + off);
        off += 4;
    }
    rx->freq = 0;
    for (i = 0; i < ARRAY_SIZE(fields); i++) {
        if (!(present & BIT(i))) {
            continue;
        }
        off = ROUND_UP(off, fields[i][1]);
        if (off + fields[i][0] > hlen) {
            return false;
        }
        switch (i) {
        case RADIOTAP_FLAGS:
            flags = buf[off];
            break;
        case RADIOTAP_CHANNEL:
            rx->freq = lduw_le_p(buf + off);
            break;
        case RADIOTAP_DBM_ANTSIGNAL:
            signal = (int8_t)buf[off];
            break;
        case RADIOTAP_DBM_TX_POWER:
            txpower = (int8_t)buf[off];
            break;
        }
        off += fields[i][0];
    }
    buf += hlen;
    len -= hlen;
    if (flags & RADIOTAP_F_FCS) {
        if (len < 4) {
            return false;
        }
        len -= 4;
    }

    /* management and data frames of three addresses */
    if (len < DOT11_HDR_LEN) {
        return false;
    }
    rx->fc = lduw_le_p(buf);
    if (rx->fc & 0x3 || DOT11_FC_TYPE(rx->fc) & 1 ||
        (rx->fc & (DOT11_FC_TODS | DOT11_FC_FROMDS)) ==
        (DOT11_FC_TODS | DOT11_FC_FROMDS)) {
        return false;
    }
    hdr = DOT11_HDR_LEN;
    rx->tid = 0;
    if (DOT11_FC_TYPE(rx->fc) == DOT11_TYPE_DATA &&
        DOT11_FC_SUBTYPE(rx->fc) & DOT11_ST_QOS_DATA) {
        if (len < hdr + DOT11_QOS_LEN) {
            return false;
        }
        rx->tid = buf[hdr] & 0x7;
        hdr += DOT11_QOS_LEN;
    }
    rx->a1 = buf + 4;
    rx->a2 = buf + 10;
    rx->a3 = buf + 16;
    rx->body = buf + hdr;
    rx->len = len - hdr;
    /* the signal the sender gives, or its power less the path loss */
    if (signal == INT_MIN) {
        signal = txpower - wl->s->path_loss;
    }
    rx->rssi = MAX(signal, INT8_MIN);
    return true;
}

/* Information elements */

static const uint8_t wl_bcast[ETH_ALEN] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
static const uint8_t wl_oui_wfa[3] = { 0x00, 0x50, 0xf2 };
static const uint8_t wl_oui_ieee[3] = { 0x00, 0x0f, 0xac };

static const uint8_t *wl_find_ie(const uint8_t *ies, size_t len, uint8_t id)
{
    while (len >= 2 && ies[1] + 2u <= len) {
        if (ies[0] == id) {
            return ies;
        }
        len -= ies[1] + 2;
        ies += ies[1] + 2;
    }
    return NULL;
}

/* A Wi-Fi Alliance vendor IE of a type (and subtype, unless negative) */
static const uint8_t *wl_find_wfa_ie(const uint8_t *ies, size_t len,
                                     uint8_t type, int subtype)
{
    while (len >= 2 && ies[1] + 2u <= len) {
        if (ies[0] == DOT11_IE_VENDOR && ies[1] >= 5 &&
            !memcmp(ies + 2, wl_oui_wfa, 3) && ies[5] == type &&
            (subtype < 0 || ies[6] == subtype)) {
            return ies;
        }
        len -= ies[1] + 2;
        ies += ies[1] + 2;
    }
    return NULL;
}

/* The length of the whole IEs in at most max bytes */
static size_t wl_ie_fit(const uint8_t *ies, size_t len, size_t max)
{
    size_t n = 0;

    while (n + 2 <= len && n + 2 + ies[n + 1] <= MIN(len, max)) {
        n += 2 + ies[n + 1];
    }
    return n;
}

static bool wl_wpa(WlBss *bss)
{
    return bss->var[WL_BV_WPA_AUTH] & (WL_WPA_AUTH_WPA1 | WL_WPA_AUTH_RSN);
}

static bool wl_privacy(WlBss *bss)
{
    return bss->var[WL_BV_WSEC] & (WL_WSEC_WEP | WL_WSEC_TKIP | WL_WSEC_AES);
}

static bool wl_wme(WlBss *bss)
{
    return !bss->var[WL_BV_WME_BSS_DISABLE];
}

static uint16_t wl_capability(WlBss *bss, bool is5g)
{
    uint16_t cap = DOT11_CAP_ESS;

    if (wl_privacy(bss)) {
        cap |= DOT11_CAP_PRIVACY;
    }
    if (!is5g) {
        cap |= DOT11_CAP_SHORT_PREAMBLE | DOT11_CAP_SHORT_SLOT;
    }
    return cap;
}

static void wl_put_rates(WlFrame *f, bool is5g)
{
    /* 802.11b/g: 1, 2, 5.5, 11 (basic), 6, 9, 12, 18; 802.11a */
    static const uint8_t rates_2g[] = {
        0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,
    };
    static const uint8_t rates_5g[] = {
        0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c,
    };

    wl_put_ie(f, DOT11_IE_RATES, is5g ? rates_5g : rates_2g, 8);
}

static void wl_put_ext_rates(WlFrame *f, bool is5g)
{
    /* 24, 36, 48, 54 */
    static const uint8_t rates[] = { 0x30, 0x48, 0x60, 0x6c };

    if (!is5g) {
        wl_put_ie(f, DOT11_IE_EXT_RATES, rates, sizeof(rates));
    }
}

static void wl_put_ht_cap(BCM4345Wl *wl, WlFrame *f, bool is5g)
{
    uint8_t ht[26] = { 0 };
    uint16_t cap = WL_HT_CAP_SGI20 | WL_HT_CAP_RX_STBC1;

    if ((is5g ? wl->bw_cap_5g : wl->bw_cap_2g) & 0x2) {
        cap |= WL_HT_CAP_40MHZ | WL_HT_CAP_SGI40;
    }
    /* capabilities, A-MPDU: 64 KiB, 4 us spacing, MCS 0-7 */
    stw_le_p(ht, cap);
    ht[2] = 0x17;
    ht[3] = 0xff;
    ht[15] = 0x01;
    wl_put_ie(f, DOT11_IE_HT_CAP, ht, sizeof(ht));
}

static void wl_put_ht_op(WlFrame *f, uint16_t cs)
{
    uint8_t op[22] = { 0 };

    /* primary channel, secondary channel offset, any channel width */
    op[0] = wl_chanspec_ctl(cs);
    if ((cs & WL_CHANSPEC_BW_MASK) != WL_CHANSPEC_BW_20) {
        op[1] = ((cs >> WL_CHANSPEC_SB_SHIFT) & 1 ? 3 : 1) | 0x04;
    }
    wl_put_ie(f, DOT11_IE_HT_OP, op, sizeof(op));
}

static void wl_put_vht_cap(WlFrame *f)
{
    uint8_t cap[12] = { 0 };

    /* capabilities, receive and transmit MCS maps: one stream, MCS 0-9 */
    stl_le_p(cap, WL_VHT_CAP_INFO);
    stw_le_p(cap + 4, 0xfffe);
    stw_le_p(cap + 8, 0xfffe);
    wl_put_ie(f, DOT11_IE_VHT_CAP, cap, sizeof(cap));
}

static void wl_put_vht_op(WlFrame *f, uint16_t cs)
{
    uint8_t op[5] = { 0 };

    /* channel width, center frequency segments, basic MCS map */
    if ((cs & WL_CHANSPEC_BW_MASK) == WL_CHANSPEC_BW_80) {
        op[0] = 1;
        op[1] = cs & 0xff;
    }
    stw_le_p(op + 3, 0xfffc);
    wl_put_ie(f, DOT11_IE_VHT_OP, op, sizeof(op));
}

/* The RSN IE, or the WPA IE, of the security settings of a BSS */
static void wl_put_wpa_ie(WlFrame *f, WlBss *bss, bool rsn)
{
    static const struct {
        uint32_t auth;
        uint8_t suite;
    } akms[] = {
        { WL_WPA_AUTH_UNSPECIFIED, 1 },
        { WL_WPA_AUTH_PSK, 2 },
        { WL_WPA2_AUTH_UNSPECIFIED, 1 },
        { WL_WPA2_AUTH_PSK, 2 },
        { WL_WPA2_AUTH_1X_SHA256, 5 },
        { WL_WPA2_AUTH_PSK_SHA256, 6 },
        { WL_WPA3_AUTH_SAE_PSK, 8 },
    };
    const uint8_t *oui = rsn ? wl_oui_ieee : wl_oui_wfa;
    uint32_t wsec = bss->var[WL_BV_WSEC];
    uint32_t auth = bss->var[WL_BV_WPA_AUTH] &
                    (rsn ? WL_WPA_AUTH_RSN : WL_WPA_AUTH_WPA1);
    int32_t mfp = rsn ? bss->var[WL_BV_MFP] : 0;
    uint8_t ie[64];
    size_t n = 0, count;
    uint16_t cap = 0;
    size_t i;

    if (!rsn) {
        memcpy(ie, wl_oui_wfa, 3);
        ie[3] = WFA_TYPE_WPA;
        n = 4;
    }
    /* version, group cipher */
    stw_le_p(ie + n, 1);
    n += 2;
    memcpy(ie + n, oui, 3);
    ie[n + 3] = wsec & WL_WSEC_TKIP ? WPA_CIPHER_TKIP : WPA_CIPHER_CCMP;
    n += 4;
    /* pairwise ciphers */
    count = n;
    n += 2;
    if (wsec & WL_WSEC_AES) {
        memcpy(ie + n, oui, 3);
        ie[n + 3] = WPA_CIPHER_CCMP;
        n += 4;
    }
    if (wsec & WL_WSEC_TKIP) {
        memcpy(ie + n, oui, 3);
        ie[n + 3] = WPA_CIPHER_TKIP;
        n += 4;
    }
    stw_le_p(ie + count, (n - count - 2) / 4);
    /* key management */
    count = n;
    n += 2;
    for (i = 0; i < ARRAY_SIZE(akms); i++) {
        if (auth & akms[i].auth) {
            memcpy(ie + n, oui, 3);
            ie[n + 3] = akms[i].suite;
            n += 4;
        }
    }
    stw_le_p(ie + count, (n - count - 2) / 4);
    if (rsn) {
        /* capabilities: 16 PTKSA replay counters with WME, MFP */
        if (wl_wme(bss)) {
            cap |= 0x000c;
        }
        if (mfp >= WL_MFP_CAPABLE) {
            cap |= 0x0080;
        }
        if (mfp == WL_MFP_REQUIRED) {
            cap |= 0x0040;
        }
        stw_le_p(ie + n, cap);
        n += 2;
        if (mfp) {
            /* no PMKIDs, the group management cipher BIP */
            stw_le_p(ie + n, 0);
            memcpy(ie + n + 2, wl_oui_ieee, 3);
            ie[n + 5] = WPA_CIPHER_BIP;
            n += 6;
        }
    }
    wl_put_ie(f, rsn ? DOT11_IE_RSN : DOT11_IE_VENDOR, ie, n);
}

static const uint8_t wl_wmm_ac_default[16] = {
    /* ACI/AIFSN, ECWmin/ECWmax, TXOP limit: BE, BK, VI, VO */
    0x03, 0xa4, 0x00, 0x00,
    0x27, 0xa4, 0x00, 0x00,
    0x42, 0x43, 0x5e, 0x00,
    0x62, 0x32, 0x2f, 0x00,
};

static void wl_put_wmm_param(WlFrame *f)
{
    /* OUI, type, subtype, version, QoS info, reserved, AC parameters */
    uint8_t wmm[8 + sizeof(wl_wmm_ac_default)] = {
        0x00, 0x50, 0xf2, WFA_TYPE_WMM, WMM_SUBTYPE_PARAM, 0x01, 0x00, 0x00,
    };

    memcpy(wmm + 8, wl_wmm_ac_default, sizeof(wl_wmm_ac_default));
    wl_put_ie(f, DOT11_IE_VENDOR, wmm, sizeof(wmm));
}

static void wl_put_wmm_info(WlFrame *f)
{
    /* OUI, type, subtype, version, QoS info */
    static const uint8_t wmm[] = {
        0x00, 0x50, 0xf2, WFA_TYPE_WMM, WMM_SUBTYPE_INFO, 0x01, 0x00,
    };

    wl_put_ie(f, DOT11_IE_VENDOR, wmm, sizeof(wmm));
}

static void wl_put_country(BCM4345Wl *wl, WlFrame *f, bool is5g)
{
    const WlLocale *loc = wl_locale(wl);
    const uint8_t (*sub)[3] = is5g ? loc->country_5g : loc->country_2g;
    size_t nsub = is5g ? ARRAY_SIZE(loc->country_5g)
                       : ARRAY_SIZE(loc->country_2g);
    uint8_t c[3 + 3 * ARRAY_SIZE(loc->country_5g) + 1];
    size_t n = 3, i;

    /* country, any environment, subbands: first channel, count, dBm */
    memcpy(c, loc->ccode, 2);
    c[2] = 0x20;
    for (i = 0; i < nsub && sub[i][0]; i++) {
        memcpy(c + n, sub[i], 3);
        n += 3;
    }
    if (n & 1) {
        c[n++] = 0;
    }
    wl_put_ie(f, DOT11_IE_COUNTRY, c, n);
}

static void wl_put_vndr_ies(WlFrame *f, WlBss *bss, uint32_t flag)
{
    size_t i;

    for (i = 0; i < WL_MAX_VNDR_IE; i++) {
        const WlVndrIe *v = &bss->vndr[i];
        size_t n = v->ie[1] + 2;

        if (v->flags & flag && f->len + n <= sizeof(f->buf)) {
            wl_put(f, v->ie, n);
        }
    }
}

/*
 * Body of the beacons and probe responses of the access point:
 * timestamp, beacon interval, capabilities, IEs
 */
static void wl_put_bss_body(BCM4345Wl *wl, WlFrame *f, unsigned subtype)
{
    WlBss *bss = &wl->bss[0];
    uint16_t cs = wl->cur.chanspec;
    bool is5g = wl_chanspec_is5g(cs);
    uint32_t auth = bss->var[WL_BV_WPA_AUTH];
    uint8_t ts[8], ch = wl_chanspec_ctl(cs), erp = 0;

    stq_le_p(ts, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL));
    wl_put(f, ts, sizeof(ts));
    wl_put_le16(f, wl->cur.beacon_period);
    wl_put_le16(f, wl_capability(bss, is5g));
    wl_put_ie(f, DOT11_IE_SSID, wl->cur.ssid,
              subtype == DOT11_ST_BEACON && bss->var[WL_BV_CLOSEDNET]
              ? 0 : wl->cur.ssid_len);
    wl_put_rates(f, is5g);
    wl_put_ie(f, DOT11_IE_DS, &ch, 1);
    if (subtype == DOT11_ST_BEACON) {
        /* DTIM count and period, no traffic buffered */
        uint8_t tim[4] = { wl->dtim_count, wl->cur.dtim_period, 0, 0 };

        wl_put_ie(f, DOT11_IE_TIM, tim, sizeof(tim));
    }
    if (wl->cvar[WL_CV_REGULATORY]) {
        wl_put_country(wl, f, is5g);
    }
    if (!is5g) {
        wl_put_ie(f, DOT11_IE_ERP, &erp, 1);
    }
    wl_put_ext_rates(f, is5g);
    if (auth & WL_WPA_AUTH_RSN) {
        wl_put_wpa_ie(f, bss, true);
    }
    wl_put_ht_cap(wl, f, is5g);
    wl_put_ht_op(f, cs);
    if (is5g) {
        wl_put_vht_cap(f);
        wl_put_vht_op(f, cs);
    }
    if (auth & WL_WPA_AUTH_WPA1) {
        wl_put_wpa_ie(f, bss, false);
    }
    if (wl_wme(bss)) {
        wl_put_wmm_param(f);
    }
    wl_put_vndr_ies(f, bss, subtype == DOT11_ST_BEACON ? WL_VNDR_IE_BEACON
                                                       : WL_VNDR_IE_PRBRSP);
}

static void wl_send_mgmt(BCM4345Wl *wl, unsigned subtype, const uint8_t *da,
                         const uint8_t *bssid, const void *body, size_t len)
{
    WlFrame f;

    wl_frame_init(wl, &f, DOT11_FC(DOT11_TYPE_MGMT, subtype), da,
                  wl->bss[0].mac, bssid);
    wl_put(&f, body, len);
    wl_air_send(wl, &f, wl->chanspec);
}

/* A deauthentication or disassociation */
static void wl_send_reason(BCM4345Wl *wl, unsigned subtype, const uint8_t *da,
                           const uint8_t *bssid, uint16_t reason)
{
    uint8_t b[2];

    stw_le_p(b, reason);
    wl_send_mgmt(wl, subtype, da, bssid, b, sizeof(b));
}

static void wl_send_probe_req(BCM4345Wl *wl, WlBss *bss, uint16_t cs,
                              const uint8_t *ssid, size_t ssid_len,
                              const uint8_t *bssid)
{
    bool is5g = wl_chanspec_is5g(cs);
    uint8_t ch = wl_chanspec_ctl(cs);
    WlFrame f;

    wl_frame_init(wl, &f, DOT11_FC(DOT11_TYPE_MGMT, DOT11_ST_PROBE_REQ),
                  wl_bcast, bss->mac, bssid ? bssid : wl_bcast);
    wl_put_ie(&f, DOT11_IE_SSID, ssid, ssid_len);
    wl_put_rates(&f, is5g);
    wl_put_ext_rates(&f, is5g);
    wl_put_ie(&f, DOT11_IE_DS, &ch, 1);
    wl_put_ht_cap(wl, &f, is5g);
    if (is5g) {
        wl_put_vht_cap(&f);
    }
    wl_put_vndr_ies(&f, bss, WL_VNDR_IE_PRBREQ);
    wl_air_send(wl, &f, cs);
}

/* BSSs heard */

/* The chanspec of a BSS, as wide as this radio can follow it */
static uint16_t wl_peer_chanspec(BCM4345Wl *wl, bool is5g, unsigned ch,
                                 const uint8_t *htop, const uint8_t *vhtop)
{
    uint32_t cap = is5g ? wl->bw_cap_5g : wl->bw_cap_2g;
    uint16_t band = is5g ? WL_CHANSPEC_BAND_5G : WL_CHANSPEC_BAND_2G;

    if (is5g && vhtop && vhtop[1] >= 3 && vhtop[2] == 1 && cap & 0x4 &&
        ch + 6 >= vhtop[3] && ch <= vhtop[3] + 6u) {
        return band | WL_CHANSPEC_BW_80 | vhtop[3] |
               ((ch + 6 - vhtop[3]) / 4) << WL_CHANSPEC_SB_SHIFT;
    }
    if (htop && htop[1] >= 2 && htop[3] & 0x4 && htop[3] & 0x1 &&
        cap & 0x2) {
        bool above = (htop[3] & 0x3) == 1;

        return band | WL_CHANSPEC_BW_40 | (above ? ch + 2 : ch - 2) |
               (above ? 0 : 1) << WL_CHANSPEC_SB_SHIFT;
    }
    return wl_chanspec_20(is5g, ch);
}

/* A BSS from the body of its beacon or probe response */
static bool wl_peer_parse(BCM4345Wl *wl, WlPeer *p, const WlRx *rx,
                          bool beacon)
{
    const uint8_t *ies = rx->body + DOT11_BCN_FIXED_LEN;
    size_t len = rx->len - DOT11_BCN_FIXED_LEN;
    const uint8_t *ie, *htop;
    bool is5g = rx->freq >= 4900;
    unsigned ch;

    memset(p, 0, sizeof(*p));
    memcpy(p->bssid, rx->a3, ETH_ALEN);
    p->beacon_period = lduw_le_p(rx->body + 8);
    p->capability = lduw_le_p(rx->body + 10);
    p->rssi = rx->rssi;
    p->from_beacon = beacon;
    ie = wl_find_ie(ies, len, DOT11_IE_SSID);
    if (!ie || ie[1] > DOT11_MAX_SSID) {
        return false;
    }
    p->ssid_len = ie[1];
    memcpy(p->ssid, ie + 2, ie[1]);
    ie = wl_find_ie(ies, len, DOT11_IE_TIM);
    if (ie && ie[1] >= 2) {
        p->dtim_period = ie[3];
    }
    htop = wl_find_ie(ies, len, DOT11_IE_HT_OP);
    ie = wl_find_ie(ies, len, DOT11_IE_DS);
    if (ie && ie[1] >= 1) {
        ch = ie[2];
    } else if (htop && htop[1] >= 1) {
        ch = htop[2];
    } else {
        ch = is5g ? (rx->freq - 5000) / 5 : (rx->freq - 2407) / 5;
    }
    if (!wl_chan_valid(wl_locale(wl), is5g, ch)) {
        return false;
    }
    p->chanspec = wl_peer_chanspec(wl, is5g, ch, htop,
                                   wl_find_ie(ies, len, DOT11_IE_VHT_OP));
    p->wme = wl_find_wfa_ie(ies, len, WFA_TYPE_WMM, WMM_SUBTYPE_PARAM);
    p->ies_len = wl_ie_fit(ies, len, sizeof(p->ies));
    memcpy(p->ies, ies, p->ies_len);
    return true;
}

/* bss_info (version 109) of a BSS */
static size_t wl_bss_info(uint8_t *bi, const WlPeer *p)
{
    static const uint8_t rate_ies[] = { DOT11_IE_RATES, DOT11_IE_EXT_RATES };
    const uint8_t *ie;
    size_t n = 0, i, k;

    memset(bi, 0, WL_BSS_INFO_FIXED_LEN);
    stl_le_p(bi, WL_BSS_INFO_VERSION);
    stl_le_p(bi + 4, WL_BSS_INFO_FIXED_LEN + p->ies_len);
    memcpy(bi + 8, p->bssid, ETH_ALEN);
    stw_le_p(bi + 14, p->beacon_period);
    stw_le_p(bi + 16, p->capability);
    bi[18] = p->ssid_len;
    memcpy(bi + 19, p->ssid, p->ssid_len);
    /* rate set */
    for (i = 0; i < ARRAY_SIZE(rate_ies); i++) {
        ie = wl_find_ie(p->ies, p->ies_len, rate_ies[i]);
        for (k = 0; ie && k < ie[1] && n < 16; k++) {
            bi[56 + n++] = ie[2 + k];
        }
    }
    stl_le_p(bi + 52, n);
    stw_le_p(bi + 72, p->chanspec);
    bi[76] = p->dtim_period;
    stw_le_p(bi + 78, (int16_t)p->rssi);
    bi[80] = (uint8_t)WL_NOISE;
    ie = wl_find_ie(p->ies, p->ies_len, DOT11_IE_HT_CAP);
    if (ie && ie[1] >= 2) {
        bi[81] = 1;
        stl_le_p(bi + 84, lduw_le_p(ie + 2));
    }
    bi[88] = wl_chanspec_ctl(p->chanspec);
    bi[96] = WL_BSS_FLAGS_RSSI_ONCHANNEL |
             (p->from_beacon ? WL_BSS_FLAGS_FROM_BEACON : 0);
    /* basic MCS set */
    ie = wl_find_ie(p->ies, p->ies_len, DOT11_IE_HT_OP);
    if (ie && ie[1] >= 22) {
        memcpy(bi + 100, ie + 8, 16);
    }
    stw_le_p(bi + 116, WL_BSS_INFO_FIXED_LEN);
    stl_le_p(bi + 120, p->ies_len);
    stw_le_p(bi + 124, p->rssi - WL_NOISE);
    memcpy(bi + WL_BSS_INFO_FIXED_LEN, p->ies, p->ies_len);
    return WL_BSS_INFO_FIXED_LEN + p->ies_len;
}

/* Scan */

static void wl_scan_done(BCM4345Wl *wl, uint32_t status)
{
    uint8_t res[WL_ESCAN_RESULTS_FIXED_LEN];

    timer_del(wl->scan_timer);
    wl->scanning = false;
    stl_le_p(res, sizeof(res));
    stl_le_p(res + 4, WL_BSS_INFO_VERSION);
    stw_le_p(res + 8, wl->scan_sync_id);
    stw_le_p(res + 10, 0);
    trace_bcm4345_wl_scan_done(status);
    wl_event(wl, &wl->bss[wl->scan_bss], WLC_E_ESCAN_RESULT, status, 0,
             NULL, res, sizeof(res));
}

/* A BSS found, once a scan */
static void wl_scan_result(BCM4345Wl *wl, const WlPeer *p)
{
    uint8_t buf[WL_ESCAN_RESULTS_FIXED_LEN + WL_BSS_INFO_FIXED_LEN +
                WL_MAX_BSS_IES];
    size_t i, n;

    for (i = 0; i < wl->scan_nseen; i++) {
        if (!memcmp(wl->scan_seen[i], p->bssid, ETH_ALEN)) {
            return;
        }
    }
    if (wl->scan_nseen < WL_MAX_SCAN_SEEN) {
        memcpy(wl->scan_seen[wl->scan_nseen++], p->bssid, ETH_ALEN);
    }
    /* buffer length, version, sync ID, count, the BSS */
    n = WL_ESCAN_RESULTS_FIXED_LEN +
        wl_bss_info(buf + WL_ESCAN_RESULTS_FIXED_LEN, p);
    stl_le_p(buf, n);
    stl_le_p(buf + 4, WL_BSS_INFO_VERSION);
    stw_le_p(buf + 8, wl->scan_sync_id);
    stw_le_p(buf + 10, 1);
    wl_event(wl, &wl->bss[wl->scan_bss], WLC_E_ESCAN_RESULT,
             WLC_E_STATUS_PARTIAL, 0, NULL, buf, n);
}

/* Active scans probe, except on radar channels */
static bool wl_scan_active(BCM4345Wl *wl, uint16_t cs)
{
    return !(wl->scan_type & WL_SCANTYPE_PASSIVE) &&
           !(wl_chanspec_is5g(cs) &&
             wl_chan_radar(wl_locale(wl), wl_chanspec_ctl(cs)));
}

static void wl_scan_channel(BCM4345Wl *wl)
{
    uint16_t cs = wl->scan_chan[wl->scan_idx];
    WlBss *bss = &wl->bss[wl->scan_bss];
    bool active = wl_scan_active(wl, cs);
    size_t i;

    timer_mod(wl->scan_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              (active ? wl->scan_active_time : wl->scan_passive_time));
    if (!active) {
        return;
    }
    if (!wl->scan_nssid) {
        wl_send_probe_req(wl, bss, cs, NULL, 0, NULL);
    }
    for (i = 0; i < wl->scan_nssid; i++) {
        wl_send_probe_req(wl, bss, cs, wl->scan_ssid[i] + 1,
                          wl->scan_ssid[i][0], NULL);
    }
}

static void wl_scan_next(void *opaque)
{
    BCM4345Wl *wl = opaque;

    if (++wl->scan_idx >= wl->scan_nchan) {
        wl_scan_done(wl, WLC_E_STATUS_SUCCESS);
        return;
    }
    wl_scan_channel(wl);
}

static void wl_scan_start(BCM4345Wl *wl)
{
    wl->scanning = true;
    wl->scan_idx = 0;
    wl->scan_nseen = 0;
    trace_bcm4345_wl_scan(wl->scan_nchan);
    wl_scan_channel(wl);
}

/* Channels to scan: those listed and valid here, or all 20 MHz ones */
static uint32_t wl_scan_chans(BCM4345Wl *wl, uint16_t *chans,
                              const uint8_t *list, uint32_t n)
{
    uint8_t buf[4 + 4 * WL_MAX_SCAN_CHANS];
    uint32_t count = 0, i;

    for (i = 0; i < n; i++) {
        uint16_t cs = lduw_le_p(list + i * 2);

        if (wl_chan_valid(wl_locale(wl), wl_chanspec_is5g(cs),
                          wl_chanspec_ctl(cs))) {
            chans[count++] = cs;
        }
    }
    if (n) {
        return count;
    }
    wl_add_chanspecs(wl, false, WL_CHANSPEC_BW_20, buf, sizeof(buf), &count);
    wl_add_chanspecs(wl, true, WL_CHANSPEC_BW_20, buf, sizeof(buf), &count);
    for (i = 0; i < count; i++) {
        chans[i] = ldl_le_p(buf + 4 + 4 * i);
    }
    return count;
}

/*
 * Scan parameters (struct wl_scan_params): SSID, BSSID, BSS type, scan
 * type, probes, active, passive and home dwell times, the number of
 * channels (and SSIDs, in the upper half), the channels, the SSIDs.
 */
static int wl_scan_params(BCM4345Wl *wl, const uint8_t *p, size_t len)
{
    uint32_t n, nssid, sl;
    int32_t t;
    size_t i, off;

    if (len < WL_SCAN_PARAMS_FIXED_LEN) {
        return BCME_BUFTOOSHORT;
    }
    wl->scan_type = p[43];
    t = ldl_le_p(p + 48);
    wl->scan_active_time = t > 0 ? t : wl->cvar[WL_CV_SCAN_CHANNEL_TIME];
    t = ldl_le_p(p + 52);
    wl->scan_passive_time = t > 0 ? t : wl->cvar[WL_CV_SCAN_PASSIVE_TIME];
    n = ldl_le_p(p + 60) & 0xffff;
    nssid = ldl_le_p(p + 60) >> 16;
    if (n > WL_MAX_SCAN_CHANS || nssid > WL_MAX_SCAN_SSIDS ||
        WL_SCAN_PARAMS_FIXED_LEN + n * 2 > len) {
        return BCME_BADARG;
    }
    wl->scan_nchan = wl_scan_chans(wl, wl->scan_chan,
                                   p + WL_SCAN_PARAMS_FIXED_LEN, n);

    /* the SSIDs after the channels, or the one of the parameters */
    wl->scan_nssid = 0;
    off = ROUND_UP(WL_SCAN_PARAMS_FIXED_LEN + n * 2, 4);
    for (i = 0; i < nssid; i++, off += 4 + DOT11_MAX_SSID) {
        if (off + 4 + DOT11_MAX_SSID > len) {
            return BCME_BUFTOOSHORT;
        }
        sl = ldl_le_p(p + off);
        if (sl > DOT11_MAX_SSID) {
            return BCME_BADARG;
        }
        wl->scan_ssid[i][0] = sl;
        memcpy(wl->scan_ssid[i] + 1, p + off + 4, sl);
    }
    wl->scan_nssid = nssid;
    sl = ldl_le_p(p);
    if (!nssid && sl) {
        if (sl > DOT11_MAX_SSID) {
            return BCME_BADARG;
        }
        wl->scan_ssid[0][0] = sl;
        memcpy(wl->scan_ssid[0] + 1, p + 4, sl);
        wl->scan_nssid = 1;
    }
    return wl->scan_nchan ? BCME_OK : BCME_BADARG;
}

/* Station: join a BSS, then keep the link to it */

static void wl_join_fail(BCM4345Wl *wl, uint32_t type, uint32_t status,
                         uint32_t reason)
{
    WlBss *sta = &wl->bss[0];

    timer_del(wl->join_timer);
    wl->join_state = WL_JOIN_IDLE;
    trace_bcm4345_wl_join_done(type, status);
    if (type != WLC_E_SET_SSID) {
        wl_event(wl, sta, type, status, reason, wl->cur.bssid, NULL, 0);
        status = WLC_E_STATUS_FAIL;
    }
    wl_event(wl, sta, WLC_E_SET_SSID, status, 0, wl->cur.bssid, NULL, 0);
}

static void wl_join_scan_channel(BCM4345Wl *wl)
{
    uint16_t cs = wl->join_chan[wl->join_idx];
    bool active = !(wl_chanspec_is5g(cs) &&
                    wl_chan_radar(wl_locale(wl), wl_chanspec_ctl(cs)));

    timer_mod(wl->join_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              (active ? wl->join_active_time : wl->join_passive_time));
    if (active) {
        wl_send_probe_req(wl, &wl->bss[0], cs, wl->cur.ssid,
                          wl->cur.ssid_len,
                          wl->join_any_bssid ? NULL : wl->cur.bssid);
    }
}

static void wl_join_auth(BCM4345Wl *wl)
{
    /* algorithm, transaction, status */
    uint8_t b[6];

    stw_le_p(b, DOT11_AUTH_OPEN);
    stw_le_p(b + 2, 1);
    stw_le_p(b + 4, DOT11_SC_SUCCESS);
    wl->join_state = WL_JOIN_AUTH;
    timer_mod(wl->join_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + WL_JOIN_TIMEOUT);
    wl_send_mgmt(wl, DOT11_ST_AUTH, wl->cur.bssid, wl->cur.bssid, b,
                 sizeof(b));
}

static void wl_join_assoc(BCM4345Wl *wl)
{
    WlBss *sta = &wl->bss[0];
    bool is5g = wl_chanspec_is5g(wl->cur.chanspec);
    const uint8_t *ies = wl->cur.ies;
    size_t ilen = wl->cur.ies_len, start;
    WlFrame f;

    wl_frame_init(wl, &f, DOT11_FC(DOT11_TYPE_MGMT, DOT11_ST_ASSOC_REQ),
                  wl->cur.bssid, sta->mac, wl->cur.bssid);
    wl_put_le16(&f, wl_capability(sta, is5g));
    wl_put_le16(&f, WL_LISTEN_INTERVAL);
    start = f.len;
    wl_put_ie(&f, DOT11_IE_SSID, wl->cur.ssid, wl->cur.ssid_len);
    wl_put_rates(&f, is5g);
    wl_put_ext_rates(&f, is5g);
    if (sta->wpaie_len && sta->wpaie[0] == DOT11_IE_RSN) {
        wl_put(&f, sta->wpaie, sta->wpaie_len);
    }
    if (wl_find_ie(ies, ilen, DOT11_IE_HT_CAP)) {
        wl_put_ht_cap(wl, &f, is5g);
    }
    if (is5g && wl_find_ie(ies, ilen, DOT11_IE_VHT_CAP)) {
        wl_put_vht_cap(&f);
    }
    if (sta->wpaie_len && sta->wpaie[0] == DOT11_IE_VENDOR) {
        wl_put(&f, sta->wpaie, sta->wpaie_len);
    }
    if (wl->cur.wme && wl_wme(sta)) {
        wl_put_wmm_info(&f);
    }
    wl_put_vndr_ies(&f, sta, WL_VNDR_IE_ASSOCREQ);
    wl->assoc_req_len = wl_ie_fit(f.buf + start, f.len - start,
                                  sizeof(wl->assoc_req_ies));
    memcpy(wl->assoc_req_ies, f.buf + start, wl->assoc_req_len);

    wl->join_state = WL_JOIN_ASSOC;
    timer_mod(wl->join_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + WL_JOIN_TIMEOUT);
    wl_air_send(wl, &f, wl->chanspec);
}

static void wl_join_done(BCM4345Wl *wl)
{
    WlBss *sta = &wl->bss[0];
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    timer_del(wl->join_timer);
    wl->join_state = WL_JOIN_ASSOCIATED;
    wl->assoc_time = wl->last_beacon = now;
    wl->sta_tx_pkts = wl->sta_rx_pkts = 0;
    wl->sta_tx_bytes = wl->sta_rx_bytes = 0;
    wl->rssi_ev_idx = WL_RSSI_LEVEL_NONE;
    timer_mod(wl->link_timer, now + MAX(wl->ivar[WL_IV_BCN_TIMEOUT], 1) *
              1000);
    trace_bcm4345_wl_join_done(WLC_E_SET_SSID, WLC_E_STATUS_SUCCESS);
    wl_event(wl, sta, WLC_E_ASSOC, WLC_E_STATUS_SUCCESS, 0, wl->cur.bssid,
             NULL, 0);
    wl_event_flags(wl, sta, WLC_E_LINK, WLC_EVENT_MSG_LINK,
                   WLC_E_STATUS_SUCCESS, 0, wl->cur.bssid, NULL, 0);
    wl_event(wl, sta, WLC_E_SET_SSID, WLC_E_STATUS_SUCCESS, 0, wl->cur.bssid,
             wl->cur.ssid, wl->cur.ssid_len);
}

static void wl_join_timeout(void *opaque)
{
    BCM4345Wl *wl = opaque;

    switch (wl->join_state) {
    case WL_JOIN_SCAN:
        if (++wl->join_idx >= wl->join_nchan) {
            wl_join_fail(wl, WLC_E_SET_SSID, WLC_E_STATUS_NO_NETWORKS, 0);
            return;
        }
        wl_join_scan_channel(wl);
        break;
    case WL_JOIN_AUTH:
        if (++wl->join_tries >= WL_JOIN_TRIES) {
            wl_join_fail(wl, WLC_E_AUTH, WLC_E_STATUS_TIMEOUT, 0);
            return;
        }
        wl_join_auth(wl);
        break;
    case WL_JOIN_ASSOC:
        if (++wl->join_tries >= WL_JOIN_TRIES) {
            wl_join_fail(wl, WLC_E_ASSOC, WLC_E_STATUS_TIMEOUT, 0);
            return;
        }
        wl_join_assoc(wl);
        break;
    }
}

/* The link ends: the access point left (type), or its beacons stopped */
static void wl_link_down(BCM4345Wl *wl, uint32_t type, uint32_t reason)
{
    WlBss *sta = &wl->bss[0];

    timer_del(wl->link_timer);
    wl->join_state = WL_JOIN_IDLE;
    trace_bcm4345_wl_link_down(type, reason);
    if (type != WLC_E_LINK) {
        wl_event(wl, sta, type, WLC_E_STATUS_SUCCESS, reason, wl->cur.bssid,
                 NULL, 0);
        reason = WLC_E_LINK_DISASSOC;
    }
    wl_event_flags(wl, sta, WLC_E_LINK, 0, WLC_E_STATUS_SUCCESS, reason,
                   wl->cur.bssid, NULL, 0);
}

static void wl_link_check(void *opaque)
{
    BCM4345Wl *wl = opaque;
    int64_t timeout = MAX(wl->ivar[WL_IV_BCN_TIMEOUT], 1) * 1000;

    if (wl->join_state != WL_JOIN_ASSOCIATED) {
        return;
    }
    if (qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) - wl->last_beacon >= timeout) {
        wl_link_down(wl, WLC_E_LINK, WLC_E_LINK_BCN_LOSS);
        return;
    }
    timer_mod(wl->link_timer, wl->last_beacon + timeout);
}

/* The station leaves its BSS (with a frame of subtype), or stops joining */
static void wl_leave(BCM4345Wl *wl, unsigned subtype, uint16_t reason)
{
    if (wl->join_state == WL_JOIN_ASSOCIATED) {
        wl_send_reason(wl, subtype, wl->cur.bssid, wl->cur.bssid, reason);
    }
    timer_del(wl->join_timer);
    timer_del(wl->link_timer);
    wl->join_state = WL_JOIN_IDLE;
}

/*
 * Join (the "join" variable or WLC_SET_SSID): SSID, BSSID (broadcast:
 * any), chanspecs, dwell times of the join scan
 */
static int wl_join(BCM4345Wl *wl, const uint8_t *ssid, uint32_t ssid_len,
                   const uint8_t *bssid, const uint8_t *chans, uint32_t nchan,
                   int32_t active_time, int32_t passive_time)
{
    if (!wl->up) {
        return BCME_NOTUP;
    }
    if (wl->cvar[WL_CV_AP]) {
        return BCME_NOTSTA;
    }
    if (ssid_len > DOT11_MAX_SSID || nchan > WL_MAX_SCAN_CHANS) {
        return BCME_BADARG;
    }
    if (wl->scanning) {
        wl_scan_done(wl, WLC_E_STATUS_ABORT);
    }
    wl_leave(wl, DOT11_ST_DEAUTH, DOT11_RC_DEAUTH_LEAVING);

    memset(&wl->cur, 0, sizeof(wl->cur));
    memcpy(wl->cur.ssid, ssid, ssid_len);
    wl->cur.ssid_len = ssid_len;
    memcpy(wl->cur.bssid, bssid, ETH_ALEN);
    wl->join_any_bssid = is_broadcast_ether_addr(bssid) ||
                         !memcmp(bssid, (uint8_t[ETH_ALEN]) { 0 }, ETH_ALEN);
    wl->join_nchan = wl_scan_chans(wl, wl->join_chan, chans, nchan);
    if (!wl->join_nchan) {
        return BCME_BADARG;
    }
    wl->join_active_time = active_time > 0 ? active_time
                           : wl->cvar[WL_CV_SCAN_UNASSOC_TIME];
    wl->join_passive_time = passive_time > 0 ? passive_time
                            : wl->cvar[WL_CV_SCAN_PASSIVE_TIME];
    wl->join_idx = 0;
    wl->join_state = WL_JOIN_SCAN;
    trace_bcm4345_wl_join(ssid_len, wl->join_nchan);
    wl_join_scan_channel(wl);
    return BCME_OK;
}

static bool wl_join_match(BCM4345Wl *wl, const WlPeer *p)
{
    if (p->ssid_len != wl->cur.ssid_len ||
        memcmp(p->ssid, wl->cur.ssid, p->ssid_len) ||
        (!wl->join_any_bssid && memcmp(p->bssid, wl->cur.bssid, ETH_ALEN)) ||
        !(p->capability & DOT11_CAP_ESS)) {
        return false;
    }
    /* privacy as the station's security settings ask */
    return !(p->capability & DOT11_CAP_PRIVACY) == !wl_privacy(&wl->bss[0]);
}

static void wl_sta_rx_mgmt(BCM4345Wl *wl, const WlRx *rx, unsigned subtype)
{
    WlBss *sta = &wl->bss[0];
    bool to_us = !memcmp(rx->a1, sta->mac, ETH_ALEN);
    bool from_bss = !memcmp(rx->a2, wl->cur.bssid, ETH_ALEN) &&
                    !memcmp(rx->a3, wl->cur.bssid, ETH_ALEN);
    uint16_t status;

    switch (subtype) {
    case DOT11_ST_AUTH:
        if (wl->join_state != WL_JOIN_AUTH || !to_us || !from_bss ||
            rx->len < 6 || lduw_le_p(rx->body + 2) != 2) {
            return;
        }
        status = lduw_le_p(rx->body + 4);
        if (status != DOT11_SC_SUCCESS) {
            wl_join_fail(wl, WLC_E_AUTH, WLC_E_STATUS_FAIL, status);
            return;
        }
        wl_event(wl, sta, WLC_E_AUTH, WLC_E_STATUS_SUCCESS, DOT11_AUTH_OPEN,
                 wl->cur.bssid, NULL, 0);
        wl->join_tries = 0;
        wl_join_assoc(wl);
        break;
    case DOT11_ST_ASSOC_RESP:
    case DOT11_ST_REASSOC_RESP:
        /* capabilities, status, association ID, IEs */
        if (wl->join_state != WL_JOIN_ASSOC || !to_us || !from_bss ||
            rx->len < 6) {
            return;
        }
        status = lduw_le_p(rx->body + 2);
        if (status != DOT11_SC_SUCCESS) {
            wl_join_fail(wl, WLC_E_ASSOC, WLC_E_STATUS_FAIL, status);
            return;
        }
        wl->assoc_resp_cap = lduw_le_p(rx->body);
        wl->assoc_status = status;
        wl->cur.aid = lduw_le_p(rx->body + 4) & ~DOT11_AID_BITS;
        wl->assoc_resp_len = wl_ie_fit(rx->body + 6, rx->len - 6,
                                       sizeof(wl->assoc_resp_ies));
        memcpy(wl->assoc_resp_ies, rx->body + 6, wl->assoc_resp_len);
        wl->cur.wme = wl->cur.wme && wl_wme(sta) &&
                      wl_find_wfa_ie(wl->assoc_resp_ies, wl->assoc_resp_len,
                                     WFA_TYPE_WMM, WMM_SUBTYPE_PARAM);
        wl_join_done(wl);
        break;
    case DOT11_ST_DEAUTH:
    case DOT11_ST_DISASSOC:
        if ((!to_us && !is_broadcast_ether_addr(rx->a1)) || !from_bss ||
            rx->len < 2) {
            return;
        }
        if (wl->join_state == WL_JOIN_ASSOCIATED) {
            wl_link_down(wl, subtype == DOT11_ST_DEAUTH ? WLC_E_DEAUTH_IND
                                                        : WLC_E_DISASSOC_IND,
                         lduw_le_p(rx->body));
        } else if (wl->join_state == WL_JOIN_AUTH ||
                   wl->join_state == WL_JOIN_ASSOC) {
            wl_join_fail(wl, wl->join_state == WL_JOIN_AUTH ? WLC_E_AUTH
                                                            : WLC_E_ASSOC,
                         WLC_E_STATUS_FAIL, lduw_le_p(rx->body));
        }
        break;
    }
}

/* Access point */

static WlScb *wl_scb_find(BCM4345Wl *wl, const uint8_t *mac)
{
    size_t i;

    for (i = 0; i < WL_MAX_SCB; i++) {
        if (wl->scb[i].state != WL_SCB_NONE &&
            !memcmp(wl->scb[i].mac, mac, ETH_ALEN)) {
            return &wl->scb[i];
        }
    }
    return NULL;
}

/* A station authenticates: its old state, if any, is gone */
static WlScb *wl_scb_new(BCM4345Wl *wl, const uint8_t *mac)
{
    WlScb *scb = wl_scb_find(wl, mac);
    size_t i;

    for (i = 0; !scb && i < WL_MAX_SCB; i++) {
        if (wl->scb[i].state == WL_SCB_NONE) {
            scb = &wl->scb[i];
        }
    }
    if (scb) {
        memset(scb, 0, sizeof(*scb));
        memcpy(scb->mac, mac, ETH_ALEN);
    }
    return scb;
}

/* The lowest free association ID */
static uint16_t wl_scb_aid(BCM4345Wl *wl, WlScb *self)
{
    uint16_t aid;
    size_t i;

    for (aid = 1;; aid++) {
        for (i = 0; i < WL_MAX_SCB; i++) {
            if (&wl->scb[i] != self && wl->scb[i].state == WL_SCB_ASSOC &&
                wl->scb[i].aid == aid) {
                break;
            }
        }
        if (i == WL_MAX_SCB) {
            return aid;
        }
    }
}

static void wl_ap_beacon(void *opaque)
{
    BCM4345Wl *wl = opaque;
    WlBss *bss = &wl->bss[0];
    WlFrame f;

    if (!wl->ap_up) {
        return;
    }
    wl_frame_init(wl, &f, DOT11_FC(DOT11_TYPE_MGMT, DOT11_ST_BEACON),
                  wl_bcast, bss->mac, bss->mac);
    wl_put_bss_body(wl, &f, DOT11_ST_BEACON);
    wl_air_send(wl, &f, wl->cur.chanspec);
    wl->dtim_count = (wl->dtim_count ? wl->dtim_count
                                     : wl->cur.dtim_period) - 1;
    timer_mod_ns(wl->bcn_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 (int64_t)wl->cur.beacon_period * DOT11_TU * SCALE_US);
}

/* The IEs and capabilities of the access point's BSS, as it sends them */
static void wl_ap_update(BCM4345Wl *wl)
{
    WlFrame f = { .len = 0 };
    size_t ies = DOT11_BCN_FIXED_LEN;

    wl_put_bss_body(wl, &f, DOT11_ST_PROBE_RESP);
    wl->cur.capability = lduw_le_p(f.buf + ies - 2);
    wl->cur.wme = wl_wme(&wl->bss[0]);
    wl->cur.ies_len = wl_ie_fit(f.buf + ies, f.len - ies,
                                sizeof(wl->cur.ies));
    memcpy(wl->cur.ies, f.buf + ies, wl->cur.ies_len);
}

static void wl_ap_stop(BCM4345Wl *wl)
{
    if (!wl->ap_up) {
        return;
    }
    wl->ap_up = false;
    timer_del(wl->bcn_timer);
    memset(wl->scb, 0, sizeof(wl->scb));
    trace_bcm4345_wl_ap(0, 0);
}

/* The access point starts its BSS (SSID) or, with no SSID, stops it */
static int wl_ap_start(BCM4345Wl *wl, const uint8_t *ssid, uint32_t ssid_len)
{
    WlBss *bss = &wl->bss[0];

    if (!wl->up) {
        return BCME_NOTUP;
    }
    if (ssid_len > DOT11_MAX_SSID) {
        return BCME_BADARG;
    }
    wl_ap_stop(wl);
    if (!ssid_len) {
        return BCME_OK;
    }
    memset(&wl->cur, 0, sizeof(wl->cur));
    memcpy(wl->cur.bssid, bss->mac, ETH_ALEN);
    memcpy(wl->cur.ssid, ssid, ssid_len);
    wl->cur.ssid_len = ssid_len;
    wl->cur.chanspec = wl->chanspec;
    wl->cur.beacon_period = wl->cvar[WL_CV_BCNPRD] > 0 &&
                            wl->cvar[WL_CV_BCNPRD] <= UINT16_MAX
                            ? wl->cvar[WL_CV_BCNPRD] : 100;
    wl->cur.dtim_period = wl->cvar[WL_CV_DTIMPRD] > 0 &&
                          wl->cvar[WL_CV_DTIMPRD] <= UINT8_MAX
                          ? wl->cvar[WL_CV_DTIMPRD] : 1;
    wl->cur.from_beacon = true;
    wl->dtim_count = 0;
    wl->ap_up = true;
    timer_mod_ns(wl->bcn_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    trace_bcm4345_wl_ap(ssid_len, wl->cur.chanspec);
    wl_event_flags(wl, bss, WLC_E_LINK, WLC_EVENT_MSG_LINK,
                   WLC_E_STATUS_SUCCESS, 0, bss->mac, NULL, 0);
    wl_event(wl, bss, WLC_E_SET_SSID, WLC_E_STATUS_SUCCESS, 0, bss->mac,
             ssid, ssid_len);
    return BCME_OK;
}

/* The host deauthenticates a station, or all (broadcast) */
static void wl_ap_deauth(BCM4345Wl *wl, const uint8_t *ea, uint16_t reason)
{
    WlBss *bss = &wl->bss[0];
    bool all = is_broadcast_ether_addr(ea);
    size_t i;

    if (all) {
        wl_send_reason(wl, DOT11_ST_DEAUTH, ea, bss->mac, reason);
    }
    for (i = 0; i < WL_MAX_SCB; i++) {
        WlScb *scb = &wl->scb[i];

        if (scb->state == WL_SCB_NONE ||
            (!all && memcmp(scb->mac, ea, ETH_ALEN))) {
            continue;
        }
        if (!all) {
            wl_send_reason(wl, DOT11_ST_DEAUTH, scb->mac, bss->mac, reason);
        }
        scb->state = WL_SCB_NONE;
        trace_bcm4345_wl_scb(scb->aid, scb->state);
        wl_event(wl, bss, WLC_E_DEAUTH, WLC_E_STATUS_SUCCESS, reason,
                 scb->mac, NULL, 0);
    }
}

static void wl_ap_rx_probe_req(BCM4345Wl *wl, const WlRx *rx)
{
    WlBss *bss = &wl->bss[0];
    const uint8_t *ssid = wl_find_ie(rx->body, rx->len, DOT11_IE_SSID);
    WlFrame f;

    if ((!is_broadcast_ether_addr(rx->a1) &&
         memcmp(rx->a1, bss->mac, ETH_ALEN)) ||
        (!is_broadcast_ether_addr(rx->a3) &&
         memcmp(rx->a3, bss->mac, ETH_ALEN)) || !ssid) {
        return;
    }
    if (!ssid[1] ? bss->var[WL_BV_CLOSEDNET] != 0
                 : ssid[1] != wl->cur.ssid_len ||
                   memcmp(ssid + 2, wl->cur.ssid, ssid[1])) {
        return;
    }
    wl_frame_init(wl, &f, DOT11_FC(DOT11_TYPE_MGMT, DOT11_ST_PROBE_RESP),
                  rx->a2, bss->mac, bss->mac);
    wl_put_bss_body(wl, &f, DOT11_ST_PROBE_RESP);
    wl_air_send(wl, &f, wl->cur.chanspec);
}

static void wl_ap_rx_auth(BCM4345Wl *wl, const WlRx *rx)
{
    WlBss *bss = &wl->bss[0];
    uint16_t alg, status = DOT11_SC_SUCCESS;
    uint8_t b[6];
    WlScb *scb;

    /* algorithm, transaction, status */
    if (rx->len < 6 || lduw_le_p(rx->body + 2) != 1) {
        return;
    }
    alg = lduw_le_p(rx->body);
    scb = alg == DOT11_AUTH_OPEN ? wl_scb_new(wl, rx->a2) : NULL;
    if (alg != DOT11_AUTH_OPEN) {
        status = DOT11_SC_AUTH_ALG;
    } else if (!scb) {
        status = DOT11_SC_ASSOC_BUSY;
    } else {
        scb->state = WL_SCB_AUTH;
        scb->rssi = rx->rssi;
        scb->last_rx = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        trace_bcm4345_wl_scb(scb->aid, scb->state);
    }
    stw_le_p(b, alg);
    stw_le_p(b + 2, 2);
    stw_le_p(b + 4, status);
    wl_send_mgmt(wl, DOT11_ST_AUTH, rx->a2, bss->mac, b, sizeof(b));
}

static void wl_ap_rx_assoc(BCM4345Wl *wl, const WlRx *rx, bool reassoc)
{
    WlBss *bss = &wl->bss[0];
    /* capabilities, listen interval, current access point */
    size_t fixed = reassoc ? 10 : 4, ilen;
    bool is5g = wl_chanspec_is5g(wl->cur.chanspec);
    WlScb *scb = wl_scb_find(wl, rx->a2);
    uint16_t status = DOT11_SC_SUCCESS;
    const uint8_t *ies, *ssid;
    WlFrame f;

    if (rx->len < fixed) {
        return;
    }
    if (!scb) {
        wl_send_reason(wl, DOT11_ST_DEAUTH, rx->a2, bss->mac,
                       DOT11_RC_NOT_AUTH);
        return;
    }
    ies = rx->body + fixed;
    ilen = rx->len - fixed;
    ssid = wl_find_ie(ies, ilen, DOT11_IE_SSID);
    if (!ssid || ssid[1] != wl->cur.ssid_len ||
        memcmp(ssid + 2, wl->cur.ssid, ssid[1])) {
        status = DOT11_SC_FAILURE;
    } else if (wl_wpa(bss) && !wl_find_ie(ies, ilen, DOT11_IE_RSN) &&
               !wl_find_wfa_ie(ies, ilen, WFA_TYPE_WPA, -1)) {
        status = DOT11_SC_INVALID_IE;
    } else {
        scb->capability = lduw_le_p(rx->body);
        scb->wme = wl_wme(bss) &&
                   wl_find_wfa_ie(ies, ilen, WFA_TYPE_WMM, -1);
        scb->ht = wl_find_ie(ies, ilen, DOT11_IE_HT_CAP);
        scb->aid = wl_scb_aid(wl, scb);
    }

    /* capabilities, status, association ID, IEs */
    wl_frame_init(wl, &f, DOT11_FC(DOT11_TYPE_MGMT,
                                   reassoc ? DOT11_ST_REASSOC_RESP
                                           : DOT11_ST_ASSOC_RESP),
                  rx->a2, bss->mac, bss->mac);
    wl_put_le16(&f, wl_capability(bss, is5g));
    wl_put_le16(&f, status);
    wl_put_le16(&f, status ? 0 : scb->aid | DOT11_AID_BITS);
    wl_put_rates(&f, is5g);
    wl_put_ext_rates(&f, is5g);
    if (status == DOT11_SC_SUCCESS) {
        wl_put_ht_cap(wl, &f, is5g);
        wl_put_ht_op(&f, wl->cur.chanspec);
        if (is5g) {
            wl_put_vht_cap(&f);
            wl_put_vht_op(&f, wl->cur.chanspec);
        }
        if (scb->wme) {
            wl_put_wmm_param(&f);
        }
        wl_put_vndr_ies(&f, bss, WL_VNDR_IE_ASSOCRSP);
    }
    wl_air_send(wl, &f, wl->chanspec);
    if (status != DOT11_SC_SUCCESS) {
        return;
    }

    scb->state = WL_SCB_ASSOC;
    scb->authorized = !wl_wpa(bss);
    scb->assoc_time = scb->last_rx = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    scb->rssi = rx->rssi;
    trace_bcm4345_wl_scb(scb->aid, scb->state);
    wl_event(wl, bss, reassoc ? WLC_E_REASSOC_IND : WLC_E_ASSOC_IND,
             WLC_E_STATUS_SUCCESS, 0, rx->a2, ies, ilen);
}

static void wl_ap_rx_mgmt(BCM4345Wl *wl, const WlRx *rx, unsigned subtype)
{
    WlBss *bss = &wl->bss[0];
    bool to_bss = !memcmp(rx->a1, bss->mac, ETH_ALEN) &&
                  !memcmp(rx->a3, bss->mac, ETH_ALEN);
    WlScb *scb;

    switch (subtype) {
    case DOT11_ST_PROBE_REQ:
        wl_ap_rx_probe_req(wl, rx);
        break;
    case DOT11_ST_AUTH:
        if (to_bss) {
            wl_ap_rx_auth(wl, rx);
        }
        break;
    case DOT11_ST_ASSOC_REQ:
    case DOT11_ST_REASSOC_REQ:
        if (to_bss) {
            wl_ap_rx_assoc(wl, rx, subtype == DOT11_ST_REASSOC_REQ);
        }
        break;
    case DOT11_ST_DEAUTH:
    case DOT11_ST_DISASSOC:
        scb = wl_scb_find(wl, rx->a2);
        if (!to_bss || rx->len < 2 || !scb) {
            break;
        }
        if (scb->state == WL_SCB_ASSOC) {
            wl_event(wl, bss, subtype == DOT11_ST_DEAUTH ? WLC_E_DEAUTH_IND
                                                         : WLC_E_DISASSOC_IND,
                     WLC_E_STATUS_SUCCESS, lduw_le_p(rx->body), rx->a2,
                     NULL, 0);
        }
        /* a disassociated station stays authenticated */
        scb->state = subtype == DOT11_ST_DEAUTH ? WL_SCB_NONE : WL_SCB_AUTH;
        scb->authorized = false;
        trace_bcm4345_wl_scb(scb->aid, scb->state);
        break;
    }
}

/* Data path: Ethernet frames of the host, 802.11 data frames on the air */

/* 802.2 LLC/SNAP headers: RFC 1042, and 802.1H for AARP and IPX */
static const uint8_t wl_llc_rfc1042[6] = { 0xaa, 0xaa, 0x03, 0, 0, 0 };
static const uint8_t wl_llc_bridge[6] = { 0xaa, 0xaa, 0x03, 0, 0, 0xf8 };

/* The EtherType of a data frame body, or its length (802.3) */
static uint16_t wl_llc_type(const uint8_t *body, size_t len, size_t *hlen)
{
    if (len >= DOT11_LLC_SNAP_LEN && (!memcmp(body, wl_llc_rfc1042, 6) ||
                                      !memcmp(body, wl_llc_bridge, 6))) {
        *hlen = DOT11_LLC_SNAP_LEN;
        return lduw_be_p(body + 6);
    }
    *hlen = 0;
    return len;
}

static void wl_send_data(BCM4345Wl *wl, uint16_t ds, bool qos, unsigned prio,
                         const uint8_t *a1, const uint8_t *a2,
                         const uint8_t *a3, const uint8_t *body, size_t len)
{
    WlFrame f;

    wl_frame_init(wl, &f, DOT11_FC(DOT11_TYPE_DATA,
                                   qos ? DOT11_ST_QOS_DATA : DOT11_ST_DATA) |
                  ds, a1, a2, a3);
    if (qos) {
        wl_put_le16(&f, prio & 0x7);
    }
    wl_put(&f, body, len);
    wl_air_send(wl, &f, wl->chanspec);
}

/* An Ethernet frame of the host, as a data frame */
static void wl_send_eth(BCM4345Wl *wl, uint16_t ds, bool qos, unsigned prio,
                        const uint8_t *a1, const uint8_t *a2,
                        const uint8_t *a3, const uint8_t *eth, size_t len)
{
    uint16_t type = lduw_be_p(eth + 12);
    uint8_t body[DOT11_LLC_SNAP_LEN + DOT11_MAX_MSDU];

    len -= ETH_HLEN;
    if (type < WL_ETH_P_802_3_MIN) {
        /* 802.3: the LLC header is in the payload */
        wl_send_data(wl, ds, qos, prio, a1, a2, a3, eth + ETH_HLEN,
                     MIN(type, len));
        return;
    }
    memcpy(body, type == WL_ETH_P_AARP || type == WL_ETH_P_IPX
                 ? wl_llc_bridge : wl_llc_rfc1042, 6);
    stw_be_p(body + 6, type);
    memcpy(body + DOT11_LLC_SNAP_LEN, eth + ETH_HLEN, len);
    wl_send_data(wl, ds, qos, prio, a1, a2, a3, body,
                 DOT11_LLC_SNAP_LEN + len);
}

/* A data frame body to the host, as an Ethernet frame */
static void wl_deliver(BCM4345Wl *wl, unsigned prio, const uint8_t *da,
                       const uint8_t *sa, const uint8_t *body, size_t len)
{
    uint8_t hdr[BDC_HEADER_LEN + ETH_HLEN];
    size_t hlen;
    uint16_t type = wl_llc_type(body, len, &hlen);

    /* BDC header: version, priority, interface, data offset */
    hdr[0] = BCDC_PROTO_VER << BCDC_FLAG_VER_SHIFT;
    hdr[1] = prio;
    hdr[2] = wl->bss[0].ifidx;
    hdr[3] = 0;
    memcpy(hdr + BDC_HEADER_LEN, da, ETH_ALEN);
    memcpy(hdr + BDC_HEADER_LEN + 6, sa, ETH_ALEN);
    stw_be_p(hdr + BDC_HEADER_LEN + 12, type);
    bcm4345_send(wl->s, SDPCM_DATA_CHANNEL, hdr, sizeof(hdr), body + hlen,
                 len - hlen);
}

static void wl_ap_rx_data(BCM4345Wl *wl, const WlRx *rx)
{
    WlBss *bss = &wl->bss[0];
    WlScb *scb, *dst;
    size_t hlen;
    uint16_t type;

    if ((rx->fc & (DOT11_FC_TODS | DOT11_FC_FROMDS)) != DOT11_FC_TODS ||
        memcmp(rx->a1, bss->mac, ETH_ALEN)) {
        return;
    }
    scb = wl_scb_find(wl, rx->a2);
    if (!scb || scb->state != WL_SCB_ASSOC) {
        /* a class 3 frame from a station not associated */
        wl_send_reason(wl, DOT11_ST_DEAUTH, rx->a2, bss->mac,
                       DOT11_RC_NOT_ASSOC);
        return;
    }
    scb->last_rx = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    scb->rssi = rx->rssi;
    if (DOT11_FC_SUBTYPE(rx->fc) & DOT11_ST_NODATA) {
        return;
    }
    type = wl_llc_type(rx->body, rx->len, &hlen);
    if (!scb->authorized && type != WL_ETH_P_EAPOL) {
        return;
    }
    scb->rx_pkts++;
    scb->rx_bytes += rx->len - hlen;

    /* to the host, and on to the stations of the BSS */
    dst = wl_scb_find(wl, rx->a3);
    if (is_multicast_ether_addr(rx->a3)) {
        wl_send_data(wl, DOT11_FC_FROMDS, false, rx->tid, rx->a3, bss->mac,
                     rx->a2, rx->body, rx->len);
    } else if (dst && dst->state == WL_SCB_ASSOC && dst->authorized) {
        dst->tx_pkts++;
        dst->tx_bytes += rx->len - hlen;
        wl_send_data(wl, DOT11_FC_FROMDS, dst->wme, rx->tid, rx->a3,
                     bss->mac, rx->a2, rx->body, rx->len);
        return;
    }
    wl_deliver(wl, rx->tid, rx->a3, rx->a2, rx->body, rx->len);
}

/* Host address tables of the offloads: a match, an addition */
static bool wl_hostip_find(const uint8_t *tbl, size_t alen, const uint8_t *ip)
{
    static const uint8_t zero[16];
    size_t i;

    if (!memcmp(ip, zero, alen)) {
        return false;
    }
    for (i = 0; i < WL_MAX_HOSTIP; i++) {
        if (!memcmp(tbl + i * alen, ip, alen)) {
            return true;
        }
    }
    return false;
}

static int wl_hostip_add(uint8_t *tbl, size_t alen, const uint8_t *ip)
{
    static const uint8_t zero[16];
    size_t i;

    if (!memcmp(ip, zero, alen)) {
        return BCME_BADARG;
    }
    if (wl_hostip_find(tbl, alen, ip)) {
        return BCME_OK;
    }
    for (i = 0; i < WL_MAX_HOSTIP; i++) {
        if (!memcmp(tbl + i * alen, zero, alen)) {
            memcpy(tbl + i * alen, ip, alen);
            return BCME_OK;
        }
    }
    return BCME_NORESOURCE;
}

/* A frame of the station's own, to its access point */
static void wl_sta_send_eth(BCM4345Wl *wl, unsigned prio, const uint8_t *eth,
                            size_t len)
{
    wl->sta_tx_pkts++;
    wl->sta_tx_bytes += len - ETH_HLEN;
    wl_send_eth(wl, DOT11_FC_TODS, wl->cur.wme, prio, wl->cur.bssid,
                wl->bss[0].mac, eth, eth, len);
}

/*
 * ARP agent with peer auto-reply (arp_ol): the firmware answers ARP
 * requests for an address of the host itself, and the host does not see
 * them. Probes and announcements still go to the host.
 */
static bool wl_arp_offload(BCM4345Wl *wl, const uint8_t *sa,
                           const uint8_t *arp, size_t len)
{
    const uint32_t mode = WL_ARP_OL_AGENT | WL_ARP_OL_PEER_AUTO_REPLY;
    uint8_t eth[ETH_HLEN + WL_ARP_LEN];
    uint8_t *r = eth + ETH_HLEN;

    if (!wl->ivar[WL_IV_ARPOE] || (wl->ivar[WL_IV_ARP_OL] & mode) != mode ||
        len < WL_ARP_LEN || lduw_be_p(arp) != 1 ||
        lduw_be_p(arp + 2) != ETH_P_IP || arp[4] != ETH_ALEN || arp[5] != 4 ||
        lduw_be_p(arp + 6) != WL_ARP_REQUEST ||
        !ldl_he_p(arp + 14) || !memcmp(arp + 14, arp + 24, 4) ||
        !wl_hostip_find(wl->arp_hostip[0], 4, arp + 24)) {
        return false;
    }
    memcpy(eth, sa, ETH_ALEN);
    memcpy(eth + 6, wl->bss[0].mac, ETH_ALEN);
    stw_be_p(eth + 12, ETH_P_ARP);
    memcpy(r, arp, 6);
    stw_be_p(r + 6, WL_ARP_REPLY);
    memcpy(r + 8, wl->bss[0].mac, ETH_ALEN);
    memcpy(r + 14, arp + 24, 4);
    memcpy(r + 18, arp + 8, ETH_ALEN);
    memcpy(r + 24, arp + 14, 4);
    wl_sta_send_eth(wl, 0, eth, sizeof(eth));
    return true;
}

/*
 * Neighbour solicitation offload (ndoe): the firmware answers solicitations
 * for an address of the host with a solicited advertisement. Duplicate
 * address detection, from the unspecified address, goes to the host.
 */
static bool wl_nd_offload(BCM4345Wl *wl, const uint8_t *sa,
                          const uint8_t *ip6, size_t len)
{
    static const uint8_t unspecified[16];
    uint8_t eth[ETH_HLEN + WL_IP6_HDR_LEN + WL_ICMP6_NA_LEN];
    uint8_t *h = eth + ETH_HLEN, *na = h + WL_IP6_HDR_LEN;
    const uint8_t *ns = ip6 + WL_IP6_HDR_LEN;
    uint32_t sum;

    if (!wl->ivar[WL_IV_NDOE] || len < WL_IP6_HDR_LEN + 24 ||
        ip6[0] >> 4 != 6 || ip6[6] != WL_IP6_NEXT_ICMP6 || ip6[7] != 255 ||
        lduw_be_p(ip6 + 4) < 24 || lduw_be_p(ip6 + 4) > len - WL_IP6_HDR_LEN ||
        ns[0] != WL_ICMP6_NS || ns[1] ||
        !memcmp(ip6 + 8, unspecified, 16) ||
        !wl_hostip_find(wl->nd_hostip[0], 16, ns + 8)) {
        return false;
    }
    memcpy(eth, sa, ETH_ALEN);
    memcpy(eth + 6, wl->bss[0].mac, ETH_ALEN);
    stw_be_p(eth + 12, ETH_P_IPV6);
    /* IPv6 header: from the target address back to the solicitor */
    memset(h, 0, WL_IP6_HDR_LEN + WL_ICMP6_NA_LEN);
    h[0] = 6 << 4;
    stw_be_p(h + 4, WL_ICMP6_NA_LEN);
    h[6] = WL_IP6_NEXT_ICMP6;
    h[7] = 255;
    memcpy(h + 8, ns + 8, 16);
    memcpy(h + 24, ip6 + 8, 16);
    /* advertisement: solicited, override; target link-layer address */
    na[0] = WL_ICMP6_NA;
    na[4] = WL_ICMP6_NA_SOLICITED | WL_ICMP6_NA_OVERRIDE;
    memcpy(na + 8, ns + 8, 16);
    na[24] = WL_ND_OPT_TARGET_LLADDR;
    na[25] = 1;
    memcpy(na + 26, wl->bss[0].mac, ETH_ALEN);
    sum = net_checksum_add(32, h + 8) + WL_ICMP6_NA_LEN + WL_IP6_NEXT_ICMP6;
    sum += net_checksum_add(WL_ICMP6_NA_LEN, na);
    stw_be_p(na + 2, net_checksum_finish(sum));
    wl_sta_send_eth(wl, 0, eth, sizeof(eth));
    return true;
}

static void wl_sta_rx_data(BCM4345Wl *wl, const WlRx *rx)
{
    WlBss *sta = &wl->bss[0];
    uint16_t type;
    size_t hlen;

    if (wl->join_state != WL_JOIN_ASSOCIATED ||
        (rx->fc & (DOT11_FC_TODS | DOT11_FC_FROMDS)) != DOT11_FC_FROMDS ||
        memcmp(rx->a2, wl->cur.bssid, ETH_ALEN) ||
        DOT11_FC_SUBTYPE(rx->fc) & DOT11_ST_NODATA) {
        return;
    }
    if (is_multicast_ether_addr(rx->a1)) {
        /* our own, sent on by the access point */
        if (!memcmp(rx->a3, sta->mac, ETH_ALEN)) {
            return;
        }
    } else if (memcmp(rx->a1, sta->mac, ETH_ALEN)) {
        return;
    }
    type = wl_llc_type(rx->body, rx->len, &hlen);
    wl->sta_rx_pkts++;
    wl->sta_rx_bytes += rx->len - hlen;
    if ((type == ETH_P_ARP &&
         wl_arp_offload(wl, rx->a3, rx->body + hlen, rx->len - hlen)) ||
        (type == ETH_P_IPV6 &&
         wl_nd_offload(wl, rx->a3, rx->body + hlen, rx->len - hlen))) {
        return;
    }
    wl_deliver(wl, rx->tid, rx->a1, rx->a3, rx->body, rx->len);
}

/*
 * RSSI events (rssi_event): one each time the signal of the access point
 * moves to another range between the levels, at most one per rate limit
 */
static void wl_rssi_event(BCM4345Wl *wl, int rssi)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    uint8_t data[12];
    unsigned idx;

    if (!wl->rssi_ev_num) {
        return;
    }
    for (idx = 0; idx < wl->rssi_ev_num && rssi > wl->rssi_ev_level[idx];
         idx++) {
    }
    if (idx == wl->rssi_ev_idx ||
        (wl->rssi_ev_idx != WL_RSSI_LEVEL_NONE && wl->rssi_ev_rate &&
         now - wl->rssi_ev_time < wl->rssi_ev_rate)) {
        return;
    }
    wl->rssi_ev_idx = idx;
    wl->rssi_ev_time = now;
    /* struct wl_event_data_rssi: RSSI, SNR, noise */
    stl_be_p(data, rssi);
    stl_be_p(data + 4, rssi - WL_NOISE);
    stl_be_p(data + 8, WL_NOISE);
    wl_event(wl, &wl->bss[0], WLC_E_RSSI, WLC_E_STATUS_SUCCESS, 0,
             wl->cur.bssid, data, sizeof(data));
}

/* A beacon or probe response */
static void wl_rx_bss(BCM4345Wl *wl, const WlRx *rx, bool beacon)
{
    WlPeer peer;
    size_t i;

    if (rx->len < DOT11_BCN_FIXED_LEN) {
        return;
    }
    if (!beacon) {
        /* probe responses to one of our addresses */
        for (i = 0; i < WL_MAX_BSS; i++) {
            if (wl->bss[i].enabled &&
                !memcmp(rx->a1, wl->bss[i].mac, ETH_ALEN)) {
                break;
            }
        }
        if (i == WL_MAX_BSS) {
            return;
        }
    }
    if (wl->join_state == WL_JOIN_ASSOCIATED &&
        !memcmp(rx->a3, wl->cur.bssid, ETH_ALEN) && beacon) {
        wl->last_beacon = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        wl->cur.rssi = rx->rssi;
        wl_rssi_event(wl, rx->rssi);
    }
    if ((!wl->scanning && wl->join_state != WL_JOIN_SCAN) ||
        !wl_peer_parse(wl, &peer, rx, beacon)) {
        return;
    }
    if (wl->scanning) {
        wl_scan_result(wl, &peer);
    }
    if (wl->join_state == WL_JOIN_SCAN && wl_join_match(wl, &peer)) {
        wl->cur = peer;
        wl->chanspec = peer.chanspec;
        wl->join_tries = 0;
        wl_join_auth(wl);
    }
}

static void wl_rx(BCM4345Wl *wl, const WlRx *rx)
{
    unsigned subtype = DOT11_FC_SUBTYPE(rx->fc);

    /* without the keys of protected frames */
    if (rx->fc & DOT11_FC_PROTECTED) {
        return;
    }
    if (DOT11_FC_TYPE(rx->fc) == DOT11_TYPE_DATA) {
        if (wl->ap_up) {
            wl_ap_rx_data(wl, rx);
        } else {
            wl_sta_rx_data(wl, rx);
        }
        return;
    }
    if (subtype == DOT11_ST_BEACON || subtype == DOT11_ST_PROBE_RESP) {
        wl_rx_bss(wl, rx, subtype == DOT11_ST_BEACON);
    } else if (wl->ap_up) {
        wl_ap_rx_mgmt(wl, rx, subtype);
    } else {
        wl_sta_rx_mgmt(wl, rx, subtype);
    }
}

/* Integer variables */

typedef struct WlIntVar {
    const char *name;
    int index;
    bool per_bss;
    bool read_only;
} WlIntVar;

static const WlIntVar wl_int_vars[] = {
    { "mpc", WL_IV_MPC },
    { "bcn_timeout", WL_IV_BCN_TIMEOUT },
    { "roam_off", WL_IV_ROAM_OFF },
    { "txbf", WL_IV_TXBF },
    { "pfn", WL_IV_PFN },
    { "arpoe", WL_IV_ARPOE },
    { "arp_ol", WL_IV_ARP_OL },
    { "ndoe", WL_IV_NDOE },
    { "obss_coex", WL_IV_OBSS_COEX },
    { "apsta", WL_IV_APSTA },
    { "allmulti", WL_IV_ALLMULTI },
    { "bus:txglom", WL_IV_TXGLOM },
    { "bus:txglomalign", WL_IV_TXGLOMALIGN },
    { "bus:rxglom", WL_IV_RXGLOM },
    { "ampdu_hostreorder", WL_IV_AMPDU_HOSTREORDER },
    { "tlv", WL_IV_TLV },
    { "pm2_sleep_ret", WL_IV_PM2_SLEEP_RET },
    { "qtxpower", WL_IV_QTXPOWER },
    { "fragthresh", WL_IV_FRAGTHRESH },
    { "rtsthresh", WL_IV_RTSTHRESH },
    { "pfnmem", WL_IV_PFNMEM },
    { "mchan", WL_IV_MCHAN },
    { "wsec", WL_BV_WSEC, true },
    { "wpa_auth", WL_BV_WPA_AUTH, true },
    { "auth", WL_BV_AUTH, true },
    { "mfp", WL_BV_MFP, true },
    { "closednet", WL_BV_CLOSEDNET, true },
    { "wme_bss_disable", WL_BV_WME_BSS_DISABLE, true },
};

/* Read-only integer variables: PHY capabilities */
static const struct {
    const char *name;
    int32_t value;
} wl_const_vars[] = {
    { "nmode", 1 },
    { "vhtmode", 1 },
    { "rxchain", 1 },
    { "txchain", 1 },
    { "txstreams", 1 },
    { "txbf_bfe_cap", 1 },
    { "txbf_bfr_cap", 0 },
};

static int32_t *wl_int_var(BCM4345Wl *wl, WlBss *bss, const WlIntVar *v)
{
    return v->per_bss ? &bss->var[v->index] : &wl->ivar[v->index];
}

/* Firmware variables */

static int wl_get_int(uint8_t *out, size_t len, int32_t value)
{
    if (len < 4) {
        return BCME_BUFTOOSHORT;
    }
    stl_le_p(out, value);
    return BCME_OK;
}

static int wl_get_data(uint8_t *out, size_t len, const void *data,
                       size_t dlen)
{
    if (len < dlen) {
        return BCME_BUFTOOSHORT;
    }
    memcpy(out, data, dlen);
    return BCME_OK;
}

static int wl_get_str(uint8_t *out, size_t len, const char *str)
{
    size_t n = strlen(str) + 1;

    if (len < n) {
        return BCME_BUFTOOSHORT;
    }
    memcpy(out, str, n);
    return BCME_OK;
}

/* Variables this firmware build does not have */
static const char *const wl_absent_vars[] = {
    "wlc_ver", "ulp_sdioctrl", "toe_ol", "tdls_enable", "tdls_sta_info",
    "sup_wpa", "scan_ver", "rsdb_mode", "arp_version", "dump_obss",
    /* no gscan in the capabilities: scheduled scans stop here on the CM4 */
    "pfn_gscan_cfg",
};

static int wl_unsupported(const char *name)
{
    qemu_log_mask(LOG_UNIMP, "bcm4345: unsupported variable %s\n", name);
    return BCME_UNSUPPORTED;
}

/* Rate (kbit/s) of a link: MCS 7 (HT) or 9 (VHT), one stream, short GI */
static uint32_t wl_link_rate(uint16_t cs, bool ht)
{
    if (!ht) {
        return 54000;
    }
    if (!wl_chanspec_is5g(cs)) {
        return 72200;
    }
    switch (cs & WL_CHANSPEC_BW_MASK) {
    case WL_CHANSPEC_BW_80:
        return 433300;
    case WL_CHANSPEC_BW_40:
        return 200000;
    default:
        return 86700;
    }
}

/* sta_info (version 4) of a station of the access point, or of it */
static int wl_sta_info(BCM4345Wl *wl, WlBss *bss, const uint8_t *ea,
                       uint8_t *out, size_t len)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    uint8_t si[WL_STA_INFO_LEN] = { 0 };
    uint32_t flags = WL_STA_AUTHE | WL_STA_ASSOC | WL_STA_SCBSTATS;
    uint32_t tx_pkts, rx_pkts, rate;
    uint64_t tx_bytes, rx_bytes;
    int64_t idle, in;
    uint16_t cap, aid;
    bool ht;
    int8_t rssi;

    if (wl->ap_up) {
        WlScb *scb = wl_scb_find(wl, ea);

        if (!scb) {
            return BCME_BADADDR;
        }
        if (scb->state != WL_SCB_ASSOC) {
            flags = WL_STA_AUTHE;
        }
        flags |= (scb->authorized ? WL_STA_AUTHO : 0) |
                 (scb->wme ? WL_STA_WME : 0) | (scb->ht ? WL_STA_N_CAP : 0);
        cap = scb->capability;
        aid = scb->aid;
        idle = now - scb->last_rx;
        in = now - scb->assoc_time;
        tx_pkts = scb->tx_pkts;
        rx_pkts = scb->rx_pkts;
        tx_bytes = scb->tx_bytes;
        rx_bytes = scb->rx_bytes;
        rssi = scb->rssi;
        ht = scb->ht;
    } else if (wl->join_state == WL_JOIN_ASSOCIATED &&
               !memcmp(ea, wl->cur.bssid, ETH_ALEN)) {
        ht = wl_find_ie(wl->cur.ies, wl->cur.ies_len, DOT11_IE_HT_CAP);
        flags |= WL_STA_AUTHO | (wl->cur.wme ? WL_STA_WME : 0) |
                 (ht ? WL_STA_N_CAP : 0);
        cap = wl->cur.capability;
        aid = wl->cur.aid;
        idle = now - wl->last_beacon;
        in = now - wl->assoc_time;
        tx_pkts = wl->sta_tx_pkts;
        rx_pkts = wl->sta_rx_pkts;
        tx_bytes = wl->sta_tx_bytes;
        rx_bytes = wl->sta_rx_bytes;
        rssi = wl->cur.rssi;
    } else {
        return BCME_BADADDR;
    }
    rate = wl_link_rate(wl->cur.chanspec, ht);

    /*
     * version, length, capabilities, flags, idle (s), address, rates,
     * associated (s), listen interval (ms), counters, signal
     */
    stw_le_p(si, WL_STA_INFO_VER);
    stw_le_p(si + 2, sizeof(si));
    stw_le_p(si + 4, cap);
    stl_le_p(si + 8, flags);
    stl_le_p(si + 12, idle / 1000);
    memcpy(si + 16, ea, ETH_ALEN);
    stl_le_p(si + 44, in / 1000);
    stl_le_p(si + 48, WL_LISTEN_INTERVAL * wl->cur.beacon_period);
    stl_le_p(si + 52, tx_pkts);
    stl_le_p(si + 60, rx_pkts);
    stl_le_p(si + 68, tx_pkts ? rate : 0);
    stl_le_p(si + 72, rx_pkts ? rate : 0);
    stl_le_p(si + 84, tx_pkts);
    stl_le_p(si + 88, rx_pkts);
    stq_le_p(si + 96, tx_bytes);
    stq_le_p(si + 104, rx_bytes);
    stq_le_p(si + 112, tx_bytes);
    stq_le_p(si + 128, rx_bytes);
    si[144] = rssi;
    si[148] = (uint8_t)WL_NOISE;
    stw_le_p(si + 152, aid);
    si[168] = rssi;
    memset(out, 0, len);
    return wl_get_data(out, len, si, MIN(len, sizeof(si)));
}

static int wl_var_get(BCM4345Wl *wl, WlBss *bss, const char *name,
                      uint8_t *out, size_t len)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(wl_int_vars); i++) {
        if (!strcmp(name, wl_int_vars[i].name)) {
            return wl_get_int(out, len, *wl_int_var(wl, bss,
                                                    &wl_int_vars[i]));
        }
    }
    for (i = 0; i < ARRAY_SIZE(wl_const_vars); i++) {
        if (!strcmp(name, wl_const_vars[i].name)) {
            return wl_get_int(out, len, wl_const_vars[i].value);
        }
    }

    if (!strcmp(name, "ver")) {
        return wl_get_str(out, len, wl->ver);
    }
    if (!strcmp(name, "cap")) {
        return wl_get_str(out, len, wl_cap);
    }
    if (!strcmp(name, "clmver")) {
        return wl->clmver[0] ? wl_get_str(out, len, wl->clmver)
                             : BCME_UNSUPPORTED;
    }
    if (!strcmp(name, "clmload_status")) {
        return wl_get_int(out, len, wl->clm_status);
    }
    if (!strcmp(name, "cur_etheraddr")) {
        return wl_get_data(out, len, bss->mac, ETH_ALEN);
    }
    if (!strcmp(name, "event_msgs")) {
        return wl_get_data(out, len, wl->event_mask,
                           MIN(len, sizeof(wl->event_mask)));
    }
    if (!strcmp(name, "event_msgs_ext")) {
        /* version, command, length, maximum length, mask */
        size_t n;

        if (len < EVENTMSGS_HDR_LEN) {
            return BCME_BUFTOOSHORT;
        }
        if (out[0] != EVENTMSGS_VER) {
            return BCME_BADARG;
        }
        n = MIN(out[2], sizeof(wl->event_mask));
        n = MIN(n, len - EVENTMSGS_HDR_LEN);
        out[1] = 0;
        out[2] = n;
        out[3] = sizeof(wl->event_mask);
        memcpy(out + EVENTMSGS_HDR_LEN, wl->event_mask, n);
        return BCME_OK;
    }
    if (!strcmp(name, "pmkid_info")) {
        return wl_get_data(out, len, wl->pmkid_info,
                           MIN(len, sizeof(wl->pmkid_info)));
    }
    if (!strcmp(name, "country")) {
        const WlLocale *loc = wl_locale(wl);
        uint8_t c[12] = { 0 };

        /* abbreviation, revision, country code */
        memcpy(c, loc->ccode, 2);
        stl_le_p(c + 4, loc->rev);
        memcpy(c + 8, loc->ccode, 2);
        return wl_get_data(out, len, c, sizeof(c));
    }
    if (!strcmp(name, "chanspecs")) {
        uint16_t filter = len >= 2 ? lduw_le_p(out) : 0;
        uint16_t bw = filter & WL_CHANSPEC_BW_MASK;
        uint32_t count = 0;
        static const uint16_t bws[] = {
            WL_CHANSPEC_BW_20, WL_CHANSPEC_BW_40, WL_CHANSPEC_BW_80,
        };
        int band;

        if (len < 4) {
            return BCME_BUFTOOSHORT;
        }
        for (band = 0; band < 2; band++) {
            for (i = 0; i < ARRAY_SIZE(bws); i++) {
                if (bw && (bws[i] != bw ||
                           (filter & WL_CHANSPEC_BAND_MASK) !=
                           (band ? WL_CHANSPEC_BAND_5G :
                            WL_CHANSPEC_BAND_2G))) {
                    continue;
                }
                if (!band && bws[i] == WL_CHANSPEC_BW_80) {
                    continue;
                }
                wl_add_chanspecs(wl, band, bws[i], out, len, &count);
            }
        }
        stl_le_p(out, count);
        return BCME_OK;
    }
    if (!strcmp(name, "per_chan_info")) {
        uint16_t cs = len >= 4 ? ldl_le_p(out) : 0;
        bool is5g = wl_chanspec_is5g(cs);
        unsigned ch = cs & 0xff;
        uint32_t info = 0;

        if (wl_chan_valid(wl_locale(wl), is5g, ch)) {
            info = WL_CHAN_VALID_HW | WL_CHAN_VALID_SW;
            if (is5g) {
                info |= WL_CHAN_BAND_5G;
                if (wl_chan_radar(wl_locale(wl), ch)) {
                    info |= WL_CHAN_RADAR | WL_CHAN_PASSIVE;
                }
            }
        }
        return wl_get_int(out, len, info);
    }
    if (!strcmp(name, "bw_cap")) {
        uint32_t band = len >= 4 ? ldl_le_p(out) : 0;

        if (band == WLC_BAND_2G) {
            return wl_get_int(out, len, wl->bw_cap_2g);
        }
        if (band == WLC_BAND_5G) {
            return wl_get_int(out, len, wl->bw_cap_5g);
        }
        return BCME_BADARG;
    }
    if (!strcmp(name, "chanspec")) {
        return wl_get_int(out, len, wl->chanspec);
    }
    if (!strcmp(name, "join_pref")) {
        return wl_get_data(out, len, wl->join_pref, wl->join_pref_len);
    }
    if (!strcmp(name, "arp_hostip")) {
        return wl_get_data(out, len, wl->arp_hostip,
                           MIN(len, sizeof(wl->arp_hostip)));
    }
    if (!strcmp(name, "rssi_event")) {
        uint8_t ev[WL_RSSI_EVENT_LEN];

        stl_le_p(ev, wl->rssi_ev_rate);
        ev[4] = wl->rssi_ev_num;
        memcpy(ev + 5, wl->rssi_ev_level, WL_MAX_RSSI_LEVELS);
        return wl_get_data(out, len, ev, sizeof(ev));
    }
    if (!strcmp(name, "pfn_macaddr")) {
        /* version, flags, address */
        uint8_t m[2 + ETH_ALEN] = { 1, 0 };

        memcpy(m + 2, wl->pfn_mac, ETH_ALEN);
        return wl_get_data(out, len, m, sizeof(m));
    }
    if (!strcmp(name, "p2p_disc")) {
        return wl_get_int(out, len, wl->ivar[WL_IV_P2P_DISC]);
    }
    if (!strcmp(name, "p2p_dev")) {
        return wl->bss[1].enabled ? wl_get_int(out, len, 1) : BCME_NOTUP;
    }
    if (!strcmp(name, "assoc_info")) {
        /*
         * IE lengths of the request and response, flags; request:
         * capabilities, listen interval, current access point;
         * response: capabilities, status, association ID
         */
        uint8_t ai[28] = { 0 };

        stl_le_p(ai, wl->assoc_req_len);
        stl_le_p(ai + 4, wl->assoc_resp_len);
        stw_le_p(ai + 12, wl_capability(bss,
                                        wl_chanspec_is5g(wl->cur.chanspec)));
        stw_le_p(ai + 14, WL_LISTEN_INTERVAL);
        stw_le_p(ai + 22, wl->assoc_resp_cap);
        stw_le_p(ai + 24, wl->assoc_status);
        stw_le_p(ai + 26, wl->cur.aid | DOT11_AID_BITS);
        return wl_get_data(out, len, ai, sizeof(ai));
    }
    if (!strcmp(name, "assoc_req_ies")) {
        return wl_get_data(out, len, wl->assoc_req_ies, wl->assoc_req_len);
    }
    if (!strcmp(name, "assoc_resp_ies")) {
        return wl_get_data(out, len, wl->assoc_resp_ies, wl->assoc_resp_len);
    }
    if (!strcmp(name, "wme_ac_sta")) {
        /* the access point's AC parameters, as in its WMM IE */
        const uint8_t *wmm = wl_find_wfa_ie(wl->assoc_resp_ies,
                                            wl->assoc_resp_len, WFA_TYPE_WMM,
                                            WMM_SUBTYPE_PARAM);

        if (wmm && wmm[1] >= 8 + sizeof(wl_wmm_ac_default)) {
            return wl_get_data(out, len, wmm + 10,
                               sizeof(wl_wmm_ac_default));
        }
        return wl_get_data(out, len, wl_wmm_ac_default,
                           sizeof(wl_wmm_ac_default));
    }
    if (!strcmp(name, "sta_info")) {
        uint8_t ea[ETH_ALEN];

        if (len < ETH_ALEN) {
            return BCME_BUFTOOSHORT;
        }
        memcpy(ea, out, ETH_ALEN);
        return wl_sta_info(wl, bss, ea, out, len);
    }
    for (i = 0; i < ARRAY_SIZE(wl_absent_vars); i++) {
        if (!strcmp(name, wl_absent_vars[i])) {
            return BCME_UNSUPPORTED;
        }
    }
    return wl_unsupported(name);
}

static void wl_listen_done(void *opaque)
{
    BCM4345Wl *wl = opaque;

    wl->p2p_state = WL_P2P_DISC_ST_SCAN;
    wl_event(wl, &wl->bss[1], WLC_E_P2P_DISC_LISTEN_COMPLETE,
             WLC_E_STATUS_SUCCESS, 0, NULL, NULL, 0);
}

static void wl_p2p_disc(BCM4345Wl *wl, bool enable)
{
    WlBss *dev = &wl->bss[1];
    uint8_t ev[5];

    if (enable == dev->enabled) {
        return;
    }
    timer_del(wl->listen_timer);
    wl->p2p_state = WL_P2P_DISC_ST_SCAN;
    /* the discovery BSS has no host interface of its own */
    dev->enabled = enable;
    dev->ifidx = 0;
    memcpy(dev->mac, wl->p2p_da, ETH_ALEN);
    memset(dev->var, 0, sizeof(dev->var));
    ev[0] = dev->ifidx;
    ev[1] = enable ? WLC_E_IF_ADD : WLC_E_IF_DEL;
    ev[2] = WLC_E_IF_FLAG_NOIF;
    ev[3] = 1;
    ev[4] = WLC_E_IF_ROLE_STA;
    wl_event(wl, dev, WLC_E_IF, WLC_E_STATUS_SUCCESS, 0, dev->mac, ev,
             sizeof(ev));
}

/* A chanspec of this locale and of the bandwidths the radio is set to */
static bool wl_chanspec_valid(BCM4345Wl *wl, uint16_t cs)
{
    bool is5g = wl_chanspec_is5g(cs);
    uint32_t cap = is5g ? wl->bw_cap_5g : wl->bw_cap_2g;
    uint8_t buf[4 + 4 * 2 * ARRAY_SIZE(((WlLocale *)0)->chan_5g)];
    uint32_t count = 0, i;

    switch (cs & WL_CHANSPEC_BW_MASK) {
    case WL_CHANSPEC_BW_20:
        break;
    case WL_CHANSPEC_BW_40:
        if (!(cap & 0x2)) {
            return false;
        }
        break;
    case WL_CHANSPEC_BW_80:
        if (!(cap & 0x4)) {
            return false;
        }
        break;
    default:
        return false;
    }
    wl_add_chanspecs(wl, is5g, cs & WL_CHANSPEC_BW_MASK, buf, sizeof(buf),
                     &count);
    for (i = 0; i < count; i++) {
        if (ldl_le_p(buf + 4 + 4 * i) == cs) {
            return true;
        }
    }
    return false;
}

/*
 * vndr_ie: commands ("add" or "del"), each with a count of IEs, each IE
 * with the frames it goes in
 */
static int wl_vndr_ie(WlBss *bss, const uint8_t *in, size_t len)
{
    while (len >= WL_VNDR_IE_HDR_LEN) {
        bool add = !memcmp(in, "add", 4);
        uint32_t count = ldl_le_p(in + 4);
        size_t off = 8, i, k;

        if (!add && memcmp(in, "del", 4)) {
            return BCME_BADARG;
        }
        for (k = 0; k < count; k++) {
            uint32_t flags;
            const uint8_t *ie;
            size_t n;

            if (off + 4 + 2 > len) {
                return BCME_BUFTOOSHORT;
            }
            flags = ldl_le_p(in + off);
            ie = in + off + 4;
            n = ie[1] + 2;
            if (off + 4 + n > len || ie[0] != DOT11_IE_VENDOR || ie[1] < 3) {
                return BCME_BADARG;
            }
            off += 4 + n;
            for (i = 0; i < WL_MAX_VNDR_IE; i++) {
                WlVndrIe *v = &bss->vndr[i];

                if (v->flags && !memcmp(v->ie, ie, n)) {
                    v->flags = add ? v->flags | flags : v->flags & ~flags;
                    break;
                }
            }
            if (i < WL_MAX_VNDR_IE || !add) {
                continue;
            }
            for (i = 0; i < WL_MAX_VNDR_IE && bss->vndr[i].flags; i++) {
                continue;
            }
            if (i == WL_MAX_VNDR_IE) {
                return BCME_NOMEM;
            }
            bss->vndr[i].flags = flags;
            memcpy(bss->vndr[i].ie, ie, n);
        }
        in += off;
        len -= off;
    }
    return len ? BCME_BUFTOOSHORT : BCME_OK;
}

static int wl_var_set(BCM4345Wl *wl, WlBss *bss, const char *name,
                      const uint8_t *in, size_t len)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(wl_int_vars); i++) {
        if (!strcmp(name, wl_int_vars[i].name)) {
            if (len < 4) {
                return BCME_BUFTOOSHORT;
            }
            *wl_int_var(wl, bss, &wl_int_vars[i]) = ldl_le_p(in);
            return BCME_OK;
        }
    }

    if (!strcmp(name, "cur_etheraddr")) {
        if (len < ETH_ALEN) {
            return BCME_BUFTOOSHORT;
        }
        memcpy(bss->mac, in, ETH_ALEN);
        return BCME_OK;
    }
    if (!strcmp(name, "event_msgs")) {
        memcpy(wl->event_mask, in, MIN(len, sizeof(wl->event_mask)));
        return BCME_OK;
    }
    if (!strcmp(name, "event_msgs_ext")) {
        size_t n;

        if (len < EVENTMSGS_HDR_LEN) {
            return BCME_BUFTOOSHORT;
        }
        n = MIN(in[2], len - EVENTMSGS_HDR_LEN);
        if (in[0] != EVENTMSGS_VER || n > sizeof(wl->event_mask)) {
            return BCME_BADARG;
        }
        for (i = 0; i < n; i++) {
            uint8_t m = in[EVENTMSGS_HDR_LEN + i];

            switch (in[1]) {
            case EVENTMSGS_SET_MASK:
                wl->event_mask[i] = m;
                break;
            case EVENTMSGS_SET_BIT:
                wl->event_mask[i] |= m;
                break;
            case EVENTMSGS_RESET_BIT:
                wl->event_mask[i] &= ~m;
                break;
            default:
                return BCME_BADARG;
            }
        }
        return BCME_OK;
    }
    if (!strcmp(name, "pmkid_info")) {
        if (len < 4 || ldl_le_p(in) > WL_MAX_PMKID ||
            4 + ldl_le_p(in) * WL_PMKSA_LEN > len) {
            return BCME_BADLEN;
        }
        memset(wl->pmkid_info, 0, sizeof(wl->pmkid_info));
        memcpy(wl->pmkid_info, in, 4 + ldl_le_p(in) * WL_PMKSA_LEN);
        return BCME_OK;
    }
    if (!strcmp(name, "join_pref")) {
        if (len > sizeof(wl->join_pref) || len % 4) {
            return BCME_BADLEN;
        }
        memcpy(wl->join_pref, in, len);
        wl->join_pref_len = len;
        return BCME_OK;
    }
    if (!strcmp(name, "clmload")) {
        /* flags, type, length, CRC, data */
        uint16_t flag;
        uint32_t dlen;

        if (len < WL_DLOAD_HDR_LEN) {
            return BCME_BUFTOOSHORT;
        }
        flag = lduw_le_p(in);
        dlen = ldl_le_p(in + 4);
        if (lduw_le_p(in + 2) != DL_TYPE_CLM ||
            dlen > len - WL_DLOAD_HDR_LEN) {
            return BCME_BADARG;
        }
        if (flag & DL_BEGIN) {
            wl->clm_len = 0;
            wl->clm_status = BCME_OK;
        }
        if (wl->clm_len + dlen > sizeof(wl->clm)) {
            wl->clm_status = BCME_BUFTOOSHORT;
            return BCME_ERROR;
        }
        memcpy(wl->clm + wl->clm_len, in + WL_DLOAD_HDR_LEN, dlen);
        wl->clm_len += dlen;
        if (flag & DL_END) {
            wl->clm_status = wl_parse_clm(wl);
            trace_bcm4345_wl_clm(wl->clm_len, wl->clm_status);
            return wl->clm_status ? BCME_ERROR : BCME_OK;
        }
        return BCME_OK;
    }
    if (!strcmp(name, "country")) {
        const WlLocale *loc = wl_locale(wl);

        if (len < 4) {
            return BCME_BUFTOOSHORT;
        }
        return !memcmp(in, loc->ccode, 2) ? BCME_OK : BCME_BADARG;
    }
    if (!strcmp(name, "bw_cap")) {
        uint32_t band, cap;

        if (len < 8) {
            return BCME_BUFTOOSHORT;
        }
        band = ldl_le_p(in);
        cap = ldl_le_p(in + 4);
        if (band == WLC_BAND_2G) {
            /*
             * The 2.4 GHz band stays HT20: the CM4's firmware refuses 40 MHz
             * there, and brcmfmac then drops HT40 from the band (iw phy
             * capabilities 0x1020 on the board).
             */
            if (cap & ~WLC_BW_CAP_20MHZ) {
                return BCME_BADARG;
            }
            wl->bw_cap_2g = cap;
        } else if (band == WLC_BAND_5G) {
            wl->bw_cap_5g = cap & WLC_BW_CAP_80MHZ;
        } else {
            return BCME_BADARG;
        }
        return BCME_OK;
    }
    if (!strcmp(name, "mcast_list")) {
        /* count, addresses */
        if (len < 4 || ldl_le_p(in) > (len - 4) / ETH_ALEN) {
            return BCME_BUFTOOSHORT;
        }
        return BCME_OK;
    }
    if (!strcmp(name, "pfn_macaddr")) {
        if (len < 2 + ETH_ALEN) {
            return BCME_BUFTOOSHORT;
        }
        memcpy(wl->pfn_mac, in + 2, ETH_ALEN);
        return BCME_OK;
    }
    if (!strcmp(name, "p2p_da_override")) {
        if (len < ETH_ALEN) {
            return BCME_BUFTOOSHORT;
        }
        memcpy(wl->p2p_da, in, ETH_ALEN);
        return BCME_OK;
    }
    if (!strcmp(name, "p2p_disc")) {
        if (len < 4) {
            return BCME_BUFTOOSHORT;
        }
        wl->ivar[WL_IV_P2P_DISC] = ldl_le_p(in) != 0;
        wl_p2p_disc(wl, wl->ivar[WL_IV_P2P_DISC]);
        return BCME_OK;
    }
    if (!strcmp(name, "p2p_state")) {
        /* state, channel, dwell time (struct wl_p2p_disc_st) */
        if (len < 6) {
            return BCME_BUFTOOSHORT;
        }
        if (bss != &wl->bss[1] || in[0] > WL_P2P_DISC_ST_SEARCH) {
            return BCME_BADARG;
        }
        timer_del(wl->listen_timer);
        wl->p2p_state = in[0];
        wl->p2p_chspec = lduw_le_p(in + 2);
        wl->p2p_dwell = lduw_le_p(in + 4);
        if (wl->p2p_state == WL_P2P_DISC_ST_LISTEN) {
            if (!wl->up) {
                return BCME_NOTUP;
            }
            timer_mod(wl->listen_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + wl->p2p_dwell);
        }
        return BCME_OK;
    }
    if (!strcmp(name, "escan")) {
        /* version, action, sync ID, scan parameters */
        int ret;

        if (len < WL_ESCAN_PARAMS_FIXED_LEN) {
            return BCME_BUFTOOSHORT;
        }
        if (ldl_le_p(in) != 1) {
            return BCME_BADARG;
        }
        if (lduw_le_p(in + 4) == WL_ESCAN_ACTION_ABORT) {
            if (wl->scanning) {
                wl_scan_done(wl, WLC_E_STATUS_ABORT);
            }
            return BCME_OK;
        }
        if (!wl->up) {
            return BCME_NOTUP;
        }
        if (wl->scanning) {
            return BCME_BUSY;
        }
        ret = wl_scan_params(wl, in + 8, len - 8);
        if (ret) {
            return ret;
        }
        wl->scan_sync_id = lduw_le_p(in + 6);
        wl->scan_bss = bss - wl->bss;
        wl_scan_start(wl);
        return BCME_OK;
    }
    if (!strcmp(name, "wpaie")) {
        if (len > sizeof(bss->wpaie) || (len && (len < 2 ||
                                                 in[1] + 2u != len))) {
            return BCME_BADLEN;
        }
        memcpy(bss->wpaie, in, len);
        bss->wpaie_len = len;
        return BCME_OK;
    }
    if (!strcmp(name, "vndr_ie")) {
        return wl_vndr_ie(bss, in, len);
    }
    if (!strcmp(name, "join")) {
        /*
         * SSID; join scan: type, probes, active, passive and home times;
         * BSSID, the number of chanspecs, the chanspecs
         */
        uint32_t n;

        if (len < WL_EXT_JOIN_FIXED_LEN) {
            return BCME_BUFTOOSHORT;
        }
        n = ldl_le_p(in + 64);
        if (n > WL_MAX_SCAN_CHANS || WL_EXT_JOIN_FIXED_LEN + 2 * n > len) {
            return BCME_BADARG;
        }
        return wl_join(wl, in + 4, ldl_le_p(in), in + 56, in + 68, n,
                       ldl_le_p(in + 44), ldl_le_p(in + 48));
    }
    if (!strcmp(name, "chanspec")) {
        uint16_t cs;

        if (len < 4) {
            return BCME_BUFTOOSHORT;
        }
        cs = ldl_le_p(in);
        if (!wl_chanspec_valid(wl, cs)) {
            return BCME_BADARG;
        }
        wl->chanspec = cs;
        return BCME_OK;
    }
    if (!strcmp(name, "wsec_key")) {
        return len < WL_WSEC_KEY_LEN || ldl_le_p(in + 4) > 32 ? BCME_BADARG
                                                               : BCME_OK;
    }
    if (!strcmp(name, "bip")) {
        /* the group management cipher suite */
        return len < 4 ? BCME_BUFTOOSHORT : BCME_OK;
    }
    /* addresses the firmware answers ARP and neighbour solicitations for */
    if (!strcmp(name, "arp_hostip")) {
        return len < 4 ? BCME_BUFTOOSHORT
                       : wl_hostip_add(wl->arp_hostip[0], 4, in);
    }
    if (!strcmp(name, "nd_hostip")) {
        return len < 16 ? BCME_BUFTOOSHORT
                        : wl_hostip_add(wl->nd_hostip[0], 16, in);
    }
    if (!strcmp(name, "arp_hostip_clear")) {
        memset(wl->arp_hostip, 0, sizeof(wl->arp_hostip));
        return BCME_OK;
    }
    if (!strcmp(name, "nd_hostip_clear")) {
        memset(wl->nd_hostip, 0, sizeof(wl->nd_hostip));
        return BCME_OK;
    }
    if (!strcmp(name, "arp_table_clear")) {
        /* the ARP cache of the agent, which only answers for the host */
        return BCME_OK;
    }
    /*
     * Preferred network offload: the parameters (struct wl_pfn_param, v2),
     * channels (wl_pfn_cfg), networks (wl_pfn, wl_pfn_bssid). Without
     * gscan the host never enables it.
     */
    if (!strcmp(name, "pfnclear")) {
        return BCME_OK;
    }
    if (!strcmp(name, "pfn_set")) {
        return len < WL_PFN_PARAM_LEN ? BCME_BUFTOOSHORT
               : ldl_le_p(in) != WL_PFN_VERSION ? BCME_VERSION : BCME_OK;
    }
    if (!strcmp(name, "pfn_cfg")) {
        return len < WL_PFN_CFG_LEN ? BCME_BUFTOOSHORT
               : ldl_le_p(in + 4) > WL_PFN_MAX_CHANS ? BCME_RANGE : BCME_OK;
    }
    if (!strcmp(name, "pfn_add")) {
        return len < WL_PFN_NET_LEN ? BCME_BUFTOOSHORT
               : ldl_le_p(in) > DOT11_MAX_SSID ? BCME_BADARG : BCME_OK;
    }
    if (!strcmp(name, "pfn_add_bssid")) {
        return len < WL_PFN_BSSID_LEN ? BCME_BUFTOOSHORT : BCME_OK;
    }
    if (!strcmp(name, "rssi_event")) {
        if (len < WL_RSSI_EVENT_LEN) {
            return BCME_BUFTOOSHORT;
        }
        if (in[4] > WL_MAX_RSSI_LEVELS) {
            return BCME_RANGE;
        }
        wl->rssi_ev_rate = ldl_le_p(in);
        wl->rssi_ev_num = in[4];
        memcpy(wl->rssi_ev_level, in + 5, WL_MAX_RSSI_LEVELS);
        wl->rssi_ev_idx = WL_RSSI_LEVEL_NONE;
        return BCME_OK;
    }
    for (i = 0; i < ARRAY_SIZE(wl_absent_vars); i++) {
        if (!strcmp(name, wl_absent_vars[i])) {
            return BCME_UNSUPPORTED;
        }
    }
    return wl_unsupported(name);
}

/* Firmware commands */

static const struct {
    uint32_t get;
    uint32_t set;
    int index;
} wl_cmd_vars[] = {
    { WLC_GET_INFRA, WLC_SET_INFRA, WL_CV_INFRA },
    { WLC_GET_SRL, WLC_SET_SRL, WL_CV_SRL },
    { WLC_GET_LRL, WLC_SET_LRL, WL_CV_LRL },
    { WLC_GET_RADIO, WLC_SET_RADIO, WL_CV_RADIO },
    { WLC_GET_BCNPRD, WLC_SET_BCNPRD, WL_CV_BCNPRD },
    { WLC_GET_DTIMPRD, WLC_SET_DTIMPRD, WL_CV_DTIMPRD },
    { WLC_GET_PM, WLC_SET_PM, WL_CV_PM },
    { WLC_GET_MONITOR, WLC_SET_MONITOR, WL_CV_MONITOR },
    { WLC_GET_AP, WLC_SET_AP, WL_CV_AP },
    { 0, WLC_SET_PROMISC, WL_CV_PROMISC },
    { 0, WLC_SET_PASSIVE_SCAN, WL_CV_PASSIVE_SCAN },
    { 0, WLC_SET_SCAN_CHANNEL_TIME, WL_CV_SCAN_CHANNEL_TIME },
    { 0, WLC_SET_SCAN_UNASSOC_TIME, WL_CV_SCAN_UNASSOC_TIME },
    { 0, WLC_SET_SCAN_PASSIVE_TIME, WL_CV_SCAN_PASSIVE_TIME },
    { 0, WLC_SET_FAKEFRAG, WL_CV_FAKEFRAG },
    { 0, WLC_SET_ASSOC_PREFER, WL_CV_ASSOC_PREFER },
    { 0, WLC_SET_SCB_TIMEOUT, WL_CV_SCB_TIMEOUT },
    { WLC_GET_REGULATORY, WLC_SET_REGULATORY, WL_CV_REGULATORY },
    { WLC_GET_KEY_PRIMARY, WLC_SET_KEY_PRIMARY, WL_CV_KEY_PRIMARY },
};

static int wl_cmd_unsupported(bool set, uint32_t cmd)
{
    qemu_log_mask(LOG_UNIMP, "bcm4345: unsupported command %s %u\n",
                  set ? "set" : "query", cmd);
    return BCME_UNSUPPORTED;
}

/* The station is associated, or the access point is up */
static bool wl_bss_up(BCM4345Wl *wl)
{
    return wl->ap_up || wl->join_state == WL_JOIN_ASSOCIATED;
}

static int wl_cmd_get(BCM4345Wl *wl, WlBss *bss, uint32_t cmd,
                      uint8_t *out, size_t len)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(wl_cmd_vars); i++) {
        if (wl_cmd_vars[i].get == cmd) {
            return wl_get_int(out, len, wl->cvar[wl_cmd_vars[i].index]);
        }
    }

    switch (cmd) {
    case WLC_GET_VERSION:
        return wl_get_int(out, len, WLC_IOCTL_VERSION);
    case WLC_GET_PHYTYPE:
        return wl_get_int(out, len, WLC_PHY_TYPE_AC);
    case WLC_GET_AUTH:
        return wl_get_int(out, len, bss->var[WL_BV_AUTH]);
    case WLC_GET_WSEC:
        return wl_get_int(out, len, bss->var[WL_BV_WSEC]);
    case WLC_GET_REVINFO: {
        uint32_t ri[17] = {
            0x14e4,                     /* vendor */
            0x43ab,                     /* device */
            0x0058030b,                 /* radio 0.88.3.11 */
            6,                          /* chip revision */
            54,                         /* 802.11 core revision */
            0x0726,                     /* board */
            0x14e4,                     /* board vendor */
            0x1101,                     /* board revision P101 */
            0x072df100,                 /* firmware 7.45.241 */
            0,                          /* microcode */
            0,                          /* bus: SDIO */
            0x4345,                     /* chip */
            WLC_PHY_TYPE_AC,
            20,                         /* PHY revision */
            0,                          /* analog revision */
            2,                          /* package */
            wl->nvramrev,
        };

        if (len < sizeof(ri)) {
            return BCME_BUFTOOSHORT;
        }
        for (i = 0; i < ARRAY_SIZE(ri); i++) {
            stl_le_p(out + 4 * i, ri[i]);
        }
        return BCME_OK;
    }
    case WLC_GET_BANDLIST: {
        if (len < 12) {
            return BCME_BUFTOOSHORT;
        }
        stl_le_p(out, 2);
        stl_le_p(out + 4, WLC_BAND_5G);
        stl_le_p(out + 8, WLC_BAND_2G);
        return BCME_OK;
    }
    case WLC_GET_BSSID:
        if (!wl_bss_up(wl)) {
            return BCME_NOTASSOCIATED;
        }
        return wl_get_data(out, len, wl->cur.bssid, ETH_ALEN);
    case WLC_GET_RSSI: {
        /* signal, of the station given in an access point */
        WlScb *scb;

        if (len < 4 + ETH_ALEN) {
            return BCME_BUFTOOSHORT;
        }
        if (wl->ap_up) {
            scb = wl_scb_find(wl, out + 4);
            if (!scb) {
                return BCME_BADADDR;
            }
            return wl_get_int(out, len, scb->rssi);
        }
        if (wl->join_state != WL_JOIN_ASSOCIATED) {
            return BCME_NOTASSOCIATED;
        }
        return wl_get_int(out, len, wl->cur.rssi);
    }
    case WLC_GET_SSID:
        /* length, SSID */
        if (len < 4 + DOT11_MAX_SSID) {
            return BCME_BUFTOOSHORT;
        }
        memset(out, 0, 4 + DOT11_MAX_SSID);
        if (wl_bss_up(wl) || wl->join_state != WL_JOIN_IDLE) {
            stl_le_p(out, wl->cur.ssid_len);
            memcpy(out + 4, wl->cur.ssid, wl->cur.ssid_len);
        }
        return BCME_OK;
    case WLC_GET_BSS_INFO:
        /* length, bss_info */
        if (!wl_bss_up(wl)) {
            return BCME_NOTASSOCIATED;
        }
        if (wl->ap_up) {
            wl_ap_update(wl);
        }
        if (len < 4 + WL_BSS_INFO_FIXED_LEN + wl->cur.ies_len) {
            return BCME_BUFTOOSHORT;
        }
        stl_le_p(out, 4 + wl_bss_info(out + 4, &wl->cur));
        return BCME_OK;
    case WLC_GET_ASSOCLIST: {
        /* count, addresses */
        uint32_t n = 0;

        if (len < 4) {
            return BCME_BUFTOOSHORT;
        }
        for (i = 0; i < WL_MAX_SCB && wl->ap_up; i++) {
            if (wl->scb[i].state == WL_SCB_ASSOC &&
                4 + (n + 1) * ETH_ALEN <= len) {
                memcpy(out + 4 + n++ * ETH_ALEN, wl->scb[i].mac, ETH_ALEN);
            }
        }
        if (wl->join_state == WL_JOIN_ASSOCIATED && len >= 4 + ETH_ALEN) {
            memcpy(out + 4 + n++ * ETH_ALEN, wl->cur.bssid, ETH_ALEN);
        }
        stl_le_p(out, n);
        return BCME_OK;
    }
    case WLC_GET_CHANNEL: {
        /* hardware, target and scan channels */
        unsigned ch = wl_chanspec_ctl(wl->chanspec);

        if (len < 12) {
            return BCME_BUFTOOSHORT;
        }
        stl_le_p(out, ch);
        stl_le_p(out + 4, ch);
        stl_le_p(out + 8, 0);
        return BCME_OK;
    }
    case WLC_GET_VAR: {
        size_t n = strnlen((char *)out, len);
        char name[WL_MAX_VAR_NAME];

        if (n == len || n >= sizeof(name)) {
            return BCME_BADARG;
        }
        memcpy(name, out, n + 1);
        if (g_str_has_prefix(name, "bsscfg:")) {
            uint32_t idx;

            if (n + 5 > len) {
                return BCME_BUFTOOSHORT;
            }
            idx = ldl_le_p(out + n + 1);
            if (idx >= WL_MAX_BSS || !wl->bss[idx].enabled) {
                return BCME_BADARG;
            }
            /* the parameters follow the index */
            memmove(out, out + n + 5, len - n - 5);
            return wl_var_get(wl, &wl->bss[idx], name + 7, out, len);
        }
        memmove(out, out + n + 1, len - n - 1);
        return wl_var_get(wl, bss, name, out, len);
    }
    }
    return wl_cmd_unsupported(false, cmd);
}

static int wl_cmd_set(BCM4345Wl *wl, WlBss *bss, uint32_t cmd,
                      const uint8_t *in, size_t len)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(wl_cmd_vars); i++) {
        if (wl_cmd_vars[i].set == cmd) {
            if (len < 4) {
                return BCME_BUFTOOSHORT;
            }
            wl->cvar[wl_cmd_vars[i].index] = ldl_le_p(in);
            return BCME_OK;
        }
    }

    switch (cmd) {
    case WLC_UP:
        wl->up = true;
        return BCME_OK;
    case WLC_DOWN:
        if (wl->scanning) {
            wl_scan_done(wl, WLC_E_STATUS_ABORT);
        }
        wl_leave(wl, DOT11_ST_DEAUTH, DOT11_RC_DEAUTH_LEAVING);
        wl_ap_stop(wl);
        timer_del(wl->listen_timer);
        wl->up = false;
        return BCME_OK;
    case WLC_SET_AP:
        if (len < 4) {
            return BCME_BUFTOOSHORT;
        }
        wl->cvar[WL_CV_AP] = ldl_le_p(in) != 0;
        if (!wl->cvar[WL_CV_AP]) {
            wl_ap_stop(wl);
        } else {
            wl_leave(wl, DOT11_ST_DEAUTH, DOT11_RC_DEAUTH_LEAVING);
        }
        return BCME_OK;
    case WLC_SET_SSID: {
        /* SSID; BSSID, the number of chanspecs, the chanspecs */
        static const uint8_t any[ETH_ALEN] = { 0 };
        uint32_t n = 0;

        if (len < 4 + DOT11_MAX_SSID) {
            return BCME_BUFTOOSHORT;
        }
        if (wl->cvar[WL_CV_AP]) {
            return wl_ap_start(wl, in + 4, ldl_le_p(in));
        }
        if (len >= WL_JOIN_FIXED_LEN) {
            n = ldl_le_p(in + 44);
            if (n > WL_MAX_SCAN_CHANS || WL_JOIN_FIXED_LEN + 2 * n > len) {
                return BCME_BADARG;
            }
        }
        return wl_join(wl, in + 4, ldl_le_p(in),
                       len >= WL_JOIN_FIXED_LEN ? in + 36 : any, in + 48, n,
                       0, 0);
    }
    case WLC_DISASSOC: {
        /* reason, address */
        bool associated = wl->join_state == WL_JOIN_ASSOCIATED;

        wl_leave(wl, DOT11_ST_DISASSOC, len >= 4 && ldl_le_p(in)
                 ? ldl_le_p(in) : DOT11_RC_DEAUTH_LEAVING);
        if (associated) {
            wl_event(wl, bss, WLC_E_DISASSOC, WLC_E_STATUS_SUCCESS, 0,
                     wl->cur.bssid, NULL, 0);
        }
        return BCME_OK;
    }
    case WLC_SCB_DEAUTHENTICATE_FOR_REASON:
        /* reason, address */
        if (len < 4 + ETH_ALEN) {
            return BCME_BUFTOOSHORT;
        }
        if (wl->ap_up) {
            wl_ap_deauth(wl, in + 4, ldl_le_p(in));
        }
        return BCME_OK;
    case WLC_SET_SCB_AUTHORIZE:
    case WLC_SET_SCB_DEAUTHORIZE: {
        WlScb *scb;

        if (len < ETH_ALEN) {
            return BCME_BUFTOOSHORT;
        }
        scb = wl->ap_up ? wl_scb_find(wl, in) : NULL;
        if (scb && scb->state == WL_SCB_ASSOC) {
            scb->authorized = cmd == WLC_SET_SCB_AUTHORIZE;
        }
        return BCME_OK;
    }
    case WLC_SET_AUTH:
        if (len < 4) {
            return BCME_BUFTOOSHORT;
        }
        bss->var[WL_BV_AUTH] = ldl_le_p(in);
        return BCME_OK;
    case WLC_SET_WSEC:
        if (len < 4) {
            return BCME_BUFTOOSHORT;
        }
        bss->var[WL_BV_WSEC] = ldl_le_p(in);
        return BCME_OK;
    case WLC_SET_ROAM_TRIGGER:
    case WLC_SET_ROAM_DELTA:
        /* level, band */
        return len < 8 ? BCME_BUFTOOSHORT : BCME_OK;
    case WLC_SET_COUNTRY:
        return len < 2 || memcmp(in, wl_locale(wl)->ccode, 2) ? BCME_BADARG
                                                               : BCME_OK;
    case WLC_SCAN:
        /* the host aborts a scan with a scan of channel -1 */
        if (len >= WL_SCAN_PARAMS_FIXED_LEN + 2 &&
            (ldl_le_p(in + 60) & 0xffff) == 1 &&
            lduw_le_p(in + WL_SCAN_PARAMS_FIXED_LEN) == 0xffff) {
            if (wl->scanning) {
                wl_scan_done(wl, WLC_E_STATUS_ABORT);
            }
            return BCME_OK;
        }
        return wl_cmd_unsupported(true, cmd);
    case WLC_SET_VAR: {
        size_t n = strnlen((const char *)in, len);
        char name[WL_MAX_VAR_NAME];

        if (n == len || n >= sizeof(name)) {
            return BCME_BADARG;
        }
        memcpy(name, in, n + 1);
        if (g_str_has_prefix(name, "bsscfg:")) {
            uint32_t idx;

            if (n + 5 > len) {
                return BCME_BUFTOOSHORT;
            }
            idx = ldl_le_p(in + n + 1);
            if (idx >= WL_MAX_BSS || !wl->bss[idx].enabled) {
                return BCME_BADARG;
            }
            return wl_var_set(wl, &wl->bss[idx], name + 7, in + n + 5,
                              len - n - 5);
        }
        return wl_var_set(wl, bss, name, in + n + 1, len - n - 1);
    }
    }
    return wl_cmd_unsupported(true, cmd);
}

static WlBss *wl_bss_of_ifidx(BCM4345Wl *wl, unsigned ifidx)
{
    size_t i;

    for (i = 0; i < WL_MAX_BSS; i++) {
        if (wl->bss[i].enabled && wl->bss[i].ifidx == ifidx) {
            return &wl->bss[i];
        }
    }
    return NULL;
}

void bcm4345_wl_control(BCM4345Wl *wl, const uint8_t *msg, size_t len)
{
    uint8_t hdr[BCDC_DCMD_LEN];
    uint32_t cmd, dlen, flags;
    g_autofree uint8_t *buf = NULL;
    char var[WL_MAX_VAR_NAME] = "";
    bool set;
    WlBss *bss;
    int ret;

    if (len < BCDC_DCMD_LEN) {
        qemu_log_mask(LOG_GUEST_ERROR, "bcm4345: short control message\n");
        return;
    }
    cmd = ldl_le_p(msg);
    dlen = MIN(ldl_le_p(msg + 4), WL_MAX_DCMD_LEN);
    flags = ldl_le_p(msg + 8);
    set = flags & BCDC_DCMD_SET;

    /* the host sends at most a frame's worth of the buffer of a query */
    buf = g_malloc0(dlen);
    memcpy(buf, msg + BCDC_DCMD_LEN, MIN(dlen, len - BCDC_DCMD_LEN));
    if (cmd == WLC_GET_VAR || cmd == WLC_SET_VAR) {
        pstrcpy(var, MIN(sizeof(var), dlen + 1), (const char *)buf);
    }

    wl->in_control = true;
    bss = wl_bss_of_ifidx(wl, (flags >> BCDC_DCMD_IF_SHIFT) &
                              BCDC_DCMD_IF_MASK);
    if (!bss) {
        ret = BCME_BADARG;
    } else if (set) {
        ret = wl_cmd_set(wl, bss, cmd, buf, dlen);
    } else {
        ret = wl_cmd_get(wl, bss, cmd, buf, dlen);
    }
    wl->in_control = false;
    trace_bcm4345_wl_control(set ? "set" : "query", cmd, var, ret);

    /* the reply: the request header with the status, and the buffer */
    memcpy(hdr, msg, BCDC_DCMD_LEN);
    flags &= ~BCDC_DCMD_ERROR;
    if (ret) {
        flags |= BCDC_DCMD_ERROR;
    }
    stl_le_p(hdr + 8, flags);
    stl_le_p(hdr + 12, ret);
    bcm4345_send(wl->s, SDPCM_CONTROL_CHANNEL, hdr, sizeof(hdr), buf, dlen);

    /* events the command caused follow its reply */
    wl_flush_events(wl);
}

/* Host and air interfaces */

void bcm4345_wl_transmit(BCM4345Wl *wl, unsigned ifidx, unsigned prio,
                         const uint8_t *eth, size_t len)
{
    WlBss *bss = &wl->bss[0];
    WlScb *scb;

    trace_bcm4345_wl_transmit(ifidx, len);
    if (ifidx || len < ETH_HLEN ||
        len - ETH_HLEN > DOT11_MAX_MSDU - DOT11_LLC_SNAP_LEN) {
        qemu_log_mask(LOG_GUEST_ERROR, "bcm4345: bad data frame "
                      "(interface %u, %zu bytes)\n", ifidx, len);
        return;
    }
    if (!wl->up) {
        return;
    }
    if (wl->join_state == WL_JOIN_ASSOCIATED) {
        wl_sta_send_eth(wl, prio, eth, len);
        return;
    }
    if (!wl->ap_up) {
        return;
    }
    if (is_multicast_ether_addr(eth)) {
        wl_send_eth(wl, DOT11_FC_FROMDS, false, prio, eth, bss->mac, eth + 6,
                    eth, len);
        return;
    }
    scb = wl_scb_find(wl, eth);
    if (!scb || scb->state != WL_SCB_ASSOC ||
        (!scb->authorized && lduw_be_p(eth + 12) != WL_ETH_P_EAPOL)) {
        return;
    }
    scb->tx_pkts++;
    scb->tx_bytes += len - ETH_HLEN;
    wl_send_eth(wl, DOT11_FC_FROMDS, scb->wme, prio, eth, bss->mac, eth + 6,
                eth, len);
}

ssize_t bcm4345_wl_receive(BCM4345Wl *wl, const uint8_t *buf, size_t len)
{
    unsigned freq;
    WlRx rx;
    size_t i;

    if (!wl->s->fw_ready || !wl->up || !wl_rx_parse(wl, buf, len, &rx) ||
        rx.rssi < WL_SENSITIVITY) {
        return len;
    }
    /* our own frames, back from the medium */
    for (i = 0; i < WL_MAX_BSS; i++) {
        if (wl->bss[i].enabled && !memcmp(rx.a2, wl->bss[i].mac, ETH_ALEN)) {
            return len;
        }
    }
    /*
     * The channel the radio is on; and the BSS of a station off it, which
     * keeps its frames while the station scans
     */
    freq = wl_chan_freq(wl_radio_chanspec(wl));
    if (rx.freq != freq &&
        !(wl->join_state == WL_JOIN_ASSOCIATED &&
          rx.freq == wl_chan_freq(wl->chanspec) &&
          !memcmp(rx.a2, wl->cur.bssid, ETH_ALEN))) {
        return len;
    }
    trace_bcm4345_wl_air_rx(rx.fc, rx.freq, rx.len, rx.rssi);
    wl_rx(wl, &rx);
    return len;
}

bool bcm4345_wl_can_receive(BCM4345Wl *wl)
{
    return bcm4345_rx_has_room(wl->s);
}

/* Firmware start and stop */

static void wl_reset(BCM4345Wl *wl)
{
    BCM4345State *s = wl->s;

    timer_del(wl->scan_timer);
    timer_del(wl->listen_timer);
    timer_del(wl->join_timer);
    timer_del(wl->link_timer);
    timer_del(wl->bcn_timer);
    wl->scanning = false;
    wl->scan_nssid = 0;
    wl->scan_nseen = 0;
    wl->num_events = 0;
    wl->tx_seq = 0;
    wl->join_state = WL_JOIN_IDLE;
    wl->join_nchan = wl->join_idx = 0;
    wl->ap_up = false;
    memset(&wl->cur, 0, sizeof(wl->cur));
    memset(wl->scb, 0, sizeof(wl->scb));
    wl->assoc_req_len = wl->assoc_resp_len = 0;
    wl->rssi_ev_rate = wl->rssi_ev_num = 0;
    wl->rssi_ev_idx = WL_RSSI_LEVEL_NONE;
    memset(wl->arp_hostip, 0, sizeof(wl->arp_hostip));
    memset(wl->nd_hostip, 0, sizeof(wl->nd_hostip));
    wl->p2p_state = WL_P2P_DISC_ST_SCAN;
    wl->p2p_chspec = 0;
    wl->p2p_dwell = 0;
    wl->up = false;
    memset(wl->ivar, 0, sizeof(wl->ivar));
    memset(wl->cvar, 0, sizeof(wl->cvar));
    memset(wl->event_mask, 0, sizeof(wl->event_mask));
    memset(wl->bss, 0, sizeof(wl->bss));
    wl->ivar[WL_IV_MPC] = 1;
    wl->ivar[WL_IV_BCN_TIMEOUT] = 4;
    wl->ivar[WL_IV_QTXPOWER] = 127;
    wl->ivar[WL_IV_FRAGTHRESH] = 2346;
    wl->ivar[WL_IV_RTSTHRESH] = 2347;
    wl->ivar[WL_IV_MCHAN] = 1;
    wl->cvar[WL_CV_INFRA] = 1;
    wl->cvar[WL_CV_SRL] = 7;
    wl->cvar[WL_CV_LRL] = 4;
    wl->cvar[WL_CV_BCNPRD] = 100;
    wl->cvar[WL_CV_DTIMPRD] = 1;
    wl->cvar[WL_CV_SCAN_CHANNEL_TIME] = WL_SCAN_ACTIVE_TIME;
    wl->cvar[WL_CV_SCAN_PASSIVE_TIME] = WL_SCAN_PASSIVE_TIME;
    wl->bw_cap_2g = WLC_BW_CAP_20MHZ;
    wl->bw_cap_5g = WLC_BW_CAP_80MHZ;
    wl->chanspec = WL_CHANSPEC_BAND_2G | WL_CHANSPEC_BW_20 | 1;
    wl->join_pref_len = 0;
    memset(wl->pfn_mac, 0, sizeof(wl->pfn_mac));
    memset(wl->pmkid_info, 0, sizeof(wl->pmkid_info));
    memset(wl->p2p_da, 0, sizeof(wl->p2p_da));
    wl->clm_len = 0;
    wl->clm_status = BCME_OK;
    wl->clmver[0] = '\0';

    /* the primary BSS, with the address of the chip */
    wl->bss[0].enabled = true;
    wl->bss[0].ifidx = 0;
    memcpy(wl->bss[0].mac, s->conf.macaddr.a, ETH_ALEN);
}

void bcm4345_wl_start(BCM4345Wl *wl)
{
    wl_reset(wl);
    wl_parse_image(wl);
    wl_parse_nvram(wl);
}

void bcm4345_wl_stop(BCM4345Wl *wl)
{
    wl_reset(wl);
}

const char *bcm4345_wl_version(BCM4345Wl *wl)
{
    return wl->ver;
}

/* Frames from the host come in chains, with hardware extension headers */
bool bcm4345_wl_rx_glom(BCM4345Wl *wl)
{
    return wl->ivar[WL_IV_RXGLOM];
}

BCM4345Wl *bcm4345_wl_new(BCM4345State *s)
{
    BCM4345Wl *wl = g_new0(BCM4345Wl, 1);

    wl->s = s;
    wl->scan_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, wl_scan_next, wl);
    wl->listen_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, wl_listen_done, wl);
    wl->join_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, wl_join_timeout, wl);
    wl->link_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, wl_link_check, wl);
    wl->bcn_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, wl_ap_beacon, wl);
    wl->maxp2g = WL_MAXP_NONE;
    memset(wl->maxp5g, WL_MAXP_NONE, sizeof(wl->maxp5g));
    wl_reset(wl);
    return wl;
}

void bcm4345_wl_free(BCM4345Wl *wl)
{
    timer_free(wl->scan_timer);
    timer_free(wl->listen_timer);
    timer_free(wl->join_timer);
    timer_free(wl->link_timer);
    timer_free(wl->bcn_timer);
    g_free(wl);
}

static const VMStateDescription vmstate_wl_vndr_ie = {
    .name = "bcm4345-wl/vndr-ie",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(flags, WlVndrIe),
        VMSTATE_BUFFER(ie, WlVndrIe),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_wl_bss = {
    .name = "bcm4345-wl/bss",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(enabled, WlBss),
        VMSTATE_UINT8(ifidx, WlBss),
        VMSTATE_UINT8_ARRAY(mac, WlBss, ETH_ALEN),
        VMSTATE_INT32_ARRAY(var, WlBss, WL_NUM_BVARS),
        VMSTATE_STRUCT_ARRAY(vndr, WlBss, WL_MAX_VNDR_IE, 1,
                             vmstate_wl_vndr_ie, WlVndrIe),
        VMSTATE_BUFFER(wpaie, WlBss),
        VMSTATE_UINT32(wpaie_len, WlBss),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_wl_peer = {
    .name = "bcm4345-wl/peer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(bssid, WlPeer, ETH_ALEN),
        VMSTATE_BUFFER(ssid, WlPeer),
        VMSTATE_UINT8(ssid_len, WlPeer),
        VMSTATE_UINT16(chanspec, WlPeer),
        VMSTATE_UINT16(capability, WlPeer),
        VMSTATE_UINT16(beacon_period, WlPeer),
        VMSTATE_UINT8(dtim_period, WlPeer),
        VMSTATE_INT8(rssi, WlPeer),
        VMSTATE_BOOL(wme, WlPeer),
        VMSTATE_BOOL(from_beacon, WlPeer),
        VMSTATE_UINT16(aid, WlPeer),
        VMSTATE_BUFFER(ies, WlPeer),
        VMSTATE_UINT32(ies_len, WlPeer),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_wl_scb = {
    .name = "bcm4345-wl/scb",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(state, WlScb),
        VMSTATE_BOOL(authorized, WlScb),
        VMSTATE_BOOL(wme, WlScb),
        VMSTATE_BOOL(ht, WlScb),
        VMSTATE_UINT8_ARRAY(mac, WlScb, ETH_ALEN),
        VMSTATE_UINT16(aid, WlScb),
        VMSTATE_UINT16(capability, WlScb),
        VMSTATE_INT8(rssi, WlScb),
        VMSTATE_INT64(assoc_time, WlScb),
        VMSTATE_INT64(last_rx, WlScb),
        VMSTATE_UINT32(tx_pkts, WlScb),
        VMSTATE_UINT32(rx_pkts, WlScb),
        VMSTATE_UINT64(tx_bytes, WlScb),
        VMSTATE_UINT64(rx_bytes, WlScb),
        VMSTATE_END_OF_LIST()
    }
};

static int wl_post_load(void *opaque, int version_id)
{
    BCM4345Wl *wl = opaque;

    size_t i;

    if (wl->clm_len > sizeof(wl->clm) || wl->scan_nchan > WL_MAX_SCAN_CHANS ||
        (wl->scanning && wl->scan_idx >= wl->scan_nchan) ||
        wl->join_pref_len > sizeof(wl->join_pref) ||
        wl->scan_bss >= WL_MAX_BSS || wl->scan_nssid > WL_MAX_SCAN_SSIDS ||
        wl->scan_nseen > WL_MAX_SCAN_SEEN ||
        wl->join_state > WL_JOIN_ASSOCIATED ||
        wl->join_nchan > WL_MAX_SCAN_CHANS ||
        (wl->join_state == WL_JOIN_SCAN && wl->join_idx >= wl->join_nchan) ||
        wl->cur.ssid_len > DOT11_MAX_SSID ||
        wl->cur.ies_len > sizeof(wl->cur.ies) ||
        wl->assoc_req_len > sizeof(wl->assoc_req_ies) ||
        wl->assoc_resp_len > sizeof(wl->assoc_resp_ies) ||
        wl->rssi_ev_num > WL_MAX_RSSI_LEVELS) {
        return -EINVAL;
    }
    for (i = 0; i < WL_MAX_SCAN_SSIDS; i++) {
        if (wl->scan_ssid[i][0] > DOT11_MAX_SSID) {
            return -EINVAL;
        }
    }
    for (i = 0; i < WL_MAX_BSS; i++) {
        if (wl->bss[i].wpaie_len > sizeof(wl->bss[i].wpaie)) {
            return -EINVAL;
        }
    }
    for (i = 0; i < WL_MAX_SCB; i++) {
        if (wl->scb[i].state > WL_SCB_ASSOC) {
            return -EINVAL;
        }
    }
    /* the version strings come from the image and the blob */
    wl->clmver[0] = '\0';
    if (wl->s->fw_ready) {
        wl_parse_image(wl);
        if (wl->clm_len && wl->clm_status == BCME_OK) {
            wl_parse_clm(wl);
        }
    }
    return 0;
}

const VMStateDescription vmstate_bcm4345_wl = {
    .name = "bcm4345-wl",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = wl_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(nvramrev, BCM4345Wl),
        VMSTATE_UINT8(maxp2g, BCM4345Wl),
        VMSTATE_UINT8_ARRAY(maxp5g, BCM4345Wl, WL_NUM_5G_SUBBANDS),
        VMSTATE_BOOL(up, BCM4345Wl),
        VMSTATE_INT32_ARRAY(ivar, BCM4345Wl, WL_NUM_IVARS),
        VMSTATE_INT32_ARRAY(cvar, BCM4345Wl, WL_NUM_CVARS),
        VMSTATE_UINT8_ARRAY(event_mask, BCM4345Wl, WL_EVENT_MASK_LEN),
        VMSTATE_UINT32(bw_cap_2g, BCM4345Wl),
        VMSTATE_UINT32(bw_cap_5g, BCM4345Wl),
        VMSTATE_UINT16(chanspec, BCM4345Wl),
        VMSTATE_BUFFER(join_pref, BCM4345Wl),
        VMSTATE_UINT32(join_pref_len, BCM4345Wl),
        VMSTATE_UINT8_ARRAY(pfn_mac, BCM4345Wl, ETH_ALEN),
        VMSTATE_UINT8_ARRAY(p2p_da, BCM4345Wl, ETH_ALEN),
        VMSTATE_BUFFER(pmkid_info, BCM4345Wl),
        VMSTATE_BUFFER(clm, BCM4345Wl),
        VMSTATE_UINT32(clm_len, BCM4345Wl),
        VMSTATE_INT32(clm_status, BCM4345Wl),
        VMSTATE_TIMER_PTR(listen_timer, BCM4345Wl),
        VMSTATE_UINT8(p2p_state, BCM4345Wl),
        VMSTATE_UINT16(p2p_chspec, BCM4345Wl),
        VMSTATE_UINT16(p2p_dwell, BCM4345Wl),
        VMSTATE_TIMER_PTR(scan_timer, BCM4345Wl),
        VMSTATE_BOOL(scanning, BCM4345Wl),
        VMSTATE_UINT8(scan_bss, BCM4345Wl),
        VMSTATE_UINT16(scan_sync_id, BCM4345Wl),
        VMSTATE_UINT8(scan_type, BCM4345Wl),
        VMSTATE_INT32(scan_active_time, BCM4345Wl),
        VMSTATE_INT32(scan_passive_time, BCM4345Wl),
        VMSTATE_UINT32(scan_nchan, BCM4345Wl),
        VMSTATE_UINT32(scan_idx, BCM4345Wl),
        VMSTATE_UINT16_ARRAY(scan_chan, BCM4345Wl, WL_MAX_SCAN_CHANS),
        VMSTATE_UINT32(scan_nssid, BCM4345Wl),
        VMSTATE_UINT8_2DARRAY(scan_ssid, BCM4345Wl, WL_MAX_SCAN_SSIDS,
                              1 + DOT11_MAX_SSID),
        VMSTATE_UINT32(scan_nseen, BCM4345Wl),
        VMSTATE_UINT8_2DARRAY(scan_seen, BCM4345Wl, WL_MAX_SCAN_SEEN,
                              ETH_ALEN),
        VMSTATE_STRUCT_ARRAY(bss, BCM4345Wl, WL_MAX_BSS, 1, vmstate_wl_bss,
                             WlBss),
        VMSTATE_UINT16(tx_seq, BCM4345Wl),
        VMSTATE_TIMER_PTR(join_timer, BCM4345Wl),
        VMSTATE_UINT8(join_state, BCM4345Wl),
        VMSTATE_UINT8(join_tries, BCM4345Wl),
        VMSTATE_BOOL(join_any_bssid, BCM4345Wl),
        VMSTATE_UINT32(join_nchan, BCM4345Wl),
        VMSTATE_UINT32(join_idx, BCM4345Wl),
        VMSTATE_UINT16_ARRAY(join_chan, BCM4345Wl, WL_MAX_SCAN_CHANS),
        VMSTATE_INT32(join_active_time, BCM4345Wl),
        VMSTATE_INT32(join_passive_time, BCM4345Wl),
        VMSTATE_STRUCT(cur, BCM4345Wl, 1, vmstate_wl_peer, WlPeer),
        VMSTATE_INT64(assoc_time, BCM4345Wl),
        VMSTATE_INT64(last_beacon, BCM4345Wl),
        VMSTATE_BUFFER(assoc_req_ies, BCM4345Wl),
        VMSTATE_UINT32(assoc_req_len, BCM4345Wl),
        VMSTATE_BUFFER(assoc_resp_ies, BCM4345Wl),
        VMSTATE_UINT32(assoc_resp_len, BCM4345Wl),
        VMSTATE_UINT16(assoc_resp_cap, BCM4345Wl),
        VMSTATE_UINT16(assoc_status, BCM4345Wl),
        VMSTATE_UINT32(sta_tx_pkts, BCM4345Wl),
        VMSTATE_UINT32(sta_rx_pkts, BCM4345Wl),
        VMSTATE_UINT64(sta_tx_bytes, BCM4345Wl),
        VMSTATE_UINT64(sta_rx_bytes, BCM4345Wl),
        VMSTATE_TIMER_PTR(link_timer, BCM4345Wl),
        VMSTATE_UINT32(rssi_ev_rate, BCM4345Wl),
        VMSTATE_UINT8(rssi_ev_num, BCM4345Wl),
        VMSTATE_ARRAY(rssi_ev_level, BCM4345Wl, WL_MAX_RSSI_LEVELS, 0,
                      vmstate_info_int8, int8_t),
        VMSTATE_UINT8(rssi_ev_idx, BCM4345Wl),
        VMSTATE_INT64(rssi_ev_time, BCM4345Wl),
        VMSTATE_UINT8_2DARRAY(arp_hostip, BCM4345Wl, WL_MAX_HOSTIP, 4),
        VMSTATE_UINT8_2DARRAY(nd_hostip, BCM4345Wl, WL_MAX_HOSTIP, 16),
        VMSTATE_BOOL(ap_up, BCM4345Wl),
        VMSTATE_TIMER_PTR(bcn_timer, BCM4345Wl),
        VMSTATE_UINT8(dtim_count, BCM4345Wl),
        VMSTATE_STRUCT_ARRAY(scb, BCM4345Wl, WL_MAX_SCB, 1, vmstate_wl_scb,
                             WlScb),
        VMSTATE_END_OF_LIST()
    }
};
