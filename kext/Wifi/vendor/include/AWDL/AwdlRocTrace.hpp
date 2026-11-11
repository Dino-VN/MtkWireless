// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef MTK_AWDL_ROC_TRACE_HPP
#define MTK_AWDL_ROC_TRACE_HPP
#include <AWDL/AwdlTimeline.hpp>
namespace mtkawdl {
// MtkWireless: bounded, controller-gated host observations, not RF retune or
// AP-ACK measurements. Owned by the heap-allocated interface, never the stack.
// Native timestamps describe the FIRST window on a grant; starts/ends count
// all its windows. Dispatch times are fresh host observations before callbacks.
struct RocTrace {
    struct Sample {
        uint64_t id, session, serviceAtUS, planAtUS, requestAtUS, idealAtUS;
        uint64_t callAtUS, returnAtUS, issuedAtUS, queueReturnedAtUS, grantAtUS, pmAtUS, readyDelayUS;
        uint64_t targetStartUS, targetEndUS;
        uint64_t firstStartUS, firstStartDispatchUS, firstEndUS, firstEndDispatchUS;
        uint32_t epoch, leadUS, durationMS, status, queueStatus, invalidReason, starts, ends;
        uint16_t counter;
        uint8_t token, channel, infraChannel, flags, reserved[2];
    };
    static_assert(sizeof(Sample)==192,"ROC host trace v1");
    enum : uint8_t { Returned=1, Issued=2, LifecycleChanged=4 };
    static constexpr unsigned capacity=32;
    Sample samples[capacity]={};
    uint64_t count=0;

    Sample *find(uint64_t id) {
        if(!id) return nullptr;
        auto &s=samples[(id-1)%capacity];
        return s.id==id ? &s : nullptr;
    }
    uint64_t begin(uint64_t session,uint64_t serviceAt,uint64_t planAt,uint64_t callAt,
                   const RadioPlan &plan,uint32_t lead,uint8_t infra) {
        if(++count==0) ++count;
        auto &s=samples[(count-1)%capacity]; s={};
        s.id=count; s.session=session; s.serviceAtUS=serviceAt; s.planAtUS=planAt;
        s.requestAtUS=plan.requestAt; s.idealAtUS=plan.window.start>lead ? plan.window.start-lead : 0;
        s.callAtUS=callAt; s.targetStartUS=plan.window.start; s.targetEndUS=plan.window.end;
        s.counter=plan.window.counter; s.channel=plan.window.channel; s.infraChannel=infra;
        s.leadUS=lead; s.durationMS=plan.durationMs; s.status=0xffffffffu;
        return s.id;
    }
    template<class Timing> void finish(uint64_t id,uint64_t at,uint32_t status,const Timing &t,bool current) {
        auto *s=find(id); if(!s) return; // overwritten during a gate-releasing call
        s->returnAtUS=at; s->status=status; s->flags|=Returned;
        if(!current) s->flags|=LifecycleChanged;
        // The HAL returns THIS call's owned observation, including failures.
        // A refused call has no issue identity and cannot adopt an older grant.
        if(!t.issuedAtUS || t.session!=s->session || t.channel!=s->channel ||
           t.issuedAtUS<s->callAtUS || t.issuedAtUS>at) return;
        s->flags|=Issued; s->issuedAtUS=t.issuedAtUS; s->epoch=t.epoch; s->token=t.token;
        s->infraChannel=t.infraChannel; s->pmAtUS=t.pmAtUS;
        s->queueReturnedAtUS=t.queueReturnedAtUS; s->queueStatus=static_cast<uint32_t>(t.queueStatus);
        observe(t);
    }
    template<class Timing> void observe(const Timing &t) {
        if(!t.issuedAtUS || !t.grantAtUS || t.grantAtUS<t.issuedAtUS) return;
        for(auto &s:samples) {
            if(!(s.flags&Issued) || s.session!=t.session || s.epoch!=t.epoch || s.token!=t.token ||
               s.channel!=t.channel || s.issuedAtUS!=t.issuedAtUS) continue;
            s.grantAtUS=t.grantAtUS;
            // Queue-helper return and response time may overlap. This does not
            // feed the lead estimator: retain its existing conservative policy.
            s.readyDelayUS=rocReadyDelayUS(s.callAtUS,s.returnAtUS,s.grantAtUS);
            break;
        }
    }
    Sample *window(uint64_t session,const Timeline &t) {
        for(auto &s:samples)
            if((s.flags&Issued) && s.session==session && s.token==t.token &&
               s.channel==t.active.channel && s.grantAtUS && s.grantAtUS==t.grantObservedAt) return &s;
        return nullptr;
    }
    void start(uint64_t session,const Timeline &t,uint64_t dispatchAt) {
        auto *s=window(session,t); if(!s) return;
        if(!s->starts) { s->firstStartUS=t.lastStart; s->firstStartDispatchUS=dispatchAt; }
        ++s->starts;
    }
    void end(uint64_t session,const Timeline &t,uint64_t effectiveAt,uint64_t dispatchAt) {
        auto *s=window(session,t); if(!s || s->ends>=s->starts) return;
        if(!s->ends) { s->firstEndUS=effectiveAt; s->firstEndDispatchUS=dispatchAt; }
        ++s->ends;
    }
};
}
#endif
