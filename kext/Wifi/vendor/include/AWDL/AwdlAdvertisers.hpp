// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef MTK_AWDL_ADVERTISERS_HPP
#define MTK_AWDL_ADVERTISERS_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace mtkawdl {

// Tahoe's native AWDL_ADVERTISERS control record. Integer fields are encoded
// explicitly because the caller buffer is packed and +20 is big-endian.
struct __attribute__((packed)) AdvertiserRecord {
    uint16_t awCounter;
    uint16_t downCounter;
    uint16_t syncContention;
    uint16_t afPeriod;
    uint16_t raw8;
    uint16_t raw10;
    uint16_t awPeriod;
    int8_t rssi;
    uint8_t reserved15;
    uint8_t presence;
    uint8_t reserved17;
    uint8_t raw18;
    uint8_t reserved19;
    uint32_t fwAfRxTime;
    uint32_t peerSelfMetric;
    uint32_t rootMetric;
    uint8_t rootMaster[6];
    uint8_t immediatePeer[6];
};

static_assert(sizeof(AdvertiserRecord) == 44, "advertiser record size");

inline uint16_t advertiserLE16(const uint8_t *p) {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}

inline uint32_t advertiserLE32(const uint8_t *p) {
    return uint32_t(advertiserLE16(p)) |
           uint32_t(advertiserLE16(p + 2)) << 16;
}

inline uint32_t advertiserBE32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
           uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

inline bool decodeAdvertiserRecord(const uint8_t *raw, size_t length,
                                   AdvertiserRecord &out) {
    if (!raw || length != 44) return false;
    memset(&out, 0, sizeof(out));
    out.awCounter = advertiserLE16(raw + 0);
    out.downCounter = advertiserLE16(raw + 2);
    out.syncContention = advertiserLE16(raw + 4);
    out.afPeriod = advertiserLE16(raw + 6);
    out.raw8 = advertiserLE16(raw + 8);
    out.raw10 = advertiserLE16(raw + 10);
    out.awPeriod = advertiserLE16(raw + 12);
    out.rssi = static_cast<int8_t>(raw[14]);
    out.reserved15 = raw[15];
    out.presence = raw[16];
    out.reserved17 = raw[17];
    out.raw18 = raw[18];
    out.reserved19 = raw[19];
    out.fwAfRxTime = advertiserBE32(raw + 20);
    out.peerSelfMetric = advertiserLE32(raw + 24);
    out.rootMetric = advertiserLE32(raw + 28);
    memcpy(out.rootMaster, raw + 32, 6);
    memcpy(out.immediatePeer, raw + 38, 6);
    return true;
}

inline bool sameAdvertiserRecord(const AdvertiserRecord &a,
                                 const AdvertiserRecord &b) {
    return a.awCounter == b.awCounter && a.downCounter == b.downCounter &&
           a.syncContention == b.syncContention && a.afPeriod == b.afPeriod &&
           a.raw8 == b.raw8 && a.raw10 == b.raw10 && a.awPeriod == b.awPeriod &&
           a.rssi == b.rssi && a.reserved15 == b.reserved15 &&
           a.presence == b.presence && a.reserved17 == b.reserved17 &&
           a.raw18 == b.raw18 && a.reserved19 == b.reserved19 &&
           a.fwAfRxTime == b.fwAfRxTime &&
           a.peerSelfMetric == b.peerSelfMetric && a.rootMetric == b.rootMetric &&
           !memcmp(a.rootMaster, b.rootMaster, 6) &&
           !memcmp(a.immediatePeer, b.immediatePeer, 6);
}

} // namespace mtkawdl

#endif
