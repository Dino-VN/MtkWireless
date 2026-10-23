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

#ifdef __cplusplus
}
#endif

#endif /* mtk_wlan_h */
