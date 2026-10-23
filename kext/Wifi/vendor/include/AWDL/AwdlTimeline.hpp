// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef MTK_AWDL_TIMELINE_HPP
#define MTK_AWDL_TIMELINE_HPP
#include <AWDL/AwdlProtocol.hpp>
namespace mtkawdl {
// Host observations use monotonic microseconds; AW counters are peer schedule
// coordinates and must never be used as a timestamp clock.
struct Window {
    uint64_t start, end;
    uint16_t counter;
    uint8_t channel, presence;
};
inline bool currentWindow(const State &s,uint64_t now,Window &out) {
    if(!validPresence(s.presence) || !validSequence(s.sequence)) return false;
    uint32_t length=unsigned(s.presence)*awUs, remaining=s.untilNext(now);
    if(!remaining || remaining>length || now<length-remaining) return false;
    out={now-(length-remaining),now+remaining,
         uint16_t(s.counter(now)-s.counter(now)%s.presence),s.channel(now),s.presence};
    return true;
}
// AWDL multicast rendezvous uses EAW slots 0 and 10 in the 16-slot cycle.
// Preserve the packet through other windows; sending an mDNS query on an
// arbitrary discovery channel does not reach the peers listening for it.
inline bool multicastReady(const State &s,uint64_t now) {
    Window w={};
    if(!currentWindow(s,now,w) || !w.channel) return false;
    unsigned slot=(s.counter(now)/s.presence)%slots;
    if(slot!=0 && slot!=10) return false;
    uint64_t guard=16*1024;
    if(guard>(w.end-w.start)/4) guard=(w.end-w.start)/4;
    return now-w.start>=guard && w.end-now>=guard;
}
// One MCU queue admission can wait 30*100ms, then ROC has a one-second
// response budget. Add one complete local/known-destination channel cycle
// and the scheduler's 8ms edge margin. Snapshot this budget on enqueue:
// changing schedules and resource retries must not extend a packet's life.
inline uint32_t dataQueueLifetimeUs(const State &s,const uint8_t *destination) {
    uint32_t cycle=slots*4*awUs;
    const Sequence *sequences[2]={&s.sequence,&s.ownSequence};
    for(const Sequence *sequence : sequences) {
        if(validSequence(*sequence)) {
            uint32_t length=unsigned(sequence->count)*(unsigned(sequence->step)+1)*awUs;
            if(length>cycle) cycle=length;
        }
    }
    int peer=s.find(destination);
    if(peer>=0 && validSequence(s.peers[peer].advertisement.sequence)) {
        const Sequence &sequence=s.peers[peer].advertisement.sequence;
        uint32_t length=unsigned(sequence.count)*(unsigned(sequence.step)+1)*awUs;
        if(length>cycle) cycle=length;
    }
    // Valid sequences cap this at 20,785,216us even without any radio grant.
    return 4*rocResponseTimeoutUs+cycle+8000;
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
        uint64_t need=uint64_t(responseUs)+preparationUs+8000;
        if(need>rocResponseTimeoutUs) need=rocResponseTimeoutUs;
        if(need<rocLeadUs) need=rocLeadUs;
        samples[next]=static_cast<uint32_t>(need); next=(next+1)%8;
        if(count<8) ++count;
        uint32_t value=rocLeadUs;
        for(unsigned i=0;i<count;++i) if(samples[i]>value) value=samples[i];
        if(value<leadUs) ++decreases;
        leadUs=value; ++updates; return value;
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
        for(const auto &entry:entries)
            if(entry.channel==channel && entry.timing.count && observations-entry.observed<8)
                return entry.timing.leadUs;
        return fallback;
    }
    void observe(uint8_t channel,uint32_t responseUs,uint64_t preparationUs) {
        if(!channel || channel>165) return;
        Entry *found=nullptr;
        for(auto &entry:entries) if(entry.channel==channel) { found=&entry; break; }
        if(!found) {
            found=&entries[next]; next=(next+1)%slots;
            *found=Entry{}; found->channel=channel;
        }
        if(observations-found->observed>=8) found->timing=RocLeadTiming{};
        found->observed=++observations;
        found->timing.observe(responseUs,preparationUs);
    }
};
// Prepare the BSS and ROC before the edge, and keep one grant for adjacent
// equal channels. A grant alone never changes the announced schedule.
struct RadioPlan { Window window; uint64_t requestAt; uint32_t durationMs; };
inline bool planRadio(const State &s,uint64_t now,RadioPlan &out,uint32_t leadUs=rocLeadUs,
                      bool skipCurrent=false,const RocChannelTiming *timing=nullptr) {
    Window target={};
    if(!currentWindow(s,now,target)) return false;
    if(skipCurrent || !target.channel || now-target.start>8000) {
        for(unsigned i=0;i<s.sequence.count;++i) {
            if(!currentWindow(s,target.end,target)) return false;
            if(target.channel) break;
        }
    }
    if(!target.channel) return false;
    // A measured response longer than an EAW cannot serve the nearest dense
    // slot. Choose a reachable future edge in the same real clock instead of
    // repeatedly receiving grants after their first target has already ended.
    // Initial/default lead retains the fast-response startup behavior.
    // The measured lead includes an 8ms preparation margin. A timer arriving
    // within that margin must retain its already reachable sparse target.
    // Examine each candidate with its own channel's latency. Jumping to
    // now+the first channel's lead also skips faster intervening channels.
    unsigned budget=rocResponseTimeoutUs/(unsigned(s.presence)*awUs)+s.sequence.count+2;
    uint32_t selectedLead=leadUs;
    for(;;) {
        // A global slow estimate can skip a cold channel forever, preventing
        // the observation needed to correct that estimate. Use the bounded
        // startup lead for cold/stale channels; a probe is not a radio grant.
        selectedLead=timing ? timing->lead(target.channel) : leadUs;
        if(target.channel && !(selectedLead>rocLeadUs &&
           selectedLead>unsigned(s.presence)*awUs+8000 &&
           target.start+8000<now+selectedLead)) break;
        if(!budget-- || !currentWindow(s,target.end,target)) return false;
    }
    uint64_t at=target.start>selectedLead ? target.start-selectedLead : 0;
    if(at<now) at=now;
    uint64_t end=target.end, cursor=end;
    for(unsigned i=1;i<s.sequence.count;++i) {
        Window next={};
        if(!currentWindow(s,cursor,next) || (next.channel && next.channel!=target.channel)) break;
        // An idle slot promises no AW, but need not tear down a radio grant
        // between same-channel windows. Never extend into trailing idle time.
        cursor=next.end;
        if(next.channel==target.channel) end=next.end;
        if(cursor-at>=uint64_t(maxRocMs)*1000) break;
    }
    uint64_t duration=(end-at+4000+999)/1000;
    if(duration>maxRocMs) duration=maxRocMs;
    out={target,at,static_cast<uint32_t>(duration)}; return true;
}
inline bool grantCoversWindow(const Window &w,const Reservation &grant,uint64_t now) {
    uint64_t at=grant.grantObservedAt>w.start ? grant.grantObservedAt : w.start;
    uint64_t check=now>w.start ? now : w.start;
    uint64_t end=grant.deadline<w.end ? grant.deadline : w.end;
    return w.channel && grant.grantObservedAt && grant.grantObservedAt<=check &&
        grant.canTransmit(w.channel,check) && end>at &&
        end-at+10*1024>=unsigned(w.presence)*awUs;
}
// Keep the original future reservation when an early grant arrives. Replanning
// with now + lead would otherwise move the target beyond this valid grant.
// Revalidate the actual schedule: an expired/short grant or a changed window
// must not pin RF ownership or produce an event for stale coordinates.
inline bool grantCoversReservedWindow(const State &s,const Window &reserved,
                                     uint8_t requestedChannel,const Reservation &grant,
                                     uint64_t now) {
    if(!requestedChannel || requestedChannel!=reserved.channel || now>=reserved.start ||
       !grant.grantObservedAt || grant.grantObservedAt>now ||
       !grantCoversWindow(reserved,grant,reserved.start)) return false;
    Window actual={};
    return currentWindow(s,reserved.start,actual) && actual.start==reserved.start &&
        actual.end==reserved.end && actual.channel==reserved.channel &&
        actual.counter==reserved.counter && actual.presence==reserved.presence;
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
    bool matchesSchedule(const State &s) const {
        return active.presence==s.presence && role==s.syncRole &&
            !memcmp(sync,s.sync,6) && !memcmp(root,s.master,6) &&
            (!scheduleRecorded || !memcmp(&schedule,&s.sequence,sizeof(schedule)));
    }
    // Report an actual grant near the EAW edge. A firmware grant may be
    // shorter than the requested dwell: track its real end instead of hiding
    // the whole AW or promising radio availability until the scheduled end.
    bool begin(const Window &w,const Reservation &grant,uint64_t now,
               const uint8_t *parent,const uint8_t *master,uint8_t nativeRole=1,
               const Sequence *radioSchedule=nullptr) {
        if(open || !w.channel || now<w.start || now>=w.end || now-w.start>8000 ||
           !grant.canTransmit(w.channel,now) || !grant.grantObservedAt ||
           grant.grantObservedAt>now) { ++late; return false; }
        // The radio is available from the accepted grant, independently of
        // timer delivery. A prepared grant covers the scheduled edge; a late
        // grant starts only at its actual observation, never before RF exists.
        uint64_t start=grant.grantObservedAt>w.start ? grant.grantObservedAt : w.start;
        uint64_t end=grant.deadline<w.end ? grant.deadline : w.end;
        // Native sanityCheckAWEnd allows +/-10 TU, not an arbitrary partial
        // dwell. Never announce a 32ms grant as a complete 64TU native AW.
        if(end-start+10*1024<unsigned(w.presence)*awUs) { ++shortGrants; return false; }
        // Early preparation closes within native's duration tolerance. A
        // control/clock change must not announce the same physical EAW again.
        if(emitted && now<active.end) return false;
        // RX clock corrections can move an edge by a fraction of a TU.
        // Identify the last notified window by counter/presence/master too;
        // its timestamp alone must not permit a second start for that AW.
        uint64_t edgeDelta=w.start>=lastWindowStart ? w.start-lastWindowStart : lastWindowStart-w.start;
        if(emitted && lastCounter==w.counter && lastPresence==w.presence &&
           !memcmp(lastSync,parent,6) && !memcmp(lastRoot,master,6) &&
           edgeDelta<unsigned(w.presence)*awUs) return false;
        active=w; open=true; emitted=true; lastWindowStart=w.start;
        radioEnd=end;
        role=nativeRole; token=grant.token; grantObservedAt=grant.grantObservedAt;
        scheduleRecorded=radioSchedule!=nullptr;
        if(radioSchedule) schedule=*radioSchedule;
        lastStart=start; lastCounter=w.counter; lastPresence=w.presence; ++starts;
        memcpy(sync,parent,6); memcpy(root,master,6);
        memcpy(lastSync,parent,6); memcpy(lastRoot,master,6); return true;
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
