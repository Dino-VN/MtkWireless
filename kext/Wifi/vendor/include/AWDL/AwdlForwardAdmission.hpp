// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef MTK_AWDL_FORWARD_ADMISSION_HPP
#define MTK_AWDL_FORWARD_ADMISSION_HPP
#include <AWDL/AwdlTimeline.hpp>

// Reuses layout-checked State/RadioPlan/RocChannelTiming. eligible has exactly
// slots bytes, one flag per schedule entry; no new shared C++/Rust struct.
extern "C" bool mtk_awdl_plan_forward(const mtkawdl::State *,uint64_t,uint64_t,uint8_t,
    const mtkawdl::RocChannelTiming *,const uint8_t *,uint8_t,mtkawdl::RadioPlan *);

namespace mtkawdl {
// The controller gate owns these lifecycle keys. Timing records can be absent
// or stale, so the caller obtains the key from the current HAL/infra state.
struct ForwardRadioContext {
    uint64_t session=0,joinAttempt=0;
    uint8_t infra=0,only=0,infraState=0,bssid[6]={};
    bool operator==(const ForwardRadioContext &x) const {
        return session==x.session && joinAttempt==x.joinAttempt && infra==x.infra &&
            only==x.only && infraState==x.infraState && !memcmp(bssid,x.bssid,6);
    }
};

struct ForwardSchedule {
    Sequence sequence{},ownSequence{};
    uint64_t epoch=0,controlsDeferred=0;
    uint16_t baseCounter=0;
    uint8_t presence=0,role=0,parent[6]={},root[6]={},nativeParent[6]={},mac[6]={};
    bool parentSelected=false,nativeClockSelected=false,clockValidated=false;
    void capture(const State &s) {
        sequence=s.sequence; ownSequence=s.ownSequence; epoch=s.epoch;
        baseCounter=s.baseCounter; controlsDeferred=s.controlsDeferred;
        presence=s.presence; role=s.syncRole;
        memcpy(parent,s.sync,6); memcpy(root,s.master,6);
        memcpy(nativeParent,s.nativeParent,6); memcpy(mac,s.mac,6);
        parentSelected=s.nativeParentSelected; nativeClockSelected=s.nativeClockSelected;
        clockValidated=s.nativeClockValidated;
    }
    bool matches(const State &s) const {
        return epoch==s.epoch && baseCounter==s.baseCounter && presence==s.presence &&
            role==s.syncRole && controlsDeferred==s.controlsDeferred && !s.controlsDirty &&
            !s.nativeClockAcquisitionPending && s.radioScheduleReady() &&
            !memcmp(&sequence,&s.sequence,sizeof(sequence)) &&
            !memcmp(&ownSequence,&s.ownSequence,sizeof(ownSequence)) &&
            !memcmp(parent,s.sync,6) && !memcmp(root,s.master,6) && !memcmp(mac,s.mac,6) &&
            !memcmp(nativeParent,s.nativeParent,6) && parentSelected==s.nativeParentSelected &&
            nativeClockSelected==s.nativeClockSelected && clockValidated==s.nativeClockValidated;
    }
};

// C++ owns progress and call/intent lifetimes; Rust performs pure target
// selection. Healthy channels still use the existing greedy scheduler.
struct ForwardAdmission {
    struct Progress {
        uint64_t since=0,nativeEnd=0,attemptAt=0,intermediateRetryAt=0;
        uint8_t channel=0;
    } progress[slots];
    uint64_t serial=0,intentId=0,callId=0,callSession=0;
    uint64_t selectedAt=0,issueBy=0;
    uint64_t selected=0,expired=0,invalidated=0,issued=0,preempted=0,responseTimeouts=0,settlingTicks=0;
    uint8_t cursor=0;
    bool active=false;
    RadioPlan plan{};
    ForwardRadioContext context{};
    ForwardSchedule schedule{};

    uint64_t nextSerial() { if(!++serial) ++serial; return serial; }
    void invalidate() {
        if(active) { ++invalidated; active=false; nextSerial(); }
    }
    void expire() {
        if(active) { ++expired; active=false; nextSerial(); }
    }
    void reset() {
        invalidate(); callId=callSession=0; nextSerial(); cursor=0;
        for(auto &p:progress) p={};
    }
    uint64_t prepareAt() const { return rocPrepareAt(plan.requestAt); }
    bool validTarget(const State &s) const {
        Window w{};
        return currentWindow(s,plan.window.start,w) && w.start==plan.window.start &&
            w.end==plan.window.end && w.counter==plan.window.counter &&
            w.channel==plan.window.channel && w.presence==plan.window.presence;
    }
    bool matches(const State &s,const ForwardRadioContext &c) const {
        return c.session && context==c && schedule.matches(s) && validTarget(s);
    }
    bool valid(const State &s,const ForwardRadioContext &c,uint64_t now) const {
        return active && now<=issueBy && matches(s,c);
    }
    void nativeWindow(uint8_t channel,uint64_t end) {
        for(auto &p:progress) if(p.channel==channel && end>p.nativeEnd) p.nativeEnd=end;
        // An intervening greedy window can resolve the deficit without using
        // the recovery intent. Never keep withholding airtime after progress.
        if(active && plan.window.channel==channel) invalidate();
    }
    void observeReservation(const State &s,const Reservation &g,uint64_t now) {
        // Only the caller's current radio-context request may be observed here.
        // A timed-out intermediate response can otherwise consume every later
        // fixed intent, despite fair selections. For one cycle after its actual
        // response deadline, retry that channel only on its own recovery turn,
        // not as speculative intermediate work. Healthy greedy work is unchanged.
        if(!validSequence(s.sequence) || !validPresence(s.presence)) return;
        const uint64_t cycle=uint64_t(s.sequence.count)*s.presence*awUs;
        for(auto &p:progress) if(p.channel && p.channel==g.channel) {
            if(g.granted) p.intermediateRetryAt=0;
            else if(g.pending && g.deadline && now>=g.deadline && now-g.deadline<cycle) {
                const uint64_t until=g.deadline>UINT64_MAX-cycle ? UINT64_MAX : g.deadline+cycle;
                if(until>p.intermediateRetryAt) { p.intermediateRetryAt=until; ++responseTimeouts; }
            }
        }
    }
    static bool ownsOriginal(const Window &reserved,uint8_t requested,const Reservation &g) {
        return requested && requested==g.channel && reserved.channel==requested && reserved.end>reserved.start;
    }
    static uint64_t protectedUntil(const Timeline &native,const Reservation &g,uint64_t now,
                                   const Window &reserved,uint8_t requested) {
        uint64_t end=native.open ? native.active.end : 0;
        if((g.pending || g.granted) && now<g.deadline) {
            // Unknown external ownership drains conservatively. Known original
            // future/short reservations remain protected through their end.
            uint64_t owned=ownsOriginal(reserved,requested,g) ? reserved.end : g.deadline;
            if(owned>end) end=owned;
        }
        return end;
    }
    void update(const State &s,uint64_t now,const ForwardRadioContext &c,
                const RocChannelTiming &timing,const uint8_t (&reachable)[slots],
                const Timeline &native,const Reservation &grant,const Window &reserved,uint8_t requested,
                const RocChannelTiming *legacy=nullptr) {
        if(callId) return; // Only the owning call completes, or reset invalidates.
        bool allowed=false;
        for(unsigned i=0;i<s.sequence.count && i<slots;++i)
            if(reachable[i] && s.sequence.channels[i]==plan.window.channel) allowed=true;
        if(active && (!matches(s,c) || !allowed)) {
            invalidate(); return; // No replacement with this tick's stale coordinates.
        }
        if(active && now>issueBy) {
            expire(); return;
        }
        uint64_t release=protectedUntil(native,grant,now,reserved,requested);
        if(active) {
            if(release>prepareAt()) invalidate();
            return; // Fixed edge, never slide it on a later observation.
        }
        if(!c.session || !validSequence(s.sequence) || !validPresence(s.presence) ||
           s.sequence.step+1!=s.presence || !s.radioScheduleReady()) return;
        // Reconcile a bounded per-distinct-channel history, retaining progress
        // for unchanged channels across phase corrections. Newly reachable
        // channels get one whole cycle for the greedy scheduler to serve them.
        for(auto &p:progress) {
            bool present=false;
            for(unsigned i=0;i<s.sequence.count;++i)
                if(reachable[i] && p.channel==s.sequence.channels[i]) present=true;
            if(!present) p={};
        }
        for(unsigned i=0;i<s.sequence.count;++i) {
            uint8_t ch=s.sequence.channels[i];
            if(!ch || !reachable[i]) continue;
            bool found=false;
            for(const auto &p:progress) if(p.channel==ch) found=true;
            if(!found) for(auto &p:progress) if(!p.channel) { p.channel=ch; p.since=now; break; }
        }
        if(native.open) nativeWindow(native.active.channel,native.radioEnd);
        // A real response wins over a speculative intent. Intermediate requests
        // made after selection may remain pending only while their original
        // window fits before the edge (the active branch above checks that).
        if(grant.pending && now<grant.deadline) return;
        const uint64_t cycle=uint64_t(s.sequence.count)*s.presence*awUs;
        uint8_t eligible[slots]={};
        bool settling=false;
        for(unsigned i=0;i<s.sequence.count;++i) {
            if(!reachable[i]) continue;
            for(const auto &p:progress) if(p.channel && p.channel==s.sequence.channels[i]) {
                uint64_t last=p.nativeEnd ? p.nativeEnd : p.since;
                // Count selections too, so repeated invalidation or missing
                // responses cannot give a failing channel every recovery turn.
                eligible[i]=now>=last && now-last>=cycle &&
                    (!p.attemptAt || (now>=p.attemptAt && now-p.attemptAt>=cycle));
                // A newly faster response can leave an old maximum in the
                // legacy eight-sample ring. Forcing another recovery each cycle
                // would refresh it before eight-other-observation aging, while
                // that oversized lead displaces healthy channels. Let legacy
                // aging finish only when Rust confirms a material reduction in
                // BOTH current-context and legacy history. Eight absent cycles
                // cap the grace even with fewer than one other grant per cycle;
                // no observation or polling slides that progress-based bound.
                if(eligible[i] && legacy && now-last<8*cycle && timing.recoveryGrace(p.channel,*legacy)) {
                    eligible[i]=0; settling=true;
                }
            }
        }
        if(settling) ++settlingTicks; // Planning ticks, not attempts or elapsed time.
        RadioPlan choice{};
        if(!mtk_awdl_plan_forward(&s,now,release,cursor,&timing,eligible,c.infra,&choice)) return;
        plan=choice; cursor=choice.window.channel; active=true; intentId=nextSerial(); ++selected;
        selectedAt=now; issueBy=plan.requestAt+8000; context=c; schedule.capture(s);
        for(auto &p:progress) if(p.channel==cursor) p.attemptAt=now;
        // No Reservation, requestedWindow, clock hold, native event, or HAL write.
    }
    bool blocks(const Window &w,bool nativeOpen,uint64_t now) const {
        return active && !nativeOpen && now<plan.window.start && w.end>prepareAt();
    }
    bool allowsIntermediate(const RadioPlan &p) const {
        // Coalesced surplus can be preempted later, but not the first promised
        // window. Do not issue it and then discover that its target pins RF.
        if(!active) return true;
        if(p.window.end>prepareAt()) return false;
        for(const auto &history:progress)
            if(history.channel==p.window.channel && p.requestAt<history.intermediateRetryAt) return false;
        return true;
    }
    bool canPreempt(const Timeline &native,const Reservation &g,uint64_t now,
                    const Window &reserved,uint8_t requested) const {
        return active && now>=prepareAt() && !native.open && !g.pending &&
            g.granted && now<g.deadline && ownsOriginal(reserved,requested,g) && now>=reserved.end;
    }
    uint64_t beginCall(uint64_t session) {
        if(callId) return 0;
        callId=nextSerial(); callSession=session; return callId;
    }
    bool completeCall(uint64_t id) {
        if(!id || callId!=id) return false;
        callId=callSession=0; return true;
    }
    void issue() { active=false; ++issued; }
};
}
#endif
