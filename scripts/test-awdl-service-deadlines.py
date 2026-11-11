#!/usr/bin/env python3
"""Source-bound granted-service deadline regression (host only).

Runs the actual complete tick/service/lifecycle bodies, not a copied deadline
formula. Reuses the lifecycle extractor and real Wi-Fi Rust State/Reservation/
Timeline FFI; also extracts the HAL's live grant/readiness accessors. Only the
OS/native queues, transmit cost/outcomes and firmware waits are host boundaries.
The timer models the documented one-shot, relative-from-now replacement API;
arming never dispatches an event. C++ is ASan/UBSan-instrumented, Rust is not.

Default: build fresh offline Wi-Fi-only FFI. --archive-provenance may reuse that
build only with unchanged Rust inputs and archive bytes. --source-cpp runs a
saved, unmodified production source as a negative control with the SAME tests.
Every run retains source bindings, commands, failures and results locally.
No kext build, hardware, network, configuration or installation operations.
Run with python3 -I -B scripts/test-awdl-service-deadlines.py.
"""
from pathlib import Path
import argparse
import importlib.util
import json
import os
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('awdl_lifecycle', ROOT / 'scripts/test-awdl-native-lifecycle.py')
L = importlib.util.module_from_spec(spec)
spec.loader.exec_module(L)

CASES = ['future-zero', 'future-work', 'crossed-radio', 'radio-due', 'cap-work',
         'future-prepare', 'crossed-prepare', 'prepare-due', 'new-aw-due-prepare', 'invalid-preparation',
         'preserved-preparation', 'delayed-grant', 'missing-grant', 'large-uptime',
         'native64', 'refill', 'pressure', 'stop', 'reenable', 'hal-reset',
         'reset-replacement', 'callback-replacement', 'peer-gate']


def one(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new, 1)


def boundaries(text, ex):
    # These substitutions name only the lifecycle fixture's explicit OS/HAL
    # stubs. No extracted frontend/helper/callsite is rewritten.
    text = one(text, 'std::atomic<uint64_t> clockUS{1000000};',
               'std::atomic<uint64_t> clockUS{1000000};\nunsigned modelMbufs=0;')
    text = one(text, '*out=new Packet; (*out)->bytes.resize(n);',
               '*out=new Packet; ++modelMbufs; (*out)->bytes.resize(n);')
    text = one(text, 'void mbuf_freem(mbuf_t p) { delete p; }',
               'void mbuf_freem(mbuf_t p) { assert(p&&modelMbufs); --modelMbufs; delete p; }')
    text = one(text, ' void setTimeoutMS(unsigned n) { pending=true; us=n*1000; ++calls; }\n'
                    ' void setTimeoutUS(unsigned n) { pending=true; us=n; }', r'''
 uint64_t deadline=0;
 std::vector<unsigned> intervals;
 void setTimeoutMS(unsigned n) { setTimeoutUS(n*1000); }
 void setTimeoutUS(unsigned n) {
  assert(n); pending=true; us=n; deadline=clockUS.load()+n; ++calls; intervals.push_back(n);
 }
''')
    # Give the real, lock-taking HAL accessors their real reservation field.
    text = one(text, '  uint32_t sc_awdl_epoch=0; AwdlRocTiming sc_awdl_roc_timing{};',
               '  uint32_t sc_awdl_epoch=0; AwdlRocTiming sc_awdl_roc_timing{}; mtkawdl::Reservation sc_awdl_roc{};')
    text = one(text, ' uint32_t duration=0; mtkawdl::Reservation grant{};',
               ' uint32_t duration=0; mtkawdl::Reservation &grant=storage.sc_awdl_roc;')
    for old, declaration in [
        (' bool awdlIsEnabled() { return on; }', ' bool awdlIsEnabled();'),
        (' bool awdlGrantSnapshot(mtkawdl::Reservation &out) { out=grant; return on; }',
         ' bool awdlGrantSnapshot(mtkawdl::Reservation &);'),
        (' bool awdlCanTransmit(unsigned c) { return grant.canTransmit(c,clockUS.load()); }',
         ' bool awdlCanTransmit(uint8_t);'),
    ]:
        text = one(text, old, declaration)
    text = one(text, ' IOReturn awdlTryTransmit(mbuf_t *m,unsigned c) { if(tryStatus)return tryStatus; auto status=awdlTransmit(*m,c); *m=nullptr; return status; }', r'''
 uint64_t modelWorkUS=0;
 unsigned modelCapacity=1000;
 IOReturn modelPeerResult=0;
 std::vector<std::vector<uint8_t>> admitted;
 std::function<void()> modelAfterTx;
 IOReturn awdlTryTransmit(mbuf_t *m,unsigned c) {
  gatedBoundary(); assert(m&&*m);
  if(tryStatus)return tryStatus;
  if(!awdlCanTransmit(c))return kIOReturnNotReady;
  if(admitted.size()>=modelCapacity)return kIOReturnNoResources;
  admitted.push_back((*m)->bytes);
  auto status=awdlTransmit(*m,c); *m=nullptr; clockUS+=modelWorkUS;
  auto hook=modelAfterTx; if(hook)hook(); return status;
 }
''')
    text = one(text, ' IOReturn awdlPeer(const uint8_t *,bool add) { gatedBoundary(); if(onPeer)onPeer(add); return add?0:peerDeleteResult; }',
               ' IOReturn awdlPeer(const uint8_t *,bool add) { gatedBoundary(); auto hook=onPeer; if(hook)hook(add); return add?modelPeerResult:peerDeleteResult; }')
    text = one(text, '  onStart=onEnd=nullptr; radio.onPeer=nullptr; radio.onReserve=nullptr; radio.onReserveTiming=nullptr; radio.wait=nullptr; radio.onEnable=nullptr;',
               '  onStart=onEnd=nullptr; radio.onPeer=nullptr; radio.onReserve=nullptr; radio.onReserveTiming=nullptr; radio.wait=nullptr; radio.onEnable=nullptr; radio.modelAfterTx=nullptr;')
    for name in ['awdlCanTransmit', 'awdlGrantSnapshot', 'awdlIsEnabled']:
        text += ex.function(L.HAL, 'MtkMwx::' + name)
    return text


MAIN = r'''
#include "extracted.cpp"
#include <AWDL/AwdlLayout.hpp>

static constexpr uint64_t B=1000000;
static bool inSubmit=false;
static const uint8_t destination[6]={2,9,8,7,6,5};

struct DeadlineFixture:NativeFixture {
 explicit DeadlineFixture(bool switching=false,uint64_t base=B,unsigned presence=4,uint64_t late=0) {
  clockUS=base;
  uint8_t local[6];memcpy(local,state.mac,6);state.init(local,base);
  state.presence=state.ownPresence=presence;state.sequence.step=presence-1;
  for(unsigned i=0;i<mtkawdl::slots;++i)state.sequence.channels[i]=switching&&(i%2) ? 44 : 6;
  state.ownSequence=state.sequence;
  radio.give(1,6,300);clockUS=base+late;
  serviceInGate();
  assert(awTimeline.open&&awTimeline.active.channel==6&&events.size()==1&&state.controlsHeld);
 }
 void turn(uint64_t at) { assert(at>=clockUS.load()); clockUS=at; serviceInGate(); }
 void next() { assert(timer->pending&&timer->deadline>clockUS.load()); turn(timer->deadline); }
 uint64_t edge() const { return awTimeline.radioEnd; }
 uint64_t prepare() const { return awTimeline.active.end-awTimeline.switchLeadUS(); }
 ~DeadlineFixture() override { radio.modelAfterTx=nullptr; }
};
static IOSkywalkPacket *packet(DeadlineFixture &d) {
 auto p=txPacket(d.txPool); p->length=64;
 memcpy(p->bytes,destination,6); p->bytes[12]=0x86;p->bytes[13]=0xdd;
 p->bytes[14]=0x60;p->bytes[19]=10; // Exactly 40+10 IPv6 bytes after Ethernet.
 return p;
}
static void submit(DeadlineFixture &d,unsigned n=1) {
 std::vector<IOSkywalkPacket *> batch;
 for(unsigned i=0;i<n;++i)batch.push_back(packet(d));
 auto before=txCompleted;
 assert(!inSubmit);inSubmit=true;
 auto consumed=gate.enter([&]{return d.submitNative(&d,d.txSubmit,batch.data(),n,nullptr);});
 inSubmit=false;
 assert(consumed==n&&txCompleted==before); // No completion before submit returns.
}
static void armIs(DeadlineFixture &d,uint64_t expected) {
 std::printf("arm now=%llu expected=%llu actual=%llu interval=%u\n",
  (unsigned long long)clockUS.load(),(unsigned long long)expected,
  (unsigned long long)d.timer->deadline,d.timer->us);
 assert(d.timer->pending&&d.timer->deadline==expected&&d.timer->us&&d.timer->us<=2000);
}
static void pairedEnd(DeadlineFixture &d,uint64_t edge,size_t index=1) {
 assert(d.events.size()>index&&d.events[index].id==43);
 assert(mtkawdl::be32(d.events[index].bytes.data()+4)==uint32_t(edge));
 assert(d.properties["MTKAWDLLastAWEndUS"]==edge);
}
static void radioDeadline(uint64_t work,uint64_t base=B) {
 DeadlineFixture d(false,base); const auto edge=d.edge();
 submit(d); d.radio.modelWorkUS=work;
 d.turn(edge-500);
 assert(clockUS.load()==edge-500+work&&d.dataTxSubmitted==1&&!d.queueCount);
 assert(d.events.size()==1); // Tail cannot dispatch native/firmware events inline.
 const auto expected=work<500 ? edge : clockUS.load()+1;
 armIs(d,expected);
 d.radio.modelWorkUS=0;d.next();pairedEnd(d,edge);
 assert(d.awTimeline.ends==1);d.next();assert(d.awTimeline.ends==1);
}
static void radioDue() {
 DeadlineFixture d;const auto edge=d.edge();
 d.turn(edge+200);pairedEnd(d,edge);assert(d.awTimeline.ends==1);
 const auto ends=d.awTimeline.ends;d.next();assert(d.awTimeline.ends==ends);
}
static void cap() {
 DeadlineFixture d;submit(d);d.radio.modelWorkUS=200;
 d.turn(B+1000);assert(clockUS.load()==B+1200&&d.dataTxSubmitted==1);
 armIs(d,B+3200);assert(d.timer->deadline<d.edge());
}
static void preparation(uint64_t work,bool due=false) {
 DeadlineFixture d(true);const auto edge=d.prepare();
 const auto oldEnd=d.edge();
 if(!due){submit(d);d.radio.modelWorkUS=work;}
 d.turn(edge-(due ? 0 : 500));
 if(!due) {
  assert(d.awTimeline.open&&d.events.size()==1&&d.radio.aborts==0);
  armIs(d,work<500 ? edge : clockUS.load()+1);d.radio.modelWorkUS=0;d.next();
 }
 assert(!d.awTimeline.open&&d.awTimeline.ends==1&&d.radio.aborts==1);
 assert(d.properties["MTKAWDLLastAWEndUS"]>=edge&&d.properties["MTKAWDLLastAWEndUS"]<oldEnd);
 assert(d.timer->pending&&d.timer->us>=1000); // Preparation consumed, not 1us polling.
}
static void newlyOpenedPreparation() {
 // A 1-AW window can begin after its 6144us preparation edge, within the
 // existing 8000us start tolerance. It was not open at the early-close check.
 DeadlineFixture d(true,B,1,7000);
 assert(d.awTimeline.open&&d.prepare()==B+6144&&d.events.size()==1);
 armIs(d,B+7001);d.next();
 assert(!d.awTimeline.open&&d.awTimeline.ends==1&&d.radio.aborts==1);
 pairedEnd(d,B+7001);assert(d.timer->us>=1000);
}
static void invalidPreparation() {
 // Invalidate the eligibility DURING the drain, not only before service entry.
 for(unsigned kind=0;kind<4;++kind) {
  DeadlineFixture d(true);const auto prep=d.prepare(),end=d.edge();submit(d);
  d.radio.modelWorkUS=200;
  d.radio.modelAfterTx=[&]{
   if(kind==0)d.radio.infraChannel=6; // Next channel is no longer reachable.
   if(kind==1)d.radio.grant.cancel(); // No old snapshot may authorize preparation.
   if(kind==2)d.state.presence=2;     // Administrative schedule invalidation.
   if(kind==3)d.radio.give(12,6,300);  // Same channel, different grant identity.
  };
  d.turn(prep-500);assert(d.events.size()==1);d.radio.modelAfterTx=nullptr;
  if(!kind)armIs(d,clockUS.load()+2000);
  else { armIs(d,clockUS.load()+1);d.next();assert(d.awTimeline.ends==1); }
  assert(d.properties["MTKAWDLLastAWEndUS"]<=end);
 }
}
static void preservedPreparation() {
 DeadlineFixture d(true);const auto prep=d.prepare(),end=d.edge();
 // A still-valid original future channel-6 reservation owns this same live
 // grant even though the immediate next window is channel 44.
 assert(mtkawdl::currentWindow(d.state,end+(end-B),d.requestedWindow));
 d.requestedChannel=6;
 assert(d.requestedWindow.channel==6&&mtkawdl::grantCoversReservedWindow(
  d.state,d.requestedWindow,d.requestedChannel,d.radio.grant,prep));
 d.turn(prep+1);
 unsigned turns=0;
 while(clockUS.load()<end-2000) {
  assert(d.awTimeline.open&&!d.radio.aborts&&d.timer->us==2000);
  d.next();assert(++turns<8);
 }
 assert(turns>=4); // Several successive turns; a one-shot assertion misses spins.
}
static void grantDelay(bool missing) {
 DeadlineFixture d(true);const auto prep=d.prepare();
 unsigned issued=0;
 d.radio.onReserve=[&]{
  ++issued;d.radio.grant.request(2,d.radio.requested,clockUS.load());
 };
 submit(d);d.radio.modelWorkUS=600;d.turn(prep-500);
 armIs(d,prep+101);d.radio.modelWorkUS=0;d.next();
 // Enter the real early-end/abort/preparation/reserve/pending path.
 unsigned turns=0;
 while(!issued) { assert(d.timer->us>=1000);d.next();assert(++turns<8); }
 assert(issued==1&&d.radio.grant.pending&&!d.radio.grant.granted&&d.awTimeline.ends==1);
 while(clockUS.load()<prep+12000) {
  assert(d.timer->us>=1000&&d.timer->us<=2000);d.next();assert(++turns<24);
  assert(issued==1&&!d.awTimeline.open&&d.awTimeline.ends==1);
 }
 if(missing) {
  for(unsigned i=0;i<48;++i) { assert(d.timer->us>=1000);d.next(); }
  assert(issued==1&&d.radio.grant.pending&&d.awTimeline.starts==1&&d.events.size()==2);
 } else {
  const auto at=clockUS.load();
  assert(d.radio.grant.grant(2,44,150,at));d.serviceInGate();
  assert(d.awTimeline.open&&d.awTimeline.active.channel==44&&d.awTimeline.starts==2&&d.awTimeline.ends==1);
  assert(d.awTimeline.lastStart==at&&d.events.size()==3);
 }
}
static void batches(bool refill) {
 DeadlineFixture d;unsigned before=txCompleted;submit(d,64);
 assert(d.queueCount==64&&d.completionCount==64);
 bool filled=false;
 d.txComplete->onEnqueue=[&]{
  assert(!inSubmit);
  if(refill&&!filled){filled=true;assert(!d.completionCount);submit(d,64);}
 };
 d.turn(B+1000);
 assert(d.dataTxSubmitted==(refill?128u:64u)&&!d.queueCount);
 assert(d.completionCount==(refill?64u:0u)&&txCompleted==before+64);
 // Deliberately unchanged shared-writer behavior: credit arbitration is NOT
 // this correction; refill data still drains in this same service invocation.
 armIs(d,B+3000);d.next();d.next();
 assert(!d.completionCount&&txCompleted==before+(refill?128u:64u));
 d.txComplete->onEnqueue=nullptr;
}
static void pressure() {
 DeadlineFixture d;submit(d,64);
 const auto slot=(d.queueHead+1)%d.kDataQueueSize;
 auto original=d.queue[slot];const auto bytes=original->bytes;
 const auto queued=d.queueTime[slot],expiry=d.queueDeadline[slot];const auto sequence=d.state.dataCounter;
 d.radio.modelCapacity=1;d.turn(B+1000);
 assert(d.dataTxSubmitted==1&&d.queueCount==63&&d.dataTxDeferred==1);
 assert(d.queue[d.queueHead]==original&&original->bytes==bytes);
 assert(d.queueTime[d.queueHead]==queued&&d.queueDeadline[d.queueHead]==expiry&&d.state.dataCounter==sequence);
 d.radio.modelCapacity=1000;d.next();
 assert(d.dataTxSubmitted==64&&!d.queueCount&&d.radio.admitted[1]==bytes&&d.state.dataCounter==sequence);
}
static void resetHal(DeadlineFixture &d) {
 d.radio.on=false;d.radio.storage.sc_awdl_enabled=false;d.radio.storage.sc_awdl_native_session=0;
 d.radio.storage.sc_resetting=true;d.radio.grant.cancel();
}
static void lifecycle(const std::string &kind) {
 DeadlineFixture d;submit(d);d.radio.tryStatus=kIOReturnNotFound;
 const auto old=d.txSession;unsigned calls=0;uint64_t replacement=0;bool entered=false;
 d.radio.onPeer=[&](bool add){
  assert(add&&!entered);entered=true;
  int event=0;
  gate.sleepHook=[&](void *p,uint64_t,unsigned){
   assert(p==&event&&!gateDepth&&!heldHalLocks);
   gate.enter([&]{
    d.radio.modelPeerResult=kIOReturnNotReady; // Firmware wait fails, no invented success.
    if(kind=="stop")d.shutdown();
    else if(kind=="reenable") {
     // No live grant during disable, so administrative cancellation is immediate.
     d.radio.grant.cancel();assert(d.setSync(false)==0&&d.setSync(true)==0);
     replacement=d.txSession;assert(replacement&&replacement!=old);
     d.radio.give(9,44,200);d.requestedChannel=44;d.timer->setTimeoutMS(37);
    } else resetHal(d);
    calls=d.timer->calls;return 0;
   });return THREAD_AWAKENED;
  };
  gate.commandSleep(&event,clockUS.load()+1000,THREAD_UNINT);gate.sleepHook=nullptr;
 };
 d.turn(B+1000);d.radio.onPeer=nullptr;
 std::printf("lifecycle %s: pending=%u interval=%u arm_calls=%u callback_calls=%u queued=%u\n",
  kind.c_str(),unsigned(d.timer->pending),d.timer->us,d.timer->calls,calls,d.queueCount);
 if(kind=="stop")assert(d.stopping&&!d.timer->pending&&d.timer->calls==calls&&!d.queueCount);
 else if(kind=="reenable") {
  assert(d.enabled&&d.txSession==replacement&&d.requestedChannel==44&&d.radio.grant.token==9);
  assert(d.timer->us==37000&&d.timer->calls==calls&&!d.queueCount);
 } else {
  assert(d.enabled&&d.timer->pending&&d.timer->us==1000&&!d.queueCount);
  if(kind=="reset-replacement")d.onEnd=[&]{
   d.radio.storage.sc_resetting=false;assert(d.setSync(true)==0);replacement=d.txSession;
   d.radio.give(10,6,200);d.serviceInGate();assert(d.awTimeline.open&&d.state.controlsHeld);
   d.timer->setTimeoutMS(43);calls=d.timer->calls;
  };
  // Deliver cleanup late at the next EAW: the real Timeline correctly refuses
  // to re-emit the already-started EAW, even if sync is enabled again earlier.
  if(kind=="reset-replacement")d.turn(d.awTimeline.active.end);else d.next();
  if(kind=="reset-replacement") {
   assert(d.enabled&&d.txSession==replacement&&replacement!=old&&d.awTimeline.open);
   assert(d.awTimeline.token==10&&d.state.controlsHeld&&d.timer->us==43000&&d.timer->calls==calls);
   d.onEnd=nullptr;
  } else assert(!d.enabled&&!d.timer->pending&&!d.awTimeline.open&&d.awTimeline.ends==1);
 }
 assert(entered&&!modelMbufs);
}
static void callbackReplacement() {
 DeadlineFixture d;const auto edge=d.edge(),old=d.txSession;unsigned calls=0;
 bool replaced=false;uint64_t replacement=0;
 d.onEnd=[&]{
  assert(!replaced);replaced=true;
  assert(d.setSync(false)==0&&d.setSync(true)==0);replacement=d.txSession;
  d.radio.give(11,6,200);d.serviceInGate();assert(d.awTimeline.open&&d.state.controlsHeld);
  d.timer->setTimeoutMS(47);calls=d.timer->calls;
 };
 d.turn(edge);
 assert(replaced&&replacement!=old&&d.txSession==replacement&&d.awTimeline.token==11);
 assert(d.timer->us==47000&&d.timer->calls==calls&&d.state.controlsHeld);d.onEnd=nullptr;
}
static void peerGate() {
 DeadlineFixture d;
 // No advertised peer is the real gate-2 unknown-peer path, not gate bypass.
 assert(d.state.find(destination)<0&&awdlPeerGateMode()==2);submit(d);d.turn(B+1000);
 assert(d.dataTxSubmitted==1&&!d.queueCount);
 // Advertised peer with unknown schedule is also permitted by gate 2.
 auto &p=d.state.peers[0];p.used=true;memcpy(p.mac,destination,6);
 p.seen=clockUS.load();p.advertisement.sequence.count=0;
 assert(d.state.find(destination)==0&&!peerScheduleKnown(p,clockUS.load()));
 submit(d);d.next();assert(d.dataTxSubmitted==2&&d.dataTxGateUnknown==1&&!d.queueCount);
 // Known schedules retain bounded slack rather than becoming unconditional.
 d.state.nativeElectionManaged=true;p.advertisement.sequence.count=16;p.advertisement.sequence.step=0;
 for(unsigned i=0;i<16;++i)p.advertisement.sequence.channels[i]=44;
 p.advertisement.sequence.channels[1]=6;p.baseCounter=0;
 p.epoch=clockUS.load()-(16384-5000);p.seen=clockUS.load();
 assert(p.channel(clockUS.load())==44&&peerScheduleKnown(p,clockUS.load()));
 assert(p.channel(clockUS.load()+8000)==6);submit(d);d.next();
 assert(d.dataTxSubmitted==3&&!d.queueCount);
 p.epoch=p.seen=clockUS.load();submit(d);
 auto queued=d.queue[d.queueHead];const auto expiry=d.queueDeadline[d.queueHead];
 d.next();assert(d.dataTxSubmitted==3&&d.queueCount==1&&d.dataTxNotReady==1);
 assert(d.queue[d.queueHead]==queued&&d.queueDeadline[d.queueHead]==expiry);
}
int main(int argc,char **argv) {
 std::setvbuf(stdout,nullptr,_IONBF,0);assert(argc==2&&mtkawdl::layoutMatches());
 assert(mwx_awdl_native_history_init());resetFaults();
 bootArguments["amtk_awdl_amsdu"]=0;bootArguments["amtk_awdl_renew_station"]=1;
 const std::string name=argv[1];
 if(name=="future-zero")radioDeadline(0);else if(name=="future-work")radioDeadline(200);
 else if(name=="crossed-radio")radioDeadline(600);else if(name=="radio-due")radioDue();
 else if(name=="cap-work")cap();else if(name=="future-prepare")preparation(200);
 else if(name=="crossed-prepare")preparation(600);else if(name=="prepare-due")preparation(0,true);
 else if(name=="new-aw-due-prepare")newlyOpenedPreparation();
 else if(name=="invalid-preparation")invalidPreparation();else if(name=="preserved-preparation")preservedPreparation();
 else if(name=="delayed-grant")grantDelay(false);else if(name=="missing-grant")grantDelay(true);
 else if(name=="large-uptime"){radioDeadline(200,(uint64_t(1)<<48)+B);radioDeadline(600,(uint64_t(1)<<48)+B);}
 else if(name=="native64")batches(false);else if(name=="refill")batches(true);else if(name=="pressure")pressure();
 else if(name=="callback-replacement")callbackReplacement();else if(name=="peer-gate")peerGate();
 else if(name=="stop"||name=="reenable"||name=="hal-reset"||name=="reset-replacement")lifecycle(name);
 else assert(false);
 noResources();assert(!modelMbufs&&!gateDepth&&!heldHalLocks&&!inSubmit);
 std::printf("PASS %s: actual service/lifecycle and Rust FFI; explicit host boundaries only\n",name.c_str());
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--diagnostics', type=Path, help='new directory below build/diagnostics')
    parser.add_argument('--archive-provenance', type=Path, help='reuse a source/hash-verified fresh Wi-Fi archive')
    parser.add_argument('--source-cpp', type=Path, help='negative control: saved pre-edit production .cpp, unchanged')
    parser.add_argument('--cases', nargs='+', choices=CASES, default=CASES)
    args = parser.parse_args()
    base = ROOT / 'build/diagnostics'
    if args.diagnostics:
        out = args.diagnostics.resolve()
        if not out.is_relative_to(base.resolve()):
            parser.error('diagnostics must be below build/diagnostics')
        out.mkdir(parents=True, exist_ok=False)
    else:
        out = Path(tempfile.mkdtemp(prefix='awdl-service-deadlines-', dir=base))
    print('Evidence: ' + str(out), flush=True)
    before = L.protected_hashes()
    before[str(Path(__file__).relative_to(ROOT))] = L.digest(Path(__file__).read_bytes())
    run = L.CommandLog(out, L.host_environment())
    result = {'scope': 'Host-only exact service/lifecycle, real Rust FFI and HAL grant accessors; no RF/native-runtime/throughput proof',
              'cases': {}, 'diagnostics': str(out)}
    try:
        if args.archive_provenance:
            fresh = json.loads(args.archive_provenance.read_text())
            assert fresh['mode'] == 'fresh-offline-host'
            assert fresh['rust_sources'] == L.rust_input_hashes(), 'Rust archive inputs changed'
            assert fresh['sha256'] == L.digest(Path(fresh['archive']).read_bytes()), 'Rust archive bytes changed'
            result['archive_reused_from'] = str(args.archive_provenance.resolve())
        else:
            fresh = L.build_fresh_archive(out, run)
        (out / 'archive-provenance.json').write_text(json.dumps(fresh, indent=2)+'\n')
        # Bind SDK selection without changing xcode-select or machine settings.
        if 'DEVELOPER_DIR' not in run.env and 'CommandLineTools' in run.checked('xcode-select-compiler', ['xcode-select', '-p']):
            run.env['DEVELOPER_DIR'] = '/Applications/Xcode.app/Contents/Developer'
        sdk = run.checked('sdk-path', ['xcrun', '--sdk', 'macosx', '--show-sdk-path']).strip()
        ex = L.Extractor()
        if args.source_cpp:
            snapshot = args.source_cpp.resolve()
            ex.cache[L.INTERFACE] = snapshot.read_text()
            result['source_control'] = {'path': str(snapshot), 'sha256': L.digest(snapshot.read_bytes())}
        generated = L.generate(ex)
        (out / 'extracted.cpp').write_text(boundaries(generated, ex))
        (out / 'main.cpp').write_text(MAIN)
        (out / 'extraction.json').write_text(json.dumps({'inventory': ex.inventory,
            'source_sha256': {str(p): L.digest(t.encode()) for p, t in ex.cache.items()}}, indent=2)+'\n')
        flags = ['/usr/bin/clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                 '-Wno-unused-function', '-Wno-unused-parameter', '-fno-omit-frame-pointer',
                 '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-pthread',
                 '-isysroot', sdk, '-I'+str(L.VENDOR / 'include')]
        result['compile'] = run('compile', flags+[str(out/'main.cpp'), fresh['archive'], '-o', str(out/'deadlines')])
        if not result['compile']:
            for case in args.cases:
                result['cases'][case] = run(case, [str(out/'deadlines'), case])
                print(run.output(case), end='', flush=True)
    except Exception as error:
        result['error'] = repr(error)
    after = L.protected_hashes()
    after[str(Path(__file__).relative_to(ROOT))] = L.digest(Path(__file__).read_bytes())
    result['source_changes_during_test'] = sorted(k for k in before.keys() | after.keys() if before.get(k) != after.get(k))
    result['passed'] = (not result.get('error') and result.get('compile') == 0 and
                        len(result['cases']) == len(args.cases) and not any(result['cases'].values()) and
                        not result['source_changes_during_test'])
    (out/'results.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2), flush=True)
    return int(not result['passed'])


if __name__ == '__main__':
    raise SystemExit(main())
