//
//  mtk_wlan.h
//  MtkWireless
//
//  C ABI of the Rust Wi-Fi core (rust/mtk-ffi/src/wifi.rs). Keep in sync.
//  Functions return 0 or an errno value.
//

#ifndef mtk_wlan_h
#define mtk_wlan_h

#include <stddef.h>
#include <stdint.h>

#include "mtk_rust.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A block to download: image[offset .. offset + len) to addr, with mode.
 * Always inside the image when the call returns 0. */
typedef struct mtk_wlan_fw_region {
    uint32_t addr;
    uint32_t len;
    uint32_t mode;
    size_t offset;
    uint8_t feature;
    bool download; /* false: RAM region marked not-downloaded, skip it */
} mtk_wlan_fw_region;

typedef struct mtk_wlan_patch_info {
    uint32_t hw_sw_ver;
    uint32_t patch_ver;
    uint32_t regions;
    char build_date[17];
} mtk_wlan_patch_info;

typedef struct mtk_wlan_ram_info {
    uint8_t chip_id;
    uint8_t eco_code;
    uint8_t regions;
    char fw_ver[11];
    char build_date[16];
    uint32_t override_addr; /* for the start-firmware command */
    uint32_t start_option;
} mtk_wlan_ram_info;

int32_t mtk_wlan_patch_parse(const uint8_t *image, size_t len, mtk_wlan_patch_info *info);
int32_t mtk_wlan_patch_region(const uint8_t *image, size_t len, uint32_t index, mtk_wlan_fw_region *out);
int32_t mtk_wlan_ram_parse(const uint8_t *image, size_t len, mtk_wlan_ram_info *info);
int32_t mtk_wlan_ram_region(const uint8_t *image, size_t len, uint32_t index, mtk_wlan_fw_region *out);

/* RX descriptor (rust/mtk-wlan/src/connac2/rxd.rs). */
enum mtk_wlan_rx_drop {
    MTK_WLAN_RX_OK = 0,
    MTK_WLAN_RX_SHORT_RXD = 1,
    MTK_WLAN_RX_BAND_IDX = 2,
    MTK_WLAN_RX_AMSDU_ERR = 3,
    MTK_WLAN_RX_HDR_TRANS = 4,
    MTK_WLAN_RX_ICV = 5,
    MTK_WLAN_RX_FCS = 6,
    MTK_WLAN_RX_TKIP_MIC = 7,
    MTK_WLAN_RX_MAX_LEN = 8,
    MTK_WLAN_RX_SHORT_GROUPS = 9,
    MTK_WLAN_RX_SHORT_80211 = 10,
};

typedef struct mtk_wlan_rx_info {
    uint32_t rxd[5];
    size_t hdr_gap; /* descriptor + padding before the 802.11 header */
    uint32_t rate;  /* P-RXV word 0, 0 without group 3 */
    uint16_t wlan_idx;
    uint8_t channel;
    bool unicast;
    bool hw_decrypted;
    bool has_rssi;
    int8_t rssi;
} mtk_wlan_rx_info;

/* 0 or a drop reason. On success hdr_gap + 16 <= len. */
uint32_t mtk_wlan_rx_parse(const uint8_t *frame, size_t len, uint8_t num_streams,
                           uint32_t antenna_mask, mtk_wlan_rx_info *info);
const char *mtk_wlan_rx_drop_name(uint32_t reason);

/* RX ring packet class (rust/mtk-wlan/src/connac2/event.rs). PKT_TYPE_*
 * values; an RX event with flag 1 is reported as PKT_TYPE_NORMAL_MCU.
 * false for packets shorter than one dword. */
bool mtk_wlan_rx_classify(const uint8_t *packet, size_t len, uint32_t *kind, uint32_t *flag,
                          uint32_t *rxd0);
/* 8-dword TX status records in a TXS packet of len bytes (from dword 2). */
size_t mtk_wlan_txs_records(size_t len);

/* MSDUs released by a TXRX_NOTIFY packet. */
typedef struct mtk_wlan_tx_free_iter {
    const uint8_t *words;
    size_t len;
    size_t index;
    size_t count;
} mtk_wlan_tx_free_iter;
typedef struct mtk_wlan_tx_freed {
    uint16_t msdu; /* still range-check against the TXWI table */
    uint8_t status; /* 0 = acknowledged */
    uint16_t count; /* transmissions */
    uint32_t raw;
} mtk_wlan_tx_freed;
void mtk_wlan_tx_free_begin(const uint8_t *packet, size_t len, mtk_wlan_tx_free_iter *it);
bool mtk_wlan_tx_free_next(mtk_wlan_tx_free_iter *it, mtk_wlan_tx_freed *out);

#ifdef __cplusplus
}
#endif

#endif /* mtk_wlan_h */
