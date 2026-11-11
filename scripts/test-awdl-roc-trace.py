#!/usr/bin/env python3
"""ASan/UBSan tests of the real C++ ROC trace and freshly built wifi Rust FFI.

The first executable includes AwdlRocTrace.hpp, extracts the exact HAL timing
ABI, and calls the real scalar timing FFI. The second reuses the local lifecycle
companion's verbatim production extraction for service, native notifications,
sync and diagnostic publication. Its HAL reserve, native callbacks, gate and
registry are host boundaries, not firmware/IOKit implementations. Observations
are absent by default and injected explicitly, never invented from a grant.

Each invocation builds wifi offline with the installed pinned nightly in a new
host target and checks source/archive provenance. Only C++ is sanitizer-built.
HAL reserve source checks complement, but do not replace, executable HAL tests.
No upstream tests, production sources, hardware or shared build targets change.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import importlib.util
import json
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
COMPANION = ROOT / "scripts/test-awdl-native-lifecycle.py"
spec = importlib.util.spec_from_file_location("awdl_lifecycle_companion", COMPANION)
lifecycle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lifecycle)


DIRECT = r'''
#include <AWDL/AwdlRocTrace.hpp>
#include <AWDL/AwdlLayout.hpp>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
struct MtkMwx {
@TIMING_ABI@
};
using Timing=MtkMwx::AwdlRocTiming;
using Trace=mtkawdl::RocTrace;
static_assert(sizeof(Timing)==56 && offsetof(Timing,epoch)==40 && offsetof(Timing,token)==48,
              "HAL timing ABI: extract, do not replace with a test struct");
static_assert(sizeof(Trace::Sample)==192 && offsetof(Trace::Sample,flags)==189,"trace v1 bytes");
static mtkawdl::RadioPlan plan(uint64_t start=200000,uint8_t channel=6) {
    mtkawdl::RadioPlan p{};
    p.window={start,start+65536,12,channel,4}; p.requestAt=150000; p.durationMs=120;
    return p;
}
static Timing timing(uint64_t session=9,uint64_t issued=100100) {
    Timing t{}; t.session=session; t.issuedAtUS=issued; t.queueReturnedAtUS=120000;
    t.pmAtUS=99000; t.epoch=17; t.token=23; t.channel=6; t.infraChannel=44;
    return t;
}
static std::vector<uint8_t> bytes(const Trace &trace) {
    auto p=reinterpret_cast<const uint8_t *>(&trace); return {p,p+sizeof(trace)};
}
static void noIssue(const Trace::Sample &s) {
    assert(!(s.flags&Trace::Issued) && !s.issuedAtUS && !s.queueReturnedAtUS && !s.grantAtUS);
    assert(!s.readyDelayUS && !s.pmAtUS && !s.epoch && !s.token && !s.queueStatus);
}
static void scalarTiming() {
    struct Case { uint64_t call,returned,grant,delay; };
    const Case cases[]={
        {100,150,120,50}, {100,120,150,50}, {100,150,150,50},
        {100,100,100,0}, {100,90,80,0}, {100,90,130,30},
        {0,0,0,0}, {0,UINT64_MAX,0,UINT64_MAX},
        {UINT64_MAX-10,UINT64_MAX-4,UINT64_MAX,10},
        {UINT64_MAX,0,UINT64_MAX,0}, {1,UINT64_MAX,UINT64_MAX-1,UINT64_MAX-1},
    };
    for(const auto &c:cases) {
        assert(mtk_awdl_roc_ready_delay_us(c.call,c.returned,c.grant)==c.delay);
        assert(mtkawdl::rocReadyDelayUS(c.call,c.returned,c.grant)==c.delay);
    }
    // Exhaust all small orderings, including clock-regression inputs. The
    // readiness edge is a union of waits, not their overlapping sum.
    for(uint64_t call=0;call<20;++call) for(uint64_t returned=0;returned<20;++returned)
        for(uint64_t grant=0;grant<20;++grant) {
            uint64_t expected=0;
            for(uint64_t tick=call;tick<20;++tick) if(tick<returned||tick<grant) ++expected;
            assert(mtk_awdl_roc_ready_delay_us(call,returned,grant)==expected);
        }
    puts("PASS scalar FFI: early/late/overlapping waits, saturation, UINT64 boundaries, 8000 orderings");
}
static void beginAndReturnIdentity() {
    auto trace=std::make_unique<Trace>(); auto p=plan();
    uint64_t id=trace->begin(9,99900,99950,100000,p,55000,44);
    auto *s=trace->find(id);
    assert(id==1 && trace->count==1 && s && !trace->find(0) && !trace->find(id+Trace::capacity));
    assert(s->session==9 && s->serviceAtUS==99900 && s->planAtUS==99950 && s->callAtUS==100000);
    assert(s->requestAtUS==150000 && s->idealAtUS==145000 && s->leadUS==55000);
    assert(s->targetStartUS==p.window.start && s->targetEndUS==p.window.end && s->counter==12);
    assert(s->channel==6 && s->infraChannel==44 && s->durationMS==120 && s->status==0xffffffffu);
    assert(!s->flags && !s->returnAtUS && !s->starts && !s->ends); noIssue(*s);
    auto t=timing(); auto before=bytes(*trace);
    trace->finish(0,130000,0,t,true); trace->finish(id+Trace::capacity,130000,0,t,true);
    assert(bytes(*trace)==before);
    auto earlyPlan=plan(1000); uint64_t early=trace->begin(9,0,0,1,earlyPlan,55000,0);
    assert(!trace->find(early)->idealAtUS);
    // Each invalid return starts from a separate record. Only the id selects
    // that record; timing must also belong to its session/channel/call interval.
    for(unsigned bad=0;bad<5;++bad) {
        auto own=timing(); uint64_t current=trace->begin(9,99900,99950,100000,p,55000,44);
        if(bad==0)++own.session; if(bad==1)++own.channel; if(bad==2)own.issuedAtUS=0;
        if(bad==3)own.issuedAtUS=99999; if(bad==4)own.issuedAtUS=130001;
        trace->finish(current,130000,0xe00002d8u,own,true); auto *r=trace->find(current);
        assert(r->flags==Trace::Returned && r->returnAtUS==130000 && r->status==0xe00002d8u); noIssue(*r);
    }
    puts("PASS return identity: exact id, session, channel, issued interval; untouched absent fields and plan edges");
}
static void matchedObservations() {
    auto trace=std::make_unique<Trace>(); auto p=plan(); auto t=timing();
    uint64_t id=trace->begin(t.session,99000,99500,100000,p,55000,44);
    trace->finish(id,130000,0,t,true);
    for(unsigned bad=0;bad<8;++bad) {
        auto other=t; other.grantAtUS=140000;
        if(bad==0)++other.session; if(bad==1)++other.epoch; if(bad==2)++other.token;
        if(bad==3)++other.channel; if(bad==4)++other.issuedAtUS;
        if(bad==5)other.grantAtUS=other.issuedAtUS-1;
        if(bad==6)other.issuedAtUS=0; if(bad==7)other.grantAtUS=0;
        auto before=bytes(*trace); trace->observe(other); assert(bytes(*trace)==before);
    }
    t.grantAtUS=140000; trace->observe(t);
    const auto *s=trace->find(id);
    assert(s->grantAtUS==140000 && s->readyDelayUS==40000 && s->epoch==17 && s->token==23);
    auto before=bytes(*trace); trace->observe(t); assert(bytes(*trace)==before);
    // Token/epoch/channel reuse after re-enable cannot cross sessions.
    auto next=t; ++next.session; next.grantAtUS=150000;
    uint64_t second=trace->begin(next.session,99000,99500,100000,p,55000,44);
    auto partial=next; partial.grantAtUS=0; trace->finish(second,135000,0,partial,true);
    trace->observe(next);
    assert(trace->find(id)->grantAtUS==140000 && trace->find(second)->grantAtUS==150000);
    puts("PASS observation identity: exact session/epoch/token/channel/issue; malformed and reused-token observations cannot attach");
}
static void earlyGrantAndPartialFailures() {
    auto trace=std::make_unique<Trace>(); auto p=plan(); auto t=timing();
    t.grantAtUS=110000; t.queueReturnedAtUS=150000;
    uint64_t id=trace->begin(t.session,99000,99500,100000,p,55000,44);
    trace->observe(t); assert(!trace->find(id)->grantAtUS); // no issue identity yet
    trace->finish(id,170000,0,t,true);
    const auto *s=trace->find(id);
    assert(s->flags==(Trace::Returned|Trace::Issued) && s->issuedAtUS==t.issuedAtUS);
    assert(s->queueReturnedAtUS==150000 && s->grantAtUS==110000 && s->returnAtUS==170000);
    assert(s->readyDelayUS==70000); // not 70000 + (110000 - 100100)
    assert(s->pmAtUS==99000 && !s->queueStatus);
    // Missing output and an older still-published observation both leave a
    // refusal without an issued identity; matching the channel alone is not enough.
    for(unsigned stale=0;stale<2;++stale) {
        uint64_t refused=trace->begin(t.session,199000,199500,200000,p,55000,44);
        trace->finish(refused,201000,0xe00002d8u,stale?t:Timing{},true);
        trace->observe(t); noIssue(*trace->find(refused));
        assert(trace->find(refused)->flags==Trace::Returned);
    }
    auto failed=timing(9,210100); failed.queueReturnedAtUS=211000; failed.queueStatus=-5;
    uint64_t partial=trace->begin(9,209000,209500,210000,p,55000,44);
    trace->finish(partial,212000,0xe00002cau,failed,false);
    s=trace->find(partial);
    assert(s->flags==(Trace::Returned|Trace::Issued|Trace::LifecycleChanged));
    assert(s->session==9 && s->status==0xe00002cau && s->queueStatus==uint32_t(-5));
    assert(s->issuedAtUS==210100 && s->queueReturnedAtUS==211000 && !s->grantAtUS && !s->readyDelayUS);
    puts("PASS partial records: early grant survives return, overlap counted once, refused calls reject old timing, canceled failures keep owned fields");
}
static void overwrittenSlots() {
    auto trace=std::make_unique<Trace>(); auto p=plan(); auto old=timing();
    uint64_t id=trace->begin(9,99000,99500,100000,p,55000,44);
    for(unsigned n=0;n<Trace::capacity;++n) trace->begin(10+n,200000+n,200000+n,200000+n,p,55000,44);
    assert(!trace->find(id) && trace->count==Trace::capacity+1);
    auto before=bytes(*trace);
    trace->finish(id,300000,0,old,false); old.grantAtUS=310000; trace->observe(old);
    assert(bytes(*trace)==before && !trace->find(trace->count)->returnAtUS);
    uint64_t last=trace->count; auto own=timing(10+Trace::capacity-1,200000+Trace::capacity);
    own.queueReturnedAtUS=250000; own.grantAtUS=260000;
    trace->finish(last,270000,0,own,true);
    assert(trace->find(last)->flags==(Trace::Returned|Trace::Issued) && trace->find(last)->grantAtUS==260000);
    puts("PASS bounded ring: overwritten ids cannot receive stale returns or observations; live replacement still completes");
}
static void nativeWindowIdentity() {
    auto trace=std::make_unique<Trace>(); auto p=plan(); auto t=timing(); t.grantAtUS=140000;
    uint64_t id=trace->begin(9,99000,99500,100000,p,55000,44); trace->finish(id,130000,0,t,true);
    mtkawdl::Timeline window{}; window.open=true; window.active=p.window;
    window.token=t.token; window.grantObservedAt=t.grantAtUS; window.lastStart=200000;
    trace->end(9,window,260000,260100); assert(!trace->find(id)->ends);
    for(unsigned bad=0;bad<4;++bad) {
        auto wrong=window; uint64_t session=9;
        if(bad==0)++session; if(bad==1)++wrong.token; if(bad==2)++wrong.active.channel;
        if(bad==3)++wrong.grantObservedAt;
        trace->start(session,wrong,200100); assert(!trace->find(id)->starts);
    }
    trace->start(9,window,200100); trace->end(9,window,260000,260700); trace->end(9,window,260000,260800);
    window.lastStart=265536; window.active.start=265536; ++window.active.counter;
    trace->start(9,window,266000); trace->end(9,window,331072,332000);
    const auto *s=trace->find(id);
    assert(s->starts==2 && s->ends==2 && s->firstStartUS==200000 && s->firstStartDispatchUS==200100);
    assert(s->firstEndUS==260000 && s->firstEndDispatchUS==260700);
    puts("PASS window identity: session/token/channel/grant, first effective and dispatch timestamps, balanced multi-window counts");
}
int main() {
    assert(mtkawdl::layoutMatches()); scalarTiming(); beginAndReturnIdentity(); matchedObservations();
    earlyGrantAndPartialFailures(); overwrittenSlots(); nativeWindowIdentity();
    puts("PASS real ROC trace header + current scalar Rust FFI (C++ ASan/UBSan)");
}
'''


NATIVE = r'''
#include "extracted.cpp"
#line 1 "roc-trace-native-assertions"
using Timing=MtkMwx::AwdlRocTiming;
using RocTrace=mtkawdl::RocTrace;
static const uint8_t localMac[6]={2,1,2,3,4,5};
static void traceSetup(NativeFixture &d) {
    d.state.init(localMac,clockUS.load());
    memset(d.state.ownSequence.channels,6,16); d.state.sequence=d.state.ownSequence;
    d.radio.grant.cancel(); d.awTimeline.reset(); d.events.clear();
    assert(d.enabled && d.txSession && !d.rocTrace.count);
}
static Timing explicitTiming(NativeFixture &d,uint64_t call) {
    Timing t{}; t.session=d.txSession; t.epoch=d.radio.storage.sc_awdl_epoch;
    t.token=7; t.channel=d.radio.requested; t.infraChannel=0;
    t.issuedAtUS=call+100; t.queueReturnedAtUS=call+600; return t;
}
static std::vector<uint8_t> sampleBytes(const RocTrace::Sample &s) {
    auto p=reinterpret_cast<const uint8_t *>(&s); return {p,p+sizeof(s)};
}
static std::vector<uint8_t> traceBytes(const RocTrace &t) {
    auto p=reinterpret_cast<const uint8_t *>(t.samples); return {p,p+sizeof(t.samples)};
}
static void cleared(const Timing &t) {
    assert(!t.session&&!t.issuedAtUS&&!t.queueReturnedAtUS&&!t.grantAtUS&&!t.pmAtUS);
    assert(!t.epoch&&!t.queueStatus&&!t.token&&!t.channel&&!t.infraChannel);
}
static void snapshotBoundaries() {
    resetFaults(); {
        NativeFixture d; Timing out{}; out.issuedAtUS=77;
        assert(!d.radio.awdlRocTimingSnapshot(out)); cleared(out);
        d.radio.requested=6; auto valid=explicitTiming(d,clockUS.load()); valid.grantAtUS=valid.issuedAtUS+100;
        auto &com=d.radio.storage; com.sc_awdl_roc_timing=valid;
        assert(d.radio.awdlRocTimingSnapshot(out) && out.session==valid.session && out.grantAtUS==valid.grantAtUS);
        for(unsigned kind=0;kind<8;++kind) {
            auto saved=d.radio.com; auto lock=com.sc_awdl_lock;
            if(kind==0)com.sc_awdl_enabled=false; if(kind==1)com.sc_resetting=true;
            if(kind==2)com.sc_detaching=true; if(kind==3)com.sc_awdl_stopping=true;
            if(kind==4)++com.sc_awdl_roc_timing.session; if(kind==5)++com.sc_awdl_roc_timing.epoch;
            if(kind==6)d.radio.com=nullptr; if(kind==7)com.sc_awdl_lock=nullptr;
            out=valid; assert(!d.radio.awdlRocTimingSnapshot(out)); cleared(out);
            d.radio.com=saved; com.sc_awdl_lock=lock; com.sc_awdl_enabled=true;
            com.sc_resetting=com.sc_detaching=com.sc_awdl_stopping=false; com.sc_awdl_roc_timing=valid;
        }
        com.sc_awdl_roc_timing={}; assert(!d.radio.awdlRocTimingSnapshot(out)); cleared(out);
    } noResources();
    puts("PASS extracted HAL snapshot: copied matching observation; missing, disabled, reset, detached, stale session/epoch and missing locks clear output");
}
static void publishWhileOff(NativeFixture &d) {
    assert(!d.enabled && !d.txSession && !d.timer->pending && d.rocTrace.count);
    const auto all=traceBytes(d.rocTrace), last=sampleBytes(*d.rocTrace.find(d.rocTrace.count));
    const auto count=d.rocTrace.count;
    // Delete old registry copies first, so success proves these exact publisher
    // methods run again after sync stopped the timer, not merely retained data.
    for(const char *key:{"MTKAWDLROCTrace","MTKAWDLROCTraceLast","MTKAWDLROCTraceCount","MTKAWDLROCTraceVersion"})d.removeProperty(key);
    const auto publishes=d.radio.publishes;
    gate.enter([&]{d.publishCacheDiagnostics();return 0;});
    assert(d.properties["MTKAWDLROCTraceVersion"]==1 && d.properties["MTKAWDLROCTraceCount"]==count);
    assert(d.byteProperties["MTKAWDLROCTrace"]==all && d.byteProperties["MTKAWDLROCTraceLast"]==last);
    assert(d.radio.publishes==publishes+1 && traceBytes(d.rocTrace)==all);
    auto hal=d.hal; d.hal=nullptr; d.removeProperty("MTKAWDLROCTrace");
    gate.enter([&]{d.publishCacheDiagnostics();return 0;});
    assert(d.byteProperties["MTKAWDLROCTrace"]==all && d.radio.publishes==publishes+1); d.hal=hal;
}
static void ownedReturnScenarios() {
    const char *names[]={"absent-success","refused-old-observation","queue-failure","disable-in-call",
                         "disable-reenable-in-call","overwritten-in-call","schedule-invalidated"};
    for(unsigned kind=0;kind<7;++kind) {
        resetFaults(); {
            NativeFixture d; traceSetup(d); const uint64_t call=clockUS.load(), session=d.txSession;
            Timing owned{}; std::vector<uint8_t> replacement;
            if(kind==1) {
                auto old=explicitTiming(d,call-10000); old.channel=6; old.grantAtUS=call-5000;
                d.radio.storage.sc_awdl_roc_timing=old; d.radio.reserveResult=kIOReturnNotReady;
            }
            if(kind>=2) {
                if(kind==2)d.radio.reserveResult=kIOReturnError;
                if(kind==3||kind==4)d.radio.reserveResult=kIOReturnNotReady;
                d.radio.onReserveTiming=[&](Timing &out){
                    owned=explicitTiming(d,call); if(kind==2)owned.queueStatus=EIO;
                    out=owned; d.radio.storage.sc_awdl_roc_timing=owned; clockUS=call+900;
                };
            }
            d.radio.onReserve=[&]{
                assert(d.rocTrace.count==1);
                auto *pending=d.rocTrace.find(1);
                assert(pending && pending->session==session && pending->callAtUS==call && !pending->flags);
                if(kind==3||kind==4) {
                    assert(d.setSync(false)==0 && !d.enabled && !d.txSession);
                    assert(d.rocTrace.count==1 && !d.rocTrace.find(1)->returnAtUS);
                    if(kind==4) {
                        assert(d.setSync(true)==0 && d.enabled && d.txSession!=session);
                        auto newer=explicitTiming(d,call+1000); newer.grantAtUS=call+1500;
                        d.radio.storage.sc_awdl_roc_timing=newer; clockUS=call+2000;
                        d.timer->setTimeoutMS(17); // replacement session owns this deadline
                    }
                }
                if(kind==5) {
                    auto p=mtkawdl::RadioPlan{}; p.window=d.requestedWindow; p.requestAt=call; p.durationMs=100;
                    for(unsigned i=0;i<RocTrace::capacity;++i) {
                        clockUS=call+1000+i; d.rocTrace.begin(session+1,clockUS,clockUS,clockUS,p,55000,0);
                    }
                    replacement=sampleBytes(*d.rocTrace.find(d.rocTrace.count));
                }
                if(kind==6)d.state.sequence.channels[0]=44;
            };
            d.serviceInGate(); d.radio.onReserve=nullptr; d.radio.onReserveTiming=nullptr;
            assert(d.radio.reserves==1);
            if(kind==3)assert(!d.timer->pending);
            if(kind==4)assert(d.timer->pending && d.timer->us==17000);
            if(kind==5) {
                assert(!d.rocTrace.find(1) && d.rocTrace.count==RocTrace::capacity+1);
                assert(sampleBytes(*d.rocTrace.find(d.rocTrace.count))==replacement);
            } else {
                auto *s=d.rocTrace.find(1); assert(s && s->session==session && s->callAtUS==call);
                assert(s->flags&RocTrace::Returned); assert(s->returnAtUS==clockUS.load());
                assert(s->status==d.radio.reserveResult && s->serviceAtUS<=s->planAtUS && s->planAtUS<=s->callAtUS);
                assert(bool(s->flags&RocTrace::LifecycleChanged)==(kind==3||kind==4));
                if(kind<2) {
                    assert(!(s->flags&RocTrace::Issued) && !s->issuedAtUS && !s->queueReturnedAtUS && !s->grantAtUS);
                    assert(!s->readyDelayUS);
                    Timing older{};
                    if(d.radio.awdlRocTimingSnapshot(older))d.rocTrace.observe(older);
                    assert(!s->issuedAtUS && !s->grantAtUS);
                } else {
                    assert(s->flags&RocTrace::Issued);
                    assert(s->issuedAtUS==owned.issuedAtUS && s->queueReturnedAtUS==owned.queueReturnedAtUS);
                    assert(s->epoch==owned.epoch && s->token==owned.token && s->channel==owned.channel);
                    assert(s->queueStatus==uint32_t(owned.queueStatus) && !s->grantAtUS && !s->readyDelayUS);
                    if(kind==4) {
                        Timing newer{}; assert(d.radio.awdlRocTimingSnapshot(newer) && newer.session==d.txSession);
                        d.rocTrace.observe(newer); assert(!s->grantAtUS && s->session==session);
                    }
                }
                if(kind==6)assert(s->invalidReason==d.reserveInvalidReason && s->invalidReason && d.reserveInvalidations==1);
            }
            if(d.enabled)assert(d.setSync(false)==0);
            publishWhileOff(d);
            const auto old=traceBytes(d.rocTrace); const auto count=d.rocTrace.count;
            assert(d.setSync(true)==0 && d.enabled && d.txSession!=session);
            assert(d.rocTrace.count==count && traceBytes(d.rocTrace)==old);
            assert(d.setSync(false)==0); publishWhileOff(d);
        } noResources();
        printf("PASS extracted service: %s; partial trace and watchdog publication survive sync off/on/off\n",names[kind]);
    }
}
static void halResetDuringReserve() {
    resetFaults(); {
        NativeFixture d; traceSetup(d); const uint64_t call=clockUS.load(), session=d.txSession;
        const uint8_t peer[6]={2,1,2,3,4,6};
        gate.enter([&]{
            uint8_t eth[54]{}; eth[0]=eth[1]=0x33; eth[5]=1;
            eth[12]=0x86; eth[13]=0xdd; eth[14]=0x60;
            assert(d.output(d.makeFrame(eth,sizeof(eth)))==kIOReturnOutputSuccess);
            assert(d.cacheDataPeer(peer,true,clockUS.load())); return 0;
        });
        assert(d.queueCount==1 && d.timer->pending);
        const auto oldStatus=d.reserveStatus;
        const auto oldWait=d.reserveWaitUS;
        Timing owned{}; d.radio.reserveResult=kIOReturnNotReady;
        d.radio.onReserveTiming=[&](Timing &out){
            owned=explicitTiming(d,call); out=owned;
            d.radio.storage.sc_awdl_roc_timing=owned; clockUS=call+900;
        };
        d.radio.onReserve=[&]{
            assert(!d.timer->pending); // the firing one-shot was consumed
            // Explicit transport boundary: reset invalidates HAL identity only.
            // Full quiesce/MCU commandSleep behavior is outside this fixture.
            auto &com=d.radio.storage;
            IOLockLock(com.sc_awdl_lock);
            ++com.sc_awdl_epoch; com.sc_awdl_native_session=0;
            com.sc_awdl_enabled=false; com.sc_resetting=true;
            d.radio.on=false; d.radio.grant.cancel();
            IOLockUnlock(com.sc_awdl_lock);
            assert(d.enabled && d.txSession==session);
        };
        d.serviceInGate(); d.radio.onReserve=nullptr; d.radio.onReserveTiming=nullptr;
        assert(d.enabled && d.txSession==session && !d.radio.awdlTxSession());
        assert(d.timer->pending && d.timer->us==1000);
        assert(d.reserveStatus==oldStatus && d.reserveWaitUS==oldWait);
        assert(d.queueCount==1 && d.state.clockHoldUntil && d.dataPeers.find(peer,clockUS.load())>=0);
        const auto *s=d.rocTrace.find(1);
        assert(s && s->flags==(RocTrace::Returned|RocTrace::Issued|RocTrace::LifecycleChanged));
        assert(s->session==session && s->issuedAtUS==owned.issuedAtUS && s->returnAtUS==call+900);
        assert(s->status==kIOReturnNotReady && !s->grantAtUS && !s->starts && !s->ends);
        const auto partial=sampleBytes(*s); const auto ticks=d.serviceTicks;
        // Watchdog publication is not a substitute for the rearmed service tick.
        gate.enter([&]{d.publishCacheDiagnostics(); return 0;});
        assert(d.enabled && d.queueCount==1 && d.serviceTicks==ticks && d.timer->pending);
        d.radio.storage.sc_resetting=false;
        clockUS=clockUS.load()+d.timer->us;
        d.serviceInGate(); // no external TX/control wake
        assert(!d.enabled && !d.timer->pending && !d.queueCount && !d.state.clockHoldUntil);
        // Existing transport-reset cleanup leaves the old frontend identity
        // until a native sync transition; disabled plus HAL session 0 gates it.
        assert(d.txSession==session && !d.radio.awdlTxSession());
        assert(d.dataPeers.find(peer,clockUS.load())<0 && d.serviceTicks==ticks+1 && d.radio.reserves==1);
        assert(sampleBytes(*d.rocTrace.find(1))==partial);
        gate.enter([&]{d.publishCacheDiagnostics(); return 0;});
        assert(d.byteProperties["MTKAWDLROCTraceLast"]==partial);
    } noResources();
    puts("PASS HAL-only reset: consumed one-shot rearms cleanup, stale state is not adopted, queue/peers/clock clear without external wake");
}
static uint64_t earlyGrant(NativeFixture &d) {
    const auto call=clockUS.load();
    d.radio.onReserveTiming=[&](Timing &out){
        out=explicitTiming(d,call); out.grantAtUS=call+200;
        d.radio.grant.request(out.token,out.channel,out.issuedAtUS);
        assert(d.radio.grant.grant(out.token,out.channel,300,out.grantAtUS));
        d.radio.storage.sc_awdl_roc_timing=out; clockUS=call+900;
    };
    d.serviceInGate(); d.radio.onReserveTiming=nullptr;
    const auto *s=d.rocTrace.find(1);
    assert(s && s->issuedAtUS==call+100 && s->queueReturnedAtUS==call+600);
    assert(s->grantAtUS==call+200 && s->returnAtUS==call+900 && s->readyDelayUS==900);
    assert(!s->starts && !s->ends && !s->invalidReason); return call;
}
static void nativeCallbacksAndWindows() {
    resetFaults(); {
        NativeFixture d; traceSetup(d); const auto call=earlyGrant(d), session=d.txSession;
        unsigned starts=0,ends=0; uint64_t firstDispatch=0,firstEndDispatch=0,firstEnd=0;
        d.onStart=[&]{
            const auto *s=d.rocTrace.find(1); ++starts;
            assert(s && s->starts==starts && s->ends==ends && d.awTimeline.open);
            if(starts==1) {
                firstDispatch=clockUS; assert(s->firstStartUS==call+200 && s->firstStartDispatchUS==firstDispatch);
                assert(mtkawdl::be32(d.events.back().bytes.data()+20)==call+200);
                clockUS=firstDispatch+37; d.serviceInGate();
                assert(d.rocTrace.find(1)->starts==1 && d.rocTrace.find(1)->firstStartDispatchUS==firstDispatch);
            } else {
                assert(starts==2 && s->firstStartUS==call+200 && s->firstStartDispatchUS==firstDispatch);
            }
        };
        d.onEnd=[&]{
            const auto *s=d.rocTrace.find(1); ++ends;
            assert(s && s->ends==ends && s->starts==starts && !d.awTimeline.open && d.state.controlsHeld);
            if(ends==1) {
                firstEndDispatch=clockUS; assert(s->firstEndUS==firstEnd && s->firstEndDispatchUS==firstEndDispatch);
                assert(mtkawdl::be32(d.events.back().bytes.data()+4)==firstEnd);
            } else assert(s->firstEndUS==firstEnd && s->firstEndDispatchUS==firstEndDispatch);
            // Reentry sees a committed end, but held controls forbid a new
            // window until the old native callback has returned.
            const auto count=s->starts; clockUS=clockUS.load()+37; d.serviceInGate();
            assert(d.rocTrace.find(1)->starts==count && !d.awTimeline.open);
        };
        clockUS=call+1000; d.serviceInGate();
        assert(starts==1 && !ends && d.awTimeline.open);
        firstEnd=d.awTimeline.radioEnd; clockUS=firstEnd+500; d.serviceInGate();
        assert(starts==2 && ends==1 && d.awTimeline.open && d.rocTrace.count==1);
        const auto secondEnd=d.awTimeline.radioEnd; clockUS=secondEnd+777;
        gate.enter([&]{d.notifyAvailabilityWindowEnd(secondEnd+10000);return 0;}); // clamp to real end
        assert(starts==2 && ends==2 && !d.awTimeline.open);
        const auto before=sampleBytes(*d.rocTrace.find(1));
        gate.enter([&]{d.notifyAvailabilityWindowEnd(clockUS.load());return 0;});
        assert(sampleBytes(*d.rocTrace.find(1))==before && d.events.size()==4);
        const auto *s=d.rocTrace.find(1);
        assert(s->session==session && s->firstStartUS<firstDispatch && s->firstEndUS<firstEndDispatch);
        assert(s->starts==2 && s->ends==2 && s->firstStartDispatchUS==firstDispatch && s->firstEndDispatchUS==firstEndDispatch);
        d.onStart=d.onEnd=nullptr; assert(d.setSync(false)==0); publishWhileOff(d);
    } noResources();
    puts("PASS native events: first effective vs dispatch times, early grant, callback reentry, old-end ordering, two windows per trace, end clamp");
}
static void freshDispatchAndSynchronousDisable() {
    resetFaults(); {
        NativeFixture d; traceSetup(d); const auto call=earlyGrant(d), session=d.txSession;
        mtkawdl::Window window{}; assert(mtkawdl::currentWindow(d.state,call+900,window));
        assert(d.awTimeline.begin(window,d.radio.grant,call+900,d.state.sync,d.state.master,d.state.syncRole,&d.state.sequence));
        d.state.holdControls();
        const uint64_t dispatch=call+1200;
        bool called=false;
        d.onStart=[&]{
            called=true; auto *s=d.rocTrace.find(1);
            assert(s->session==session && s->starts==1 && !s->ends && s->firstStartDispatchUS==dispatch);
            assert(s->firstStartUS==call+200 && s->firstStartDispatchUS!=call+900);
            clockUS=dispatch+50; d.radio.grant.cancel(); // revocation makes disable close now, not in the future
            assert(d.setSync(false)==0 && !d.enabled && !d.txSession);
            assert(d.rocTrace.find(1)->ends==1);
        };
        clockUS=dispatch;
        gate.enter([&]{d.notifyAvailabilityWindowStart(6,call+900);return 0;});
        d.onStart=nullptr; assert(called && !d.awTimeline.open && d.events.size()==2);
        const auto *s=d.rocTrace.find(1);
        assert(s->firstStartDispatchUS==dispatch && s->firstEndUS==dispatch+50 && s->firstEndDispatchUS==dispatch+50);
        assert(s->starts==1 && s->ends==1 && s->session==session); publishWhileOff(d);
    } noResources();
    puts("PASS native fresh dispatch: stale service argument is not dispatch time; synchronous start callback disables and records old-session end exactly once");
}
int main() {
    assert(mwx_awdl_native_history_init()); snapshotBoundaries(); ownedReturnScenarios();
    halResetDuringReserve(); nativeCallbacksAndWindows(); freshDispatchAndSynchronousDisable();
    noResources(); assert(!gateDepth && !heldHalLocks);
    puts("PASS extracted native ROC trace integration (explicit host boundaries, not RF/native runtime validation)");
}
'''


def source_contract(ex) -> dict:
    reserve = ex.function(lifecycle.HAL, "MtkMwx::awdlReserve")
    cleared_at = reserve.index("if(timing) *timing={};")
    assert cleared_at < reserve.index("MwxAwdlCall call(com)") < reserve.index("return kIOReturnNotReady")
    assert reserve.index("AwdlRocTiming ownTiming=") < reserve.index("mwx_mcu_send_msg(")
    assert reserve.index("if(timing) *timing=ownTiming;") < reserve.index("if(!current) return kIOReturnAborted")
    header = ex.text(lifecycle.HAL_HEADER)
    assert "AwdlRocTiming *timing=nullptr" in header
    return {"checks": ["optional third timing argument", "output cleared before early refusal",
                       "call-owned timing copied before send", "owned partial result returned before canceled-call exit"],
            "limitation": "These are source-order checks only. This fixture does not execute production awdlReserve or MCU transport."}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--diagnostics", type=Path, help="new empty directory below build/diagnostics")
    args = parser.parse_args()
    parent = ROOT / "build/diagnostics"
    parent.mkdir(parents=True, exist_ok=True)
    if args.diagnostics:
        out = args.diagnostics.resolve()
        if not out.is_relative_to(parent.resolve()):
            parser.error("diagnostics must be below build/diagnostics")
        out.mkdir(parents=True, exist_ok=False)
    else:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        out = Path(tempfile.mkdtemp(prefix="awdl-roc-trace-"+stamp+"-", dir=parent))
    print("AWDL ROC trace evidence: "+str(out), flush=True)
    env = lifecycle.host_environment()
    run = lifecycle.CommandLog(out, env)
    before = lifecycle.protected_hashes()
    before[str(COMPANION.relative_to(ROOT))] = lifecycle.digest(COMPANION.read_bytes())
    before[str(Path(__file__).resolve().relative_to(ROOT))] = lifecycle.digest(Path(__file__).read_bytes())
    (out / "source-before.json").write_text(json.dumps(before, indent=2)+"\n")
    result = {"started_at": lifecycle.utc(), "diagnostics": str(out), "compile": {}, "tests": {}}
    archive, provenance = None, None
    try:
        provenance = lifecycle.build_fresh_archive(out, run)
        archive = Path(provenance["archive"])
        (out / "archive-provenance.json").write_text(json.dumps(provenance, indent=2)+"\n")
        ex = lifecycle.Extractor()
        result["source_contract"] = source_contract(ex)
        (out / "trace.cpp").write_text(DIRECT.replace("@TIMING_ABI@", ex.structure(lifecycle.HAL_HEADER, "AwdlRocTiming")))
        extracted = lifecycle.generate(ex)
        (out / "extracted.cpp").write_text(extracted)
        (out / "native-trace.cpp").write_text(NATIVE)
        (out / "extraction.json").write_text(json.dumps(ex.inventory, indent=2)+"\n")
        flags = ["/usr/bin/clang++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-parameter", "-Wno-unused-function", "-fno-omit-frame-pointer",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-pthread",
                 "-I"+str(lifecycle.VENDOR / "include")]
        for name in ["trace", "native-trace"]:
            binary = out / name
            result["compile"][name] = run("compile-"+name, flags+[str(out / (name+".cpp")), str(archive), "-o", str(binary)])
            if result["compile"][name]:
                print(run.output("compile-"+name), end="", flush=True)
                continue
            result["tests"][name] = run("run-"+name, [str(binary)])
            print(run.output("run-"+name), end="", flush=True)
        # Drop only the HAL-reset wake, preserving stale-session rejection and
        # trace ownership. The consumed-one-shot regression must then fail.
        rearm = "                if(!stopping && enabled && txSession==serviceSession)\n                    timer->setTimeoutMS(1);"
        assert extracted.count(rearm) == 1, "reset wake negative-control anchor changed"
        (out / "extracted-no-reset-rearm.cpp").write_text(extracted.replace(rearm, "                // Negative control: lost reset cleanup wake.", 1))
        (out / "no-reset-rearm.cpp").write_text(NATIVE.replace('#include "extracted.cpp"', '#include "extracted-no-reset-rearm.cpp"', 1))
        negative = {"compile": run("compile-no-reset-rearm", flags+[str(out / "no-reset-rearm.cpp"), str(archive), "-o", str(out / "no-reset-rearm")]),
                    "expected_failure_detected": False}
        if not negative["compile"]:
            negative["exit_code"] = run("run-no-reset-rearm", [str(out / "no-reset-rearm")])
            output = run.output("run-no-reset-rearm")
            negative["expected_failure_detected"] = bool(negative["exit_code"] and
                "Assertion failed" in output and "halResetDuringReserve" in output and
                "d.timer->pending && d.timer->us==1000" in output)
        result["negative_controls"] = {"hal-reset-no-rearm": negative}
        print("HAL-reset lost-wake negative control: "+("detected" if negative["expected_failure_detected"] else "FAILED"), flush=True)
    except Exception as error:
        result["error"] = repr(error)
        print(repr(error), flush=True)
    finally:
        after = lifecycle.protected_hashes()
        after[str(COMPANION.relative_to(ROOT))] = lifecycle.digest(COMPANION.read_bytes())
        after[str(Path(__file__).resolve().relative_to(ROOT))] = lifecycle.digest(Path(__file__).read_bytes())
        changes = {k: {"before": before.get(k), "after": after.get(k)}
                   for k in before.keys() | after.keys() if before.get(k) != after.get(k)}
        result.update(ended_at=lifecycle.utc(), source_changes=changes,
                      archive_unchanged=bool(archive and archive.is_file() and lifecycle.digest(archive.read_bytes())==provenance["sha256"]),
                      limitations=["Rust host archive is real but not sanitizer-instrumented",
                                   "No executable production HAL reserve/MCU transport, firmware or hardware coverage here",
                                   "Gate release, native callback delivery, registry copying and OS ownership remain explicit host models",
                                   "No TSan, kernel execution or claim of native runtime reentrancy correctness"])
        failed = bool(result.get("error") or changes or not result["archive_unchanged"]
                      or any(result["compile"].values()) or any(result["tests"].values())
                      or set(result["tests"]) != {"trace", "native-trace"}
                      or not result.get("negative_controls", {}).get("hal-reset-no-rearm", {}).get("expected_failure_detected"))
        result["result"] = "FAIL" if failed else "PASS"
        (out / "source-after.json").write_text(json.dumps(after, indent=2)+"\n")
        (out / "results.json").write_text(json.dumps(result, indent=2)+"\n")
    print("AWDL ROC trace "+result["result"]+"; evidence: "+str(out), flush=True)
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
