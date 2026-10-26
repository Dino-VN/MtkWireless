// SPDX-License-Identifier: GPL-2.0-or-later
// MtkWireless: the AWDL structs are shared with Rust (rust/mtk-ffi/src/awdl.rs,
// LAYOUT). layoutMatches() compares every size, alignment and field offset;
// a mismatch means the two declarations drifted and AWDL must not run.
#ifndef MTK_AWDL_LAYOUT_HPP
#define MTK_AWDL_LAYOUT_HPP
#include <stddef.h>
#include <AWDL/AwdlProtocol.hpp>
#include <AWDL/AwdlTimeline.hpp>
#include <AWDL/AwdlAdvertisers.hpp>

extern "C" size_t mtk_awdl_layout(size_t index);
extern "C" size_t mtk_awdl_layout_len();

namespace mtkawdl {

#define MTK_AWDL_TYPE(T) sizeof(T), alignof(T)
#define MTK_AWDL_AT(T, f) offsetof(T, f)
// Same order as LAYOUT in rust/mtk-ffi/src/awdl.rs.
static const size_t kLayout[] = {
    MTK_AWDL_TYPE(Sequence), MTK_AWDL_AT(Sequence, channels), MTK_AWDL_AT(Sequence, count),
    MTK_AWDL_AT(Sequence, step),
    MTK_AWDL_TYPE(Advertisement), MTK_AWDL_AT(Advertisement, sequence), MTK_AWDL_AT(Advertisement, master),
    MTK_AWDL_AT(Advertisement, sync), MTK_AWDL_AT(Advertisement, period), MTK_AWDL_AT(Advertisement, counter),
    MTK_AWDL_AT(Advertisement, down), MTK_AWDL_AT(Advertisement, presence), MTK_AWDL_AT(Advertisement, metric),
    MTK_AWDL_AT(Advertisement, distance), MTK_AWDL_AT(Advertisement, masterCounter),
    MTK_AWDL_AT(Advertisement, selfCounter),
    MTK_AWDL_TYPE(Peer), MTK_AWDL_AT(Peer, mac), MTK_AWDL_AT(Peer, advertisement), MTK_AWDL_AT(Peer, seen),
    MTK_AWDL_AT(Peer, epoch), MTK_AWDL_AT(Peer, baseCounter), MTK_AWDL_AT(Peer, lastData),
    MTK_AWDL_AT(Peer, replay), MTK_AWDL_AT(Peer, used), MTK_AWDL_AT(Peer, hasData),
    MTK_AWDL_TYPE(DataReplayWindow), MTK_AWDL_AT(DataReplayWindow, bitmap),
    MTK_AWDL_AT(DataReplayWindow, lastRx), MTK_AWDL_AT(DataReplayWindow, sequence), MTK_AWDL_AT(DataReplayWindow, active),
    MTK_AWDL_AT(DataReplayWindow, sequenceBits),
    MTK_AWDL_TYPE(DataPeers::Entry), MTK_AWDL_AT(DataPeers::Entry, replayState),
    MTK_AWDL_AT(DataPeers::Entry, windows), MTK_AWDL_AT(DataPeers::Entry, seen),
    MTK_AWDL_AT(DataPeers::Entry, cached), MTK_AWDL_AT(DataPeers::Entry, traffic),
    MTK_AWDL_TYPE(DataPeers), MTK_AWDL_AT(DataPeers, entries),
    MTK_AWDL_TYPE(State), MTK_AWDL_AT(State, mac), MTK_AWDL_AT(State, master), MTK_AWDL_AT(State, sync),
    MTK_AWDL_AT(State, forcedSync), MTK_AWDL_AT(State, sequence), MTK_AWDL_AT(State, ownSequence),
    MTK_AWDL_AT(State, peers), MTK_AWDL_AT(State, epoch), MTK_AWDL_AT(State, baseCounter),
    MTK_AWDL_AT(State, dataCounter), MTK_AWDL_AT(State, frameCounter), MTK_AWDL_AT(State, psfPeriod),
    MTK_AWDL_AT(State, metric), MTK_AWDL_AT(State, masterMetric), MTK_AWDL_AT(State, distance),
    MTK_AWDL_AT(State, masterCounter), MTK_AWDL_AT(State, selfCounter), MTK_AWDL_AT(State, synchronized),
    MTK_AWDL_AT(State, electionEpoch), MTK_AWDL_AT(State, presence), MTK_AWDL_AT(State, ownPresence),
    MTK_AWDL_AT(State, syncRole), MTK_AWDL_AT(State, minExtension), MTK_AWDL_AT(State, nativeElectionManaged),
    MTK_AWDL_AT(State, nativeParentSelected), MTK_AWDL_AT(State, nativeParent),
    MTK_AWDL_AT(State, clockSampleAt), MTK_AWDL_AT(State, clockCorrections), MTK_AWDL_AT(State, clockRejected),
    MTK_AWDL_AT(State, clockHoldUntil), MTK_AWDL_AT(State, clockDeferredAt), MTK_AWDL_AT(State, clockDeferred),
    MTK_AWDL_AT(State, nativeClockSelected), MTK_AWDL_AT(State, nativeTimelineStarted),
    MTK_AWDL_AT(State, controlsHeld), MTK_AWDL_AT(State, controlsDirty), MTK_AWDL_AT(State, nativeClockHoldover),
    MTK_AWDL_AT(State, nativeClockValidated), MTK_AWDL_AT(State, nativeClockAcquisitionPending),
    MTK_AWDL_AT(State, clockSampleRejected), MTK_AWDL_AT(State, nativeHoldoverTransitions),
    MTK_AWDL_AT(State, nativeClockAcquisitions), MTK_AWDL_AT(State, clockParent), MTK_AWDL_AT(State, clockRoot),
    MTK_AWDL_AT(State, acquisitionParent), MTK_AWDL_AT(State, acquisitionRoot),
    MTK_AWDL_AT(State, acquisitionRootKnown), MTK_AWDL_AT(State, controlsDeferred),
    MTK_AWDL_AT(State, pendingControls),
    MTK_AWDL_TYPE(TxRate), MTK_AWDL_AT(TxRate, index), MTK_AWDL_AT(TxRate, successes),
    MTK_AWDL_AT(TxRate, failed), MTK_AWDL_AT(TxRate, probing), MTK_AWDL_AT(TxRate, backoff),
    MTK_AWDL_AT(TxRate, stale), MTK_AWDL_AT(TxRate, probeAfter),
    MTK_AWDL_TYPE(TxStatusCounters), MTK_AWDL_AT(TxStatusCounters, dataAcked),
    MTK_AWDL_AT(TxStatusCounters, dataFailed), MTK_AWDL_AT(TxStatusCounters, multicastCompleted),
    MTK_AWDL_AT(TxStatusCounters, multicastFailed), MTK_AWDL_AT(TxStatusCounters, actionCompleted),
    MTK_AWDL_AT(TxStatusCounters, actionFailed), MTK_AWDL_AT(TxStatusCounters, lastStatus),
    MTK_AWDL_AT(TxStatusCounters, lastSequence), MTK_AWDL_AT(TxStatusCounters, lastWcid),
    MTK_AWDL_AT(TxStatusCounters, lastPid), MTK_AWDL_AT(TxStatusCounters, rates),
    MTK_AWDL_AT(TxStatusCounters, ofdmOnly),
    MTK_AWDL_TYPE(Reservation), MTK_AWDL_AT(Reservation, deadline), MTK_AWDL_AT(Reservation, grantObservedAt),
    MTK_AWDL_AT(Reservation, requestedAt), MTK_AWDL_AT(Reservation, token), MTK_AWDL_AT(Reservation, channel),
    MTK_AWDL_AT(Reservation, pending), MTK_AWDL_AT(Reservation, granted),
    MTK_AWDL_AT(Reservation, responseDelayUS),
    MTK_AWDL_TYPE(RocGrant), MTK_AWDL_AT(RocGrant, bss), MTK_AWDL_AT(RocGrant, token),
    MTK_AWDL_AT(RocGrant, status), MTK_AWDL_AT(RocGrant, channel), MTK_AWDL_AT(RocGrant, requestType),
    MTK_AWDL_AT(RocGrant, durationMs),
    MTK_AWDL_TYPE(DataView), MTK_AWDL_AT(DataView, destination), MTK_AWDL_AT(DataView, source),
    MTK_AWDL_AT(DataView, payload), MTK_AWDL_AT(DataView, length), MTK_AWDL_AT(DataView, sequence),
    MTK_AWDL_TYPE(Window), MTK_AWDL_AT(Window, start), MTK_AWDL_AT(Window, end), MTK_AWDL_AT(Window, counter),
    MTK_AWDL_AT(Window, channel), MTK_AWDL_AT(Window, presence),
    MTK_AWDL_TYPE(RocLeadTiming), MTK_AWDL_AT(RocLeadTiming, samples), MTK_AWDL_AT(RocLeadTiming, leadUs),
    MTK_AWDL_AT(RocLeadTiming, count), MTK_AWDL_AT(RocLeadTiming, next), MTK_AWDL_AT(RocLeadTiming, updates),
    MTK_AWDL_AT(RocLeadTiming, decreases),
    MTK_AWDL_TYPE(RocChannelTiming), MTK_AWDL_AT(RocChannelTiming, entries), MTK_AWDL_AT(RocChannelTiming, next),
    MTK_AWDL_AT(RocChannelTiming, observations),
    MTK_AWDL_TYPE(RadioPlan), MTK_AWDL_AT(RadioPlan, window), MTK_AWDL_AT(RadioPlan, requestAt),
    MTK_AWDL_AT(RadioPlan, durationMs),
    MTK_AWDL_TYPE(Timeline), MTK_AWDL_AT(Timeline, active), MTK_AWDL_AT(Timeline, open),
    MTK_AWDL_AT(Timeline, emitted), MTK_AWDL_AT(Timeline, radioEnd), MTK_AWDL_AT(Timeline, role),
    MTK_AWDL_AT(Timeline, token), MTK_AWDL_AT(Timeline, grantObservedAt), MTK_AWDL_AT(Timeline, lastStart),
    MTK_AWDL_AT(Timeline, lastWindowStart), MTK_AWDL_AT(Timeline, starts), MTK_AWDL_AT(Timeline, ends),
    MTK_AWDL_AT(Timeline, late), MTK_AWDL_AT(Timeline, shortGrants), MTK_AWDL_AT(Timeline, lastCounter),
    MTK_AWDL_AT(Timeline, sync), MTK_AWDL_AT(Timeline, root), MTK_AWDL_AT(Timeline, lastSync),
    MTK_AWDL_AT(Timeline, lastRoot), MTK_AWDL_AT(Timeline, lastPresence), MTK_AWDL_AT(Timeline, schedule),
    MTK_AWDL_AT(Timeline, scheduleRecorded),
    MTK_AWDL_TYPE(AdvertiserRecord), MTK_AWDL_AT(AdvertiserRecord, awCounter),
    MTK_AWDL_AT(AdvertiserRecord, downCounter), MTK_AWDL_AT(AdvertiserRecord, syncContention),
    MTK_AWDL_AT(AdvertiserRecord, afPeriod), MTK_AWDL_AT(AdvertiserRecord, raw8),
    MTK_AWDL_AT(AdvertiserRecord, raw10), MTK_AWDL_AT(AdvertiserRecord, awPeriod),
    MTK_AWDL_AT(AdvertiserRecord, rssi), MTK_AWDL_AT(AdvertiserRecord, reserved15),
    MTK_AWDL_AT(AdvertiserRecord, presence), MTK_AWDL_AT(AdvertiserRecord, reserved17),
    MTK_AWDL_AT(AdvertiserRecord, raw18), MTK_AWDL_AT(AdvertiserRecord, reserved19),
    MTK_AWDL_AT(AdvertiserRecord, fwAfRxTime), MTK_AWDL_AT(AdvertiserRecord, peerSelfMetric),
    MTK_AWDL_AT(AdvertiserRecord, rootMetric), MTK_AWDL_AT(AdvertiserRecord, rootMaster),
    MTK_AWDL_AT(AdvertiserRecord, immediatePeer),
};
#undef MTK_AWDL_TYPE
#undef MTK_AWDL_AT

// Index of the first mismatch, or -1 when C++ and Rust agree.
inline long layoutMismatch() {
    const size_t n = sizeof(kLayout) / sizeof(kLayout[0]);
    if (mtk_awdl_layout_len() != n) return long(n);
    for (size_t i = 0; i < n; ++i)
        if (mtk_awdl_layout(i) != kLayout[i]) return long(i);
    return -1;
}
inline bool layoutMatches() { return layoutMismatch() < 0; }

} // namespace mtkawdl
#endif
