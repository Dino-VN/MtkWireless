// SPDX-License-Identifier: GPL-2.0-or-later
// Host boundaries are in test-awdl-forward-admission.py; all scheduler bodies
// below come from the production extraction, linked to real offline Rust FFI.
#include "extracted.cpp"
#include <AWDL/AwdlLayout.hpp>
#include <memory>

static constexpr uint64_t B=2775808610ULL,W=65536,CYCLE=16*W;
static constexpr uint16_t BASE=36172;
static const uint8_t MAC[6]={2,1,2,3,4,5};
// Timing-only inputs from the retained 1.0.17 capture. No device/peer identity.
static const uint8_t CAPTURE[16]={149,149,149,149,149,149,9,9,6,149,149,149,149,149,9,9};
struct Issue { uint64_t at,start,end,grantAt,selectedAt; uint32_t lead,dwell; uint8_t channel,token; bool recovery; };
struct Start {
    uint64_t at,end; uint8_t channel,token;
    bool operator==(const Start &x) const { return at==x.at&&end==x.end&&channel==x.channel&&token==x.token; }
};
struct Result {
    std::map<unsigned,unsigned> native,attempts;
    unsigned intermediate=0; uint64_t selected=0;
    std::vector<Start> windows;
};

struct Simulation {
    static std::unique_ptr<NativeFixture> make(bool forward) {
        resetFaults(); bootArguments["amtk_awdl_forward"]=forward?1:0;
        return std::make_unique<NativeFixture>();
    }
    std::unique_ptr<NativeFixture> object;
    NativeFixture &d;
    std::vector<Issue> issues;
    std::vector<Start> starts;
    std::map<unsigned,unsigned> native,attempts;
    std::map<unsigned,uint32_t> latencies;
    std::set<unsigned> unanswered;
    uint64_t nextTick=B,due=0,issuedCount=0;
    uint8_t token=58,lastGranted=149,expected[16]{};
    unsigned ends=0,tx=0,intermediate=0,scenario=0,recoveryPmRefusals=0;
    bool callRecovery=false;
    explicit Simulation(bool forward,bool warm=true,unsigned kind=0):object(make(forward)),d(*object),scenario(kind) {
        assert(d.forwardEnabled==forward); // Exercises the production boot-arg parser.
        clockUS=B; d.state.init(MAC,B); d.awTimeline.reset(); d.resetForwardAdmission(); d.events.clear();
        if(kind==0) {
            memcpy(expected,CAPTURE,16); d.state.baseCounter=BASE; d.radio.infraChannel=9;
            latencies={{6,145136},{9,33796},{149,133880}};
        } else {
            const uint8_t alternate[16]={36,36,1,44,11,36,1,0,44,11,36,0,1,44,11,36};
            memcpy(expected,alternate,16); d.radio.infraChannel=36;
            latencies={{1,60000},{11,212000},{36,12000},{44,178000}};
            if(kind==2) for(auto &v:latencies) v.second=5000;
            if(kind==3) { memset(expected,36,16); latencies={{36,5000}}; }
        }
        memcpy(d.state.sequence.channels,expected,16); d.state.ownSequence=d.state.sequence;
        d.radio.storage.sc_awdl_epoch=7;
        d.forwardContext=d.forwardRadioContext(); d.forwardContextSet=true;
        d.forwardTimingContext=d.forwardContext; d.forwardTimingContextSet=true;
        if(kind==0) {
            d.reserveChannelTiming.observe(149,152124,0); d.reserveChannelTiming.observe(9,33794,3);
            d.forwardTiming.observeRetained(149,152124,0); d.forwardTiming.observeRetained(9,33794,3);
            if(warm) { d.reserveChannelTiming.observe(6,145133,4); d.forwardTiming.observeRetained(6,145133,4); }
            d.radio.modelPS=true; d.radio.modelPMAt=2775645614;
            d.radio.grant.request(58,149,2775648530);
            assert(d.radio.grant.grant(58,149,300,2775782315));
            d.grantLatencyObservedAt=d.forwardGrantObservedAt=2775782315;
            d.requestedWindow={B,B+W,BASE,149,4}; d.requestedChannel=149;
        } else if(warm) for(const auto &v:latencies) {
            d.reserveChannelTiming.observe(v.first,v.second-3,4);
            d.forwardTiming.observeRetained(v.first,v.second-3,4);
        }
        d.reserveLeadUS=d.reserveTiming.observe(152124,0); d.reserveWaitUS=35;
        d.radio.modelReserveEntry=[&] {
            callRecovery=d.forward.issued>issuedCount; issuedCount=d.forward.issued;
            if(callRecovery && !d.radio.modelScan && !d.radio.modelJoin && d.radio.infraChannel &&
               d.radio.requested!=d.radio.infraChannel &&
               (!d.radio.modelPS || clockUS.load()-d.radio.modelPMAt<2000)) ++recoveryPmRefusals;
        };
        d.radio.onReserveTiming=[&](MtkMwx::AwdlRocTiming &out) {
            const uint64_t call=clockUS.load(); const uint8_t ch=d.radio.requested;
            assert(!d.awTimeline.open);
            if(d.forwardEnabled) assert(d.forward.callId&&d.forward.callSession==d.txSession);
            const bool recovery=callRecovery;
            if(d.forwardEnabled&&d.forward.active) {
                assert(d.requestedWindow.end<=d.forward.prepareAt()); ++intermediate;
            }
            if(++token==0)++token;
            uint32_t latency=latencies.at(ch);
            if(scenario==0&&ch==149&&lastGranted==6)latency=152249;
            out.session=d.txSession; out.epoch=d.radio.storage.sc_awdl_epoch;
            out.token=token; out.channel=ch; out.infraChannel=d.radio.infraChannel;
            out.issuedAtUS=call+1; out.queueReturnedAtUS=call+4;
            out.pmAtUS=ch!=d.radio.infraChannel?d.radio.modelPMAt:0;
            assert(ch==d.radio.infraChannel || (out.issuedAtUS-out.pmAtUS>=2000&&d.radio.duration<=300));
            d.radio.grant.request(token,ch,out.queueReturnedAtUS); due=out.issuedAtUS+latency;
            d.radio.storage.sc_awdl_roc_timing=out;
            const auto *trace=d.rocTrace.find(d.rocTrace.count); assert(trace);
            issues.push_back({call,d.requestedWindow.start,d.requestedWindow.end,due,d.forward.selectedAt,
                trace->leadUS,d.radio.duration,ch,token,recovery}); ++attempts[ch];
            clockUS=call+4;
        };
        d.radio.modelBeforeAbort=[&]{ assert(!d.awTimeline.open); };
        d.radio.modelTx=[&](unsigned ch) {
            assert(d.radio.grant.granted&&d.radio.grant.channel==ch&&clockUS.load()+2000<d.radio.grant.deadline);
            assert(d.state.channel(clockUS.load())==ch); ++tx;
        };
        d.onStart=[&] {
            const auto &t=d.awTimeline; const auto &g=d.radio.grant;
            assert(starts.size()==ends&&t.open&&g.granted);
            assert(t.token==g.token&&t.grantObservedAt==g.grantObservedAt&&t.active.channel==g.channel);
            assert(clockUS.load()>=t.active.start&&clockUS.load()-t.active.start<=8000);
            assert(t.lastStart>=g.grantObservedAt&&t.radioEnd<=g.deadline);
            assert(t.radioEnd-t.lastStart+10*1024>=W);
            starts.push_back({t.lastStart,t.radioEnd,t.active.channel,t.token}); ++native[t.active.channel];
            uint8_t eth[54]{}; eth[0]=2;eth[1]=9;eth[2]=8;eth[3]=7;eth[4]=6;eth[5]=5;
            eth[12]=0x86;eth[13]=0xdd;eth[14]=0x60;
            assert(d.output(d.makeFrame(eth,sizeof(eth)))==kIOReturnOutputSuccess);
        };
        d.onEnd=[&] {
            assert(starts.size()==ends+1&&!d.awTimeline.open&&d.state.controlsHeld);
            const uint32_t end=mtkawdl::be32(d.events.back().bytes.data()+4);
            const uint32_t duration=end-uint32_t(starts.back().at);
            assert(duration+10*1024>=W&&duration<=W+10*1024);
            starts.back().end=starts.back().at+duration; ++ends;
        };
    }
    ~Simulation() {
        d.onStart=d.onEnd=nullptr; d.radio.onReserveTiming=nullptr; d.radio.onReserve=nullptr;
        d.radio.modelBeforeAbort=d.radio.modelAfterAbort=d.radio.modelAfterSleep=d.radio.modelReserveEntry=nullptr; d.radio.modelTx=nullptr;
    }
    void deliver(uint64_t at) {
        if(!d.radio.grant.pending||!due||due>at)return;
        if(unanswered.count(d.radio.requested)){due=0;return;}
        clockUS=due; assert(d.radio.grant.grant(token,d.radio.requested,d.radio.duration,due));
        lastGranted=d.radio.requested; d.radio.storage.sc_awdl_roc_timing.grantAtUS=due; due=0;
    }
    void advance(uint64_t stop) {
        while(nextTick<=stop&&d.enabled&&!d.stopping) {
            deliver(nextTick); clockUS=nextTick; d.serviceInGate(); // actual tick -> service entry
            assert(!memcmp(d.state.sequence.channels,expected,16)&&d.state.presence==4);
            if(d.enabled&&!d.stopping) { assert(d.timer->pending&&d.timer->us); nextTick=clockUS.load()+d.timer->us; }
        }
        deliver(stop); clockUS=stop;
    }
    Result result(const char *name,unsigned cycles) {
        assert(starts.size()==ends+unsigned(d.awTimeline.open));
        printf("RESULT {\"name\":\"%s\",\"cycles\":%u,\"forward\":%s,\"starts\":%zu,\"ends\":%u,\"open\":%s,\"tx\":%u,\"selected\":%llu,\"issued\":%llu,\"expired\":%llu,\"invalidated\":%llu,\"preempted\":%llu,\"response_timeouts\":%llu,\"settling_ticks\":%llu,\"intermediate\":%u,\"pm_refusals\":%llu,\"recovery_pm_refusals\":%u,\"native\":{",
            name,cycles,d.forwardEnabled?"true":"false",starts.size(),ends,d.awTimeline.open?"true":"false",tx,(unsigned long long)d.forward.selected,
            (unsigned long long)d.forward.issued,(unsigned long long)d.forward.expired,(unsigned long long)d.forward.invalidated,
            (unsigned long long)d.forward.preempted,(unsigned long long)d.forward.responseTimeouts,
            (unsigned long long)d.forward.settlingTicks,intermediate,(unsigned long long)d.radio.modelPMRefused,recoveryPmRefusals);
        bool comma=false;
        for(const auto &v:latencies) { printf("%s\"%u\":%u",comma?",":"",v.first,native[v.first]); comma=true; }
        printf("},\"attempts\":{"); comma=false;
        for(const auto &v:latencies) { printf("%s\"%u\":%u",comma?",":"",v.first,attempts[v.first]); comma=true; }
        printf("},\"native_us\":{");comma=false;
        for(const auto &v:latencies) {
            uint64_t us=0;
            for(const auto &w:starts)if(w.channel==v.first)us+=std::min(w.end,clockUS.load())-w.at;
            printf("%s\"%u\":%llu",comma?",":"",v.first,(unsigned long long)us);comma=true;
        }
        puts("}}");
        return {native,attempts,intermediate,d.forward.selected,starts};
    }
};

static void closedLoops() {
    for(unsigned cycles:{6,8}) for(bool warm:{false,true}) {
        Result baseline;
        { Simulation s(false,warm); s.advance(B+cycles*CYCLE); baseline=s.result(warm?"captured-warm-baseline":"captured-cold-baseline",cycles);
          assert(!s.native[6]&&s.native[9]&&s.native[149]&&!s.d.forward.selected); }
        noResources();
#ifndef RETENTION_ONLY
        { Simulation s(true,warm); s.advance(B+cycles*CYCLE); auto got=s.result(warm?"captured-warm-selective":"captured-cold-selective",cycles);
          assert(got.native[6]>=2&&got.native[9]&&got.native[149]);
          // Compare exactly the SAME simulated interval, not 6 vs 8 cycles.
          // The rejected drain/round-robin policy delivered only 9 native-149
          // windows in 8 cycles. Require at least 85% of greedy bulk-channel
          // window coverage here, in addition to restored slow-channel visits.
          assert(got.native[149]*100>=baseline.native[149]*85);
          assert(!s.recoveryPmRefusals);
          for(const auto &i:s.issues) if(i.recovery&&i.channel==6&&i.lead>55000) {
              assert(i.start-i.at>=i.lead-8000&&i.grantAt<=i.start+8000&&i.dwell<=300);
          }
        }
        noResources();
#endif
    }
#ifdef RETENTION_ONLY
    { Simulation s(false,true,1);s.latencies[44]=12000;
      s.d.reserveChannelTiming.observe(44,990000,20000);
      for(unsigned i=0;i<100;++i)s.d.reserveChannelTiming.observe(36,12000,0);
      assert(s.d.reserveChannelTiming.lead(44)==1000000);
      s.advance(B+52*CYCLE);s.result("retention-outlier",52);
      assert(!s.attempts[44]&&!s.native[44]&&s.d.reserveChannelTiming.lead(44)==1000000); }
    noResources();
#endif
#ifndef RETENTION_ONLY
    for(unsigned kind:{1,2,3}) {
        const unsigned cycles=12;
        Result baseline;
        { Simulation s(false,true,kind); s.advance(B+cycles*CYCLE); baseline=s.result(kind==1?"multiple-slow-baseline":kind==2?"healthy-baseline":"single-baseline",cycles); }
        noResources();
        { Simulation s(true,true,kind); s.advance(B+cycles*CYCLE); auto got=s.result(kind==1?"multiple-slow-selective":kind==2?"healthy-selective":"single-selective",cycles);
          for(const auto &v:s.latencies) assert(got.native[v.first]>=3);
          if(kind!=1) assert(got.native==baseline.native&&got.attempts==baseline.attempts&&got.windows==baseline.windows&&!got.selected);
          else assert(got.intermediate); // Intent did not drain/stop useful work.
          assert(!s.recoveryPmRefusals);
        }
        noResources();
    }
    { Simulation s(true,true,1); s.latencies[44]=12000;
      s.d.forwardTiming.observeRetained(44,990000,20000);
      for(unsigned i=0;i<100;++i)s.d.forwardTiming.observeRetained(36,12000,0);
      assert(s.d.forwardTiming.leadRetained(44)==1000000);
      s.advance(B+52*CYCLE); s.result("outlier-resampling",52);
      assert(s.attempts[44]>=9&&s.d.forwardTiming.leadRetained(44)==55000&&s.native[44]);
      assert(s.native[1]&&s.native[11]&&s.native[36]); }
    noResources();
    { Simulation s(false,true,1); s.unanswered={44}; s.advance(B+12*CYCLE); s.result("unanswered-baseline",12); }
    noResources();
    { Simulation s(true,true,1); s.unanswered={44}; s.advance(B+12*CYCLE); s.result("unanswered-selective",12);
      assert(s.attempts[44]>=2&&!s.native[44]);
      // Protect the whole pending response, but do not let repeated known
      // timeouts as intermediate work starve EVERY other selected intent.
      for(unsigned ch:{1,11,36}) assert(std::any_of(s.starts.begin(),s.starts.end(),[&](const Start &start) {
          return start.channel==ch&&start.at>B+6*CYCLE;
      })); }
    noResources();
    { Simulation s(true,true,1); s.d.radio.modelOffchan=false; s.advance(B+4*CYCLE); s.result("only-home",4);
      assert(s.native[36]&&!s.attempts[1]&&!s.attempts[11]&&!s.attempts[44]); }
    noResources();
#endif
}

#ifndef RETENTION_ONLY
static void sparseTimeoutRecovery() {
    // An expired pending response can be removed by idle/unreachable cleanup
    // before the ordinary observation point. Keep real 24-cycle scheduler runs:
    // the off control demonstrates existing starvation, not a new regression.
    for(uint8_t filler:{uint8_t(0),uint8_t(52)}) for(unsigned phase:{0u,2u}) {
        for(bool forward:{false,true}) {
            {
                Simulation s(forward,true,1); auto &d=s.d;
                memset(s.expected,filler,sizeof(s.expected));
                s.expected[phase]=36; s.expected[phase+8]=44;
                memcpy(d.state.sequence.channels,s.expected,16); d.state.ownSequence=d.state.sequence;
                s.latencies={{36,5000},{44,12000}}; s.unanswered={44};
                d.reserveChannelTiming={}; d.forwardTiming={};
                d.reserveChannelTiming.observe(36,5000,0);
                d.forwardTiming.observeRetained(36,5000,0);
                unsigned idleTimeoutAborts=0,observedBeforeAbort=0;
                d.radio.modelBeforeAbort=[&] {
                    assert(!d.awTimeline.open);
                    const auto &g=d.radio.grant;
                    const uint64_t now=clockUS.load();
                    const uint8_t channel=d.state.channel(now);
                    if(g.pending && now>=g.deadline && now-g.deadline<CYCLE &&
                       d.requestedChannel && d.requestedWindow.end<=now &&
                       (!channel || !d.radio.awdlChannelReachable(channel))) {
                        ++idleTimeoutAborts;
                        for(const auto &p:d.forward.progress)
                            if(p.channel==g.channel && p.intermediateRetryAt==g.deadline+CYCLE)
                                ++observedBeforeAbort;
                    }
                };
                s.advance(B+24*CYCLE);
                s.result(forward?"sparse-timeout-selective":"sparse-timeout-baseline",24);
                unsigned later=0;
                for(const auto &w:s.starts) if(w.channel==36 && w.at>=B+12*CYCLE) ++later;
                printf("sparse timeout filler=%u phase=%u forward=%u: later=%u idle_aborts=%u observed_before_abort=%u\n",
                    filler,phase,unsigned(forward),later,idleTimeoutAborts,observedBeforeAbort);
                assert(!s.native[44] && idleTimeoutAborts && !s.recoveryPmRefusals);
                if(forward) {
                    assert(observedBeforeAbort==idleTimeoutAborts && d.forward.responseTimeouts);
                    assert(later>=4); // Repeated healthy service, not just the initial window.
                } else assert(!observedBeforeAbort && !d.forward.responseTimeouts);
            }
            noResources();
        }
    }
    puts("PASS timeout observation precedes idle/unreachable cancellation and permits sustained healthy-channel recovery");
}

static mtkawdl::State capturedState() {
    mtkawdl::State s; s.init(MAC,B); s.baseCounter=BASE;
    memcpy(s.sequence.channels,CAPTURE,16); s.ownSequence=s.sequence; return s;
}
static const uint8_t ALLOWED[16]={1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
static void armUnderserved(mtkawdl::ForwardAdmission &f,const mtkawdl::State &s,const mtkawdl::ForwardRadioContext &c,
                          const mtkawdl::RocChannelTiming &t,const mtkawdl::Timeline &n,const mtkawdl::Reservation &g) {
    f.update(s,B,c,t,ALLOWED,n,g,{},0);
    assert(!f.active); // No cold round robin: allow a full greedy cycle first.
    f.nativeWindow(149,B+CYCLE); f.nativeWindow(9,B+CYCLE);
    f.update(s,B+CYCLE,c,t,ALLOWED,n,g,{},0); assert(f.active&&f.plan.window.channel==6);
}
static void intentInvariants() {
    auto s=capturedState(); mtkawdl::ForwardRadioContext c; c.session=17;c.infra=9;c.infraState=4;
    mtkawdl::RocChannelTiming timing; timing.observeRetained(6,145133,4);
    mtkawdl::Timeline native; mtkawdl::Reservation grant;
    for(unsigned kind=0;kind<15;++kind) {
        auto changed=s; auto context=c; mtkawdl::ForwardAdmission f;
        armUnderserved(f,s,c,timing,native,grant);
        assert(!grant.pending&&!grant.granted&&!native.open&&!s.clockHoldUntil&&!s.controlsHeld);
        if(kind==0)context.session++;
        if(kind==1)changed.sync[5]++;
        if(kind==2)changed.master[5]++;
        if(kind==3)changed.sequence.channels[0]=11;
        if(kind==4)changed.epoch++;
        if(kind==5)changed.baseCounter++;
        if(kind==6)changed.controlsDirty=true;
        if(kind==7)changed.controlsDeferred++;
        if(kind==8)changed.ownSequence.channels[0]=11;
        if(kind==9)changed.presence=2;
        if(kind==10)changed.syncRole=3;
        if(kind==11)changed.nativeParentSelected=true;
        if(kind==12)changed.nativeClockAcquisitionPending=true;
        if(kind==13)context.bssid[0]++;
        if(kind==14)f.plan.window.counter++;
        f.update(changed,B+CYCLE+1,context,timing,ALLOWED,native,grant,{},0);
        assert(!f.active&&f.invalidated==1);
    }
    { mtkawdl::ForwardAdmission f; armUnderserved(f,s,c,timing,native,grant);
      const auto plan=f.plan;
      f.update(s,f.issueBy,c,timing,ALLOWED,native,grant,{},0);
      assert(f.active&&f.plan.requestAt==plan.requestAt&&f.plan.window.start==plan.window.start);
      f.update(s,f.issueBy+1,c,timing,ALLOWED,native,grant,{},0); assert(!f.active&&f.expired==1); }
    { mtkawdl::ForwardAdmission f; armUnderserved(f,s,c,timing,native,grant);
      const auto edge=f.prepareAt();
      mtkawdl::RadioPlan before{{edge-W,edge,0,149,4},edge-W-55000,150};
      assert(f.allowsIntermediate(before)); before.window.end++; assert(!f.allowsIntermediate(before));
      assert(f.blocks(before.window,false,edge-1)); assert(!f.blocks(before.window,true,edge-1));
      auto original=before.window;
      grant.request(3,149,edge-10000); assert(grant.grant(3,149,300,edge-9000));
      assert(!f.canPreempt(native,grant,edge,original,149));
      original.end=edge;
      assert(!f.canPreempt(native,grant,edge-1,original,149));
      assert(f.canPreempt(native,grant,edge,original,149));
      native.open=true;native.active=original; assert(!f.canPreempt(native,grant,edge,original,149));
      native.open=false; grant.cancel(); }
    { mtkawdl::ForwardAdmission f; armUnderserved(f,s,c,timing,native,grant);
      const auto old=f.beginCall(c.session); assert(old);
      f.reset(); const auto replacement=f.beginCall(c.session+1); assert(replacement!=old);
      assert(!f.completeCall(old)&&f.callId==replacement); assert(f.completeCall(replacement)); }
    // All channels remain underserved in this control. Duplicate schedule
    // entries do not buy extra turns, and expired attempts rotate as well.
    { mtkawdl::ForwardAdmission f; f.update(s,B,c,timing,ALLOWED,native,grant,{},0);
      unsigned index=0; const uint8_t expected[]={6,9,149,6,9,149}; uint64_t now=B;
      for(auto channel:expected) {
          now+=2*CYCLE; f.update(s,now,c,timing,ALLOWED,native,grant,{},0);
          assert(f.active&&f.plan.window.channel==channel); ++index;
          f.update(s,f.issueBy+1,c,timing,ALLOWED,native,grant,{},0); assert(!f.active&&f.expired==index);
      }
    }
    { mtkawdl::ForwardAdmission f;f.update(s,B,c,timing,ALLOWED,native,grant,{},0);
      mtkawdl::Reservation lost;lost.pending=true;lost.channel=149;lost.deadline=B+100;
      f.observeReservation(s,lost,lost.deadline);
      f.observeReservation(s,lost,lost.deadline+5000);assert(f.responseTimeouts==1);
      f.nativeWindow(149,B+CYCLE);f.nativeWindow(9,B+CYCLE);
      f.update(s,B+CYCLE,c,timing,ALLOWED,native,grant,{},0);assert(f.active);
      mtkawdl::RadioPlan intermediate{{B+CYCLE+W,B+CYCLE+2*W,0,149,4},lost.deadline+CYCLE-1,150};
      assert(!f.allowsIntermediate(intermediate));++intermediate.requestAt;assert(f.allowsIntermediate(intermediate));
      --intermediate.requestAt;lost.pending=false;lost.granted=true;
      f.observeReservation(s,lost,B+CYCLE);assert(f.allowsIntermediate(intermediate));
      f.reset();assert(!f.active&&f.responseTimeouts==1);
      for(const auto &p:f.progress)assert(!p.intermediateRetryAt);
    }
    puts("PASS full-cycle selectivity, distinct eligible rotation, 15 invalidations, fixed deadlines, no intent RF ownership, bounded timeout hold and serial containment");
}

static void settlingGrace() {
    const auto state=capturedState();
    mtkawdl::ForwardRadioContext context;context.session=17;context.infra=9;context.infraState=4;
    mtkawdl::RocChannelTiming timing,legacy;
    for(auto response:{500000u,12000u}) {
        timing.observeRetained(6,response,0);legacy.observe(6,response,0);
    }
    assert(timing.recoveryGrace(6,legacy));
    mtkawdl::Timeline native;mtkawdl::Reservation grant;
    // Ordinary eight-other aging can resolve this outlier without repeated
    // selective requests refreshing its old maximum. This only delays selection.
    {
        auto own=timing,ordinary=legacy;mtkawdl::ForwardAdmission f;
        f.update(state,B,context,own,ALLOWED,native,grant,{},0,&ordinary);
        f.nativeWindow(149,B+CYCLE);f.nativeWindow(9,B+CYCLE);
        f.update(state,B+CYCLE,context,own,ALLOWED,native,grant,{},0,&ordinary);
        assert(!f.active&&f.settlingTicks==1&&!f.selected);
        for(unsigned i=0;i<7;++i) { own.observeRetained(9,12000,0);ordinary.observe(9,12000,0); }
        f.update(state,B+CYCLE+1,context,own,ALLOWED,native,grant,{},0,&ordinary);
        assert(!f.active&&f.settlingTicks==2&&ordinary.lead(6)==508000);
        own.observeRetained(9,12000,0);ordinary.observe(9,12000,0);
        f.update(state,B+CYCLE+2,context,own,ALLOWED,native,grant,{},0,&ordinary);
        assert(f.active&&f.plan.window.channel==6&&f.settlingTicks==2&&ordinary.lead(6)==55000);
        assert(!grant.pending&&!grant.granted&&!native.open&&!state.clockHoldUntil);
    }
    // With no other observations the grace still ends at eight absent cycles,
    // not eight cycles after the latest poll/sample. Refreshing this channel's
    // still-low response cannot slide that bound or count as native progress.
    {
        mtkawdl::ForwardAdmission f;
        f.update(state,B,context,timing,ALLOWED,native,grant,{},0,&legacy);
        for(uint64_t at:{B+CYCLE,B+3*CYCLE,B+8*CYCLE-1}) {
            f.nativeWindow(149,at);f.nativeWindow(9,at);
            timing.observeRetained(6,12000,0);legacy.observe(6,12000,0);
            f.update(state,at,context,timing,ALLOWED,native,grant,{},0,&legacy);
            assert(!f.active&&!f.selected&&timing.recoveryGrace(6,legacy));
        }
        f.update(state,B+8*CYCLE,context,timing,ALLOWED,native,grant,{},0,&legacy);
        assert(f.active&&f.plan.window.channel==6&&f.selected==1&&f.settlingTicks==3);
        assert(!grant.pending&&!grant.granted&&!native.open&&!state.clockHoldUntil);
        f.reset();assert(!f.active&&f.settlingTicks==3);
    }
    puts("PASS current-context legacy settling, unchanged eight-other aging and non-sliding eight-cycle selection grace");
}

static void protectedReservations() {
    const uint64_t target=B+5*W;
    for(unsigned mode=0;mode<3;++mode) {
        { Simulation s(true);auto &d=s.d;d.onStart=d.onEnd=nullptr;d.radio.onReserveTiming=nullptr;
          d.radio.modelBeforeAbort=nullptr;d.radio.modelTx=nullptr;
          clockUS=target-5000;d.forward.reset();d.awTimeline.reset();
          d.requestedWindow={target,target+W,uint16_t(BASE+20),6,4};d.requestedChannel=6;
          const uint32_t duration=mode==0?150:mode==1?40:1;
          const uint64_t observed=target-10000;
          d.radio.grant.request(91,6,observed-1000);assert(d.radio.grant.grant(91,6,duration,observed));
          d.grantLatencyObservedAt=d.forwardGrantObservedAt=observed;
          const auto aborts=d.radio.aborts,reserves=d.radio.reserves;
          d.serviceInGate();assert(d.radio.aborts==aborts&&d.radio.reserves==reserves&&d.radio.grant.token==91);
          if(mode<2) {
              clockUS=target;d.serviceInGate();
              assert(d.radio.aborts==aborts&&d.radio.reserves==reserves&&d.radio.grant.token==91);
              assert(d.awTimeline.open==(mode==0));
          }
        } noResources();
    }
    { Simulation s(true);auto &d=s.d;d.onStart=d.onEnd=nullptr;d.radio.onReserveTiming=nullptr;
      d.radio.modelBeforeAbort=nullptr;d.radio.modelTx=nullptr;
      d.radio.grant.request(92,149,B);assert(d.radio.grant.grant(92,149,30,B));d.grantLatencyObservedAt=d.forwardGrantObservedAt=B;
      const auto aborts=d.radio.aborts,reserves=d.radio.reserves;
      for(unsigned i=0;i<28;++i) { clockUS=B+i*1000;d.serviceInGate();
          assert(d.radio.aborts==aborts&&d.radio.reserves==reserves&&d.radio.grant.token==92&&!d.awTimeline.open); }
      assert(d.radio.grant.canTransmit(149,B+27999)&&!d.radio.grant.canTransmit(149,B+28000)); }
    noResources();
    { Simulation s(true);auto &d=s.d;d.onStart=d.onEnd=nullptr;d.radio.onReserveTiming=nullptr;
      d.radio.modelBeforeAbort=nullptr;d.radio.modelTx=nullptr;
      d.radio.grant.request(93,6,B);const auto deadline=d.radio.grant.deadline;
      d.requestedWindow={B-2*W,B-W,0,6,4};d.requestedChannel=6;
      const auto aborts=d.radio.aborts,reserves=d.radio.reserves;
      clockUS=B+200000;d.serviceInGate();
      assert(d.radio.grant.pending&&d.radio.grant.deadline==deadline&&!d.forward.active);
      assert(d.radio.reserves==reserves&&d.radio.aborts==aborts); }
    noResources();
    puts("PASS production original future/full/short/current/pending grants, token identity and final 2ms guard");
}

// Seed only observation history, not a grant or native availability. The real
// service still decides preparation, admission, request and native start/end.
static void prepareIntent(Simulation &s) {
    auto &d=s.d;
    d.awTimeline.reset(); d.forward.reset(); d.radio.grant.cancel();
    d.requestedWindow={}; d.requestedChannel=d.grantedChannel=0;
    d.requestDeadline=0; d.state.holdClock(0); d.state.discardControls();
    armUnderserved(d.forward,d.state,d.forwardRadioContext(),d.forwardTiming,d.awTimeline,d.radio.grant);
    clockUS=B+CYCLE;
}

static void activeIntentBoundaries() {
    // Merely selecting/preparing a future target cannot license TX or native
    // availability. A request then remains pending until the actual response.
    { Simulation s(true); prepareIntent(s); auto &d=s.d;
      const auto plan=d.forward.plan; const uint64_t edge=d.forward.prepareAt();
      const auto reserves=d.radio.reserves,aborts=d.radio.aborts;
      uint8_t eth[54]{}; eth[0]=2;eth[12]=0x86;eth[13]=0xdd;eth[14]=0x60;
      assert(d.output(d.makeFrame(eth,sizeof(eth)))==kIOReturnOutputSuccess);
      for(uint64_t at:{edge-1,edge,plan.requestAt-1}) {
          clockUS=at;d.serviceInGate();
          assert(d.forward.active&&!d.radio.grant.pending&&!d.radio.grant.granted);
          assert(!d.awTimeline.open&&!d.awStartNotifications&&!d.state.clockHoldUntil);
          assert(!d.requestedChannel&&!d.requestedWindow.end&&!d.radio.transmits);
          assert(d.radio.reserves==reserves&&d.radio.aborts==aborts);
      }
      clockUS=plan.requestAt;d.serviceInGate();
      assert(!d.forward.active&&d.forward.issued==1&&d.radio.grant.pending&&!d.radio.grant.granted);
      assert(!d.awTimeline.open&&!d.radio.transmits&&d.requestedChannel==plan.window.channel);
      assert(!d.radio.grant.canTransmit(plan.window.channel,clockUS.load())); }
    noResources();
    // An intermediate request can exceed its expected response time. Never
    // steal its token/response deadline to satisfy a speculative fixed edge.
    for(bool extendsPastEdge:{false,true}) {
        { Simulation s(true);prepareIntent(s);auto &d=s.d;
          const auto edge=d.forward.prepareAt();const auto issueBy=d.forward.issueBy;
          d.radio.grant.request(94,149,B+CYCLE);
          d.requestedChannel=149;d.requestedWindow={B+CYCLE,B+CYCLE+W,0,149,4};
          if(extendsPastEdge)d.requestedWindow.end=edge+1;
          const auto deadline=d.radio.grant.deadline;
          const auto aborts=d.radio.aborts,reserves=d.radio.reserves;
          for(uint64_t at:{edge,issueBy,issueBy+1}) {
              clockUS=at;d.serviceInGate();
              assert(d.radio.grant.pending&&d.radio.grant.token==94&&d.radio.grant.deadline==deadline);
              assert(d.radio.aborts==aborts&&d.radio.reserves==reserves&&!d.awTimeline.open&&!d.radio.transmits);
          }
          assert(!d.forward.active);
          assert(extendsPastEdge ? d.forward.invalidated==1 : d.forward.expired==1);
        } noResources();
    }
    // Full and short original FUTURE grants coexist with an active intent.
    // The short case is not native availability, but its token is still owned.
    for(unsigned duration:{150u,40u}) {
        { Simulation s(true);prepareIntent(s);auto &d=s.d;
          d.onStart=d.onEnd=nullptr;d.radio.modelTx=nullptr;
          const uint64_t target=B+CYCLE+W,observed=target-10000;
          d.requestedWindow={target,target+W,uint16_t(BASE+68),149,4};d.requestedChannel=149;
          d.radio.grant.request(95,149,observed-1000);assert(d.radio.grant.grant(95,149,duration,observed));
          d.grantLatencyObservedAt=d.forwardGrantObservedAt=observed;
          const auto aborts=d.radio.aborts,reserves=d.radio.reserves;
          for(uint64_t at:{observed,target,target+20000}) {
              clockUS=at;d.serviceInGate();
              assert(d.forward.active&&d.radio.grant.token==95&&d.radio.grant.granted);
              assert(d.radio.aborts==aborts&&d.radio.reserves==reserves);
          }
          assert(d.awTimeline.open==(duration==150));
        } noResources();
    }
    // A selected recovery never bypasses the HAL's scan/join arbitration.
    for(bool joining:{false,true}) {
        { Simulation s(true);prepareIntent(s);auto &d=s.d;
          const auto edge=d.forward.prepareAt(),request=d.forward.plan.requestAt;
          d.radio.modelJoin=joining;d.radio.modelScan=!joining;d.radio.modelPS=false;
          clockUS=edge;d.serviceInGate();clockUS=request;d.serviceInGate();
          assert(!d.radio.grant.pending&&!d.radio.grant.granted&&!d.awTimeline.open&&!d.radio.transmits);
          assert(!d.state.clockHoldUntil&&!d.requestedChannel&&s.issues.empty());
          if(joining)assert(!d.radio.modelPS);
        } noResources();
    }
    puts("PASS active-intent pending/future/short ownership, no intent-only TX/native/clock, scan/join priority");
}

static void callbackOwnership() {
    for(unsigned kind=0;kind<6;++kind) {
        { Simulation s(true);prepareIntent(s);auto &d=s.d;
          const auto old=d.txSession;const auto reserves=d.radio.reserves;
          bool called=false;unsigned timerCalls=0;
          d.radio.modelAfterSleep=[&] {
              if(called)return;called=true;
              if(kind==0)++d.radio.ic.ic_join_diag.Attempt;
              if(kind==1)++d.state.epoch;
              if(kind==2) {
                  assert(d.setSync(false)==0&&d.setSync(true)==0&&d.txSession!=old);
                  d.timer->setTimeoutMS(37);timerCalls=d.timer->calls;
              }
              if(kind==3) {
                  d.radio.on=false;d.radio.storage.sc_awdl_enabled=false;
                  d.radio.storage.sc_awdl_native_session=0;d.radio.grant.cancel();
              }
              if(kind==4) {
                  assert(d.forward.callId&&d.forward.callSession==old);
                  d.serviceInGate();assert(d.radio.reserves==reserves&&d.forward.callId);
              }
              if(kind==5)clockUS+=15000;
          };
          clockUS=d.forward.prepareAt();d.serviceInGate();assert(called&&d.radio.reserves==reserves);
          if(kind<2)assert(!d.forward.active&&d.forward.invalidated==1);
          if(kind==2)assert(d.enabled&&d.timer->pending&&d.timer->us==37000&&d.timer->calls==timerCalls);
          if(kind==3) {
              assert(d.enabled&&d.timer->pending);++clockUS;d.serviceInGate();
              assert(!d.enabled&&!d.timer->pending&&!d.forward.callId);
          }
          if(kind==4)assert(d.forward.active&&!d.forward.callId&&d.timer->pending);
          if(kind==5)assert(!d.forward.active&&d.forward.expired==1&&!d.forward.issued);
        } noResources();
    }
    // A preemption wait may change the context, schedule or entire session.
    // The completion must not clear/issue against its replacement.
    for(unsigned kind=0;kind<6;++kind) {
        { Simulation s(true);prepareIntent(s);auto &d=s.d;
          const auto edge=d.forward.prepareAt();
          d.requestedWindow={B+CYCLE,B+CYCLE+W,0,149,4};d.requestedChannel=149;
          d.radio.grant.request(96,149,edge-100000);assert(d.radio.grant.grant(96,149,300,edge-99000));
          d.grantLatencyObservedAt=d.forwardGrantObservedAt=edge-99000;
          const auto reserves=d.radio.reserves;const auto old=d.txSession;unsigned timerCalls=0;bool called=false;
          d.radio.modelAfterAbort=[&] {
              if(called)return;called=true;
              if(kind==0)++d.radio.ic.ic_bss->ni_bssid[5];
              if(kind==1)++d.state.epoch;
              if(kind==2) {
                  assert(d.setSync(false)==0&&d.setSync(true)==0&&d.txSession!=old);
                  d.radio.give(97,9,150);d.requestedChannel=9;d.timer->setTimeoutMS(43);timerCalls=d.timer->calls;
              }
              if(kind==3) {
                  d.radio.on=false;d.radio.storage.sc_awdl_enabled=false;d.radio.storage.sc_awdl_native_session=0;
              }
              if(kind==4) {
                  assert(d.forward.callId);d.serviceInGate();assert(d.radio.reserves==reserves&&d.forward.callId);
              }
              if(kind==5)clockUS+=12000;
          };
          clockUS=edge;d.serviceInGate();assert(called&&d.radio.reserves==reserves);
          if(kind<2)assert(!d.forward.active&&d.forward.invalidated==1);
          if(kind==2)assert(d.enabled&&d.radio.grant.token==97&&d.requestedChannel==9&&d.timer->us==43000&&d.timer->calls==timerCalls);
          if(kind==3) { assert(d.enabled&&d.timer->pending);++clockUS;d.serviceInGate();assert(!d.enabled&&!d.timer->pending); }
          if(kind==4)assert(d.forward.active&&!d.forward.callId&&d.forward.preempted==1);
          if(kind==5)assert(!d.forward.active&&d.forward.expired==1&&!d.forward.issued);
        } noResources();
    }
    // Even an end callback can disable/enable and open a replacement AW. Old
    // postMessage return must not release the replacement's held controls or
    // let old reset cleanup discard that AW/its timer.
    for(bool halReset:{false,true}) {
        { Simulation s(true);auto &d=s.d;d.serviceInGate();assert(d.awTimeline.open);
          bool replaced=false;uint64_t session=0;unsigned calls=0;
          auto normal=d.onEnd;
          d.onEnd=[&] {
              if(!halReset)normal();else ++s.ends;
              if(replaced)return;replaced=true;
              if(halReset) {
                  d.radio.storage.sc_resetting=false;
                  assert(d.setSync(true)==0);
              } else assert(d.setSync(false)==0&&d.setSync(true)==0);
              session=d.txSession;d.radio.give(98,149,150);
              d.serviceInGate();assert(d.awTimeline.open&&d.state.controlsHeld);
              d.timer->setTimeoutMS(47);calls=d.timer->calls;
          };
          clockUS=d.awTimeline.radioEnd;
          if(halReset) {
              d.radio.on=false;d.radio.storage.sc_awdl_enabled=false;
              d.radio.storage.sc_awdl_native_session=0;d.radio.storage.sc_resetting=true;d.radio.grant.cancel();
          }
          d.serviceInGate();assert(replaced&&d.enabled&&d.txSession==session);
          assert(d.awTimeline.open&&d.awTimeline.token==98&&d.state.controlsHeld);
          assert(d.timer->pending&&d.timer->us==47000&&d.timer->calls==calls);
        } noResources();
    }
    { Simulation s(true);auto &d=s.d;
      auto normal=d.onStart;d.onStart=[&] {normal();++d.radio.ic.ic_join_diag.Attempt;};
      d.onEnd=[&] {assert(!d.awTimeline.open);++s.ends;}; // Administrative context change, not an on-time AW.
      d.serviceInGate();assert(s.starts.size()==1&&s.ends==1&&!d.awTimeline.open&&!d.radio.transmits);
    } noResources();
    // Early native end still precedes abort, but a new session started by that
    // end callback now owns the grant. Do not abort its future reservation.
    { Simulation s(true);auto &d=s.d;clockUS=B+2*W;
      d.radio.give(99,149,150);d.requestedWindow={B+2*W,B+3*W,uint16_t(BASE+8),149,4};d.requestedChannel=149;
      d.serviceInGate();assert(d.awTimeline.open);
      bool replaced=false;unsigned calls=0,aborts=0;const auto old=d.txSession;
      auto normal=d.onEnd;d.onEnd=[&] {
          normal();if(replaced)return;replaced=true;
          assert(d.setSync(false)==0&&d.setSync(true)==0&&d.txSession!=old);
          d.radio.give(100,9,150);d.requestedChannel=9;
          d.requestedWindow={B+3*W,B+4*W,uint16_t(BASE+12),9,4};
          d.timer->setTimeoutMS(31);calls=d.timer->calls;aborts=d.radio.aborts;
      };
      clockUS=B+3*W-10000;d.serviceInGate();
      assert(replaced&&d.radio.grant.token==100&&d.requestedChannel==9);
      assert(d.radio.aborts==aborts&&d.timer->us==31000&&d.timer->calls==calls);
    } noResources();
    // Schedule/context changes while reserve releases the gate cause cleanup;
    // even that cleanup's abort cannot complete over a replacement session.
    { Simulation s(true);prepareIntent(s);auto &d=s.d;bool replaced=false;unsigned calls=0;
      d.radio.onReserve=[&] {++d.radio.ic.ic_join_diag.Attempt;};
      d.radio.modelAfterAbort=[&] {
          if(replaced)return;replaced=true;
          assert(d.setSync(false)==0&&d.setSync(true)==0);
          d.radio.give(101,9,150);d.requestedChannel=9;d.timer->setTimeoutMS(29);calls=d.timer->calls;
      };
      const auto request=d.forward.plan.requestAt;
      clockUS=d.forward.prepareAt();d.serviceInGate();clockUS=request;d.serviceInGate();
      assert(replaced&&d.radio.grant.token==101&&d.requestedChannel==9);
      assert(d.timer->us==29000&&d.timer->calls==calls);
    } noResources();
    puts("PASS PM/abort/native callback context, schedule, session, reentry and end/reset timer ownership");
}

static void contextAndReentry() {
    for(unsigned kind=0;kind<4;++kind) {
        { Simulation s(true);auto &d=s.d;assert(d.forwardTiming.leadRetained(6)>55000);
          if(kind==0)++d.radio.ic.ic_join_diag.Attempt;
          if(kind==1)++d.radio.ic.ic_bss->ni_bssid[5];
          if(kind==2)d.radio.infraChannel=149;
          if(kind==3)d.radio.modelOffchan=false;
          d.serviceInGate();assert(!d.forwardTiming.observations&&d.forwardTiming.leadRetained(6,123)==123); }
        noResources();
    }
    { Simulation s(true);unsigned callbacks=0;
      s.d.radio.onReserve=[&]{const auto n=s.d.radio.reserves;assert(s.d.forward.callId);
          s.d.serviceInGate();assert(s.d.radio.reserves==n&&s.d.forward.callId);++callbacks;};
      s.advance(B+4*CYCLE);assert(callbacks&&s.d.timer->pending); }
    noResources();
    { Simulation s(true);bool reset=false;
      s.d.radio.onReserve=[&]{if(reset)return;reset=true;
          s.d.radio.on=false;s.d.radio.storage.sc_awdl_enabled=false;s.d.radio.storage.sc_resetting=true;
          s.d.radio.storage.sc_awdl_native_session=0;s.d.radio.grant.cancel();};
      while(!reset)s.advance(s.nextTick);
      assert(s.d.enabled&&s.d.timer->pending); // HAL-only reset needs its one-shot.
      s.advance(s.nextTick); assert(!s.d.enabled&&!s.d.timer->pending&&!s.d.forward.active&&!s.d.forward.callId); }
    noResources();
    { Simulation s(true);bool replaced=false;uint64_t replacement=0,session=0;unsigned timerCalls=0;
      s.d.radio.onReserve=[&]{if(replaced)return;replaced=true;
          const auto old=s.d.txSession;assert(s.d.setSync(false)==0);assert(s.d.setSync(true)==0);
          assert(s.d.txSession!=old);session=s.d.txSession;
          s.d.serviceInGate();replacement=s.d.forward.serial;timerCalls=s.d.timer->calls;
      };
      while(!replaced)s.advance(s.nextTick);
      assert(s.d.enabled&&s.d.txSession==session&&s.d.forward.serial==replacement);
      assert(s.d.timer->pending&&s.d.timer->calls==timerCalls); }
    noResources();
    { Simulation s(true);unsigned reentries=0;auto normal=s.d.onEnd;
      s.d.onEnd=[&]{normal();const auto before=s.d.radio.reserves;
          assert(s.d.state.controlsHeld&&!s.d.awTimeline.open);s.d.serviceInGate();
          assert(s.d.radio.reserves==before&&!s.d.awTimeline.open);++reentries;};
      s.advance(B+3*CYCLE);assert(reentries); }
    noResources();
    puts("PASS context reset, callback reentry, HAL-only one-shot recovery and replacement timer ownership");
}
#endif

int main() {
    setbuf(stdout,nullptr); assert(mtkawdl::layoutMatches());
#ifndef RETENTION_ONLY
    intentInvariants();settlingGrace();
#endif
    closedLoops();
#ifndef RETENTION_ONLY
    sparseTimeoutRecovery();protectedReservations();activeIntentBoundaries();callbackOwnership();contextAndReentry();
#endif
    assert(!gateDepth&&!heldHalLocks); noResources();
    puts("PASS actual production scheduler / host-boundary regression; no hardware throughput claim");
}
