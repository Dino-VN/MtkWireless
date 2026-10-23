//
//  Common.h
//  SimpleMtkWlan
//
//  Created by laobamac (王孝慈) on 2026/4/27.
//  Copyright © 2026 laobamac (王孝慈). All rights reserved.
//

/*
* Copyright (C) 2020  钟先耀
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/

#ifndef Common_h
#define Common_h

#include "IoctlId.h"

#define IOCTL_MASK 0x800000
#define IOCTL_VERSION 1
#define NWID_LEN 32
#define WPA_KEY_LEN 128

#define kIONoScanResult 20008

struct ioctl_driver_info {
    unsigned int version;
    char fw_version[32];    //firmware version string
    char driver_version[32];    //driver version string
    char bsd_name[32];  //interface bsd name, eg. en1
};

enum mtk_phy_mode {
    MTK80211_MODE_11A,
    MTK80211_MODE_11B,
    MTK80211_MODE_11G,
    MTK80211_MODE_11N,
    MTK80211_MODE_11AC,
    MTK80211_MODE_11AX
};

struct ioctl_sta_info {
    unsigned int version;
    enum mtk_phy_mode op_mode;
    int max_mcs;
    int cur_mcs;
    uint channel;
    uint16_t band_width;//20 40 80 160
    int16_t rssi;
    int16_t noise;
    uint rate;
    unsigned char ssid[NWID_LEN];
    uint8_t bssid[ETHER_ADDR_LEN];
};

struct ioctl_power {
    unsigned int version;
    unsigned int enabled;    //1 == on, 0 == off
    unsigned int maxsleep;   //max sleep in ms
};

enum mtk_80211_state {
    MTK80211_S_INIT    = 0,    /* default state */
    MTK80211_S_SCAN    = 1,    /* scanning */
    MTK80211_S_AUTH    = 2,    /* try to authenticate */
    MTK80211_S_ASSOC    = 3,    /* try to assoc */
    MTK80211_S_RUN        = 4    /* associated */
};

struct ioctl_state {
    unsigned int version;
    int state; //mtk_80211_state
};

struct ioctl_nw_id {
    unsigned int version;
    unsigned int len;
    char nwid[NWID_LEN];
};

struct ioctl_nw_bssid {
    unsigned int version;
    char bssid[ETHER_ADDR_LEN];
};

struct ioctl_wpa_key {
    unsigned int version;
    unsigned int len;
    char key[WPA_KEY_LEN];
};

struct ioctl_associate {
    unsigned int version;
    struct ioctl_nw_id nwid;
    struct ioctl_wpa_key wpa_key;
};

struct ioctl_disassociate {
    unsigned int version;
    unsigned char ssid[NWID_LEN];
};

struct ioctl_join {
    unsigned int version;
    struct ioctl_nw_id nwid;
    struct ioctl_wpa_key wpa_key;
};

struct ioctl_scan {
    unsigned int version;
};

struct ioctl_scan_result {
    unsigned int version;
};

struct ioctl_tx_power {
    unsigned int version;
};

/*
 * 802.11 ciphers.
 */
enum mtk80211_cipher {
    MTK80211_CIPHER_NONE        = 0x00000000,
    MTK80211_CIPHER_USEGROUP    = 0x00000001,
    MTK80211_CIPHER_WEP40        = 0x00000002,
    MTK80211_CIPHER_TKIP        = 0x00000004,
    MTK80211_CIPHER_CCMP        = 0x00000008,
    MTK80211_CIPHER_WEP104        = 0x00000010,
    MTK80211_CIPHER_BIP        = 0x00000020    /* 11w */
};

enum mtk80211_wpa_proto {
    MTK80211_WPA_PROTO_WPA1 = 0x01,
    MTK80211_WPA_PROTO_WPA2 = 0x02
};

enum mtk80211_proto {
    MTK80211_PROTO_NONE = 0,
    MTK80211_PROTO_RSN = 1 << 0,
    MTK80211_PROTO_WPA = 1 << 1
};

/*
 * 802.11 Authentication and Key Management Protocols.
 */
enum mtk80211_akm {
    MTK80211_AKM_NONE        = 0x00000000,
    MTK80211_AKM_8021X        = 0x00000001,
    MTK80211_AKM_PSK        = 0x00000002,
    MTK80211_AKM_SHA256_8021X    = 0x00000004,    /* 11w */
    MTK80211_AKM_SHA256_PSK    = 0x00000008    /* 11w */
};

struct ioctl_network_info {
    unsigned char ssid[NWID_LEN];
    int16_t noise;
    int16_t rssi;
    uint8_t bssid[ETHER_ADDR_LEN];
    uint32_t channel;
    unsigned int supported_rsnprotos;   //mtk80211_proto
    unsigned int rsn_protos;    //mtk80211_wpa_proto
    unsigned int supported_rsnakms; //mtk80211_akm
    unsigned int rsn_akms;  //rsn_akms
    unsigned int rsn_ciphers;
    enum mtk80211_cipher    rsn_groupcipher;
    enum mtk80211_cipher    rsn_groupmgmtcipher;
    u_int16_t        ni_rsncaps;
    enum mtk80211_cipher    ni_rsncipher;
    uint32_t recv_timestamp;
};

/* Bring-up counters and WFDMA state (IOCTL_80211_DEBUG). */
struct ioctl_debug {
    unsigned int version;
    uint32_t ic_state;
    uint32_t sc_flags;
    uint32_t intr_count;
    uint32_t mcu_events;
    uint32_t last_mcu_eid;
    uint32_t mcu_bad_seq;
    uint32_t mcu_timeouts;
    uint32_t scan_requests;
    uint32_t scan_done_count;
    uint32_t scan_watchdog_fired;
    uint32_t rx_pkt_normal;
    uint32_t rx_pkt_unknown;
    uint32_t rx_fill_fail;
    uint32_t lpctl;
    uint32_t int_sta;
    uint32_t int_ena;
    uint32_t rxmcu_cpu, rxmcu_dma, rxmcu_cons;
    uint32_t rxfwdl_cpu, rxfwdl_dma, rxfwdl_cons;
    uint32_t rxdata_cpu, rxdata_dma, rxdata_cons;
    uint32_t txmcu_cpu, txmcu_dma, txmcu_cons;
    uint32_t nodes;
    int32_t intr_index;
    uint32_t intr_is_msi;
    uint32_t port_valid;
    uint32_t node_flags;
    uint32_t wcid;
    uint32_t rsn_supp_state;
    uint32_t txdata_cpu, txdata_dma, txdata_cons;
    uint32_t rx_delivered, rx_normal_mcu, rx_event, rx_notify;
    uint32_t if_ierrors, if_oerrors;
    /* bit 31 set = value captured */
    uint32_t aspm_lnkctl_before, aspm_lnkctl_after;
    uint32_t aspm_l1ss_before, aspm_l1ss_after, aspm_parent_l1ss_before;
    uint32_t glo_cfg;
    uint32_t pci_status;
    uint32_t txdata_desc[4];
    uint32_t txdata_txp[4];
    uint32_t dummy_cr, mcu_cmd;
    uint32_t knobs, dma_stalls, full_resets, mcu_cmd_irqs, last_mcu_cmd;
    /* RX Block Ack reordering (net80211 ic_stats) */
    uint32_t rx_ctl, rx_bar, rx_dup;
    uint32_t ba_agreements, ba_below_winstart, ba_above_winend;
    uint32_t ba_window_slide, ba_window_jump, ba_no_buf;
    uint32_t ba_frame_lost, ba_gap_timeout, ba_gap_timeout_ms;
    /* TX flow control */
    uint32_t rx_batches;
    uint32_t tx_stalls, tx_nobufs, tx_ring_full;
    uint32_t txwi_inuse, txwi_peak, tx_oactive;
};

enum {
    IOCTL_DEBUG_OP_KNOBS = 1,   /* val = MWX_KNOB_* bits */
    IOCTL_DEBUG_OP_RESET = 2,   /* WFSYS reset + firmware reload */
    IOCTL_DEBUG_OP_DUMP = 3,    /* write DMA state to the log */
    IOCTL_DEBUG_OP_BA_GAP = 4,  /* val = RX BA gap timeout in ms, 0 = default */
    IOCTL_DEBUG_OP_BGSCAN_DWELL = 5, /* val = background scan dwell in ms, 0 = firmware, ~0 = auto */
    IOCTL_DEBUG_OP_AWDL_CREATE = 6, /* Tahoe Airport: create native interface, val must be 0 */
};

struct ioctl_debug_set {
    unsigned int version;
    uint32_t op;
    uint32_t val;
};

#define MTK_LOG_CHUNK 3800

struct ioctl_log {
    unsigned int version;
    uint32_t len;
    uint64_t cursor;    /* position after this chunk */
    uint64_t head;      /* total bytes ever logged */
    char data[MTK_LOG_CHUNK];
};

#endif /* Common_h */
