// SPDX-License-Identifier: GPL-2.0-or-later
// Wire protocol and scheduling primitives. No IOKit dependencies: the same
// implementation is exercised by the userspace sanitizer tests.
//
// MtkWireless: the logic lives in Rust (rust/mtk-wlan/src/awdl). This header
// keeps the original structs (data members, same order; AwdlLayout.hpp
// checks them against the Rust side) and forwards every function and
// method through the C ABI in rust/mtk-ffi/src/awdl.rs.
#ifndef MTK_AWDL_PROTOCOL_HPP
#define MTK_AWDL_PROTOCOL_HPP
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace mtkawdl {
struct Sequence; struct Advertisement; struct Peer; struct DataPeers; struct State;
struct DataView; struct TxStatusCounters; struct Reservation; struct RocGrant;
enum class RocResult : uint8_t;
}

extern "C" {
bool mtk_awdl_valid_presence(uint32_t presence);
bool mtk_awdl_valid_sequence(const mtkawdl::Sequence *s);
bool mtk_awdl_parse_sequence(const uint8_t *p, size_t n, mtkawdl::Sequence *out);
bool mtk_awdl_valid_action_envelope(const uint8_t *f, size_t n);
bool mtk_awdl_parse_action(const uint8_t *f, size_t n, mtkawdl::Advertisement *out);
uint8_t mtk_awdl_peer_channel(const mtkawdl::Peer *p, uint64_t now);
void mtk_awdl_data_peers_reset(mtkawdl::DataPeers *d);
bool mtk_awdl_data_peers_accept(mtkawdl::DataPeers *d, size_t index, size_t stream, uint16_t seq, uint64_t now);
bool mtk_awdl_data_peers_live(const void *entry, uint64_t now);
int32_t mtk_awdl_data_peers_find(const mtkawdl::DataPeers *d, const uint8_t *mac, uint64_t now);
int32_t mtk_awdl_data_peers_observe(mtkawdl::DataPeers *d, const uint8_t *mac, uint64_t now);
bool mtk_awdl_data_peers_can_observe(const mtkawdl::DataPeers *d, const uint8_t *mac, uint64_t now);
bool mtk_awdl_data_peers_set_cached(mtkawdl::DataPeers *d, const uint8_t *mac, bool value, uint64_t now);
bool mtk_awdl_data_peers_set_traffic(mtkawdl::DataPeers *d, const uint8_t *mac, bool value, uint64_t now);
void mtk_awdl_state_hold_controls(mtkawdl::State *s);
bool mtk_awdl_state_requested_parent_is(const mtkawdl::State *s, const uint8_t *address);
void mtk_awdl_state_select_native_parent(mtkawdl::State *s, const uint8_t *address, const uint8_t *root, bool acquire);
void mtk_awdl_state_clear_native_parent(mtkawdl::State *s);
bool mtk_awdl_state_release_controls(mtkawdl::State *s, uint64_t now);
void mtk_awdl_state_init(mtkawdl::State *s, const uint8_t *address, uint64_t now);
uint16_t mtk_awdl_state_counter(const mtkawdl::State *s, uint64_t now);
uint32_t mtk_awdl_state_until_next(const mtkawdl::State *s, uint64_t now);
uint8_t mtk_awdl_state_channel(const mtkawdl::State *s, uint64_t now);
bool mtk_awdl_state_radio_schedule_ready(const mtkawdl::State *s);
bool mtk_awdl_state_set_sync_role(mtkawdl::State *s, uint32_t role, const uint8_t *address);
bool mtk_awdl_state_set_presence(mtkawdl::State *s, uint8_t value, uint64_t now);
bool mtk_awdl_state_set_sequence(mtkawdl::State *s, const mtkawdl::Sequence *value, uint64_t now);
void mtk_awdl_state_set_social_channel(mtkawdl::State *s, uint32_t index, uint8_t channel, uint64_t now);
int32_t mtk_awdl_state_find(const mtkawdl::State *s, const uint8_t *address);
void mtk_awdl_state_elect(mtkawdl::State *s, uint64_t now);
int32_t mtk_awdl_state_receive_action(mtkawdl::State *s, const uint8_t *f, size_t n, uint64_t now);
bool mtk_awdl_accept_data(mtkawdl::Peer *p, uint16_t seq);
size_t mtk_awdl_build_action(mtkawdl::State *s, uint8_t *f, size_t capacity, uint64_t now);
bool mtk_awdl_valid_template_tlvs(const uint8_t *p, size_t n);
size_t mtk_awdl_build_action_with_template(mtkawdl::State *s, uint8_t *f, size_t capacity, uint64_t now,
                                           const uint8_t *p, size_t n);
bool mtk_awdl_block_ack_action(const uint8_t *f, size_t n, const uint8_t *mac);
uint8_t mtk_awdl_data_channel(const mtkawdl::Sequence *s);
void mtk_awdl_dense_sequence(const mtkawdl::Sequence *s, uint8_t channel, mtkawdl::Sequence *out);
bool mtk_awdl_addba_request(const uint8_t *own, const uint8_t *request, size_t n, uint8_t *tid, uint16_t *ssn,
                            uint16_t *window);
bool mtk_awdl_delba_rx_tid(const uint8_t *own, const uint8_t *f, size_t n, uint8_t *tid);
size_t mtk_awdl_build_addba_accept(mtkawdl::State *s, const uint8_t *request, size_t n, uint16_t window,
                                   uint8_t *f, size_t capacity);
size_t mtk_awdl_build_addba_decline(mtkawdl::State *s, const uint8_t *request, size_t n, uint8_t *f, size_t capacity);
size_t mtk_awdl_refresh_action(mtkawdl::State *s, uint8_t *f, size_t n, uint64_t now);
size_t mtk_awdl_encode_data(uint8_t *out, size_t capacity, const uint8_t *eth, size_t n, uint16_t seq);
size_t mtk_awdl_data_header_length(const uint8_t *f, size_t n);
bool mtk_awdl_relocate_deaggregated_header(uint8_t *f, size_t n);
size_t mtk_awdl_data_view(const uint8_t *f, size_t n, size_t at, mtkawdl::DataView *out);
bool mtk_awdl_valid_data_frame(const uint8_t *f, size_t n);
size_t mtk_awdl_decode_data_view(uint8_t *out, size_t capacity, const mtkawdl::DataView *view);
size_t mtk_awdl_decode_data(uint8_t *out, size_t capacity, const uint8_t *f, size_t n, uint16_t *seq);
bool mtk_awdl_tx_status_observe(mtkawdl::TxStatusCounters *c, const uint8_t *p, size_t n);
uint16_t mtk_awdl_tx_rate(const mtkawdl::TxStatusCounters *c, uint16_t wcid);
uint16_t mtk_awdl_tx_rate_mbps(const mtkawdl::TxStatusCounters *c, size_t peer);
void mtk_awdl_reservation_request(mtkawdl::Reservation *r, uint8_t token, uint8_t channel, uint64_t now);
mtkawdl::RocResult mtk_awdl_reservation_accept(mtkawdl::Reservation *r, uint8_t token, uint8_t channel,
                                               uint32_t durationMs, uint64_t now);
bool mtk_awdl_reservation_can_transmit(const mtkawdl::Reservation *r, uint8_t channel, uint64_t now);
void mtk_awdl_reservation_cancel(mtkawdl::Reservation *r);
mtkawdl::RocResult mtk_awdl_decode_roc_grant(const uint8_t *p, size_t n, mtkawdl::RocGrant *out);
mtkawdl::RocResult mtk_awdl_receive_roc_grant(mtkawdl::Reservation *r, const uint8_t *p, size_t n, uint8_t bss,
                                              bool enabled, uint64_t now, mtkawdl::RocGrant *out);
}

namespace mtkawdl {
static const uint8_t bssid[6] = {0, 0x25, 0, 0xff, 0x94, 0x73};
static const uint8_t snap[8] = {0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0};
static const unsigned maxPeers = 16, slots = 16;
static const uint64_t peerLifetimeUs = 3000000;
static const uint32_t awUs = 16 * 1024;
// Largest supported EAW is 64*16TU (1048.576ms). Leave room for ROC
// request lead/guard while bounding continuous RF ownership below MT76's 5s.
static const uint32_t maxRocMs = 1100;
// mt7921_set_roc waits HZ (one second). A delayed firmware response must
// retain its request identity; this timeout does not grant any RF time.
static const uint32_t rocResponseTimeoutUs = 1000000;
static const uint32_t rocTokenQuarantineUs = rocResponseTimeoutUs + maxRocMs*1000;
// Byte helpers for callers' own, already bounds-checked buffers.
inline uint16_t le16(const uint8_t *p) { return p[0] | uint16_t(p[1]) << 8; }
inline uint32_t le32(const uint8_t *p) { return le16(p) | uint32_t(le16(p+2)) << 16; }
inline uint32_t be32(const uint8_t *p) { return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3]; }
inline void put16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
inline void put32(uint8_t *p, uint32_t v) { put16(p,v); put16(p+2,v>>16); }
inline void putBE32(uint8_t *p,uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
inline bool unicast(const uint8_t *p) {
    static const uint8_t zero[6] = {};
    return !(p[0]&1) && memcmp(p,zero,6);
}
struct Sequence {
    uint8_t channels[slots];
    uint8_t count, step;
};
inline bool validPresence(unsigned presence) { return mtk_awdl_valid_presence(presence); }
inline bool validSequence(const Sequence &s) { return mtk_awdl_valid_sequence(&s); }
inline bool parseSequence(const uint8_t *p, size_t n, Sequence &out) { return mtk_awdl_parse_sequence(p,n,&out); }
struct Advertisement {
    Sequence sequence;
    uint8_t master[6], sync[6];
    uint16_t period, counter, down;
    uint8_t presence;
    uint32_t metric, distance, masterCounter, selfCounter;
};
inline bool validActionEnvelope(const uint8_t *f,size_t n) { return mtk_awdl_valid_action_envelope(f,n); }
inline bool parseAction(const uint8_t *f, size_t n, Advertisement &out) { return mtk_awdl_parse_action(f,n,&out); }
struct Peer {
    uint8_t mac[6]; Advertisement advertisement;
    uint64_t seen, epoch;
    uint16_t baseCounter, lastData;
    uint64_t replay;
    bool used, hasData;
    uint8_t channel(uint64_t now) const { return mtk_awdl_peer_channel(this,now); }
};
// Native cache/traffic registration is authoritative for data membership;
// synchronization advertisements have a separate, short-lived radio schedule.
struct DataReplayWindow {
    uint64_t bitmap, lastRx;
    uint16_t sequence;
    bool active;
    uint8_t sequenceBits;
};
struct DataPeers {
    struct Entry {
        Peer replayState;
        DataReplayWindow windows[17]; // TID 0..15; non-QoS at 16
        uint64_t seen;
        bool cached, traffic;
    } entries[maxPeers]={};
    void reset() { mtk_awdl_data_peers_reset(this); }
    bool acceptData(size_t index,size_t stream,uint16_t seq,uint64_t now) { return mtk_awdl_data_peers_accept(this,index,stream,seq,now); }
    bool live(const Entry &e,uint64_t now) const { return mtk_awdl_data_peers_live(&e,now); }
    int find(const uint8_t *mac,uint64_t now) const { return mtk_awdl_data_peers_find(this,mac,now); }
    int observe(const uint8_t *mac,uint64_t now) { return mtk_awdl_data_peers_observe(this,mac,now); }
    bool canObserve(const uint8_t *mac,uint64_t now) const { return mtk_awdl_data_peers_can_observe(this,mac,now); }
    bool setCached(const uint8_t *mac,bool value,uint64_t now) { return mtk_awdl_data_peers_set_cached(this,mac,value,now); }
    bool setTraffic(const uint8_t *mac,bool value,uint64_t now) { return mtk_awdl_data_peers_set_traffic(this,mac,value,now); }
};
struct State {
    uint8_t mac[6], master[6], sync[6], forcedSync[6];
    Sequence sequence, ownSequence; Peer peers[maxPeers];
    uint64_t epoch; uint16_t baseCounter, dataCounter, frameCounter, psfPeriod;
    uint32_t metric, masterMetric, distance, masterCounter, selfCounter; bool synchronized;
    uint64_t electionEpoch;
    uint8_t presence, ownPresence, syncRole, minExtension[4];
    bool nativeElectionManaged, nativeParentSelected;
    uint8_t nativeParent[6];
    uint64_t clockSampleAt, clockCorrections, clockRejected;
    // Software equivalent of a firmware AW clock: peer observations remain
    // fresh, but phase discipline waits until the promised radio AW ends.
    uint64_t clockHoldUntil, clockDeferredAt, clockDeferred;
    void holdClock(uint64_t until) { clockHoldUntil=until; }
    // The selected clock survives opmode changes and temporary peer expiry.
    // Effective radio controls are a transaction applied between AW events.
    bool nativeClockSelected, nativeTimelineStarted, controlsHeld, controlsDirty;
    bool nativeClockHoldover, nativeClockValidated, nativeClockAcquisitionPending;
    bool clockSampleRejected;
    uint64_t nativeHoldoverTransitions, nativeClockAcquisitions;
    uint8_t clockParent[6], clockRoot[6];
    uint8_t acquisitionParent[6], acquisitionRoot[6];
    bool acquisitionRootKnown;
    uint64_t controlsDeferred;
    struct Controls {
        Sequence sequence;
        uint8_t presence, role, forced[6], parent[6];
        bool parentSelected;
    } pendingControls;
    void holdControls() { mtk_awdl_state_hold_controls(this); }
    void discardControls() { controlsHeld=controlsDirty=false; }
    uint8_t requestedRole() const { return controlsHeld ? pendingControls.role : syncRole; }
    bool requestedParentIs(const uint8_t *address) const { return mtk_awdl_state_requested_parent_is(this,address); }
    void selectNativeParent(const uint8_t *address,const uint8_t *root=nullptr,bool acquire=false) {
        mtk_awdl_state_select_native_parent(this,address,root,acquire);
    }
    void clearNativeParent() { mtk_awdl_state_clear_native_parent(this); }
    bool releaseControls(uint64_t now) { return mtk_awdl_state_release_controls(this,now); }
    void init(const uint8_t *address, uint64_t now) { mtk_awdl_state_init(this,address,now); }
    uint16_t counter(uint64_t now) const { return mtk_awdl_state_counter(this,now); }
    uint32_t untilNext(uint64_t now) const { return mtk_awdl_state_until_next(this,now); }
    uint8_t channel(uint64_t now) const { return mtk_awdl_state_channel(this,now); }
    bool radioScheduleReady() const { return mtk_awdl_state_radio_schedule_ready(this); }
    bool setSyncRole(unsigned role,const uint8_t *address) { return mtk_awdl_state_set_sync_role(this,role,address); }
    bool setPresence(uint8_t value,uint64_t now) { return mtk_awdl_state_set_presence(this,value,now); }
    bool setSequence(const Sequence &value,uint64_t now) { return mtk_awdl_state_set_sequence(this,&value,now); }
    void setSocialChannel(unsigned index,uint8_t channel,uint64_t now) {
        mtk_awdl_state_set_social_channel(this,index,channel,now);
    }
    int find(const uint8_t *address) const { return mtk_awdl_state_find(this,address); }
    void elect(uint64_t now) { mtk_awdl_state_elect(this,now); }
    int receiveAction(const uint8_t *f,size_t n,uint64_t now) { return mtk_awdl_state_receive_action(this,f,n,now); }
    bool acceptData(Peer &p,uint16_t seq) { return mtk_awdl_accept_data(&p,seq); }
};
inline size_t buildAction(State &s,uint8_t *f,size_t capacity,uint64_t now) {
    return mtk_awdl_build_action(&s,f,capacity,now);
}
// Tahoe's native SYNC_FRAME_TEMPLATE contains supplemental TLVs, not the
// vendor action header or synchronization TLV.
inline bool validTemplateTLVs(const uint8_t *p,size_t n) { return mtk_awdl_valid_template_tlvs(p,n); }
inline size_t buildActionWithTemplate(State &s,uint8_t *f,size_t capacity,
                                     uint64_t now,const uint8_t *p,size_t n) {
    return mtk_awdl_build_action_with_template(&s,f,capacity,now,p,n);
}
// 802.11 Block Ack action frames (category 3) addressed to this AWDL MAC.
// An RX request is accepted once the HAL installs the firmware recipient
// session (MtkMwx::awdlRxBlockAck), else declined. No reorder buffer: the
// MSDU replay window admits out-of-order sequences within 64.
enum : uint8_t { blockAckCategory=3, addbaRequest=0, addbaResponse=1, delba=2 };
enum : uint16_t { statusRequestDeclined=37, rxBlockAckWindow=64 };
inline bool blockAckAction(const uint8_t *f,size_t n,const uint8_t *mac) {
    return mtk_awdl_block_ack_action(f,n,mac);
}
inline uint8_t dataChannel(const Sequence &s) { return mtk_awdl_data_channel(&s); }
inline Sequence denseSequence(const Sequence &s,uint8_t channel) {
    Sequence d=s; mtk_awdl_dense_sequence(&s,channel,&d); return d;
}
inline bool parseAddbaRequest(const uint8_t *own,const uint8_t *request,size_t n,uint8_t *tid,uint16_t *ssn,uint16_t *window) {
    return mtk_awdl_addba_request(own,request,n,tid,ssn,window);
}
inline bool delbaRxTid(const uint8_t *own,const uint8_t *f,size_t n,uint8_t *tid) {
    return mtk_awdl_delba_rx_tid(own,f,n,tid);
}
inline size_t buildAddbaAccept(State &s,const uint8_t *request,size_t n,uint16_t window,uint8_t *f,size_t capacity) {
    return mtk_awdl_build_addba_accept(&s,request,n,window,f,capacity);
}
inline size_t buildAddbaDecline(State &s,const uint8_t *request,size_t n,uint8_t *f,size_t capacity) {
    return mtk_awdl_build_addba_decline(&s,request,n,f,capacity);
}
// Refresh a native action-frame template in place (validation first; a
// rejection leaves the template and State untouched).
inline size_t refreshAction(State &s,uint8_t *f,size_t n,uint64_t now) {
    return mtk_awdl_refresh_action(&s,f,n,now);
}
inline size_t encodeData(uint8_t *out,size_t capacity,const uint8_t *eth,size_t n,uint16_t seq) {
    return mtk_awdl_encode_data(out,capacity,eth,n,seq);
}
inline size_t dataHeaderLength(const uint8_t *f,size_t n) { return mtk_awdl_data_header_length(f,n); }
// MT7921 hardware deaggregation adds two bytes after the 802.11 header.
inline bool relocateDeaggregatedHeader(uint8_t *f,size_t n) { return mtk_awdl_relocate_deaggregated_header(f,n); }
struct DataView {
    const uint8_t *destination, *source, *payload;
    size_t length; uint16_t sequence;
};
inline size_t dataView(const uint8_t *f,size_t n,size_t at,DataView &out) { return mtk_awdl_data_view(f,n,at,&out); }
inline bool validDataFrame(const uint8_t *f,size_t n) { return mtk_awdl_valid_data_frame(f,n); }
inline size_t decodeDataView(uint8_t *out,size_t capacity,const DataView &view) {
    return mtk_awdl_decode_data_view(out,capacity,&view);
}
// Single-MSDU API retained for callers expecting exactly one Ethernet frame.
inline size_t decodeData(uint8_t *out,size_t capacity,const uint8_t *f,size_t n,uint16_t &seq) {
    return mtk_awdl_decode_data(out,capacity,f,n,&seq);
}
// Reserved software PIDs distinguish AWDL frame classes in MT7921 TXS.
// WCIDs 15..18 belong exclusively to AWDL. A DMA release is not a TX result.
constexpr uint8_t txPidUnicast=125, txPidMulticast=126, txPidAction=127;
struct TxRate {
    uint8_t index=0, successes=0;
    bool failed=false, probing=false;
    uint8_t backoff=0, stale=0;
    uint16_t probeAfter=0;
};
struct TxStatusCounters {
    uint64_t dataAcked=0, dataFailed=0, multicastCompleted=0, multicastFailed=0;
    uint64_t actionCompleted=0, actionFailed=0;
    uint32_t lastStatus=0;
    uint16_t lastSequence=0, lastWcid=0;
    uint8_t lastPid=0;
    TxRate rates[3]={};
    bool ofdmOnly=false; // boot-arg amtk_awdl_ht=0
    bool observe(const uint8_t *p,size_t n) { return mtk_awdl_tx_status_observe(this,p,n); }
    uint16_t rate(uint16_t wcid) const { return mtk_awdl_tx_rate(this,wcid); }
    uint16_t rateMbps(size_t peer) const { return mtk_awdl_tx_rate_mbps(this,peer); }
};
// A matching, unexpired firmware grant is mandatory before any transmission.
enum class RocResult : uint8_t {
    Accepted, Malformed, NoGrant, WrongBss, FirmwareStatus, WrongType,
    Disabled, NotPending, Token, Channel, Late, Duration, Count
};
struct Reservation {
    uint64_t deadline, grantObservedAt, requestedAt; uint8_t token, channel; bool pending, granted;
    uint32_t responseDelayUS;
    void request(uint8_t t,uint8_t c,uint64_t now) { mtk_awdl_reservation_request(this,t,c,now); }
    RocResult accept(uint8_t t,uint8_t c,uint32_t durationMs,uint64_t now) {
        return mtk_awdl_reservation_accept(this,t,c,durationMs,now);
    }
    bool grant(uint8_t t,uint8_t c,uint32_t durationMs,uint64_t now) {
        return accept(t,c,durationMs,now)==RocResult::Accepted;
    }
    bool canTransmit(uint8_t c,uint64_t now) const { return mtk_awdl_reservation_can_transmit(this,c,now); }
    void cancel() { mtk_awdl_reservation_cancel(this); }
};
// Unified ROC event: four-byte header followed by bounded TLVs. MT7921's
// grant tag is zero and its complete structure is 20 bytes (mt7921.h).
struct RocGrant {
    uint8_t bss, token, status, channel, requestType;
    uint32_t durationMs;
};
inline RocResult decodeRocGrant(const uint8_t *p,size_t n,RocGrant &out) { return mtk_awdl_decode_roc_grant(p,n,&out); }
inline RocResult receiveRocGrant(Reservation &reservation,const uint8_t *p,size_t n,
                                uint8_t bss,bool enabled,uint64_t now,RocGrant &out) {
    return mtk_awdl_receive_roc_grant(&reservation,p,n,bss,enabled,now,&out);
}
}
#endif
