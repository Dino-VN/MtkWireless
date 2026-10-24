// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef MTK_AWDL_TIMELINE_HPP
#define MTK_AWDL_TIMELINE_HPP
#include <AWDL/AwdlProtocol.hpp>
// MtkWireless: logic in rust/mtk-wlan/src/awdl/timeline.rs; structs keep
// their original layout (AwdlLayout.hpp).
namespace mtkawdl { struct Window; struct RocLeadTiming; struct RocChannelTiming; struct RadioPlan; struct Timeline; }
extern "C" {
bool mtk_awdl_current_window(const mtkawdl::State *s, uint64_t now, mtkawdl::Window *out);
bool mtk_awdl_multicast_ready(const mtkawdl::State *s, uint64_t now);
uint32_t mtk_awdl_data_queue_lifetime_us(const mtkawdl::State *s, const uint8_t *destination);
uint32_t mtk_awdl_roc_lead_observe(mtkawdl::RocLeadTiming *l, uint32_t responseUs, uint64_t preparationUs);
uint32_t mtk_awdl_roc_channel_lead(const mtkawdl::RocChannelTiming *c, uint8_t channel, uint32_t fallback);
void mtk_awdl_roc_channel_observe(mtkawdl::RocChannelTiming *c, uint8_t channel, uint32_t responseUs,
                                  uint64_t preparationUs);
bool mtk_awdl_plan_radio(const mtkawdl::State *s, uint64_t now, mtkawdl::RadioPlan *out, uint32_t leadUs,
                         bool skipCurrent, const mtkawdl::RocChannelTiming *timing, uint8_t onlyChannel);
bool mtk_awdl_grant_covers_window(const mtkawdl::Window *w, const mtkawdl::Reservation *grant, uint64_t now);
bool mtk_awdl_grant_covers_reserved_window(const mtkawdl::State *s, const mtkawdl::Window *reserved,
                                           uint8_t requestedChannel, const mtkawdl::Reservation *grant,
                                           uint64_t now);
bool mtk_awdl_timeline_matches_schedule(const mtkawdl::Timeline *t, const mtkawdl::State *s);
bool mtk_awdl_timeline_begin(mtkawdl::Timeline *t, const mtkawdl::Window *w, const mtkawdl::Reservation *grant,
                             uint64_t now, const uint8_t *parent, const uint8_t *master, uint8_t nativeRole,
                             const mtkawdl::Sequence *radioSchedule);
}
namespace mtkawdl {
// Host observations use monotonic microseconds; AW counters are peer schedule
// coordinates and must never be used as a timestamp clock.
struct Window {
    uint64_t start, end;
    uint16_t counter;
    uint8_t channel, presence;
};
inline bool currentWindow(const State &s,uint64_t now,Window &out) { return mtk_awdl_current_window(&s,now,&out); }
// AWDL multicast rendezvous uses EAW slots 0 and 10 in the 16-slot cycle.
// Preserve the packet through other windows; sending an mDNS query on an
// arbitrary discovery channel does not reach the peers listening for it.
inline bool multicastReady(const State &s,uint64_t now) { return mtk_awdl_multicast_ready(&s,now); }
// One MCU queue admission can wait 30*100ms, then ROC has a one-second
// response budget. Add one complete local/known-destination channel cycle
// and the scheduler's 8ms edge margin. Snapshot this budget on enqueue:
// changing schedules and resource retries must not extend a packet's life.
inline uint32_t dataQueueLifetimeUs(const State &s,const uint8_t *destination) {
    return mtk_awdl_data_queue_lifetime_us(&s,destination);
}
// Idle slots leave time for the secondary BSS RLM response and off-channel
// arbitration before the next EAW. A live EAW is still closed only within
// native's duration tolerance; this lead never fabricates an early AW-start.
constexpr uint32_t rocLeadUs=55000;
// Keep a bounded history of actual accepted ROC response/preparation delays.
// A boot-time outlier must not permanently skip nearby AWs after firmware
// recovers. Repeated polling of the same grant is not a timing observation.
struct RocLeadTiming {
    uint32_t samples[8]={}, leadUs=rocLeadUs;
    unsigned count=0, next=0;
    uint64_t updates=0, decreases=0;
    uint32_t observe(uint32_t responseUs,uint64_t preparationUs) {
        return mtk_awdl_roc_lead_observe(this,responseUs,preparationUs);
    }
};
// Arbitration differs by destination channel (for example, ~133ms on 6
// and ~12ms on 149). Keep bounded independent histories so a slow channel
// does not hide a reachable AW on a fast channel. An unseen/evicted channel
// gets a bounded startup probe until its own accepted grant is observed.
// Age a channel out after eight newer grants on other channels, matching
// the global history's horizon. Otherwise one old startup spike can prevent
// revisiting that channel forever, so its estimate can never recover.
struct RocChannelTiming {
    struct Entry { uint8_t channel=0; uint64_t observed=0; RocLeadTiming timing; } entries[slots];
    unsigned next=0;
    uint64_t observations=0;
    uint32_t lead(uint8_t channel,uint32_t fallback=rocLeadUs) const {
        return mtk_awdl_roc_channel_lead(this,channel,fallback);
    }
    void observe(uint8_t channel,uint32_t responseUs,uint64_t preparationUs) {
        mtk_awdl_roc_channel_observe(this,channel,responseUs,preparationUs);
    }
};
// Prepare the BSS and ROC before the edge, and keep one grant for adjacent
// equal channels. A grant alone never changes the announced schedule.
struct RadioPlan { Window window; uint64_t requestAt; uint32_t durationMs; };
// onlyChannel != 0: treat windows on other channels as idle (while
// associated to an AP on that channel).
inline bool planRadio(const State &s,uint64_t now,RadioPlan &out,uint32_t leadUs=rocLeadUs,
                      bool skipCurrent=false,const RocChannelTiming *timing=nullptr,
                      uint8_t onlyChannel=0) {
    return mtk_awdl_plan_radio(&s,now,&out,leadUs,skipCurrent,timing,onlyChannel);
}
inline bool grantCoversWindow(const Window &w,const Reservation &grant,uint64_t now) {
    return mtk_awdl_grant_covers_window(&w,&grant,now);
}
// Keep the original future reservation when an early grant arrives. Replanning
// with now + lead would otherwise move the target beyond this valid grant.
// Revalidate the actual schedule: an expired/short grant or a changed window
// must not pin RF ownership or produce an event for stale coordinates.
inline bool grantCoversReservedWindow(const State &s,const Window &reserved,
                                     uint8_t requestedChannel,const Reservation &grant,
                                     uint64_t now) {
    return mtk_awdl_grant_covers_reserved_window(&s,&reserved,requestedChannel,&grant,now);
}
struct Timeline {
    Window active={};
    bool open=false, emitted=false;
    uint64_t radioEnd=0;
    uint8_t role=1, token=0;
    uint64_t grantObservedAt=0;
    uint64_t lastStart=0, lastWindowStart=0, starts=0, ends=0, late=0, shortGrants=0;
    uint16_t lastCounter=0;
    uint8_t sync[6]={}, root[6]={};
    uint8_t lastSync[6]={}, lastRoot[6]={};
    uint8_t lastPresence=0;
    Sequence schedule={};
    bool scheduleRecorded=false;
    bool matchesSchedule(const State &s) const { return mtk_awdl_timeline_matches_schedule(this,&s); }
    // Report an actual grant near the EAW edge. A firmware grant may be
    // shorter than the requested dwell: track its real end instead of hiding
    // the whole AW or promising radio availability until the scheduled end.
    bool begin(const Window &w,const Reservation &grant,uint64_t now,
               const uint8_t *parent,const uint8_t *master,uint8_t nativeRole=1,
               const Sequence *radioSchedule=nullptr) {
        return mtk_awdl_timeline_begin(this,&w,&grant,now,parent,master,nativeRole,radioSchedule);
    }
    bool due(uint64_t now) const { return open && now>=radioEnd; }
    uint32_t switchLeadUS() const {
        uint64_t lateStart=lastStart-active.start;
        return lateStart<10*1024 ? static_cast<uint32_t>(10*1024-lateStart) : 0;
    }
    void close() { if(open) { open=false; ++ends; } }
    void reset() { open=false; emitted=false; lastWindowStart=radioEnd=0; }
};
}
#endif
