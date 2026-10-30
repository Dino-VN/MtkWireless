//
//  mtk_bt.h
//  MtkWireless
//
//  C ABI of the Rust Bluetooth core (rust/mtk-ffi/src/bt.rs). Keep in sync.
//

#ifndef mtk_bt_h
#define mtk_bt_h

#include <stddef.h>
#include <stdint.h>

#include "mtk_rust.h"

#ifdef __cplusplus
extern "C" {
#endif

/* USB access for the bring-up. Every callback returns an IOReturn. */
typedef struct mtk_bt_transport {
    void *ctx;
    int32_t (*reg_read)(void *ctx, uint32_t reg, uint32_t *value);
    int32_t (*uhw_read)(void *ctx, uint32_t reg, uint32_t *value);
    int32_t (*uhw_write)(void *ctx, uint32_t reg, uint32_t value);
    /* Send an HCI command on EP0, then read the WMT event answering it. */
    int32_t (*wmt)(void *ctx, const uint8_t *cmd, uint32_t cmd_len,
                   uint8_t *event, uint32_t event_cap, uint32_t *event_len);
    /* Send an HCI command on EP0 (skipped when cmd_len is 0), then read one
     * HCI event from the interrupt IN endpoint. */
    int32_t (*hci)(void *ctx, const uint8_t *cmd, uint32_t cmd_len,
                   uint8_t *event, uint32_t event_cap, uint32_t *event_len);
    void (*sleep_ms)(void *ctx, uint32_t ms);
    /* Decompressed firmware by file name; returns a handle or NULL. */
    const void *(*fw_load)(void *ctx, const char *name, const uint8_t **data, size_t *len);
    void (*fw_release)(void *ctx, const void *handle);
} mtk_bt_transport;

typedef struct mtk_bt_setup_info {
    uint32_t dev_id;
    uint32_t fw_version;
    uint32_t fw_flavor;
    char firmware[49];
} mtk_bt_setup_info;

/* Chip id, ROM patch download, endpoint reset, FUNC_CTRL on, then an
 * HCI_Reset probe (subsystem reset + one more download if it goes unanswered;
 * still unanswered is only logged).
 * 0 on success, negative on failure (reason logged). */
int32_t mtk_bt_setup(const mtk_bt_transport *t, mtk_bt_setup_info *info);
int32_t mtk_bt_enable(const mtk_bt_transport *t, bool enable);

/* HCI_Read_Clock Which_Clock 0 -> 1 in an EP0 OUT command. */
bool mtk_bt_fix_read_clock(uint8_t bm_request_type, uint8_t *cmd, size_t len);

/* ACL packet boundary 0b10 -> 0b00 on bulk OUT. */
bool mtk_bt_acl_candidate(uint8_t ep_attributes, uint8_t ep_address, uint32_t length);
bool mtk_bt_acl_non_flushable(const uint8_t header[4], uint32_t length, uint8_t *byte1);

/* LE feature-mask (0xFD3C) compatibility. All mtk_bt_le_* calls except
 * mtk_bt_le_candidate_request must be serialized by the caller (one lock). */
bool mtk_bt_le_candidate_request(uint8_t bm_request_type, uint8_t b_request,
                                 uint16_t w_value, uint16_t w_index, uint16_t w_length);
uint64_t mtk_bt_le_command(uint64_t registry_id, const uint8_t *cmd, size_t len);
void mtk_bt_le_cancel(uint64_t registry_id, uint64_t token);
uint64_t mtk_bt_le_controller_for_pipe(const void *pipe);
void mtk_bt_le_bind_event_pipe(const void *pipe, uint64_t registry_id, bool mediatek);

enum { MTK_BT_LE_NONE = 0, MTK_BT_LE_UNCHANGED = 1, MTK_BT_LE_REWRITTEN = 2 };
typedef struct mtk_bt_le_received {
    uint32_t reply;
    size_t offset;
    uint8_t original[6];
} mtk_bt_le_received;
mtk_bt_le_received mtk_bt_le_receive(uint64_t registry_id, uint8_t *events, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* mtk_bt_h */
