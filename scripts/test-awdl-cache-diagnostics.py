#!/usr/bin/env python3
"""Exercise extracted production cache/diagnostic paths with the real Rust FFI.

The recursive controller gate, firmware status/commandSleep boundary, copied
controller messages and superclass dispatch below are explicit host models.
They do not establish private IO80211 ownership, queueing, copying, delivery or
reentrancy, nor hardware behavior. ASan/UBSan instrument the C++ fixture, not
Rust's normal host build. Every run builds offline with the installed pinned
nightly and retains commands, full logs, statuses and input fingerprints in a
new build/diagnostics directory. MTK_AWDL_CACHE_HOST_TARGET overrides the normal
build/rust-host target directory; --require-rebuild verifies a fresh Rust build.
"""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
base = root / 'kext/Wifi/vendor/AirportMtkWlan/AirportMtkAwdlInterface'
signatures = (
    'void AirportMtkAwdlInterface::invalidateDataRxAdmission(',
    'void AirportMtkAwdlInterface::resetDataPeers(',
    'int AirportMtkAwdlInterface::observeDataPeer(',
    'bool AirportMtkAwdlInterface::cacheDataPeer(',
    'bool AirportMtkAwdlInterface::trafficDataPeer(',
    'IOReturn AirportMtkAwdlInterface::programDataPeer(',
    'IOReturn AirportMtkAwdlInterface::reportDataPathEvents(',
    'IOReturn AirportMtkAwdlInterface::recordCacheControlResult(',
    'IOReturn AirportMtkAwdlInterface::setPEER_CACHE_CONTROL(',
    'IOReturn AirportMtkAwdlInterface::peerCacheGated(',
    'IOReturn AirportMtkAwdlInterface::setPeerCacheInGate(',
)


def fixture(source, header):
    # Whole production definitions, including the public wrapper and its real
    # callback/transaction chain. Only external kernel/native boundaries are
    # modeled; no test-only replacement for a production helper is linked.
    methods, declarations = [], []
    for signature in signatures:
        assert source.count(signature) == 1, signature
        start = source.index(signature)
        method = source[start:source.index('\n}\n', start) + 3]
        methods.append(method)
        declaration = method[:method.index('\n{')].replace('AirportMtkAwdlInterface::', '', 1)
        if re.search(r'\w+Gated\(', declaration):
            declaration = 'static ' + declaration
        declarations.append(declaration + ';')
    start = header.index('    struct CacheDiagnostic {')
    diagnostics = header[start:header.index('    CacheDiagnostic cacheDiagnostic;', start)
                         + len('    CacheDiagnostic cacheDiagnostic;')]
    start = header.index('    mtkawdl::DataPeers dataPeers;')
    admission = header[start:header.index('    void invalidateDataRxAdmission();', start)]
    return r'''
#include <AWDL/AwdlLayout.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>
#define CHECK_MSG(condition, message) do { if(!(condition)) { \
    std::fprintf(stderr,"CHECK failed: %s [%s:%d, %s]: %s\n",message,__FILE__,__LINE__,__func__,#condition); \
    std::exit(1); } } while(0)
#define CHECK(condition) CHECK_MSG(condition, #condition)
using IOReturn = unsigned;
constexpr unsigned APPLE80211_VERSION=1, kIOReturnSuccess=0,
    kIOReturnNotReady=0xe00002d8, kIOReturnBadArgument=0xe00002c2,
    kIOReturnUnsupported=0xe00002c7, kIOReturnNoResources=0xe00002be,
    kIOReturnIOError=0xe00002ca, THREAD_UNINT=0;
static uint64_t now=1000000;
uint64_t awdlNow() { return now; }
using Mac=std::array<uint8_t,6>;
using Message=std::array<uint8_t,0x1d0>;
const Mac peer{{2,1,2,3,4,5}}, other{{2,6,7,8,9,10}};
struct apple80211_peer_cache_control { uint8_t bytes[0x1d0]{}; };
static uint64_t load64(const uint8_t *p) { uint64_t value; memcpy(&value,p,8); return value; }
static apple80211_peer_cache_control requestFor(const Mac &mac,uint32_t op=0,uint32_t version=0) {
    apple80211_peer_cache_control request;
    mtkawdl::put32(request.bytes,version); mtkawdl::put32(request.bytes+4,op);
    memcpy(request.bytes+8,mac.data(),6); request.bytes[14]=0x34; request.bytes[15]=0x12;
    return request;
}
struct OSObject {
    int retains=0; unsigned retainCalls=0,releaseCalls=0,maxRetains=0;
    void retain() { ++retainCalls; ++retains; maxRetains=std::max(maxRetains,unsigned(retains)); }
    void release() { CHECK(retains>0); ++releaseCalls; --retains; }
};
struct WorkLoop { unsigned depth=0; bool inGate() const { return depth!=0; } };
struct GateScope {
    WorkLoop &loop;
    explicit GateScope(WorkLoop &value):loop(value) { ++loop.depth; }
    ~GateScope() { CHECK(loop.depth>0); --loop.depth; }
};
struct IOCommandGate {
    using Action=IOReturn(*)(OSObject*,void*,void*,void*,void*);
    WorkLoop loop; OSObject owner;
    unsigned calls=0,callbacks=0,rejections=0,maxDepth=0,sleeps=0;
    IOReturn nextFailure=kIOReturnSuccess;
    Action lastAction=nullptr; void *arguments[4]{};
    std::function<void()> onEntry,sleepCallback;
    WorkLoop *getWorkLoop() { return &loop; }
    IOReturn runAction(Action action,void *a=nullptr,void *b=nullptr,void *c=nullptr,void *d=nullptr) {
        ++calls; lastAction=action;
        arguments[0]=a; arguments[1]=b; arguments[2]=c; arguments[3]=d;
        if(nextFailure) { auto result=nextFailure; nextFailure=0; ++rejections; return result; }
        GateScope scope(loop); maxDepth=std::max(maxDepth,loop.depth); ++callbacks;
        auto hook=std::move(onEntry); onEntry=nullptr; if(hook) hook();
        return action(&owner,a,b,c,d);
    }
    IOReturn commandSleep(void *event,uint64_t wake,unsigned options) {
        CHECK(event && options==THREAD_UNINT && loop.inGate());
        const unsigned depth=loop.depth; ++sleeps;
        // Release ALL recursive entries, run an explicitly injected runnable
        // callback, then reacquire the previous depth. This is not a claim
        // about which private native callbacks actually run during a wait.
        loop.depth=0; now=std::max(now,wake);
        auto hook=std::move(sleepCallback); sleepCallback=nullptr; if(hook) hook();
        CHECK(!loop.inGate()); loop.depth=depth; return kIOReturnSuccess;
    }
};
IOCommandGate *_fCommandGate=nullptr;
struct AirportMtkAwdlInterface;
struct FirmwareCommand { Mac address; bool add; IOReturn status; };
struct FirmwareBoundary {
    AirportMtkAwdlInterface *interface;
    explicit FirmwareBoundary(AirportMtkAwdlInterface *value):interface(value) {}
    unsigned calls=0,busyRejections=0,maxProgramming=0;
    bool lastAdd=false,busy=false; IOReturn result=kIOReturnSuccess;
    std::function<void()> onCall,beforeWait,duringWait,afterWait;
    std::vector<FirmwareCommand> commands;
    IOReturn awdlPeer(const uint8_t*,bool);
};
struct NativeInterface : OSObject {
    unsigned calls=0,message=0; void *data=nullptr; unsigned long length=0; bool sync=false;
    IOReturn result=kIOReturnSuccess;
    std::function<void()> onReport;
    IOReturn reportDataPathEvents(unsigned m,void *p,unsigned long n,bool s) {
        ++calls; message=m; data=p; length=n; sync=s;
        auto hook=std::move(onReport); onReport=nullptr; if(hook) hook(); return result;
    }
};
#define super NativeInterface
struct AirportMtkAwdlInterface : NativeInterface {
''' + diagnostics + '\n' + admission + '\n' + '\n'.join(declarations) + r'''
    uint8_t discoveryTxPeers[3][6]{};
    bool stopping=false,enabled=true;
    void *owner=this;
    FirmwareBoundary radio{this}; FirmwareBoundary *hal=&radio;
};
IOReturn FirmwareBoundary::awdlPeer(const uint8_t *address,bool add) {
    CHECK(_fCommandGate && _fCommandGate->getWorkLoop()->inGate());
    CHECK(interface->dataRxPeerProgramming>0);
    ++calls; lastAdd=add;
    maxProgramming=std::max(maxProgramming,interface->dataRxPeerProgramming);
    Mac submitted; memcpy(submitted.data(),address,6);
    // MtkMwx's MwxAwdlOperation rejects an overlapping station operation
    // with NotReady (sc_awdl_busy). Model that boundary outcome, not a second
    // concurrent successful firmware transaction or the full HAL implementation.
    const IOReturn status=busy ? kIOReturnNotReady : result;
    commands.push_back({submitted,add,status});
    auto hook=std::move(onCall); onCall=nullptr; if(hook) hook();
    if(busy) { ++busyRejections; return status; }
    busy=true;
    hook=std::move(beforeWait); beforeWait=nullptr; if(hook) hook();
    CHECK(!_fCommandGate->sleepCallback);
    _fCommandGate->sleepCallback=std::move(duringWait); duringWait=nullptr;
    _fCommandGate->commandSleep(this,now+20000,THREAD_UNINT);
    CHECK(_fCommandGate->getWorkLoop()->inGate());
    hook=std::move(afterWait); afterWait=nullptr; if(hook) hook();
    CHECK_MSG(!memcmp(address,submitted.data(),6),"programDataPeer stable address copy"); busy=false; return status;
}
static unsigned posts=0;
static IOReturn postResult=kIOReturnSuccess;
static Message savedMessage;
struct PostedMessage { Message bytes; unsigned programming; uint64_t generation; };
static std::vector<PostedMessage> messages;
IOReturn awdlControllerMessage(void *owner,AirportMtkAwdlInterface *self,unsigned message,
                              void *data,unsigned long length,bool sync) {
    CHECK(owner==self->owner && self->retains>0);
    CHECK(_fCommandGate && _fCommandGate->getWorkLoop()->inGate());
    CHECK(message==0x33 && data && length==savedMessage.size() && sync);
    ++posts; memcpy(savedMessage.data(),data,length);
    // This boundary model deliberately copies. Delayed observation tests the
    // extracted observer's forwarding, NOT native postMessage's copy/queue ABI.
    messages.push_back({savedMessage,self->dataRxPeerProgramming,self->dataRxAdmissionGeneration});
    return postResult;
}
''' + '\n'.join(methods) + r'''
struct Fixture {
    IOCommandGate gate; AirportMtkAwdlInterface d;
    Fixture() {
        CHECK(!_fCommandGate); _fCommandGate=&gate; now=1000000;
        posts=0; postResult=0; savedMessage.fill(0); messages.clear();
    }
    ~Fixture() {
        CHECK(!gate.loop.inGate() && !gate.sleepCallback && !d.radio.busy);
        CHECK(!d.retains && d.retainCalls==d.releaseCalls && !d.dataRxPeerProgramming);
        CHECK(_fCommandGate==&gate); _fCommandGate=nullptr;
    }
    void gated(std::function<void()> callback) {
        CHECK(gate.runAction([](OSObject*,void *arg,void*,void*,void*) -> IOReturn {
            (*static_cast<std::function<void()> *>(arg))(); return kIOReturnSuccess;
        },&callback)==kIOReturnSuccess);
    }
};
static void completionIs(const Message &message,const Mac &mac,unsigned operation) {
    CHECK(mtkawdl::le32(message.data())==APPLE80211_VERSION);
    CHECK(mtkawdl::le32(message.data()+4)==operation && !memcmp(message.data()+8,mac.data(),6));
    for(size_t i=14;i<message.size();++i) CHECK(!message[i]);
}
static void test_public_wrapper() {
    Fixture f; auto &d=f.d; auto &gate=f.gate; auto request=requestFor(peer);
    const auto generation=d.dataRxAdmissionGeneration;
    _fCommandGate=nullptr;
    CHECK(d.setPEER_CACHE_CONTROL(&request)==kIOReturnNotReady);
    CHECK(d.cacheDiagnostic.requests==1 && d.cacheDiagnostic.failures==1);
    CHECK(d.cacheDiagnostic.requestTimeUS==now && !d.cacheDiagnostic.requestVersionOp && !d.cacheDiagnostic.requestAddressTag);
    CHECK(d.cacheDiagnostic.result==kIOReturnNotReady && !d.cacheDiagnostic.durationUS && !d.cacheDiagnostic.maxDurationUS);
    CHECK(!gate.calls && !d.radio.calls && !posts && !d.retains);
    CHECK(d.retainCalls==1 && d.releaseCalls==1 && d.dataRxAdmissionGeneration==generation);
    _fCommandGate=&gate;
    const auto beforeCalls=gate.calls, beforeCallbacks=gate.callbacks;
    const auto start=now; bool forwarded=false;
    gate.onEntry=[&] {
        CHECK(gate.lastAction==AirportMtkAwdlInterface::peerCacheGated);
        CHECK(gate.arguments[0]==&d && !gate.arguments[1] && gate.arguments[2] && !gate.arguments[3]);
        CHECK(*static_cast<uint64_t *>(gate.arguments[2])==start);
        CHECK(gate.loop.depth==1 && d.retains==1); forwarded=true;
    };
    CHECK(d.setPEER_CACHE_CONTROL(nullptr)==kIOReturnNotReady);
    CHECK_MSG(gate.calls==beforeCalls+1 && gate.callbacks==beforeCallbacks+1 && forwarded,
              "public peerCacheGated forwarding");
    CHECK(d.cacheDiagnostic.requests==2 && d.cacheDiagnostic.failures==2 && !d.retains);
    CHECK(d.retainCalls==2 && d.releaseCalls==2 && !d.radio.calls && !posts);
    // A runAction rejection never calls the transaction. Preserve that actual
    // distinction: public request samples update, but result/failure samples
    // remain unchanged because production does not record this gate result.
    gate.nextFailure=kIOReturnIOError;
    CHECK(d.setPEER_CACHE_CONTROL(&request)==kIOReturnIOError);
    CHECK(gate.calls==beforeCalls+2 && gate.callbacks==beforeCallbacks+1 && gate.rejections==1);
    CHECK(d.cacheDiagnostic.requests==3 && d.cacheDiagnostic.failures==2);
    CHECK(d.cacheDiagnostic.result==kIOReturnNotReady && !d.cacheDiagnostic.durationUS);
    CHECK(!d.cacheDiagnostic.requestVersionOp && !d.cacheDiagnostic.requestAddressTag);
    CHECK(d.retainCalls==3 && d.releaseCalls==3 && !d.retains);
    CHECK(d.dataRxAdmissionGeneration==generation && !d.dataRxPeerProgramming && !d.radio.calls && !posts);
    std::puts("PASS public wrapper: missing gate, real callback routing, gate rejection, retain balance");
}
static void test_validation() {
    Fixture f; auto &d=f.d; auto request=requestFor(peer);
    auto reject=[&](IOReturn expected) {
        CHECK(d.setPEER_CACHE_CONTROL(&request)==expected);
        CHECK(d.cacheDiagnostic.result==expected && d.cacheDiagnostic.failures==d.cacheDiagnostic.requests);
        CHECK(!d.radio.calls && !posts && !d.retains && !d.dataRxPeerProgramming);
        CHECK(d.dataRxAdmissionGeneration==1 && !d.cacheDiagnostic.durationUS);
    };
    mtkawdl::put32(request.bytes,APPLE80211_VERSION+1); reject(kIOReturnBadArgument);
    CHECK(d.cacheDiagnostic.requestVersionOp==load64(request.bytes));
    CHECK(d.cacheDiagnostic.requestAddressTag==load64(request.bytes+8));
    request=requestFor(peer); memset(request.bytes+8,0,6); reject(kIOReturnBadArgument);
    request=requestFor(peer); request.bytes[8]|=1; reject(kIOReturnBadArgument);
    request=requestFor(peer,1); reject(kIOReturnUnsupported);
    d.enabled=false; request=requestFor(peer); reject(kIOReturnNotReady);
    CHECK(!d.cacheDiagnostic.requestVersionOp && !d.cacheDiagnostic.requestAddressTag);
    d.enabled=true; d.stopping=true; reject(kIOReturnNotReady);
    CHECK(!d.cacheDiagnostic.requestVersionOp && !d.cacheDiagnostic.requestAddressTag);
    std::puts("PASS validation: version/address/op and readiness reject before firmware or admission mutation");
}
static void test_firmware_diagnostics_and_events() {
    Fixture f; auto &d=f.d; auto request=requestFor(peer);
    CHECK(d.setPEER_CACHE_CONTROL(nullptr)==kIOReturnNotReady);
    d.radio.result=kIOReturnIOError;
    auto generation=d.dataRxAdmissionGeneration;
    d.radio.onCall=[&] {
        CHECK_MSG(d.dataRxAdmissionGeneration==generation+1,"programDataPeer entry invalidation");
        CHECK(d.dataRxPeerProgramming==1 && d.retains==1 && f.gate.loop.inGate());
    };
    d.radio.duringWait=[&] {
        CHECK(!f.gate.loop.inGate() && d.dataRxPeerProgramming==1 && d.retains==1);
        CHECK(d.dataRxAdmissionGeneration==generation+1);
    };
    d.radio.afterWait=[&] { CHECK(d.dataRxAdmissionGeneration==generation+1 && d.dataRxPeerProgramming==1); };
    CHECK(d.setPEER_CACHE_CONTROL(&request)==kIOReturnIOError);
    CHECK(!posts && d.dataPeers.find(peer.data(),now)<0 && !d.retains);
    CHECK(d.cacheDiagnostic.firmwareFailures==1 && d.cacheDiagnostic.failures==2);
    CHECK(d.cacheDiagnostic.durationUS==20000 && d.cacheDiagnostic.firmwareDurationUS==20000);
    CHECK(d.cacheDiagnostic.firmwareCalls==1 && d.cacheDiagnostic.firmwareStatus==kIOReturnIOError);
    CHECK_MSG(d.dataRxAdmissionGeneration==generation+2,"programDataPeer return invalidation");
    CHECK(!d.dataRxPeerProgramming);
    CHECK(d.cacheDiagnostic.requestVersionOp==load64(request.bytes));
    CHECK(d.cacheDiagnostic.requestAddressTag==load64(request.bytes+8));
    d.radio.result=0; generation=d.dataRxAdmissionGeneration;
    CHECK(!d.setPEER_CACHE_CONTROL(&request) && posts==1 && !d.retains);
    CHECK_MSG(d.dataRxAdmissionGeneration==generation+3,"cacheDataPeer commit invalidation");
    CHECK(!d.dataRxPeerProgramming && messages.back().programming==0 && messages.back().generation==generation+3);
    CHECK(d.cacheDiagnostic.posts==1 && !d.cacheDiagnostic.postFailures && !d.cacheDiagnostic.postStatus);
    CHECK(d.cacheDiagnostic.postTimeUS==now && d.cacheDiagnostic.requestTimeUS==now-20000);
    CHECK(d.cacheDiagnostic.requests==3 && d.cacheDiagnostic.failures==2 && !d.cacheDiagnostic.result);
    completionIs(savedMessage,peer,1);
    // The model-owned copy is observed after the setter's completion stack
    // storage has expired. Original arguments and a non-success base result
    // are preserved; this does not test native ownership or async delivery.
    d.result=kIOReturnUnsupported;
    const auto gateCalls=f.gate.calls;
    d.onReport=[&] {
        CHECK(d.cacheDiagnostic.events==1 && d.cacheDiagnostic.eventLength==savedMessage.size());
        CHECK(d.data==savedMessage.data() && !d.sync);
    };
    CHECK(d.reportDataPathEvents(0x33,savedMessage.data(),savedMessage.size(),false)==d.result);
    CHECK(d.data==savedMessage.data() && d.length==savedMessage.size() && !d.sync);
    CHECK(d.cacheDiagnostic.events==1 && !d.cacheDiagnostic.invalidEvents);
    CHECK(d.cacheDiagnostic.eventStatus==d.result && d.cacheDiagnostic.eventTimeUS==now);
    CHECK(d.cacheDiagnostic.eventVersionOp==load64(savedMessage.data()));
    CHECK(d.cacheDiagnostic.eventAddressTag==load64(savedMessage.data()+8));
    const auto count=d.cacheDiagnostic.events, time=d.cacheDiagnostic.eventTimeUS;
    CHECK(d.reportDataPathEvents(0x35,nullptr,0,true)==d.result);
    CHECK(d.cacheDiagnostic.events==count && d.message==0x35 && d.sync);
    CHECK(d.cacheDiagnostic.eventTimeUS==time && d.cacheDiagnostic.eventLength==savedMessage.size());
    // ASan catches an observer read past these short allocations. The mock
    // base receives each original malformed pointer/length without reading it.
    for(unsigned n=0;n<16;++n) {
        auto bytes=new uint8_t[n]{};
        CHECK(d.reportDataPathEvents(0x33,bytes,n,false)==d.result);
        CHECK(d.data==bytes && d.length==n && !d.sync);
        CHECK(!d.cacheDiagnostic.eventVersionOp && !d.cacheDiagnostic.eventAddressTag);
        delete[] bytes;
    }
    CHECK(d.reportDataPathEvents(0x33,nullptr,0x1d0,true)==d.result);
    CHECK(d.cacheDiagnostic.invalidEvents==17 && d.sync && !d.data && d.length==0x1d0);
    CHECK(f.gate.calls==gateCalls && d.cacheDiagnostic.eventStatus==d.result);
    // Submission failure remains observable without synthesizing delivery or
    // changing the pre-existing successful cache transaction return contract.
    postResult=kIOReturnNoResources; mtkawdl::put32(request.bytes+4,6);
    generation=d.dataRxAdmissionGeneration;
    CHECK(!d.setPEER_CACHE_CONTROL(&request));
    CHECK(d.cacheDiagnostic.postFailures==1 && d.cacheDiagnostic.postStatus==postResult);
    CHECK(d.cacheDiagnostic.events==count+17 && d.dataRxAdmissionGeneration==generation+3);
    CHECK(posts==2 && d.cacheDiagnostic.posts==2 && !d.cacheDiagnostic.result);
    completionIs(savedMessage,peer,1); postResult=0;
    f.gated([&] { CHECK(d.trafficDataPeer(peer.data(),true,now)); });
    const auto calls=d.radio.calls; const auto firmwareCalls=d.cacheDiagnostic.firmwareCalls;
    const auto firmwareDuration=d.cacheDiagnostic.firmwareDurationUS;
    memcpy(d.discoveryTxPeers[0],peer.data(),6);
    mtkawdl::put32(request.bytes+4,2); generation=d.dataRxAdmissionGeneration;
    CHECK(!d.setPEER_CACHE_CONTROL(&request));
    CHECK(d.radio.calls==calls && mtkawdl::le32(savedMessage.data()+4)==3);
    int member=d.dataPeers.find(peer.data(),now);
    CHECK(member>=0 && d.dataPeers.entries[member].traffic && !d.dataPeers.entries[member].cached);
    CHECK(!memcmp(d.discoveryTxPeers[0],peer.data(),6));
    CHECK(d.cacheDiagnostic.firmwareCalls==firmwareCalls && d.cacheDiagnostic.firmwareDurationUS==firmwareDuration);
    CHECK(d.dataRxAdmissionGeneration==generation+1 && !d.dataRxPeerProgramming);
    // Readiness change is an injected input, not a modeled native lifecycle or
    // queue teardown. Firmware success after it cannot post a false completion.
    mtkawdl::put32(request.bytes+4,0); const auto before=posts;
    d.radio.duringWait=[&] {
        CHECK(!f.gate.loop.inGate() && d.retains==1 && d.dataRxPeerProgramming==1);
        f.gated([&] { d.invalidateDataRxAdmission(); d.stopping=true; });
    };
    CHECK(d.setPEER_CACHE_CONTROL(&request)==kIOReturnNotReady);
    CHECK(posts==before && !d.retains && !d.dataRxPeerProgramming);
    CHECK(d.cacheDiagnostic.result==kIOReturnNotReady && d.cacheDiagnostic.maxDurationUS==20000);
    CHECK(d.dataPeers.entries[member].traffic && !d.dataPeers.entries[member].cached);
    std::puts("PASS diagnostics: failed firmware, model-copied completion, bounded unchanged forwarding, post failure, traffic owner, teardown");
}
static void test_event_prefix_bounds() {
    Fixture f; auto &d=f.d; d.result=kIOReturnIOError;
    for(unsigned n : {16u,17u,0x1cfu,0x1d0u,0x1d1u}) {
        auto bytes=new uint8_t[n]; memset(bytes,0xa5,n);
        CHECK(d.reportDataPathEvents(0x33,bytes,n,true)==kIOReturnIOError);
        CHECK(d.cacheDiagnostic.eventVersionOp==load64(bytes) && d.cacheDiagnostic.eventAddressTag==load64(bytes+8));
        CHECK(d.data==bytes && d.length==n && d.sync); delete[] bytes;
    }
    CHECK(d.cacheDiagnostic.events==5 && d.cacheDiagnostic.invalidEvents==4 && !f.gate.calls);
    std::puts("PASS event prefix: exactly bounded 16-byte sampling, exact full-size validity, unchanged base result");
}
static void test_remove_and_replay_ownership() {
    Fixture f; auto &d=f.d; auto request=requestFor(peer,0,APPLE80211_VERSION);
    CHECK(!d.setPEER_CACHE_CONTROL(&request));
    const int member=d.dataPeers.find(peer.data(),now); CHECK(member>=0);
    // Seed replay through the real Rust FFI under the test gate; this is not
    // an assertion about native RX admission, allocation or packet ownership.
    f.gated([&] { d.invalidateDataRxAdmission(); CHECK(d.dataPeers.acceptData(member,5,77,now)); });
    const auto sequence=d.dataPeers.entries[member].windows[5].sequence;
    const auto bitmap=d.dataPeers.entries[member].windows[5].bitmap;
    memcpy(d.discoveryTxPeers[0],peer.data(),6); memcpy(d.discoveryTxPeers[1],other.data(),6);
    memcpy(d.discoveryTxPeers[2],peer.data(),6);
    mtkawdl::put32(request.bytes+4,2); d.radio.result=kIOReturnIOError;
    auto generation=d.dataRxAdmissionGeneration; auto before=posts;
    CHECK(d.setPEER_CACHE_CONTROL(&request)==kIOReturnIOError);
    CHECK(posts==before && d.dataPeers.entries[member].cached && d.dataPeers.entries[member].replayState.used);
    CHECK(d.dataPeers.entries[member].windows[5].sequence==sequence && d.dataPeers.entries[member].windows[5].bitmap==bitmap);
    CHECK(!memcmp(d.discoveryTxPeers[0],peer.data(),6) && !memcmp(d.discoveryTxPeers[2],peer.data(),6));
    CHECK(d.dataRxAdmissionGeneration==generation+2 && !d.dataRxPeerProgramming && !d.retains);
    d.radio.result=0; generation=d.dataRxAdmissionGeneration;
    CHECK(!d.setPEER_CACHE_CONTROL(&request));
    CHECK(!d.radio.lastAdd && posts==before+1 && d.dataPeers.find(peer.data(),now)<0);
    CHECK(!d.dataPeers.entries[member].replayState.used && !d.dataPeers.entries[member].windows[5].active);
    const Mac zero{};
    CHECK(!memcmp(d.discoveryTxPeers[0],zero.data(),6) && !memcmp(d.discoveryTxPeers[2],zero.data(),6));
    CHECK(!memcmp(d.discoveryTxPeers[1],other.data(),6));
    CHECK(d.dataRxAdmissionGeneration==generation+3); completionIs(savedMessage,peer,3);
    // An absent peer removal is permitted and still goes through programming.
    const auto calls=d.radio.calls; CHECK(!d.setPEER_CACHE_CONTROL(&request));
    CHECK(d.radio.calls==calls+1 && d.dataPeers.find(peer.data(),now)<0);
    std::puts("PASS removal: firmware failure preserves membership/replay/discovery, success revokes and clears matching discovery slots");
}
static void test_recursive_gate() {
    Fixture f; auto &d=f.d; auto request=requestFor(peer);
    f.gated([&] {
        CHECK(f.gate.loop.depth==1);
        f.gate.onEntry=[&] {
            CHECK(f.gate.lastAction==AirportMtkAwdlInterface::peerCacheGated);
            CHECK(f.gate.arguments[0]==&d && f.gate.arguments[1]==&request && f.gate.loop.depth==2 && d.retains==1);
        };
        d.radio.duringWait=[&] { CHECK(!f.gate.loop.inGate() && d.dataRxPeerProgramming==1); };
        d.radio.afterWait=[&] { CHECK(f.gate.loop.depth==2); };
        CHECK(!d.setPEER_CACHE_CONTROL(&request));
        CHECK(f.gate.loop.depth==1 && !d.retains);
    });
    CHECK(f.gate.calls==2 && f.gate.callbacks==2 && f.gate.maxDepth==2 && f.gate.sleeps==1);
    CHECK(d.dataRxAdmissionGeneration==4 && !d.dataRxPeerProgramming);
    std::puts("PASS recursive boundary gate: actual public wrapper re-enters and commandSleep releases/restores both entries");
}
static void test_nested_cache_during_firmware_wait() {
    Fixture f; auto &d=f.d; auto request=requestFor(peer), nested=requestFor(other,6,APPLE80211_VERSION);
    const auto generation=d.dataRxAdmissionGeneration, start=now;
    d.radio.duringWait=[&] {
        CHECK(!f.gate.loop.inGate() && d.retains==1 && d.dataRxPeerProgramming==1 && d.radio.busy);
        CHECK(d.dataRxAdmissionGeneration==generation+1);
        d.radio.onCall=[&] {
            CHECK(f.gate.loop.depth==1 && d.retains==2 && d.dataRxPeerProgramming==2);
            CHECK(d.dataRxAdmissionGeneration==generation+2);
        };
        CHECK(d.setPEER_CACHE_CONTROL(&nested)==kIOReturnNotReady);
        CHECK(!f.gate.loop.inGate() && d.retains==1 && d.dataRxPeerProgramming==1);
        CHECK(d.dataRxAdmissionGeneration==generation+3 && !posts && d.dataPeers.find(other.data(),now)<0);
        CHECK(d.cacheDiagnostic.firmwareFailures==1 && d.cacheDiagnostic.result==kIOReturnNotReady);
        // Mutate caller-owned storage as a test input. The extracted setter
        // and programming helper already copied the prefix/address they use.
        request=requestFor(other,2,APPLE80211_VERSION);
    };
    CHECK(!d.setPEER_CACHE_CONTROL(&request));
    CHECK(d.radio.calls==2 && d.radio.busyRejections==1 && d.radio.maxProgramming==2 && f.gate.sleeps==1);
    CHECK(d.dataRxAdmissionGeneration==generation+5 && !d.dataRxPeerProgramming && !d.retains);
    CHECK(d.maxRetains==2 && d.retainCalls==2 && d.releaseCalls==2);
    CHECK(posts==1 && d.dataPeers.find(peer.data(),now)>=0 && d.dataPeers.find(other.data(),now)<0);
    completionIs(savedMessage,peer,1);
    CHECK(d.cacheDiagnostic.requests==2 && d.cacheDiagnostic.failures==1 && !d.cacheDiagnostic.result);
    CHECK(d.cacheDiagnostic.durationUS==20000 && d.cacheDiagnostic.maxDurationUS==20000);
    CHECK(d.cacheDiagnostic.firmwareCalls==2 && d.cacheDiagnostic.firmwareFailures==1 && !d.cacheDiagnostic.firmwareStatus);
    CHECK(d.cacheDiagnostic.firmwareDurationUS==20000 && d.cacheDiagnostic.requestTimeUS==start+20000);
    // Diagnostic last fields are independent samples, not an invented atomic
    // transaction snapshot: request prefix stays nested, result is now outer.
    CHECK(d.cacheDiagnostic.requestVersionOp==load64(nested.bytes));
    CHECK(d.cacheDiagnostic.requestAddressTag==load64(nested.bytes+8));
    std::puts("PASS nested cache during commandSleep: busy firmware rejection, real wrapper routing, balanced nested programming/retains, stable copied address");
}
static void test_nested_traffic_owned_removal() {
    Fixture f; auto &d=f.d; auto existing=requestFor(other);
    CHECK(!d.setPEER_CACHE_CONTROL(&existing));
    f.gated([&] { CHECK(d.trafficDataPeer(other.data(),true,now)); });
    auto request=requestFor(peer), nested=requestFor(other,2);
    const auto generation=d.dataRxAdmissionGeneration; const auto calls=d.radio.calls, before=posts;
    d.radio.duringWait=[&] {
        CHECK(!f.gate.loop.inGate() && d.dataRxPeerProgramming==1);
        CHECK(!d.setPEER_CACHE_CONTROL(&nested)); // no overlapping HAL call
        CHECK(d.radio.calls==calls+1 && d.dataRxPeerProgramming==1 && d.retains==1);
        CHECK(!f.gate.loop.inGate() && d.dataRxAdmissionGeneration==generation+2);
        CHECK(posts==before+1 && messages.back().programming==1);
        completionIs(messages.back().bytes,other,3);
    };
    CHECK(!d.setPEER_CACHE_CONTROL(&request));
    int member=d.dataPeers.find(other.data(),now);
    CHECK(member>=0 && d.dataPeers.entries[member].traffic && !d.dataPeers.entries[member].cached);
    CHECK(d.radio.calls==calls+1 && !d.radio.busyRejections && posts==before+2);
    CHECK(d.dataRxAdmissionGeneration==generation+4 && !d.dataRxPeerProgramming && !d.retains);
    CHECK(d.maxRetains==2 && messages.back().programming==0); completionIs(savedMessage,peer,1);
    std::puts("PASS nested traffic-owned cache removal: succeeds during the wait without overlapping firmware or revoking the traffic owner");
}
static void test_capacity_race_and_compensation() {
    Fixture f; auto &d=f.d; std::vector<Mac> observed;
    f.gated([&] {
        for(unsigned i=0;i<mtkawdl::maxPeers-1;++i) {
            Mac mac{{2,0x10,0x20,0x30,0x40,uint8_t(i+1)}}; observed.push_back(mac);
            CHECK(d.observeDataPeer(mac.data(),now)>=0);
        }
    });
    auto request=requestFor(peer); const auto generation=d.dataRxAdmissionGeneration;
    CHECK(d.dataPeers.canObserve(peer.data(),now));
    d.radio.duringWait=[&] {
        CHECK(!f.gate.loop.inGate() && d.dataRxPeerProgramming==1);
        // Actual advertisement observation can fill the final Rust table slot
        // without a second firmware station command (HAL only has three slots).
        f.gated([&] { CHECK(d.observeDataPeer(other.data(),now)>=0); });
        CHECK(!d.dataPeers.canObserve(peer.data(),now));
    };
    CHECK(d.setPEER_CACHE_CONTROL(&request)==kIOReturnNoResources);
    CHECK(!posts && !d.retains && !d.dataRxPeerProgramming && d.dataPeers.find(peer.data(),now)<0);
    CHECK(d.radio.commands.size()==2 && d.radio.commands[0].add && !d.radio.commands[1].add);
    CHECK(d.radio.commands[0].address==peer && d.radio.commands[1].address==peer && f.gate.sleeps==2);
    CHECK(d.dataRxAdmissionGeneration==generation+6);
    for(const auto &mac:observed) CHECK(d.dataPeers.find(mac.data(),now)>=0);
    CHECK(d.dataPeers.find(other.data(),now)>=0);
    CHECK(d.cacheDiagnostic.requests==1 && d.cacheDiagnostic.failures==1 && d.cacheDiagnostic.result==kIOReturnNoResources);
    // Production counts the requested firmware transaction, not its cleanup.
    CHECK(d.cacheDiagnostic.firmwareCalls==1 && !d.cacheDiagnostic.firmwareFailures && !d.cacheDiagnostic.firmwareStatus);
    CHECK(d.cacheDiagnostic.firmwareDurationUS==20000 && d.cacheDiagnostic.durationUS==40000 && d.cacheDiagnostic.maxDurationUS==40000);
    const auto calls=d.radio.calls; const auto unchanged=d.dataRxAdmissionGeneration;
    CHECK(d.setPEER_CACHE_CONTROL(&request)==kIOReturnNoResources);
    CHECK(d.radio.calls==calls && d.dataRxAdmissionGeneration==unchanged && !posts);
    // Removal doesn't require a free slot, including an absent peer.
    mtkawdl::put32(request.bytes+4,2); CHECK(!d.setPEER_CACHE_CONTROL(&request));
    CHECK(d.radio.calls==calls+1 && posts==1); completionIs(savedMessage,peer,3);
    CHECK(d.cacheDiagnostic.maxDurationUS==40000 && d.cacheDiagnostic.durationUS==20000);
    std::puts("PASS capacity: preflight is not a reservation, wait-time observation invalidates, failed cache commit compensates and cannot post success");
}
static void test_disabled_during_wait_and_missing_hal() {
    for(IOReturn firmware : {kIOReturnSuccess,kIOReturnIOError}) {
        Fixture f; auto &d=f.d; auto request=requestFor(peer);
        d.radio.result=firmware;
        d.radio.duringWait=[&] {
            CHECK(!f.gate.loop.inGate());
            f.gated([&] { d.invalidateDataRxAdmission(); d.enabled=false; });
        };
        const auto expected=firmware ? firmware : kIOReturnNotReady;
        CHECK(d.setPEER_CACHE_CONTROL(&request)==expected);
        CHECK(!posts && !d.retains && !d.dataRxPeerProgramming && d.dataPeers.find(peer.data(),now)<0);
        CHECK(d.cacheDiagnostic.result==expected && d.cacheDiagnostic.firmwareStatus==firmware);
        CHECK(d.cacheDiagnostic.firmwareFailures==(firmware ? 1u : 0u) && d.cacheDiagnostic.failures==1);
        CHECK(d.dataRxAdmissionGeneration==4 && d.cacheDiagnostic.maxDurationUS==20000);
    }
    Fixture f; auto &d=f.d; auto request=requestFor(peer); d.hal=nullptr;
    CHECK(d.setPEER_CACHE_CONTROL(&request)==kIOReturnNotReady);
    CHECK(!d.radio.calls && !f.gate.sleeps && !posts && !d.retains && !d.dataRxPeerProgramming);
    CHECK(d.dataRxAdmissionGeneration==3 && d.cacheDiagnostic.firmwareCalls==1 && d.cacheDiagnostic.firmwareFailures==1);
    CHECK(d.cacheDiagnostic.firmwareStatus==kIOReturnNotReady && !d.cacheDiagnostic.firmwareDurationUS);
    std::puts("PASS readiness/absent HAL: no false completion, genuine firmware errors preserved, programming and retains balanced");
}
static void test_traffic_admission_invalidation() {
    Fixture f; auto &d=f.d;
    f.gated([&] {
        auto generation=d.dataRxAdmissionGeneration;
        CHECK(d.trafficDataPeer(peer.data(),true,now));
        CHECK_MSG(d.dataRxAdmissionGeneration==generation+1,"trafficDataPeer ownership invalidation");
        int member=d.dataPeers.find(peer.data(),now);
        CHECK(member>=0 && d.dataPeers.entries[member].traffic);
        generation=d.dataRxAdmissionGeneration;
        CHECK(d.trafficDataPeer(peer.data(),true,now));
        CHECK_MSG(d.dataRxAdmissionGeneration==generation+1,"trafficDataPeer refresh invalidation");
        generation=d.dataRxAdmissionGeneration;
        CHECK(d.trafficDataPeer(peer.data(),false,now));
        CHECK_MSG(d.dataRxAdmissionGeneration==generation+1,"trafficDataPeer removal invalidation");
        CHECK(d.dataPeers.find(peer.data(),now)<0);
    });
    CHECK(!d.radio.calls && !posts);
    std::puts("PASS traffic admission: ownership acquisition, refresh and removal each invalidate the candidate generation");
}
static void test_program_address_copy() {
    Fixture f; auto &d=f.d; Mac callerAddress=peer;
    const auto generation=d.dataRxAdmissionGeneration;
    d.radio.duringWait=[&] {
        CHECK(!f.gate.loop.inGate() && d.dataRxPeerProgramming==1);
        callerAddress=other;
    };
    f.gated([&] { CHECK(d.programDataPeer(callerAddress.data(),true)==kIOReturnSuccess); });
    CHECK(d.radio.commands.size()==1 && d.radio.commands[0].address==peer);
    CHECK(callerAddress==other && d.dataRxAdmissionGeneration==generation+2);
    CHECK(!d.dataRxPeerProgramming && !posts && !d.retains && f.gate.sleeps==1);
    std::puts("PASS programming address: direct helper preserves its copy across caller-storage mutation during commandSleep");
}
static void test_admission_metadata_exhaustion() {
    Fixture f; auto &d=f.d;
    f.gated([&] {
        auto generation=d.dataRxAdmissionGeneration;
        CHECK(d.cacheDataPeer(peer.data(),true,now)); CHECK(d.dataRxAdmissionGeneration==generation+1);
        generation=d.dataRxAdmissionGeneration;
        CHECK(d.cacheDataPeer(peer.data(),true,now)); CHECK(d.dataRxAdmissionGeneration==generation+1);
        const Mac zero{}; generation=d.dataRxAdmissionGeneration;
        CHECK(!d.cacheDataPeer(zero.data(),true,now)); CHECK(d.dataRxAdmissionGeneration==generation+1);
        generation=d.dataRxAdmissionGeneration; d.resetDataPeers();
        CHECK(d.dataRxAdmissionGeneration==generation+1 && d.dataPeers.find(peer.data(),now)<0);
        d.dataRxAdmissionGeneration=UINT64_MAX-1; d.invalidateDataRxAdmission();
        CHECK(d.dataRxAdmissionGeneration==UINT64_MAX); d.invalidateDataRxAdmission();
        CHECK(!d.dataRxAdmissionGeneration); d.invalidateDataRxAdmission(); CHECK(!d.dataRxAdmissionGeneration);
        CHECK(d.cacheDataPeer(peer.data(),true,now)); CHECK(!d.dataRxAdmissionGeneration);
    });
    // Zero stays a fail-closed admission token, not a claim that cache commands
    // themselves are disabled. Counter exhaustion refuses programming instead
    // of wrapping and cannot authorize or acknowledge a new cache peer.
    auto request=requestFor(other); d.dataRxAdmissionGeneration=7; d.dataRxPeerProgramming=~0u;
    CHECK(d.setPEER_CACHE_CONTROL(&request)==kIOReturnNotReady);
    CHECK(!d.dataRxAdmissionGeneration && d.dataRxPeerProgramming==~0u && !d.radio.calls && !posts && !d.retains);
    CHECK(d.cacheDiagnostic.firmwareCalls==1 && d.cacheDiagnostic.firmwareFailures==1 && d.cacheDiagnostic.failures==1);
    CHECK(d.dataPeers.find(other.data(),now)<0); d.dataRxPeerProgramming=0;
    std::puts("PASS admission metadata: cache refresh/failure/reset invalidation, permanent zero generation, fail-closed programming-counter exhaustion");
}
int main() {
    CHECK(mtkawdl::layoutMatches());
    test_public_wrapper();
    test_validation();
    test_firmware_diagnostics_and_events();
    test_event_prefix_bounds();
    test_remove_and_replay_ownership();
    test_recursive_gate();
    test_nested_cache_during_firmware_wait();
    test_nested_traffic_owned_removal();
    test_capacity_race_and_compensation();
    test_disabled_during_wait_and_missing_hal();
    test_traffic_admission_invalidation();
    test_program_address_copy();
    test_admission_metadata_exhaustion();
    std::puts("PASS AWDL cache: extracted production chain + current Rust FFI; C++ ASan/UBSan; native boundaries remain modeled");
}
'''


class Evidence:
    def __init__(self):
        parent = root / 'build/diagnostics'
        parent.mkdir(parents=True, exist_ok=True)
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
        self.path = Path(tempfile.mkdtemp(prefix='awdl-cache-' + stamp + '-', dir=parent))
        self.commands = []
        self.status = {'result': 'RUNNING', 'argv': [str(Path(__file__).resolve()), *sys.argv[1:]]}
        self.save('status.json', self.status)
        print('AWDL cache evidence: ' + str(self.path), flush=True)

    def save(self, name, value):
        (self.path / name).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')

    def run(self, name, command, *, cwd=root, env=None, check=True):
        command = [str(arg) for arg in command]
        label = f'{len(self.commands) + 1:02d}-{name}'
        stdout, stderr = self.path / (label + '.stdout.log'), self.path / (label + '.stderr.log')
        overrides = {key: env[key] for key in (
            'DEVELOPER_DIR', 'CARGO_NET_OFFLINE', 'RUSTUP_TOOLCHAIN', 'RUSTC',
            'RUSTC_WRAPPER', 'RUSTC_WORKSPACE_WRAPPER', 'ASAN_OPTIONS', 'UBSAN_OPTIONS',
            'RUSTFLAGS', 'CARGO_ENCODED_RUSTFLAGS',
        ) if env is not None and key in env}
        record = {'command': command, 'cwd': str(cwd), 'environment': overrides,
                  'stdout': str(stdout), 'stderr': str(stderr),
                  'started_at': datetime.datetime.now(datetime.timezone.utc).isoformat()}
        print('$ ' + shlex.join(command), flush=True)
        try:
            result = subprocess.run(command, cwd=cwd, env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        except OSError as error:
            stdout.write_text(''); stderr.write_text(str(error) + '\n')
            record['error'] = str(error)
            raise
        else:
            stdout.write_text(result.stdout); stderr.write_text(result.stderr)
            record['returncode'] = result.returncode
            if check and result.returncode:
                print(result.stdout + result.stderr, end='', flush=True)
                raise subprocess.CalledProcessError(result.returncode, command, result.stdout, result.stderr)
            return result
        finally:
            record['finished_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
            self.commands.append(record)
            self.save('commands.json', self.commands)


def input_fingerprints():
    paths = {Path(__file__).resolve(), base.with_suffix('.cpp'), base.with_suffix('.hpp'),
             root / 'kext/Wifi/vendor/SimpleMtkWlan/hal_mwx/MtkMwx.cpp', root / 'VERSION'}
    paths.update((root / 'kext/Wifi/vendor/include/AWDL').rglob('*.hpp'))
    paths.update((root / 'rust').glob('*.toml'))
    paths.add(root / 'rust/Cargo.lock')
    for crate in (root / 'rust').glob('mtk-*'):
        paths.update(crate.rglob('*.rs')); paths.update(crate.rglob('*.toml'))
    for directory in ('kext/Wifi/vendor/tests/awdl', 'kext/Wifi/vendor/tools/awdl'):
        paths.update(path for path in (root / directory).rglob('*') if path.is_file())
    return {str(path.relative_to(root)): {'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                                       'size': path.stat().st_size}
            for path in sorted(paths)}


def check_build_processes(evidence, name):
    result = evidence.run(name, ['ps', '-axo', 'pid=,ppid=,state=,command='])
    conflicts = []
    compilers = {'rustc', 'rustdoc', 'clang', 'clang++', 'cc', 'c++', 'ld', 'ar', 'ranlib',
                 'xcodebuild', 'make', 'gmake', 'ninja'}
    cargo_actions = {'build', 'test', 'check', 'rustc', 'run', 'bench', 'clean', 'fix', 'install'}
    for line in result.stdout.splitlines():
        row = line.strip().split(None, 3)
        if len(row) != 4 or not row[0].isdigit() or int(row[0]) == os.getpid():
            continue
        try:
            argv = shlex.split(row[3])
        except ValueError:
            argv = row[3].split()
        if not argv:
            continue
        executable = Path(argv[0]).name
        building = executable in compilers or (
            executable == 'cargo' and bool(cargo_actions.intersection(argv[1:])))
        building |= executable == 'rustup' and 'run' in argv[1:] and bool(cargo_actions.intersection(argv[1:]))
        # Inspect actual script arguments, never a shell -c string or a Python
        # workflow's embedded task text mentioning cargo/build commands.
        if (executable.startswith('python') or executable in {'sh', 'bash', 'zsh'}) and not any(
                option in argv[1:] for option in ('-c', '-lc', '-xc')):
            for arg in argv[1:]:
                path = Path(arg)
                if path.parent in {root / 'scripts', Path('scripts')} and (
                        path.name in {'build.sh', 'bootstrap.sh', 'check.sh', 'test-awdl.sh'}
                        or path.name.startswith('test-') and path.suffix == '.py'):
                    building = True
        if building:
            conflicts.append({'pid': int(row[0]), 'ppid': int(row[1]), 'state': row[2], 'command': row[3]})
    evidence.save(name + '.json', {'conflicting_build_processes': conflicts})
    if conflicts:
        raise RuntimeError('Conflicting Cargo/compiler/project-build processes; see ' + str(evidence.path / (name + '.json')))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--negative-controls', action='store_true',
                        help='also compile in-memory mutations and require the relevant assertions to fail')
    parser.add_argument('--require-rebuild', action='store_true',
                        help='require Cargo to actually rebuild mtk-ffi and mtk-wlan, e.g. with a new host target')
    args = parser.parse_args()
    evidence = Evidence()
    before = input_fingerprints()
    evidence.save('source-fingerprints.before.json', before)
    try:
        env = os.environ.copy()
        if 'DEVELOPER_DIR' not in env and 'CommandLineTools' in evidence.run(
                'xcode-select', ['xcode-select', '-p']).stdout:
            env['DEVELOPER_DIR'] = '/Applications/Xcode.app/Contents/Developer'
        channel = re.search(r'^channel\s*=\s*"([^"]+)"',
                            (root / 'rust/rust-toolchain.toml').read_text(), re.MULTILINE).group(1)
        installed = evidence.run('installed-toolchains', ['rustup', 'toolchain', 'list'], env=env).stdout
        if not any(line.split()[0] == channel or line.split()[0].startswith(channel + '-')
                   for line in installed.splitlines() if line.split()):
            raise RuntimeError('Pinned toolchain is not installed: ' + channel + ' (will not install/download)')
        env['RUSTUP_TOOLCHAIN'] = channel
        rustc = evidence.run('pinned-rustc-path', ['rustup', 'which', '--toolchain', channel, 'rustc'], env=env).stdout.strip()
        cargo = evidence.run('pinned-cargo-path', ['rustup', 'which', '--toolchain', channel, 'cargo'], env=env).stdout.strip()
        env.update(RUSTC=rustc, RUSTC_WRAPPER='', RUSTC_WORKSPACE_WRAPPER='', CARGO_NET_OFFLINE='true')
        evidence.run('pinned-rustc-version', [rustc, '--version', '--verbose'], env=env)
        evidence.run('pinned-cargo-version', [cargo, '--version', '--verbose'], env=env)
        evidence.run('clang-version', ['/usr/bin/clang++', '--version'], env=env)
        host = Path(os.environ.get('MTK_AWDL_CACHE_HOST_TARGET', root / 'build/rust-host')).expanduser()
        if not host.is_absolute():
            host = root / host
        host = host.resolve()
        evidence.status.update(toolchain=channel, host_target=str(host))
        check_build_processes(evidence, 'before-cargo-processes')
        # Always invoke normal Cargo, even if a static library already exists.
        # JSON artifacts prove Cargo validated the current wifi source graph;
        # --require-rebuild additionally rejects reuse of either relevant crate.
        built = evidence.run('cargo-build', [cargo, 'build', '--offline', '--locked', '-vv',
                             '-p', 'mtk-ffi', '--features', 'wifi', '--target-dir', host,
                             '--message-format=json-render-diagnostics'], cwd=root / 'rust', env=env)
        output = [json.loads(line) for line in built.stdout.splitlines() if line.startswith('{')]
        assert any(row.get('reason') == 'build-finished' and row.get('success') for row in output)
        artifacts = {row['target']['name']: row for row in output if row.get('reason') == 'compiler-artifact'}
        ffi, wlan = artifacts['mtk_ffi'], artifacts['mtk_wlan']
        assert 'wifi' in ffi['features'] and 'bt' not in ffi['features']
        if args.require_rebuild:
            assert not ffi['fresh'] and not wlan['fresh'], 'Cargo reused Rust artifacts instead of rebuilding'
        lib = next(Path(name) for name in ffi['filenames'] if name.endswith('.a'))
        assert lib.is_file() and host in lib.parents
        evidence.save('rust-build-freshness.json', {
            'toolchain': channel, 'rustc': rustc, 'cargo': cargo, 'offline': True,
            'required_rebuild': args.require_rebuild, 'ffi_artifact': ffi, 'wlan_artifact': wlan,
            'library': str(lib), 'library_sha256': hashlib.sha256(lib.read_bytes()).hexdigest(),
            'library_size': lib.stat().st_size,
        })
        assert before == input_fingerprints(), 'Test/production inputs changed during Cargo build'
        source, header = base.with_suffix('.cpp').read_text(), base.with_suffix('.hpp').read_text()
        env['ASAN_OPTIONS'] = 'detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1'
        env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'

        def compile_and_run(name, production, *, check=True):
            cpp, executable = evidence.path / (name + '.cpp'), evidence.path / name
            cpp.write_text(fixture(production, header))
            check_build_processes(evidence, 'before-' + name + '-compile-processes')
            evidence.run(name + '-compile', ['/usr/bin/clang++', '-std=c++14', '-Wall', '-Wextra', '-Werror',
                         '-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined',
                         '-fno-sanitize-recover=all', '-I' + str(root / 'kext/Wifi/vendor/include'),
                         cpp, lib, '-o', executable], env=env)
            result = evidence.run(name + '-run', [executable], env=env, check=check)
            print(result.stdout, end='', flush=True)
            return result

        compile_and_run('cache-positive', source)
        evidence.status['positive'] = 'PASS'
        controls = (
            ('gate-forwarding', '_fCommandGate->runAction(peerCacheGated,this,d,&start)',
             'setPeerCacheInGate(d,start)', 'public peerCacheGated forwarding'),
            ('program-entry-invalidation',
             'IOReturn AirportMtkAwdlInterface::programDataPeer(const uint8_t *address,bool value)\n{\n    invalidateDataRxAdmission();',
             'IOReturn AirportMtkAwdlInterface::programDataPeer(const uint8_t *address,bool value)\n{',
             'programDataPeer entry invalidation'),
            ('program-return-invalidation', 'invalidateDataRxAdmission(); --dataRxPeerProgramming;',
             '--dataRxPeerProgramming;', 'programDataPeer return invalidation'),
            ('cache-commit-invalidation', 'invalidateDataRxAdmission(); return dataPeers.setCached(address,value,now);',
             'return dataPeers.setCached(address,value,now);', 'cacheDataPeer commit invalidation'),
            ('traffic-ownership-invalidation', 'invalidateDataRxAdmission(); return dataPeers.setTraffic(address,value,now);',
             'return dataPeers.setTraffic(address,value,now);', 'trafficDataPeer ownership invalidation'),
            ('program-address-copy', 'hal->awdlPeer(peer,value)',
             'hal->awdlPeer(address,value)', 'programDataPeer stable address copy'),
        )
        evidence.status['negative_controls'] = []
        if args.negative_controls:
            for name, old, new, marker in controls:
                assert source.count(old) == 1, name
                mutated = source.replace(old, new, 1)  # In memory only; production never written.
                result = compile_and_run('negative-' + name, mutated, check=False)
                evidence.status['negative_controls'].append({
                    'name': name, 'returncode': result.returncode, 'required_assertion': marker,
                    'original': old, 'replacement': new,
                    'result': 'PASS' if result.returncode == 1 and 'CHECK failed: ' + marker in result.stderr else 'FAIL',
                })
                assert result.returncode == 1 and 'CHECK failed: ' + marker in result.stderr, (
                    'Negative control did not fail its relevant assertion: ' + name + '\n' + result.stderr)
                assert 'ERROR: AddressSanitizer' not in result.stderr and 'runtime error:' not in result.stderr
                print('PASS negative control: ' + name + ' failed only the expected assertion', flush=True)
        assert before == input_fingerprints(), 'Test/production inputs changed during fixture runs'
        evidence.status['result'] = 'PASS'
    except Exception as error:
        evidence.status.update(result='FAIL', error=str(error))
        raise
    finally:
        evidence.save('source-fingerprints.after.json', input_fingerprints())
        evidence.save('status.json', evidence.status)
        print('AWDL cache ' + evidence.status['result'] + '; evidence: ' + str(evidence.path), flush=True)


if __name__ == '__main__':
    main()
