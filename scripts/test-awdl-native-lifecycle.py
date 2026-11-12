#!/usr/bin/env python3
"""Local, production-extracted coverage for the gated native AWDL lifecycle.

No kext, hardware, or upstream-file writes. All companion scenarios run with
strict ASan/UBSan, without recovery or suppression. An isolated, expected-failure
old-reference negative control proves packed alignment detection. --fresh-ffi
builds the current Wi-Fi Rust core offline using the installed pinned nightly in
a new target directory, checking Cargo artifacts and source/archive provenance.
Without it, the previously retained archive is accepted only when its recorded
Rust inputs still match. Both supply the REAL State/DataPeers/Timeline code.
Source bodies are copied verbatim, including invalidation, polling, and dispatch.
Only private OS/IOKit/HAL boundaries are modeled.  The unchanged native-control
and native-window assertion blocks are read as AST string literals, not executed
as extractors, and are run on the new complete extraction.

This is a companion, NOT a repair of the upstream extractors.  In particular it
is not the whole MI-discovery fixture, the UMI terminal-state table, or the
KERNEL-only RX gate/BPF/scratch-layout contract. The Rust archive is not sanitizer-
instrumented; the extracted C++ and its host boundaries are. The HAL reserve
boundary returns absent timing by default; only explicitly injected observations
are measured test inputs, never inferred from synthetic grants.

Run with python3 -I -B scripts/test-awdl-native-lifecycle.py --fresh-ffi.
Every invocation creates a new diagnostics directory and retains commands,
sources and logs. --compare-head-upstream reruns unchanged upstream extractors
with only their production-source reads redirected to git HEAD snapshots; it
does not build or claim to validate an entire HEAD tree.
"""
from __future__ import annotations

import argparse
import ast
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "kext/Wifi/vendor"
RETAINED = ROOT / "build/diagnostics/offline-rx-matrix-20261109T102331Z-8z7jq4xw"
ARCHIVE = RETAINED / "target-native-ffi/debug/libmtk_ffi.a"
ARCHIVE_SHA256 = "66977f3d62123fb0e718b525eef56d898620dc35b9b0f1abc64f340a255fbea8"
INTERFACE = VENDOR / "AirportMtkWlan/AirportMtkAwdlInterface.cpp"
INTERFACE_HEADER = INTERFACE.with_suffix(".hpp")
OWNER = VENDOR / "AirportMtkWlan/AirportMtkWlan.cpp"
OWNER_HEADER = OWNER.with_suffix(".hpp")
HAL = VENDOR / "SimpleMtkWlan/hal_mwx/MtkMwx.cpp"
HAL_HEADER = HAL.with_suffix(".hpp")
IOCTL = VENDOR / "include/Airport/apple80211_ioctl.h"
VAR = VENDOR / "include/Airport/apple80211_var.h"
NATIVE_ETH = VENDOR / "include/Airport/macOS26/IOSkywalkEthernetInterface.h"

# An explicit, bounded extraction inventory makes new dependency drift visible.
INTERFACE_GROUPS = {
    "admission": "invalidateDataRxAdmission resetDataPeers observeDataPeer cacheDataPeer trafficDataPeer programDataPeer",
    "start_resources": "start startGated startInGate createDataPath releaseDataPath releaseDataPathGated releaseDataPathInGate getTxSubQueue getTxCompQueue getRxCompQueue getTxPacketPool getRxPacketPool",
    "bsd_datapath": "getHardwareAssists setHardwareAssists publishBSDOffloadState prepareBSDInterface enable enableGated enableInGate disable disableGated disableInGate dataPathGated enableDatapath disableDatapath setDataPathEnabled submitNative drainCompletions inputNative",
    "canonical_origin": "admitDataRx inputCanonicalView deliverNativeView inputNativeView",
    "init_teardown": "initWithController setBSDInterface getHardwareAddress getSelfMacAddr isChipInterfaceReady shutdown shutdownGated shutdownInGate stop stopGated stopInGate free",
    "public_controls": "setMacAddress macAddressGated setMacAddressInGate setInterfaceEnable interfaceEnableGated setInterfaceEnableInGate setLinkState linkStateGated setLinkStateInGate setSync syncGated awdlSystemReady cancelSyncEnable syncEnableCurrent scheduleSyncEnable publishSyncStatus continueSyncEnable serviceSyncEnable setSyncInGate finishAvailabilityWindowForDisable",
    "timer_polling_windows": "makeFrame buildAmsdu output tick service drainTxStatus notifyAvailabilityWindowStart notifyAvailabilityWindowEnd publishRocDiagnostics publishCacheDiagnostics publishScheduler serviceInGate receive",
    "native_controls": "setAWDL_SYNC_STATE setAWDL_SYNC_PARAMS setAWDL_PRESENCE_MODE setAWDL_MASTER_CHANNEL setAWDL_SECONDARY_MASTER_CHANNEL setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE setAWDL_SYNC_FRAME_TEMPLATE getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES setAWDL_ELECTION_PARAMS setAWDL_AWDL_ADVERTISERS setAWDL_SYNC_ENABLED getAWDL_SYNC_ENABLED setAWDL_CONFIG applySequence updatePresence setAWDL_PEER_TRAFFIC_REGISTRATION peerTrafficGated setPeerTrafficInGate",
}
OWNER_NAMES = "recordAwdlCreation requestAwdlCreation createAwdlGated createVirtualInterface enableVirtualInterface disableVirtualInterface createAwdlInterfaceRequest createAwdlInterface stopAwdlInterface".split()
STRUCT_NAMES = "apple80211_virt_if_create_data apple80211_awdl_sync_params apple80211_awdl_extension_state_machine_parameter apple80211_awdl_election_id apple80211_awdl_presence_mode apple80211_awdl_sync_state apple80211_awdl_election_rssi_thresholds apple80211_awdl_af_tx_mode apple80211_awdl_min_rate apple80211_awdl_sync_channel_sequence apple80211_awdl_sync_frame_template apple80211_awdl_cap apple80211_awdl_master_channel apple80211_awdl_secondary_master_channel apple80211_awdl_election_params apple80211_awdl_peer_traffic_registration apple80211_awdl_sync_enabled".split()
CASES = ["init-start", "datapath", "sync", "origins", "module-poll", "bsd", "packed-sequence", "native-controls", "native-windows"]


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def utc() -> str:
    return datetime.now(timezone.utc).isoformat()


def balanced_end(text: str, opening: int) -> int:
    """Match a C++ brace without treating comments/quoted text as syntax."""
    left = text[opening]
    if left not in "{(":
        raise ValueError("expected opening brace or parenthesis")
    right = {"{": "}", "(": ")"}[left]
    depth, at, quote = 0, opening, None
    while at < len(text):
        ch = text[at]
        if quote:
            if ch == "\\":
                at += 2
                continue
            if ch == quote:
                quote = None
        elif text.startswith("//", at):
            end = text.find("\n", at)
            at = len(text) if end < 0 else end
            continue
        elif text.startswith("/*", at):
            end = text.find("*/", at + 2)
            if end < 0:
                raise ValueError("unterminated comment")
            at = end + 2
            continue
        elif ch in "\"'":
            quote = ch
        elif ch == left:
            depth += 1
        elif ch == right:
            depth -= 1
            if not depth:
                return at + 1
        at += 1
    raise ValueError("unterminated brace")


class Extractor:
    def __init__(self) -> None:
        self.cache: dict[Path, str] = {}
        self.inventory: list[dict] = []

    def text(self, path: Path) -> str:
        if path not in self.cache:
            self.cache[path] = path.read_text()
        return self.cache[path]

    def span(self, path: Path, start: int, end: int, name: str) -> str:
        text = self.text(path)
        body = text[start:end]
        line = text.count("\n", 0, start) + 1
        self.inventory.append({"name": name, "path": str(path), "line": line,
                               "end_line": text.count("\n", 0, end) + 1,
                               "sha256": digest(body.encode()), "bytes": len(body.encode())})
        return f'#line {line} {json.dumps(str(path))}\n{body}\n'

    def function(self, path: Path, qualified: str) -> str:
        text = self.text(path)
        # A return-type/constructor prefix, not a call such as (void)helper().
        pattern = r"(?m)^[ \t]*(?:[\w:*&<>]+[ \t]+)*[*&]*" + re.escape(qualified) + r"\s*\("
        matches = []
        for match in re.finditer(pattern, text):
            if re.match(r"\s*(?:return|if|else|while|for)\b", match.group()):
                continue
            params_end = balanced_end(text, match.end() - 1)
            opening = text.find("{", params_end)
            suffix = text[params_end:opening].strip()
            constructor = qualified.rsplit("::", 1)[-1] == qualified.rsplit("::", 2)[-2] if "::" in qualified else False
            if opening >= 0 and (suffix in ("", "const", "noexcept") or
                                 (constructor and suffix.startswith(":") and ";" not in suffix)):
                matches.append(match)
        if len(matches) != 1:
            raise ValueError(f"expected one definition of {qualified}, got {len(matches)}")
        start = matches[0].start()
        opening = text.index("{", matches[0].end())
        return self.span(path, start, balanced_end(text, opening), qualified)

    def structure(self, path: Path, name: str) -> str:
        text = self.text(path)
        matches = list(re.finditer(r"\bstruct\s+" + re.escape(name) + r"\s*\{", text))
        if len(matches) != 1:
            raise ValueError(f"expected one struct {name}, got {len(matches)}")
        start = matches[0].start()
        end = balanced_end(text, text.index("{", start))
        end = text.index(";", end) + 1  # retain packed suffix verbatim
        return self.span(path, start, end, "struct " + name)

    def assertions(self, path: Path, marker: str) -> str:
        """Read an upstream literal, never eval/import/exec its extractor."""
        tree = ast.parse(self.text(path), filename=str(path))
        candidates = [n for n in ast.walk(tree) if isinstance(n, ast.Constant)
                      and isinstance(n.value, str) and marker in n.value and "int main(" in n.value]
        if len(candidates) != 1:
            raise ValueError(f"upstream assertion-block drift in {path}")
        node = candidates[0]
        literal = node.value
        # Controls' last literal starts at main; windows includes its init helper.
        start = literal.index("int main(") if marker == "Tahoe KC" else 0
        literal = literal[start:]
        self.inventory.append({"name": "UNCHANGED assertion block", "path": str(path),
                               "line": node.lineno, "sha256": digest(literal.encode()),
                               "assertions": len(re.findall(r"\bassert\s*\(", literal))})
        return f'#line {node.lineno} {json.dumps(str(path) + " [assertions]")}\n{literal}\n'


PREAMBLE = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <cerrno>
#include <AWDL/AwdlProtocol.hpp>
#include <AWDL/AwdlTimeline.hpp>
#include <AWDL/AwdlForwardAdmission.hpp>
#include <AWDL/AwdlRocTrace.hpp>
#include <AWDL/AwdlAdvertisers.hpp>
#define XYLog(...) ((void)0)
#define IOLog(...) ((void)0)
#define mtk_printf(...) ((void)0)
#define bzero(p,n) memset((p),0,(n))
#define OSSafeReleaseNULL(x) do { if(x) { (x)->release(); (x)=nullptr; } } while(0)
#define OSDynamicCast(type,p) dynamic_cast<type *>(p)
#define OSDeclareDefaultStructors(type) public: \
 static void *operator new(size_t n) noexcept { return failInterfaceAllocation ? nullptr : ::operator new(n,std::nothrow); } \
 static void operator delete(void *p) noexcept { ::operator delete(p); }
#define APPLE80211_ADDR_LEN 6
#define IFNAMSIZ 16
#define APPLE80211_VERSION 1
using IOReturn=unsigned; using UInt=unsigned; using UInt32=uint32_t; using SInt32=int32_t;
using apple80211_wme_ac=unsigned; using AbsoluteTime=uint64_t; using IOThread=void *;
constexpr unsigned kIOReturnSuccess=0,kIOReturnNotReady=1,kIOReturnNoMemory=2,
 kIOReturnBadArgument=3,kIOReturnUnsupported=4,kIOReturnNotFound=5,kIOReturnError=6,
 kIOReturnNoResources=7,kIOReturnBusy=8,kIOReturnTimeout=9,kIOReturnAborted=12,kIOReturnIOError=13,
 kIOReturnOutputSuccess=0,kIOReturnOutputDropped=1,
 kMillisecondScale=1,THREAD_UNINT=0,THREAD_AWAKENED=10,THREAD_TIMED_OUT=11,
 APPLE80211_C_FLAG_20MHZ=1,APPLE80211_C_FLAG_2GHZ=8,APPLE80211_C_FLAG_5GHZ=16,
 APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START=41,
 APPLE80211_M_AWDL_AVAILABILITY_WINDOW_EXTENSIONS_END=43,
 IFF_RUNNING=0x40,IFF_UP=1;
constexpr int IEEE80211_M_STA=1,IEEE80211_S_SCAN=0,IEEE80211_S_AUTH=2,IEEE80211_S_ASSOC=3,IEEE80211_S_RUN=4;
constexpr unsigned IEEE80211_F_RSNON=1,MWX_FLAG_SCANNING=1,MWX_FLAG_BGSCAN=2;
void OSSynchronizeIO() { std::atomic_thread_fence(std::memory_order_seq_cst); }
enum IO80211LinkState { kIO80211NetworkLinkDown=0,kIO80211NetworkLinkUp=1 };
struct ether_addr { uint8_t bytes[6]; };
struct apple80211_awdl_config_request; // native opaque version prefix
struct awdl_peer_advert_add { uint8_t bytes[44]; }; // verified opaque native payload
struct apple80211_peer_cache_control; // unexercised cache-control declaration
std::atomic<uint64_t> clockUS{1000000};
uint64_t mach_absolute_time() { return clockUS.load()*1000; }
void absolutetime_to_nanoseconds(uint64_t n,uint64_t *out) { *out=n; }
void clock_interval_to_deadline(unsigned n,unsigned scale,AbsoluteTime *out) {
 assert(scale==kMillisecondScale); *out=clockUS.load()+uint64_t(n)*1000;
}
IOThread IOThreadSelf() { thread_local unsigned identity; return &identity; }
bool failInterfaceAllocation=false;
std::map<std::string,int> bootArguments;
bool PE_parse_boot_argn(const char *name,void *value,int length) {
 auto found=bootArguments.find(name); if(found==bootArguments.end())return false;
 assert(length==sizeof(int)); memcpy(value,&found->second,sizeof(int)); return true;
}
struct Faults {
 bool social=true,base=false,virtualInit=false,timer=false,timerAdd=false,attach=false,
 resource=false,deferral=false,nativeStart=false,mac=false,macMismatch=false,
 registrationInfo=false,registration=false,allocation=false,prepare=false,buffers=false,
 offset=false,length=false,address=false,rxEnqueue=false,txEnqueue=false;
 unsigned factory=0,failFactory=0,queueAdds=0,failQueueAdd=0;
 IOReturn nativeEnable=0,nativeDisable=0,interfaceEnable=0,linkReport=0;
 bool nativeLink=true;
} fault;
std::vector<std::string> trace;
unsigned poolsLive=0,queuesLive=0,packetsLive=0,timersLive=0,
 nativeStarts=0,nativeStops=0,baseFreed=0,lowerFreed=0,publications=0,
 rxTransferred=0,rxReclaimed=0,txCompleted=0,packetFinalized=0;
std::function<void()> nativeEnableHook,nativeDisableHook,nativeInterfaceHook,nativeLinkHook,nativeStopHook;
std::function<void(void *)> publishHook;
void mark(const std::string &s) { trace.push_back(s); }
struct OSObject {
 std::atomic<unsigned> refs{1},retains{0},releases{0};
 virtual ~OSObject() { assert(refs==0 || refs==1); }
 virtual void free() {}
 void retain() { assert(refs); ++refs; ++retains; }
 virtual void release() { assert(refs); ++releases; if(--refs==0) { free(); delete this; } }
};
struct OSDictionary:OSObject {};
struct OSIterator:OSObject { OSObject *getNextObject() { return nullptr; } };
struct OSData:OSObject { unsigned getLength() { return 0; } void *getBytesNoCopy() { return nullptr; } };
struct IOService:OSObject {
 std::map<std::string,uint64_t> properties;
 std::map<std::string,std::vector<uint8_t>> byteProperties;
 std::map<std::string,std::string> stringProperties;
 bool inactive=false;
 bool isInactive() { return inactive; }
 void setProperty(const char *key,uint64_t value,unsigned) { properties[key]=value; }
 void setProperty(const char *key,bool value) { properties[key]=value; }
 void setProperty(const char *key,const char *value) { stringProperties[key]=value; }
 void setProperty(const char *key,const void *p,unsigned n) {
  const auto b=static_cast<const uint8_t *>(p); byteProperties[key]={b,b+n};
 }
 OSObject *getProperty(const char *key) { return properties.count(key)&&properties[key] ? this : nullptr; }
 void removeProperty(const char *key) { properties.erase(key); stringProperties.erase(key); byteProperties.erase(key); }
 // No GPU-registry match in this host model; no GPU readiness claim is made.
 static OSDictionary *serviceMatching(const char *name) { assert(!strcmp(name,"IOPCIDevice")); return nullptr; }
 static OSIterator *getMatchingServices(OSDictionary *) { return nullptr; }
 static OSDictionary *resourceMatching(const char *name) { assert(!strcmp(name,"IOBSD")); return new OSDictionary; }
 static IOService *copyMatchingService(OSDictionary *) { return fault.resource ? nullptr : new IOService; }
};
struct IOSkywalkPacketQueue;
struct IOTimerEventSource;
struct IOCommandGate;
thread_local unsigned gateDepth=0,heldHalLocks=0;
struct WorkLoop:OSObject {
 std::set<OSObject *> sources;
 IOReturn addEventSource(OSObject *);
 void removeEventSource(OSObject *);
 bool inGate() { return gateDepth!=0; }
};
WorkLoop loop;
struct IOCommandGate:OSObject {
 using Action=IOReturn(*)(OSObject *,void *,void *,void *,void *);
 OSObject *target=nullptr; WorkLoop *workloop=&loop; std::recursive_mutex mutex;
 unsigned actions=0,sleeps=0,wakeups=0,maxDepth=0,lastSleepDepth=0;
 // Hook is an OS wait boundary, invoked only AFTER all recursive gate levels drop.
 std::function<IOReturn(void *,uint64_t,unsigned)> sleepHook;
 WorkLoop *getWorkLoop() { return workloop; }
 IOReturn runAction(Action action,void *a=nullptr,void *b=nullptr,void *c=nullptr,void *d=nullptr) {
  std::lock_guard<std::recursive_mutex> lock(mutex); ++gateDepth; ++actions;
  maxDepth=std::max(maxDepth,gateDepth); IOReturn result=action(target,a,b,c,d); --gateDepth; return result;
 }
 template<typename F> auto enter(F fn)->decltype(fn()) {
  std::lock_guard<std::recursive_mutex> lock(mutex); ++gateDepth; ++actions;
  maxDepth=std::max(maxDepth,gateDepth); auto result=fn(); --gateDepth; return result;
 }
 IOReturn commandSleep(void *event,AbsoluteTime deadline,unsigned flags) {
  assert(flags==THREAD_UNINT&&gateDepth); unsigned saved=gateDepth;
  ++sleeps; lastSleepDepth=saved; gateDepth=0;
  for(unsigned i=0;i<saved;++i) mutex.unlock();
  IOReturn status;
  if(sleepHook) status=sleepHook(event,deadline,saved);
  else { clockUS.store(std::max(clockUS.load(),deadline)); std::this_thread::yield(); status=THREAD_TIMED_OUT; }
  for(unsigned i=0;i<saved;++i) mutex.lock();
  gateDepth=saved; return status;
 }
 void commandWakeup(void *,bool) { assert(gateDepth); ++wakeups; }
} gate;
IOCommandGate *_fCommandGate=&gate;
void gatedBoundary() { assert(_fCommandGate&&gateDepth); }
struct IOLock { std::mutex mutex; };
std::atomic<unsigned> locksAllocated{0},locksFreed{0}; bool failLock=false;
IOLock *IOLockAlloc() { if(failLock)return nullptr; ++locksAllocated; return new IOLock; }
void IOLockFree(IOLock *l) { ++locksFreed; delete l; }
void IOLockLock(IOLock *l) { l->mutex.lock(); ++heldHalLocks; }
void IOLockUnlock(IOLock *l) { assert(heldHalLocks); --heldHalLocks; l->mutex.unlock(); }
struct Ifnet { uint8_t mac[6]{}; unsigned offload=0x37f,enabled=0xffff,supported=0xffff; };
using ifnet_t=Ifnet *;
constexpr unsigned IFCAP_HWCSUM=3,IFCAP_TSO=0x60,IFCAP_LRO=0x80,
 IFCAP_CSUM_PARTIAL=0x2000,IFCAP_CSUM_ZERO_INVERT=0x4000;
struct BsdBoundary {
 unsigned failStep=0,step=0,assistsCalls=0,assistsValue=99,assistsMask=0,
  prepareCalls=0,options=0,lladdrCalls=0;
 bool lie=false; IOReturn prepareStatus=0;
} bsdBoundary;
int ifnet_set_offload(ifnet_t p,unsigned n) {
 if(++bsdBoundary.step==bsdBoundary.failStep)return EINVAL;
 if(!bsdBoundary.lie)p->offload=n; return 0;
}
int ifnet_set_capabilities_enabled(ifnet_t p,unsigned n,unsigned mask) {
 if(++bsdBoundary.step==bsdBoundary.failStep)return EINVAL;
 if(!bsdBoundary.lie)p->enabled=(p->enabled&~mask)|(n&mask); return 0;
}
int ifnet_set_capabilities_supported(ifnet_t p,unsigned n,unsigned mask) {
 if(++bsdBoundary.step==bsdBoundary.failStep)return EINVAL;
 if(!bsdBoundary.lie)p->supported=(p->supported&~mask)|(n&mask); return 0;
}
unsigned ifnet_offload(ifnet_t p) { return p->offload; }
unsigned ifnet_capabilities_enabled(ifnet_t p) { return p->enabled; }
unsigned ifnet_capabilities_supported(ifnet_t p) { return p->supported; }
int ifnet_set_lladdr(ifnet_t p,const void *mac,unsigned n) { assert(n==6); ++bsdBoundary.lladdrCalls; memcpy(p->mac,mac,n); return 0; }
struct Packet { std::vector<uint8_t> bytes; unsigned flags=1; };
using mbuf_t=Packet *;
constexpr unsigned MBUF_DONTWAIT=0,MBUF_PKTHDR=1,MBUF_WAITOK=2;
int mbuf_allocpacket(unsigned flags,size_t n,unsigned *chunks,mbuf_t *out) {
 assert((flags==MBUF_DONTWAIT||flags==MBUF_WAITOK)&&*chunks==1); *out=new Packet; (*out)->bytes.resize(n); return 0;
}
// Deliberately single-segment mbufs; kernel chain allocation is outside coverage.
mbuf_t mbuf_next(mbuf_t) { return nullptr; }
size_t mbuf_len(mbuf_t p) { return p->bytes.size(); }
size_t mbuf_maxlen(mbuf_t p) { return p->bytes.size(); }
void mbuf_freem(mbuf_t p) { delete p; }
void *mbuf_data(mbuf_t p) { return p->bytes.data(); }
size_t mbuf_pkthdr_len(mbuf_t p) { return p->bytes.size(); }
unsigned mbuf_flags(mbuf_t p) { return p->flags; }
void mbuf_setlen(mbuf_t p,size_t n) { p->bytes.resize(n); }
void mbuf_pkthdr_setlen(mbuf_t p,size_t n) { p->bytes.resize(n); }
int mbuf_copydata(mbuf_t p,size_t at,size_t n,void *out) {
 if(at+n>p->bytes.size())return EINVAL; memcpy(out,p->bytes.data()+at,n); return 0;
}
size_t strlcpy(char *to,const char *from,size_t n) {
 size_t len=strlen(from); if(n){size_t copy=std::min(n-1,len); memcpy(to,from,copy); to[copy]=0;} return len;
}
struct Event { unsigned id; std::vector<uint8_t> bytes; };
class AirportMtkAwdlInterface;
class AirportMtkWlan;
'''

# Private boundary support.  There is deliberately no fake AWDL State/Timeline,
# shutdown, sync implementation, admission helper, or UMI state-machine table.
HAL_SUPPORT = r'''
struct MtkMwx:IOService {
'''
HAL_AFTER_ABI = r'''
 struct TerminalBoundary {
  std::deque<AwdlTxResult> injected;
  unsigned expires=0,takes=0; uint64_t expiredAt=0;
  void expire(uint64_t now) { assert(heldHalLocks); ++expires; expiredAt=now; }
  bool take(AwdlTxResult &out) { assert(heldHalLocks); ++takes; if(injected.empty())return false;
   out=injected.front(); injected.pop_front(); return true; }
 };
 struct IC { uint8_t ic_myaddr[6]={2,1,2,3,4,5}; int ic_state=4,ic_opmode=IEEE80211_M_STA; bool ic_wcl_mac_reconfig=false;
  unsigned ic_flags=IEEE80211_F_RSNON;
  struct { uint64_t Attempt=0; } ic_join_diag;
  struct Node { uint8_t ni_bssid[6]={2,9,9,9,9,9}; bool ni_port_valid=true; } modelBss; Node *ic_bss=&modelBss;
  struct { unsigned if_flags=IFF_UP|IFF_RUNNING; } ic_if;
 };
 struct Com {
  IOLock *sc_awdl_lock=nullptr;
  bool sc_awdl_enabled=false,sc_awdl_stopping=false,sc_resetting=false,sc_detaching=false,sc_awdl_busy=false;
  int sc_ns_state=IEEE80211_S_RUN; unsigned sc_flags=0;
  SInt32 sc_power_req=0,sc_power_done=0;
  unsigned sc_awdl_infra_busy=0,sc_awdl_init_busy=0; bool sc_awdl_ns_pending=false;
  uint64_t sc_awdl_native_session=0;
  uint32_t sc_awdl_epoch=0; AwdlRocTiming sc_awdl_roc_timing{};
  void *sc_awdl_context=nullptr; AwdlReceive sc_awdl_receive=nullptr;
  IC sc_ic; TerminalBoundary sc_awdl_native_tx;
 } storage;
 Com *com=&storage;
 IC &ic=storage.sc_ic;
 bool on=false,thirdChannel=false,homeChannel8=false,allow11=false,dense=false;
 // Firmware startup has a ready infrastructure link. NativeFixture later
 // removes this modeled link after the actual first-sync readiness check.
 uint8_t infraChannel=6,requested=0,preparedChannel=0;
 unsigned reserves=0,aborts=0,prepares=0,transmits=0,enables=0,disables=0,publishes=0,wakes=0,sleeps=0;
 unsigned receiverSets=0,receiverClears=0,transportEpoch=0;
 uint32_t duration=0; mtkawdl::Reservation grant{};
 IOReturn enableStatus=0,peerDeleteResult=0,tryStatus=0,reserveResult=0;
 std::function<void(AwdlRocTiming &)> onReserveTiming;
 std::function<void()> onReserve,onPrepare,wait;
 std::function<void(bool)> onPeer,onEnable;
 MtkMwx() { storage.sc_awdl_lock=IOLockAlloc(); assert(storage.sc_awdl_lock); }
 ~MtkMwx() override { if(storage.sc_awdl_lock)IOLockFree(storage.sc_awdl_lock); }
 uint64_t awdlTxSession(); bool awdlTakeTxResult(AwdlTxResult &);
 bool awdlInterfaceReady(); bool awdlSyncSnapshot(AwdlSyncSnapshot &);
 IC *get80211Controller() { return com ? &com->sc_ic : nullptr; }
 bool awdlChannelAllowed(unsigned c) { return fault.social&&(c==6||c==44||(thirdChannel&&c==149)||(homeChannel8&&c==8)||(allow11&&c==11)); }
 uint8_t awdlInfraChannel() { return infraChannel; }
 uint8_t awdlOnlyChannel() { return infraChannel; }
 bool awdlChannelReachable(unsigned c) { return awdlChannelAllowed(c)&&(!infraChannel||infraChannel==c); }
 void awdlInfraSleep(unsigned) { ++sleeps; }
 void awdlInfraWakeIfIdle() { ++wakes; }
 bool awdlDensePresenceAllowed(uint8_t) { return dense; }
 bool awdlIsEnabled() { return on; }
 bool awdlGrantSnapshot(mtkawdl::Reservation &out) { out=grant; return on; }
 bool awdlRocTimingSnapshot(AwdlRocTiming &);
 bool awdlCanTransmit(unsigned c) { return grant.canTransmit(c,clockUS.load()); }
 void awdlAbort() { gatedBoundary(); ++aborts; grant.cancel(); }
 IOReturn awdlPrepareChannel(unsigned c) { gatedBoundary(); ++prepares; preparedChannel=c; if(onPrepare)onPrepare(); return 0; }
 IOReturn awdlReserve(uint8_t c,uint32_t ms,AwdlRocTiming *timing=nullptr) {
  if(timing)*timing={}; gatedBoundary(); ++reserves; requested=c; duration=ms;
  // Boundary-only input: never manufacture timing from the modeled clock or
  // grant. Keep this invocation's explicit partial output across callback reentry.
  AwdlRocTiming own{}; const IOReturn result=reserveResult;
  auto timingHook=onReserveTiming; if(timingHook)timingHook(own);
  auto hook=onReserve; if(hook)hook();
  if(timing)*timing=own; return result;
 }
 IOReturn awdlPeer(const uint8_t *,bool add) { gatedBoundary(); if(onPeer)onPeer(add); return add?0:peerDeleteResult; }
 IOReturn awdlRxBlockAck(const uint8_t *,uint8_t,uint16_t,uint16_t,bool value) { gatedBoundary(); return value?kIOReturnUnsupported:0; }
 IOReturn awdlEnable(const uint8_t *,bool,const AwdlSyncGuard *guard=nullptr);
 IOReturn awdlTransmit(mbuf_t m,unsigned) { gatedBoundary(); ++transmits; mbuf_freem(m); return 0; }
 IOReturn awdlTryTransmit(mbuf_t *m,unsigned c) { if(tryStatus)return tryStatus; auto status=awdlTransmit(*m,c); *m=nullptr; return status; }
 void awdlPublishDiagnostics() { ++publishes; }
 void give(uint8_t token,uint8_t c,uint32_t ms) { grant.request(token,c,clockUS.load()); assert(grant.grant(token,c,ms,clockUS.load())); }
};
'''

RESOURCES = r'''
struct IOSkywalkPacketBufferPool;
struct IOSkywalkPacket:OSObject {
 IOSkywalkPacketBufferPool *origin;
 uint8_t bytes[2048]{}; unsigned length=64,offset=0,buffers=1;
 bool prepared=false,tx=false; IOSkywalkPacketQueue *source=nullptr;
 explicit IOSkywalkPacket(IOSkywalkPacketBufferPool *p):origin(p) { ++packetsLive; }
 ~IOSkywalkPacket() override { --packetsLive; }
 unsigned getDataLength() { return length; }
 unsigned getDataOffset() { return offset; }
 unsigned getPacketBufferCount() { return buffers; }
 void *getDataVirtualAddress() { assert(prepared); return fault.address?nullptr:bytes; }
 IOReturn setDataOffset(unsigned n) { assert(prepared); offset=n; return fault.offset?kIOReturnError:0; }
 IOReturn setDataLength(unsigned n) { assert(prepared&&n<=2048); length=n; return fault.length?kIOReturnError:0; }
 IOReturn prepareWithQueue(IOSkywalkPacketQueue *q,unsigned direction,unsigned) {
  assert(direction==2); source=q; prepared=true; buffers=fault.buffers?2:1;
  return fault.prepare?kIOReturnError:0;
 }
 void finalize() { if(prepared){assert(!source); prepared=false; ++packetFinalized;} }
};
struct IOSkywalkPacketBufferPool:OSObject {
 struct PoolOptions { unsigned packets,buffers,size,maxBuffers,segment,flags; uint64_t reserved; };
 unsigned id; std::function<void()> onAllocate,onDeallocate;
 IOSkywalkPacketBufferPool():id(++fault.factory) { ++poolsLive; }
 ~IOSkywalkPacketBufferPool() override { --poolsLive; mark("pool-release-"+std::to_string(id)); }
 static IOSkywalkPacketBufferPool *withName(const char *,OSObject *,unsigned,const PoolOptions *options) {
  gatedBoundary(); assert(options->packets==256&&options->buffers==256&&options->size==2048&&options->maxBuffers==1);
  if(fault.factory+1==fault.failFactory){++fault.factory; return nullptr;}
  return new IOSkywalkPacketBufferPool;
 }
 IOReturn allocatePacket(IOSkywalkPacket **out,unsigned flags) {
  assert(flags==1); if(fault.allocation)return kIOReturnNoMemory;
  *out=new IOSkywalkPacket(this); auto hook=onAllocate; if(hook)hook(); return 0;
 }
 void deallocatePacket(IOSkywalkPacket *p) {
  assert(p&&p->origin==this); if(p->tx)++txCompleted; else ++rxReclaimed;
  p->finalize(); auto hook=onDeallocate; if(hook)hook(); p->release();
 }
};
struct IOSkywalkPacketQueue:OSObject {
 IOSkywalkPacketBufferPool *pool; unsigned id; bool enabled=false,added=false;
 std::function<void()> onDisable,onEnqueue;
 explicit IOSkywalkPacketQueue(IOSkywalkPacketBufferPool *p):pool(p),id(++fault.factory) { ++queuesLive; pool->retain(); }
 ~IOSkywalkPacketQueue() override { assert(!added); pool->release(); --queuesLive; mark("queue-release-"+std::to_string(id)); }
 void enable() { gatedBoundary(); enabled=true; mark("queue-enable-"+std::to_string(id)); }
 void disable() { gatedBoundary(); enabled=false; mark("queue-disable-"+std::to_string(id)); auto hook=onDisable; if(hook)hook(); }
};
struct IOSkywalkTxSubmissionQueue:IOSkywalkPacketQueue {
 using IOSkywalkPacketQueue::IOSkywalkPacketQueue;
 static IOSkywalkTxSubmissionQueue *withPool(IOSkywalkPacketBufferPool *p,unsigned n,unsigned,OSObject *,
  unsigned(*)(OSObject *,IOSkywalkTxSubmissionQueue *,IOSkywalkPacket *const *,unsigned,void *),void *,unsigned) {
  assert(n==64); if(fault.factory+1==fault.failFactory){++fault.factory; return nullptr;} return new IOSkywalkTxSubmissionQueue(p);
 }
};
struct IOSkywalkTxCompletionQueue:IOSkywalkPacketQueue {
 using IOSkywalkPacketQueue::IOSkywalkPacketQueue;
 static IOSkywalkTxCompletionQueue *withPool(IOSkywalkPacketBufferPool *p,unsigned n,unsigned,OSObject *,void *,void *,unsigned) {
  assert(n==128); if(fault.factory+1==fault.failFactory){++fault.factory; return nullptr;} return new IOSkywalkTxCompletionQueue(p);
 }
 IOReturn enqueuePackets(IOSkywalkPacket *const *packets,unsigned n,unsigned) {
  gatedBoundary(); assert(n<128); auto hook=onEnqueue; if(hook)hook();
  if(fault.txEnqueue||!enabled)return kIOReturnError;
  for(unsigned i=0;i<n;++i)pool->deallocatePacket(packets[i]); return 0;
 }
};
struct IOSkywalkRxCompletionQueue:IOSkywalkPacketQueue {
 using IOSkywalkPacketQueue::IOSkywalkPacketQueue;
 static IOSkywalkRxCompletionQueue *withPool(IOSkywalkPacketBufferPool *p,unsigned n,unsigned,OSObject *,void *,void *,unsigned) {
  assert(n==128); if(fault.factory+1==fault.failFactory){++fault.factory; return nullptr;} return new IOSkywalkRxCompletionQueue(p);
 }
 IOReturn enqueuePackets(IOSkywalkPacket *const *packets,unsigned n,unsigned) {
  gatedBoundary(); assert(n==1); auto hook=onEnqueue; if(hook)hook();
  if(fault.rxEnqueue||!enabled)return kIOReturnError;
  assert(packets[0]->origin==pool); packets[0]->finalize(); packets[0]->release(); ++rxTransferred; return 0;
 }
};
struct IOTimerEventSource:OSObject {
 using Action=void(*)(OSObject *,IOTimerEventSource *);
 OSObject *target; Action action; bool pending=false,enabled=true; unsigned us=0,calls=0;
 IOTimerEventSource(OSObject *o,Action a):target(o),action(a) { ++timersLive; }
 ~IOTimerEventSource() override { --timersLive; }
 static IOTimerEventSource *timerEventSource(OSObject *o,Action a) { return fault.timer?nullptr:new IOTimerEventSource(o,a); }
 void setTimeoutMS(unsigned n) { pending=true; us=n*1000; ++calls; }
 void setTimeoutUS(unsigned n) { pending=true; us=n; }
 void cancelTimeout() { pending=false; us=0; mark("timer-cancel"); }
 void disable() { enabled=false; mark("timer-disable"); }
 void enable() { enabled=true; mark("timer-enable"); }
 // Consume the one-shot before dispatch. A callback must explicitly rearm it.
 // Tests may also force entry to exercise synchronous native callback reentry.
 void fire() { gatedBoundary(); pending=false; us=0; action(target,this); }
};
IOReturn WorkLoop::addEventSource(OSObject *o) {
 if(dynamic_cast<IOTimerEventSource *>(o)) { if(fault.timerAdd)return kIOReturnError; }
 else if(auto q=dynamic_cast<IOSkywalkPacketQueue *>(o)) {
  if(++fault.queueAdds==fault.failQueueAdd)return kIOReturnError; assert(!q->added); q->added=true;
 }
 assert(sources.insert(o).second); o->retain(); return 0;
}
void WorkLoop::removeEventSource(OSObject *o) {
 assert(sources.erase(o)==1); if(auto q=dynamic_cast<IOSkywalkPacketQueue *>(o))q->added=false;
 mark("remove-source"); o->release();
}
struct IO80211Buffer:OSObject {
 uint8_t bytes[2048]{}; size_t length=0;
 void *getBytesNoCopy() { return bytes; } size_t getCapacity() { return sizeof(bytes); }
 void setLength(size_t n) { assert(n<=sizeof(bytes)); length=n; }
 void returnBuffer() { release(); }
};
struct IO80211BufferPool { IO80211Buffer *getBuffer() { return new IO80211Buffer; } };
struct IOSkywalkEthernetInterface:IOService {
'''
NATIVE_AFTER_REGISTRATION = r'''
 bool baseStarted=false,baseRegistered=false,attached=false;
 ifnet_t nativeBSD=nullptr;
 virtual void free() override { ++lowerFreed; mark("lower-free"); }
 virtual const char *getBSDNamePrefix() { return "en"; }
 bool initRegistrationInfo(RegistrationInfo *info,unsigned version,unsigned long n) {
  gatedBoundary(); assert(version==1&&n==sizeof(*info)); return !fault.registrationInfo;
 }
 IOReturn registerEthernetInterface(const RegistrationInfo *info,IOSkywalkPacketQueue **q,unsigned n,
  IOSkywalkPacketBufferPool *tx,IOSkywalkPacketBufferPool *rx,unsigned) {
  gatedBoundary(); assert(baseStarted&&getProperty("IODeferBSDAttach")&&n==3&&tx&&rx);
  uint32_t family,media,unit; const char *prefix;
  memcpy(&family,info->bytes+12,4); memcpy(&media,info->bytes+40,4);
  memcpy(&prefix,info->bytes+48,sizeof(prefix)); memcpy(&unit,info->bytes+56,4);
  assert(family==3&&media==0x80&&!strcmp(prefix,"awdl")&&unit==0);
  assert(q[0]->added&&q[1]->added&&q[2]->added);
  assert(q[0]->pool==tx&&q[1]->pool==tx&&q[2]->pool==rx);
  mark("native-register"); if(fault.registration)return kIOReturnError;
  baseRegistered=true; return 0;
 }
 bool attach(IOService *) { mark("attach"); attached=!fault.attach; return attached; }
 void detach(IOService *) { gatedBoundary(); assert(attached&&!baseStarted); attached=false; mark("detach"); }
 ifnet_t getBSDInterface() { return nativeBSD; }
 const char *getBSDName() { return "awdl0"; }
};
struct IO80211SkywalkInterface:IOSkywalkEthernetInterface {
 bool ivars=false; ether_addr agentMac{}; unsigned macAgentCalls=0;
 bool init() { ivars=!fault.base; mark("base-init"); return ivars; }
 void free() override { assert(ivars); ivars=false; ++baseFreed; mark("base-free"); IOSkywalkEthernetInterface::free(); }
 virtual IOReturn setMacAddress(ether_addr &) = 0;
 IOReturn setHardwareAddress(ether_addr *mac) {
  gatedBoundary(); assert(baseStarted); ++macAgentCalls; if(fault.mac)return kIOReturnError;
  IOReturn status=setMacAddress(*mac); if(!status){agentMac=*mac; if(fault.macMismatch)agentMac.bytes[5]^=1;} return status;
 }
 virtual ether_addr getSelfMacAddr() { return agentMac; }
 virtual IOReturn getHardwareAddress(ether_addr *) = 0;
 virtual bool isChipInterfaceReady() = 0;
 void deferBSDAttach(bool value) {
  if(value){if(!fault.deferral)setProperty("IODeferBSDAttach",true); mark("defer-bsd");}
  else {gatedBoundary(); assert(baseRegistered&&baseStarted); removeProperty("IODeferBSDAttach"); ++publications; mark("publish-bsd"); if(publishHook)publishHook(this);}
 }
};
struct IO80211VirtualInterface:IO80211SkywalkInterface {
 std::vector<Event> events; std::function<void()> onStart,onEnd;
 IO80211BufferPool mgmt;
 virtual IOReturn setInterfaceEnable(bool) {
  gatedBoundary(); mark("native-interface"); if(nativeInterfaceHook)nativeInterfaceHook(); return fault.interfaceEnable;
 }
 virtual bool setLinkState(IO80211LinkState,unsigned) {
  gatedBoundary(); mark("native-link"); if(nativeLinkHook)nativeLinkHook(); return fault.nativeLink;
 }
 IOReturn reportLinkStatus(unsigned status,unsigned reason) {
  gatedBoundary(); assert((status==3&&reason==0x80)||(status==1&&reason==0x82)); mark("report-link"); return fault.linkReport;
 }
 void setInterfaceRole(unsigned role) { assert(role==6); }
 void setInterfaceId(unsigned id) { assert(id==2); }
 void postMessage(unsigned id,void *p,size_t n,bool sync) {
  gatedBoundary(); assert(sync&&!heldHalLocks); auto b=static_cast<uint8_t *>(p);
  events.push_back({id,{b,b+n}}); mark(id==41?"aw-start":"aw-end");
  auto hook=id==41?onStart:onEnd; if(hook)hook();
 }
 IO80211BufferPool *getMgmtFramePool() { return &mgmt; }
 void bpfTapInput(IO80211Buffer *p,unsigned kind,void *,unsigned n) { assert(kind==12&&n==72); p->returnBuffer(); }
};
struct IO80211AWDLProtocol:IO80211VirtualInterface {
 unsigned cachedAssists=0;
 bool init(IOService *,ether_addr *,unsigned role,const char *name) { assert(ivars&&role==6&&!strcmp(name,"awdl")); return !fault.virtualInit; }
 virtual bool start(IOService *) {
  gatedBoundary(); assert(ivars&&getProperty("IODeferBSDAttach")); ++nativeStarts; mark("native-start"); baseStarted=!fault.nativeStart; return baseStarted;
 }
 virtual void stop(IOService *) {
  gatedBoundary(); assert(baseStarted); ++nativeStops; mark("native-stop"); baseStarted=baseRegistered=false; if(nativeStopHook)nativeStopHook();
 }
 virtual IOReturn enable(unsigned) {
  gatedBoundary(); mark("native-enable"); if(nativeEnableHook)nativeEnableHook();
  return fault.nativeEnable?fault.nativeEnable:setInterfaceEnable(true);
 }
 virtual IOReturn disable(unsigned) {
  gatedBoundary(); mark("native-disable"); if(nativeDisableHook)nativeDisableHook();
  return fault.nativeDisable?fault.nativeDisable:setInterfaceEnable(false);
 }
 virtual void enableDatapath()=0; virtual void disableDatapath()=0;
 virtual UInt getHardwareAssists() { return cachedAssists; }
 virtual void setHardwareAssists(unsigned v,unsigned mask) {
  ++bsdBoundary.assistsCalls; bsdBoundary.assistsValue=v; bsdBoundary.assistsMask=mask;
  cachedAssists=(cachedAssists&~mask)|(v&mask);
 }
 virtual IOReturn prepareBSDInterface(ifnet_t p,unsigned options) {
  ++bsdBoundary.prepareCalls; bsdBoundary.options=options;
  if(bsdBoundary.prepareStatus)return bsdBoundary.prepareStatus;
  // Model only the native call's virtual hardware-assist lookup/offload write.
  // This is not validation of the private native implementation.
  if(!bsdBoundary.lie)p->offload=getHardwareAssists(); return 0;
 }
 virtual void *getTxSubQueue(apple80211_wme_ac)=0; virtual void *getTxCompQueue()=0; virtual void *getRxCompQueue()=0;
 virtual void *getTxPacketPool()=0; virtual void *getRxPacketPool()=0;
'''
OWNER_SUPPORT = r'''
struct IO80211Controller:IOService {
 std::vector<Event> messages; std::function<void(unsigned)> onMessage; IOReturn messageStatus=0;
 virtual IO80211VirtualInterface *createVirtualInterface(ether_addr *,unsigned)=0;
 virtual SInt32 enableVirtualInterface(IO80211VirtualInterface *)=0;
 virtual SInt32 disableVirtualInterface(IO80211VirtualInterface *)=0;
};
struct AirportMtkEthernetInterface:IOService {};
class AirportMtkWlan:public IO80211Controller {
public:
 MtkMwx hal; Ifnet infraBSD; IOService infraInterface;
 IOService *fHalService=&hal,*fNetIf=&infraInterface;
 ifnet_t bsdInterface=&infraBSD;
 WorkLoop *getWorkLoop() { return &loop; }
'''
POST_TYPES = r'''
IOReturn awdlControllerMessage(IO80211Controller *owner,IO80211SkywalkInterface *,unsigned id,void *p,unsigned long n,bool sync) {
 gatedBoundary(); assert(owner&&sync&&!heldHalLocks); auto b=static_cast<const uint8_t *>(p);
 owner->messages.push_back({id,{b,b+n}}); auto hook=owner->onMessage; if(hook)hook(id); return owner->messageStatus;
}
IOReturn MtkMwx::awdlEnable(const uint8_t *,bool value,const AwdlSyncGuard *guard) {
 // An unshared, never-enabled transport may be quiesced in partial/final free.
 // Live firmware operations remain gate-ordered; waits require full gate depth.
 assert((!value&&!on&&!_fCommandGate)||gateDepth);
 unsigned epoch=transportEpoch; if(value)++enables; else {++disables; ++transportEpoch; ++storage.sc_awdl_epoch;}
 if(onEnable)onEnable(value); if(wait)wait();
 // MCU transport commit is a boundary, not a replacement interface lifecycle.
 // A canceled in-flight enable cannot restart a transport stopped by teardown.
 IOReturn status=value&&epoch!=transportEpoch?kIOReturnNotReady:enableStatus;
 if(value&&guard) {
  IOReturn lifecycle=mwx_awdl_sync_status(&storage,guard->epoch,guard);
  if(lifecycle!=kIOReturnSuccess)status=lifecycle;
 }
 if(!status) {
  IOLockLock(storage.sc_awdl_lock);
  if(value&&!storage.sc_awdl_enabled)storage.sc_awdl_native_session=mwx_awdl_native_new_session();
  if(!value){storage.sc_awdl_native_session=0; grant.cancel();}
  storage.sc_awdl_enabled=value; on=value;
  IOLockUnlock(storage.sc_awdl_lock);
 }
 return status;
}
#define super IO80211AWDLProtocol
'''

FIXTURE = r'''
// Test-only convenience names expose boundaries expected by upstream assertions.
// The setup itself runs the actual init/start/enable/sync production chains.
struct NativeFixture:AirportMtkAwdlInterface {
 AirportMtkWlan controller; MtkMwx &radio; IOTimerEventSource &t,&clock;
 IOTimerEventSource &initialize() {
  gate.target=&controller; ether_addr mac{{2,1,2,3,4,5}};
  assert(mwx_awdl_native_history_init());
  assert(initWithController(&controller,&mac,&radio)); assert(attach(&controller));
  assert(start(&controller)); assert(enable(0)==0); assert(setSync(true)==0);
  assert(enabled&&readyGateBypassed); radio.infraChannel=0;
  afMode.mode=3; syncParams.synchronization_frame_period=440;
  timer->calls=0; return *timer;
 }
 NativeFixture():radio(controller.hal),t(initialize()),clock(t) {}
 ~NativeFixture() override {
  onStart=onEnd=nullptr; radio.onPeer=nullptr; radio.onReserve=nullptr; radio.onReserveTiming=nullptr; radio.wait=nullptr; radio.onEnable=nullptr;
  gate.enter([&]{shutdown(); stop(&controller); if(attached)detach(&controller); free(); return 0;});
  assert(refs==1&&retains==releases);
 }
 // Simulated timer event entry acquires the shared controller gate, then invokes
 // the actual tick -> service -> polling/completion/scheduler chain verbatim.
 void serviceInGate() { gate.enter([&]{timer->fire(); return 0;}); }
};
struct ControlFixture:NativeFixture { ControlFixture() { radio.thirdChannel=radio.allow11=true; } };
void resetFaults() {
 assert(!poolsLive&&!queuesLive&&!packetsLive&&!timersLive&&loop.sources.empty()&&loop.refs==1);
 fault=Faults{}; failInterfaceAllocation=false; trace.clear();
 nativeEnableHook=nativeDisableHook=nativeInterfaceHook=nativeLinkHook=nativeStopHook=nullptr;
 publishHook=nullptr; gate.sleepHook=nullptr; _fCommandGate=&gate; gate.workloop=&loop; clockUS=1000000;
}
void noResources() { assert(!poolsLive&&!queuesLive&&!packetsLive&&!timersLive&&loop.sources.empty()&&loop.refs==1); }
bool ownerCreate(AirportMtkWlan &c) { gate.target=&c; return gate.enter([&]{return c.createAwdlInterface();}); }
void ownerStop(AirportMtkWlan &c) { gate.target=&c; gate.enter([&]{c.stopAwdlInterface(); return 0;}); }
size_t position(const std::string &item) { auto it=std::find(trace.begin(),trace.end(),item); assert(it!=trace.end()); return size_t(it-trace.begin()); }
void checkInitStart() {
 for(unsigned stage=0;stage<6;++stage) {
  resetFaults(); AirportMtkWlan c; ether_addr mac{{2,1,2,3,4,5}};
  unsigned beforeBase=baseFreed,beforeLower=lowerFreed;
  fault.social=stage!=0; fault.base=stage==1; fault.virtualInit=stage==2; fault.timer=stage==3; fault.timerAdd=stage==4;
  auto p=new AirportMtkAwdlInterface;
  assert(p->initWithController(&c,&mac,&c.hal)==(stage==5));
  assert(p->receiverRegistered==(stage==5));
  auto refs=p->refs.load(); unsigned actions=gate.actions;
  // Unshared partial/final-cleanup fallback and gate-attached cleanup are both real.
  if(stage%2)_fCommandGate=nullptr;
  p->shutdown(); assert(p->refs==refs); if(!(stage%2))assert(gate.actions>actions);
  p->release(); _fCommandGate=&gate;
  assert(baseFreed-beforeBase==(stage>=2?1U:0U)&&lowerFreed-beforeLower==1);
  assert(!c.hal.storage.sc_awdl_context&&!c.hal.storage.sc_awdl_receive);
  assert(c.hal.disables==(stage==5?1U:0U)); noResources();
 }
 for(unsigned stage=0;stage<20;++stage) {
  resetFaults(); AirportMtkWlan c; c.hal.thirdChannel=true;
  if(stage<5)fault.failFactory=stage+1;
  else if(stage<8)fault.failQueueAdd=stage-4;
  else if(stage==8)fault.resource=true;
  else if(stage==9)fault.deferral=true;
  else if(stage==10)fault.nativeStart=true;
  else if(stage==11)fault.mac=true;
  else if(stage==12)fault.macMismatch=true;
  else if(stage==13)fault.registrationInfo=true;
  else if(stage==14)fault.registration=true;
  else if(stage==15)fault.attach=true;
  else if(stage==16)failInterfaceAllocation=true;
  else if(stage==17)c.hal.storage.sc_resetting=true;
  else if(stage==18)c.fNetIf=nullptr;
  else fault.social=false;
  unsigned before=publications;
  assert(!ownerCreate(c)); assert(!c.awdlInterface&&!c.awdlStarted&&!c.awdlAttached&&!c.awdlCreating&&!c.awdlStopping);
  assert(publications==before); ownerStop(c); noResources();
 }
 resetFaults(); AirportMtkWlan c; c.hal.thirdChannel=true;
 publishHook=[&](void *object){auto p=static_cast<IO80211SkywalkInterface *>(object);
  assert(c.awdlStarted&&!c.awdlCreating&&c.properties["MTKAWDLInterfaceReady"]&&p->baseRegistered);
  assert(c.awdlInterface->nativeRegistered&&c.awdlInterface->queuesAdded==3);};
 gate.target=&c; unsigned retains=c.retains,releases=c.releases;
 assert(c.requestAwdlCreation()==0&&c.refs==1&&c.retains==retains+1&&c.releases==releases+1);
 auto p=c.awdlInterface; assert(p&&p->macAgentCalls==1&&!memcmp(p->state.mac,p->agentMac.bytes,6));
 assert(position("native-register")<position("publish-bsd"));
 assert(c.requestAwdlCreation()==0&&c.awdlInterface==p);
 unsigned count=publications; c.hal.storage.sc_resetting=true; assert(ownerCreate(c)&&publications==count);
 c.hal.storage.sc_resetting=false;
 apple80211_virt_if_create_data request{}; request.version=1; request.role=6; memcpy(request.mac,p->state.mac,6);
 assert(gate.enter([&]{return c.createAwdlInterfaceRequest(&request);})==EEXIST&&!strcmp((char *)request.bsd_name,"awdl0"));
 nativeStopHook=[&]{assert(p->stopping&&!p->nativeStarted&&!p->nativeRegistered);c.stopAwdlInterface();};
 trace.clear(); ownerStop(c); nativeStopHook=nullptr; publishHook=nullptr;
 assert(position("timer-disable")<position("native-stop")&&position("native-stop")<position("detach")&&position("detach")<position("base-free"));
 ownerStop(c); noResources();
 // Role 6 remains independent of infrastructure association state.
 for(int state=0;state<=4;++state) {
  resetFaults(); AirportMtkWlan a; a.hal.ic.ic_state=state;
  apple80211_virt_if_create_data d{};d.version=1;d.role=6;d.mac[0]=2;d.mac[5]=7;
  assert(gate.enter([&]{return a.createAwdlInterfaceRequest(&d);})==0&&a.hal.ic.ic_state==state);
  assert(gate.enter([&]{return a.createVirtualInterface(nullptr,6);})==a.awdlInterface);
  assert(!gate.enter([&]{return a.createVirtualInterface(nullptr,5);})); ownerStop(a);noResources();
 }
 puts("PASS init-start: six partial-init/free stages; 20 creation failure stages; real wrapper/recursive MAC startup; registration/readiness/BSD ordering; idempotent role-6 creation; owner shutdown-stop-detach-release and retains.");
}
IOSkywalkPacket *txPacket(IOSkywalkPacketBufferPool *pool) { auto p=new IOSkywalkPacket(pool);p->tx=p->prepared=true;return p; }
void checkDatapath() {
 resetFaults(); {
  NativeFixture d; auto refs=d.refs.load(); unsigned retain=d.retains,release=d.releases;
  gate.enter([&]{d.disableDatapath();return 0;}); assert(!d.dataPathEnabled);
  fault.nativeEnable=kIOReturnError; assert(d.enable(7)==kIOReturnError&&!d.dataPathEnabled);fault.nativeEnable=0;
  d.nativeRegistered=false; assert(d.enable(7)==kIOReturnNotReady);d.nativeRegistered=true;
  d.queuesAdded=2;assert(d.enable(7)==kIOReturnNotReady);d.queuesAdded=3;
  _fCommandGate=nullptr; assert(d.enable(7)==kIOReturnNotReady);d.enableDatapath();assert(!d.dataPathEnabled);_fCommandGate=&gate;
  trace.clear(); assert(d.enable(7)==0&&d.dataPathEnabled);
  assert(position("queue-enable-"+std::to_string(d.txComplete->id))<position("queue-enable-"+std::to_string(d.rxComplete->id)));
  assert(position("queue-enable-"+std::to_string(d.rxComplete->id))<position("queue-enable-"+std::to_string(d.txSubmit->id)));
  uint8_t bytes[64]{}; auto rx=rxTransferred;
  gate.enter([&]{d.inputNative(bytes,sizeof(bytes));return 0;});assert(rxTransferred==rx+1&&d.nativeRx==1);
  for(bool *flag:{&fault.allocation,&fault.prepare,&fault.buffers,&fault.offset,&fault.length,&fault.address,&fault.rxEnqueue}) {
   *flag=true;gate.enter([&]{d.inputNative(bytes,sizeof(bytes));return 0;});*flag=false;assert(!packetsLive&&d.nativeRx==1);
  }
  // Preserve deferred TX bookkeeping, bounded batching, and backpressure intent.
  std::vector<IOSkywalkPacket *> batch;for(unsigned i=0;i<129;++i)batch.push_back(txPacket(d.txPool));
  d.enabled=false; unsigned done=txCompleted;
  assert(gate.enter([&]{return d.submitNative(&d,d.txSubmit,batch.data(),129,nullptr);})==128);
  assert(d.completionCount==128&&txCompleted==done);batch.back()->release();
  nativeDisableHook=[&]{assert(!d.dataPathEnabled&&!d.txSubmit->enabled&&!d.rxComplete->enabled&&!d.completionCount);};
  trace.clear();assert(d.disable(7)==0&&txCompleted==done+128&&!d.txComplete->enabled);
  nativeDisableHook=nullptr;
  assert(position("queue-disable-"+std::to_string(d.txSubmit->id))<position("native-disable"));
  assert(d.disable(7)==0&&d.enable(7)==0&&d.dataPathEnabled);
  // Drop completion queued by a second request during the disabling firmware wait.
  d.radio.wait=[&]{int event=0;gate.sleepHook=[&](void *,uint64_t,unsigned){
   auto p=txPacket(d.txPool); assert(gate.runAction([](OSObject *,void *a,void *packet,void *,void *){
    auto q=static_cast<AirportMtkAwdlInterface *>(a);auto p=static_cast<IOSkywalkPacket *>(packet);
    assert(q->submitNative(q,q->txSubmit,&p,1,nullptr)==1);return kIOReturnSuccess;},&d,p)==0);
   return THREAD_AWAKENED;};
   gate.commandSleep(&event,clockUS.load()+1000,THREAD_UNINT);gate.sleepHook=nullptr;
  };
  assert(d.setSync(false)==0&&!d.completionCount&&!d.timer->pending&&!packetsLive);d.radio.wait=nullptr;
  // Interface/link wrappers dispatch even for zero-valued enums and options.
  assert(d.setInterfaceEnable(true)==0);fault.linkReport=kIOReturnError;assert(d.setInterfaceEnable(true)==kIOReturnError);fault.linkReport=0;
  assert(d.setLinkState(kIO80211NetworkLinkDown,0));fault.nativeLink=false;assert(!d.setLinkState(kIO80211NetworkLinkUp,123));fault.nativeLink=true;
  assert(d.refs==refs&&d.retains-retain==d.releases-release&&gate.maxDepth>=3);
  ether_addr changed{{2,9,8,7,6,5}},invalid{{1,0,0,0,0,0}};
  auto seq=d.state.ownSequence;assert(d.setMacAddress(invalid)==kIOReturnBadArgument);
  assert(d.setMacAddress(changed)==0&&!memcmp(d.state.mac,changed.bytes,6)&&!memcmp(&seq,&d.state.ownSequence,sizeof(seq)));
  assert(d.setSync(true)==0&&d.setMacAddress(changed)==kIOReturnBusy);assert(d.setSync(false)==0);
  // A synchronous native-enable callback tears down before any queue activation.
  nativeEnableHook=[&]{d.shutdown();};assert(d.enable(0)==kIOReturnNotReady&&!d.dataPathEnabled);nativeEnableHook=nullptr;
  assert(d.setInterfaceEnable(true)==kIOReturnNotReady&&!d.setLinkState(kIO80211NetworkLinkUp,0));
 } noResources();
 puts("PASS datapath: public native/BSD/interface/link/MAC dispatch; recursive gates; queue ordering; partial guards; RX cleanup; TX defer/backpressure/second drain; native reentrant shutdown; balanced retains.");
}
'''

SYNC_TESTS = r'''
void checkSync() {
 resetFaults(); {
  NativeFixture d; assert(d.setSync(false)==0);
  struct Wait {std::mutex lock;std::condition_variable cv;bool begun=false,second=false,release=false;unsigned depth=0;} w;
  int firmwareEvent;
  gate.sleepHook=[&](void *event,uint64_t,unsigned depth){
   assert(!gateDepth&&!heldHalLocks);std::unique_lock<std::mutex> lock(w.lock);
   if(event==&firmwareEvent){w.begun=true;w.depth=depth;w.cv.notify_all();assert(w.cv.wait_for(lock,std::chrono::seconds(3),[&]{return w.release;}));}
   else {w.second=true;w.cv.notify_all();w.cv.wait_for(lock,std::chrono::milliseconds(2));}
   return THREAD_AWAKENED;
  };
  std::atomic<bool> first{true};
  d.radio.onEnable=[&](bool value){if(value&&first.exchange(false))gate.commandSleep(&firmwareEvent,clockUS.load()+10000,THREAD_UNINT);};
  IOReturn a=99,b=99;
  // Real nested native-interface callback puts the first firmware wait at depth >= 3.
  nativeInterfaceHook=[&]{if(first.load())assert(d.setSync(true)==kIOReturnAborted);};
  std::thread one([&]{a=d.setInterfaceEnable(true);});
  {std::unique_lock<std::mutex> lock(w.lock);assert(w.cv.wait_for(lock,std::chrono::seconds(3),[&]{return w.begun;}));}
  std::thread two([&]{b=d.setSync(true);});
  {std::unique_lock<std::mutex> lock(w.lock);assert(w.cv.wait_for(lock,std::chrono::seconds(3),[&]{return w.second;}));assert(w.depth>=2);w.release=true;w.cv.notify_all();}
  one.join();two.join();nativeInterfaceHook=nullptr;d.radio.onEnable=nullptr;gate.sleepHook=nullptr;
  assert(a==0&&b==0&&d.enabled&&d.txSession&&d.syncWaits&&d.refs==1&&d.retains==d.releases);
  // Same-thread recursion is Busy, not an invented transition/no-op.
  d.radio.onEnable=[&](bool){assert(d.setSync(false)==kIOReturnBusy);};assert(d.setSync(true)==0);d.radio.onEnable=nullptr;
  // A foreign-owner request remains bounded at exactly the production 3s limit.
  unsigned foreign;d.syncOwner=&foreign;uint64_t begin=clockUS;
  assert(d.setSync(false)==kIOReturnTimeout&&clockUS.load()-begin==3000000);d.syncOwner=nullptr;
  // Hardware status is propagated. Disable failure still closes local traffic.
  d.radio.enableStatus=kIOReturnError;assert(d.setSync(false)==kIOReturnError&&!d.enabled&&!d.txSession);d.radio.enableStatus=0;
  assert(d.setSync(true)==0);
 } noResources();
 // Serialize a second disable behind an enable while the full gate is released.
 resetFaults(); {
  NativeFixture d;assert(d.setSync(false)==0);
  std::mutex lock;std::condition_variable cv;bool begun=false,second=false,release=false;int event;
  gate.sleepHook=[&](void *p,uint64_t,unsigned){std::unique_lock<std::mutex> guard(lock);
   if(p==&event){begun=true;cv.notify_all();assert(cv.wait_for(guard,std::chrono::seconds(3),[&]{return release;}));}
   else {second=true;cv.notify_all();cv.wait_for(guard,std::chrono::milliseconds(2));}return THREAD_AWAKENED;};
  d.radio.onEnable=[&](bool value){if(value)gate.commandSleep(&event,clockUS.load()+1000,THREAD_UNINT);};
  IOReturn a=99,b=99;std::thread one([&]{a=d.setSync(true);});
  {std::unique_lock<std::mutex> g(lock);assert(cv.wait_for(g,std::chrono::seconds(3),[&]{return begun;}));}
  std::thread two([&]{b=d.setSync(false);});
  {std::unique_lock<std::mutex> g(lock);assert(cv.wait_for(g,std::chrono::seconds(3),[&]{return second;}));release=true;cv.notify_all();}
  one.join();two.join();d.radio.onEnable=nullptr;gate.sleepHook=nullptr;assert(a==kIOReturnAborted&&b==0&&!d.enabled&&!d.txSession&&!d.syncOwner);
 } noResources();
 // Owner teardown genuinely enters while a retained public request sleeps.
 resetFaults(); {
  AirportMtkWlan c;assert(ownerCreate(c));auto p=c.awdlInterface;p->retain();
  std::mutex lock;std::condition_variable cv;bool begun=false,release=false;int event;
  gate.sleepHook=[&](void *address,uint64_t,unsigned){assert(address==&event&&!gateDepth);
   std::unique_lock<std::mutex> g(lock);begun=true;cv.notify_all();assert(cv.wait_for(g,std::chrono::seconds(3),[&]{return release;}));return THREAD_AWAKENED;};
  c.hal.onEnable=[&](bool value){if(value)gate.commandSleep(&event,clockUS.load()+10000,THREAD_UNINT);};
  IOReturn status=99;std::thread request([&]{status=p->setSync(true);});
  {std::unique_lock<std::mutex> g(lock);assert(cv.wait_for(g,std::chrono::seconds(3),[&]{return begun;}));}
  ownerStop(c);assert(!c.awdlInterface&&p->stopping&&!p->nativeStarted&&!p->nativeRegistered&&!p->txPool&&p->refs==2);
  {std::lock_guard<std::mutex> g(lock);release=true;cv.notify_all();}request.join();
  c.hal.onEnable=nullptr;gate.sleepHook=nullptr;
  assert(status==kIOReturnNotReady&&!p->enabled&&!p->txSession&&!p->syncOwner&&p->refs==1);
  p->release();noResources();
 }
 // Real grant duration: 41TU disable stops TX immediately, waits to >=54TU,
 // then emits native end. Revocation/teardown do not fabricate the future time.
 for(unsigned scenario=0;scenario<4;++scenario) {
  resetFaults(); {
   NativeFixture d;d.state.init((const uint8_t *)"\x02\x01\x02\x03\x04\x05",clockUS.load());d.radio.give(1,6,70);d.serviceInGate();
   assert(d.awTimeline.open);uint64_t start=d.awTimeline.lastStart;clockUS=start+41*1024;
   uint64_t disableAt=clockUS;unsigned before=gate.sleeps;
   if(scenario==1)d.radio.grant.cancel();
   if(scenario==2)d.radio.on=false;
   if(scenario==3)gate.sleepHook=[&](void *,uint64_t,unsigned){assert(gate.runAction([](OSObject *,void *p,void *,void *,void *){
     static_cast<AirportMtkAwdlInterface *>(p)->shutdown();return kIOReturnSuccess;},&d)==0);return THREAD_AWAKENED;};
   IOReturn status=d.setSync(false);gate.sleepHook=nullptr;
   assert(status==(scenario==3?kIOReturnNotReady:kIOReturnSuccess));
   assert(!d.enabled&&!d.awTimeline.open&&d.events.size()==2&&d.events[1].id==43);
   uint32_t end=mtkawdl::be32(d.events[1].bytes.data()+4);
   if(!scenario)assert(end>=start+54*1024&&end<=clockUS.load()&&gate.sleeps>before&&clockUS.load()>disableAt);
   else assert(end==disableAt);
  }noResources();
 }
 puts("PASS sync: enable/enable and enable/disable serialization; recursive-depth gate release; same-thread Busy; bounded timeout; real status; retained owner teardown during firmware wait; physical 41-to-54TU disable and immediate revocation/reset/teardown.");
}
'''

ORIGIN_TESTS = r'''
void checkOrigins() {
 resetFaults(); {
  NativeFixture d;uint8_t peer[6]={2,1,2,3,4,6};
  gate.enter([&]{assert(d.observeDataPeer(peer,clockUS.load())>=0);return 0;});
  uint64_t gen=d.dataRxAdmissionGeneration;
  gate.enter([&]{assert(d.cacheDataPeer(peer,true,clockUS.load()));assert(d.trafficDataPeer(peer,true,clockUS.load()));return 0;});
  assert(d.dataRxAdmissionGeneration>gen);
  d.dataRxAdmissionGeneration=UINT64_MAX;gate.enter([&]{d.resetDataPeers();d.invalidateDataRxAdmission();return 0;});assert(!d.dataRxAdmissionGeneration);
  d.dataRxAdmissionGeneration=1;d.dataRxPeerProgramming=~0u;
  gate.enter([&]{assert(d.programDataPeer(peer,true)==kIOReturnNotReady);return 0;});assert(!d.dataRxAdmissionGeneration);
  d.dataRxPeerProgramming=0;d.dataRxAdmissionGeneration=10;
  d.radio.onPeer=[&](bool){assert(d.dataRxPeerProgramming==1&&d.dataRxAdmissionGeneration==11);};
  assert(gate.enter([&]{return d.programDataPeer(peer,true);})==0&&d.dataRxAdmissionGeneration==12&&!d.dataRxPeerProgramming);d.radio.onPeer=nullptr;
  // Full-table traffic handoff drops old ownership first; real Rust DataPeers.
  d.dataPeers.reset();apple80211_awdl_peer_traffic_registration traffic{};traffic.active=1;memcpy(traffic.addr,peer,6);
  assert(d.setAWDL_PEER_TRAFFIC_REGISTRATION(&traffic)==0);
  for(unsigned n=1;n<16;++n){uint8_t other[6]={2,3,4,5,6,uint8_t(n)};assert(d.dataPeers.setCached(other,true,clockUS.load()));}
  uint8_t replacement[6]={2,9,9,9,9,9};memcpy(traffic.addr,replacement,6);
  assert(d.setAWDL_PEER_TRAFFIC_REGISTRATION(&traffic)==0&&!memcmp(d.trafficPeer,replacement,6));
  assert(d.dataPeers.find(peer,clockUS.load())<0);
  // Captured original queues/pools/batch survive recursive release+replacement.
  auto oldTx=d.txPool,oldRx=d.rxPool;oldTx->retain();oldRx->retain();
  auto packet=txPacket(oldTx);d.completions[d.completionCount++]=packet;
  bool nested=false;auto oldQueue=d.txSubmit;
  oldQueue->onDisable=[&]{if(nested)return;nested=true;
   assert(!d.txPool&&!d.rxPool&&!d.txSubmit&&!d.txComplete&&!d.rxComplete&&!d.completionCount&&!d.queuesAdded);
   d.releaseDataPath();assert(d.createDataPath());
  };
  unsigned done=txCompleted;gate.enter([&]{d.releaseDataPath();return 0;});
  assert(nested&&txCompleted==done+1&&d.txPool!=oldTx&&d.rxPool!=oldRx&&d.queuesAdded==3);
  assert(d.txPool->refs==3&&d.rxPool->refs==2&&oldTx->refs==1&&oldRx->refs==1);oldTx->release();oldRx->release();
  assert(gate.enter([&]{d.enableDatapath();return 0;})==0);
  // Real allocation-origin retain/currentPeer guard rejects readiness/identity/
  // admission changes; its destructor frees only the origin, never replacement.
  d.dataRxAdmissionGeneration=100;d.dataPeers.reset();assert(d.dataPeers.setCached(peer,true,clockUS.load()));
  uint8_t destination[6];memcpy(destination,d.state.mac,6);
  uint8_t ipv6[48]={0xaa,0xaa,3,0,0,0,0x86,0xdd,0x60};
  mtkawdl::DataView view{};view.source=peer;view.destination=destination;view.payload=ipv6;view.length=sizeof(ipv6);
  {AirportMtkAwdlInterface::NativeRxAllocation allocation(&d,d.dataRxAdmissionGeneration,d.state.mac);
   assert(d.refs==2&&allocation.pool==d.rxPool&&allocation.queue==d.rxComplete);
   assert(allocation.currentPeer(peer,view)>=0);
   d.dataRxPeerProgramming=1;assert(allocation.currentPeer(peer,view)<0);d.dataRxPeerProgramming=0;
   d.syncOwner=IOThreadSelf();assert(allocation.currentPeer(peer,view)<0);d.syncOwner=nullptr;
   gate.enter([&]{d.invalidateDataRxAdmission();return 0;});assert(allocation.currentPeer(peer,view)<0);
  }assert(d.refs==1);
  auto origin=d.rxPool;origin->retain();unsigned reclaimed=rxReclaimed;
  origin->onAllocate=[&]{origin->onAllocate=nullptr;d.releaseDataPath();assert(d.createDataPath());d.enableDatapath();};
  gate.enter([&]{assert(!d.inputNativeView(view));return 0;});assert(rxReclaimed==reclaimed+1&&d.rxPool!=origin&&origin->refs==1);origin->release();
 }noResources();
 puts("PASS origins: production generation/programming fail-closed exhaustion and before/after invalidation; full native traffic handoff; detached metadata and original batch/pool/queue cleanup under recursion/replacement; allocation-origin retains and cancellation.");
}
void checkModulePoll() {
 resetFaults(); {
  NativeFixture d;auto &radio=d.radio;auto &com=radio.storage;
  assert(d.txSession&&radio.awdlTxSession()==d.txSession);
  for(bool *flag:{&com.sc_awdl_stopping,&com.sc_resetting,&com.sc_detaching}){*flag=true;assert(!radio.awdlTxSession());*flag=false;}
  com.sc_awdl_enabled=false;assert(!radio.awdlTxSession());com.sc_awdl_enabled=true;
  auto saved=radio.com;radio.com=nullptr;MtkMwx::AwdlTxResult out{};assert(!radio.awdlTxSession()&&!radio.awdlTakeTxResult(out)&&!radio.awdlInterfaceReady());radio.com=saved;
  auto savedLock=com.sc_awdl_lock;com.sc_awdl_lock=nullptr;assert(!radio.awdlTxSession()&&!radio.awdlTakeTxResult(out)&&!radio.awdlInterfaceReady());com.sc_awdl_lock=savedLock;
  for(bool *flag:{&com.sc_awdl_stopping,&com.sc_resetting,&com.sc_detaching,&com.sc_ic.ic_wcl_mac_reconfig}){*flag=true;assert(!radio.awdlInterfaceReady());*flag=false;}
  unsigned savedFlags=com.sc_ic.ic_if.if_flags;
  com.sc_ic.ic_if.if_flags=0;assert(!radio.awdlInterfaceReady());
  com.sc_ic.ic_if.if_flags=IFF_RUNNING;assert(radio.awdlInterfaceReady());
  MtkMwx::AwdlSyncSnapshot snapshot{};assert(radio.awdlSyncSnapshot(snapshot));
  assert(!snapshot.wantedUp&&!snapshot.ready&&!snapshot.session);
  com.sc_ic.ic_if.if_flags=savedFlags;
  auto result=[&](uint64_t session,bool failed){MtkMwx::AwdlTxResult r{};r.address[0]=2;r.address[5]=7;r.failed=failed;r.reason=failed?9:0;r.session=session;return r;};
  com.sc_awdl_native_tx.injected.push_back(result(d.txSession,true));
  d.controller.onMessage=[&](unsigned id){assert(id==0x45&&!heldHalLocks);assert(radio.awdlTxSession()==d.txSession);};
  d.serviceInGate();assert(d.txStatusCompletions==1&&d.controller.messages.back().bytes.size()==7&&d.controller.messages.back().bytes[6]==1);
  assert(com.sc_awdl_native_tx.expires==com.sc_awdl_native_tx.takes&&com.sc_awdl_native_tx.expiredAt==clockUS.load());
  uint64_t discarded=d.txStatusDiscarded;
  for(unsigned scenario=0;scenario<5;++scenario){
   uint64_t original=d.txSession;bool enabled=d.enabled,stopping=d.stopping;uint64_t live=com.sc_awdl_native_session;
   auto r=result(original,false);
   if(scenario==0)d.enabled=false;if(scenario==1)d.stopping=true;if(scenario==2)d.txSession=0;
   if(scenario==3)r.session=original+1;if(scenario==4)com.sc_awdl_native_session=original+1;
   com.sc_awdl_native_tx.injected.push_back(r);gate.enter([&]{d.drainTxStatus();return 0;});
   assert(d.txStatusDiscarded==++discarded&&d.txStatusCompletions==1);
   d.txSession=original;d.enabled=enabled;d.stopping=stopping;com.sc_awdl_native_session=live;
  }
  // Inject at the terminal-result boundary only, not a replica of its state table.
  // A recursive producer keeps results available; one drain is STILL at most 32.
  d.controller.onMessage=[&](unsigned id){assert(id==0x45&&!heldHalLocks);com.sc_awdl_native_tx.injected.push_back(result(d.txSession,false));};
  com.sc_awdl_native_tx.injected.push_back(result(d.txSession,false));uint64_t completions=d.txStatusCompletions;
  gate.enter([&]{d.drainTxStatus();return 0;});assert(d.txStatusCompletions==completions+32&&com.sc_awdl_native_tx.injected.size()==1);
  // Lifecycle callback during native message rejects the following result.
  d.controller.onMessage=[&](unsigned){d.shutdown();};com.sc_awdl_native_tx.injected.push_back(result(d.txSession,false));
  gate.enter([&]{d.drainTxStatus();return 0;});assert(d.stopping&&d.txStatusDiscarded==discarded+1&&!d.txSession);d.controller.onMessage=nullptr;
 }noResources();
 // The module allocator is shared across objects/reattachments and exhausted
 // permanently. No counter reset or alternative session allocator is used.
 assert(mwx_awdl_native_history_lock);
 uint64_t serial=mwx_awdl_native_session_serial;
 std::set<uint64_t> sessions;std::mutex resultLock;std::vector<std::thread> threads;
 for(unsigned i=0;i<8;++i)threads.emplace_back([&]{for(unsigned n=0;n<16;++n){auto s=mwx_awdl_native_new_session();std::lock_guard<std::mutex> guard(resultLock);assert(s&&sessions.insert(s).second);}});
 for(auto &t:threads)t.join();assert(sessions.size()==128&&mwx_awdl_native_session_serial==serial+128);
 mwx_awdl_native_session_serial=UINT64_MAX-1;assert(mwx_awdl_native_new_session()==UINT64_MAX);
 for(unsigned n=0;n<4;++n)assert(!mwx_awdl_native_new_session());assert(mwx_awdl_native_session_serial==UINT64_MAX);
 {MtkMwx reattached;assert(!mwx_awdl_native_new_session());}
 puts("PASS module-poll: extracted module lock/session allocator; concurrent uniqueness and permanent exhaustion; real HAL receiver/session/readiness/expire-take lock boundaries; service polling; stale/disabled/zero-session rejection; native callback unlocked and 32-result bound. UMI table intentionally not replicated.");
}
'''


BSD_TESTS = r'''
void checkBSD() {
 resetFaults(); {
  NativeFixture d; Ifnet a,b; bsdBoundary=BsdBoundary{};
  unsigned calls=bsdBoundary.lladdrCalls;
  d.setBSDInterface(&a); assert(d.bsd==&a&&!memcmp(a.mac,d.state.mac,6));
  assert(bsdBoundary.lladdrCalls==calls+1);
  auto saved=a; d.setBSDInterface(nullptr); assert(!d.bsd&&bsdBoundary.lladdrCalls==calls+1);
  d.setBSDInterface(&b); assert(d.bsd==&b&&!memcmp(b.mac,d.state.mac,6)&&!memcmp(a.mac,saved.mac,6));
  d.cachedAssists=~0u; assert(d.getHardwareAssists()==0&&d.cachedAssists==~0u);
  for(unsigned mask:{0u,1u,~0u}) {
   d.cachedAssists=~0u; d.setHardwareAssists(0x12345678,mask);
   assert(!d.cachedAssists&&bsdBoundary.assistsValue==0&&bsdBoundary.assistsMask==~0u);
  }
  const unsigned mask=IFCAP_HWCSUM|IFCAP_TSO|IFCAP_LRO|IFCAP_CSUM_PARTIAL|IFCAP_CSUM_ZERO_INVERT|0x8000u;
  for(unsigned scenario=0;scenario<6;++scenario) {
   bsdBoundary=BsdBoundary{}; Ifnet p; d.cachedAssists=~0u;
   p.enabled=0xafffffff; p.supported=0xdfffffff;
   unsigned active=p.enabled,supported=p.supported;
   for(const char *key:{"MTKAWDLBSDOffloadStatus","MTKAWDLBSDPreparedOffload","MTKAWDLDriverHardwareAssists"})d.removeProperty(key);
   if(scenario==1)bsdBoundary.prepareStatus=kIOReturnTimeout;
   if(scenario>=2&&scenario<=4)bsdBoundary.failStep=scenario-1;
   if(scenario==5)bsdBoundary.lie=true;
   auto status=d.prepareBSDInterface(&p,0x12345678);
   assert(bsdBoundary.assistsCalls==1&&!d.cachedAssists&&bsdBoundary.assistsValue==0&&bsdBoundary.assistsMask==~0u);
   assert(bsdBoundary.prepareCalls==1&&bsdBoundary.options==0x12345678);
   if(scenario==1) {
    assert(status==kIOReturnTimeout&&!bsdBoundary.step&&p.offload==0x37f&&p.enabled==active&&p.supported==supported);
    assert(!d.properties.count("MTKAWDLBSDOffloadStatus")&&!d.properties.count("MTKAWDLDriverHardwareAssists")); continue;
   }
   assert(status==(scenario?kIOReturnError:kIOReturnSuccess));
   assert(bsdBoundary.step==(scenario>=2&&scenario<=4?scenario-1:3));
   unsigned expected=scenario==5?EIO:scenario?EINVAL:0;
   assert(d.properties["MTKAWDLBSDOffloadStatus"]==expected);
   assert(d.properties["MTKAWDLBSDPreparedOffload"]==p.offload&&d.properties["MTKAWDLBSDCurrentOffload"]==p.offload);
   assert(d.properties["MTKAWDLBSDPreparedCapabilities"]==p.supported&&d.properties["MTKAWDLBSDCurrentCapabilities"]==p.supported);
   assert(d.properties["MTKAWDLBSDPreparedEnabled"]==p.enabled&&d.properties["MTKAWDLBSDCurrentEnabled"]==p.enabled);
   assert(d.properties["MTKAWDLDriverHardwareAssists"]==0);
   if(!scenario)assert(!p.offload&&p.enabled==(active&~mask)&&p.supported==(supported&~mask));
   if(scenario==2)assert(p.enabled==active&&p.supported==supported);
   if(scenario==3)assert(p.enabled==active&&p.supported==supported);
   if(scenario==4)assert(p.enabled==(active&~mask)&&p.supported==supported);
  }
  d.removeProperty("MTKAWDLBSDCurrentOffload"); d.cachedAssists=7;
  d.publishBSDOffloadState(nullptr); assert(d.properties["MTKAWDLDriverHardwareAssists"]==7&&!d.properties.count("MTKAWDLBSDCurrentOffload"));
  d.setBSDInterface(nullptr); bsdBoundary=BsdBoundary{};
 } noResources();
 puts("PASS BSD: actual setBSDInterface attach/clear/replace; getHardwareAssists zero without cache mutation; setHardwareAssists clears full native cache; prepareBSDInterface options/status, six success/native-failure/setter-failure/lying-readback paths, capability preservation and diagnostic snapshots. Native/ifnet calls are models.");
}
'''

PACKED_TEST_PREFIX = r'''
void checkPackedSequence(bool oldReference) {
 // align1 containing objects may legitimately start at ANY byte. Placement new
 // establishes lifetime without lying about apple80211_channel's align4.
 alignas(4) unsigned char storage[sizeof(apple80211_awdl_sync_channel_sequence)+4];
 memset(storage,0xa5,sizeof(storage));
 auto ptr=new(storage+1) apple80211_awdl_sync_channel_sequence;
 auto &channelSequence=*ptr;
 struct { struct { uint8_t channels[16]; } sequence; } state{};
 for(unsigned i=0;i<16;++i)state.sequence.channels[i]=i%3==0?6:i%3==1?44:149;
 unsigned char expected[sizeof(channelSequence)]; memcpy(expected,ptr,sizeof(expected));
 for(unsigned i=0;i<16;++i) {
  apple80211_channel c; memcpy(&c,expected+offsetof(apple80211_awdl_sync_channel_sequence,seqs)+i*sizeof(c),sizeof(c));
  c.channel=state.sequence.channels[i]; c.flags=APPLE80211_C_FLAG_20MHZ|(c.channel<=14?APPLE80211_C_FLAG_2GHZ:APPLE80211_C_FLAG_5GHZ);
  memcpy(expected+offsetof(apple80211_awdl_sync_channel_sequence,seqs)+i*sizeof(c),&c,sizeof(c));
 }
 assert(alignof(apple80211_awdl_sync_channel_sequence)==1&&alignof(apple80211_channel)==4);
 assert(offsetof(apple80211_awdl_sync_channel_sequence,seqs)==12&&sizeof(apple80211_channel)==12);
 assert((uintptr_t(ptr)+12)%4==1);
 if(oldReference) {
  // Exactly the former production expressions: expected strict UBSan failure.
  for(unsigned i=0;i<16;++i) {
   auto &c=channelSequence.seqs[i]; c.channel=state.sequence.channels[i];
   c.flags=APPLE80211_C_FLAG_20MHZ | (c.channel<=14 ? APPLE80211_C_FLAG_2GHZ : APPLE80211_C_FLAG_5GHZ);
  }
 } else {
'''
PACKED_TEST_SUFFIX = r'''
 }
 assert(!memcmp(expected,ptr,sizeof(expected))&&storage[0]==0xa5&&storage[sizeof(storage)-1]==0xa5);
 resetFaults(); {
  NativeFixture d;
  printf("MODEL alignment: interface size=%zu align=%zu channelSequence offset=%zu address mod4=%zu seqs offset=%zu stride=%zu address mod4=%zu\n",
   sizeof(AirportMtkAwdlInterface),alignof(AirportMtkAwdlInterface),size_t((uint8_t *)&d.channelSequence-(uint8_t *)&d),uintptr_t(&d.channelSequence)%4,
   offsetof(apple80211_awdl_sync_channel_sequence,seqs),sizeof(apple80211_channel),(uintptr_t(&d.channelSequence)+12)%4);
 } noResources();
 puts("PASS packed-sequence: extracted production loop on deliberately misaligned legitimate align1 storage; all version/padding/unused descriptors/tail bytes preserved; actual init exercised in model layout, not a native runtime layout claim.");
}
'''


def generate(ex: Extractor) -> str:
    names = [n for group in INTERFACE_GROUPS.values() for n in group.split()]
    if len(names) != len(set(names)) or len(names) > 120:
        raise ValueError("invalid extraction inventory")
    header = ex.text(INTERFACE_HEADER)
    public = header[header.index("public:") + len("public:"):header.index("private:")]
    wanted = set(names) | {"nativeDataPathAvailable", "getBSDNamePrefix"}
    declarations = [line for line in public.splitlines() if any(re.search(r"\b"+re.escape(n)+r"\s*\(", line) for n in wanted)]
    selected = "\n".join(declarations)
    # Mock pure virtual API declarations, not altered production declarations.
    explicit = {"start", "stop", "free", "enable", "disable", "enableDatapath", "disableDatapath",
                "getHardwareAssists", "setHardwareAssists", "prepareBSDInterface", "getTxSubQueue",
                "getTxCompQueue", "getRxCompQueue", "getTxPacketPool", "getRxPacketPool",
                "getBSDNamePrefix", "setMacAddress", "getHardwareAddress", "getSelfMacAddr",
                "isChipInterfaceReady", "setInterfaceEnable", "setLinkState"}
    pure = []
    for declaration in declarations:
        if "override;" in declaration:
            name = re.search(r"\b(\w+)\s*\(", declaration).group(1)
            if name not in explicit:
                pure.append("virtual " + declaration.strip().replace("override;", "=0;"))
    private_start = header.index("private:") + len("private:")
    private_end = header.rindex("};")
    # Exact private declarations; only host assertion visibility differs.
    private = ex.span(INTERFACE_HEADER, private_start, private_end, "complete private declarations")
    owner_header = ex.text(OWNER_HEADER)
    owner_start = owner_header.index("    IO80211VirtualInterface *createVirtualInterface(")
    owner_end = owner_header.index("#endif", owner_start)
    owner_decl = ex.span(OWNER_HEADER, owner_start, owner_end, "native owner declarations and fields")
    hal_header = ex.text(HAL_HEADER)
    abi_start = hal_header.index("    typedef bool (*AwdlReceive)")
    abi_struct = hal_header.index("    struct AwdlTxResult", abi_start)
    abi_end = hal_header.index(";", balanced_end(hal_header, hal_header.index("{", abi_struct))) + 1
    abi = ex.span(HAL_HEADER, abi_start, abi_end, "HAL receive/result ABI")
    text = PREAMBLE
    text += ex.structure(VAR, "apple80211_channel")
    text += "\n".join(ex.structure(IOCTL, name) for name in STRUCT_NAMES)
    text += HAL_SUPPORT + abi + ex.structure(HAL_HEADER, "AwdlRocTiming")
    text += ex.structure(HAL_HEADER, "AwdlSyncSnapshot") + ex.structure(HAL_HEADER, "AwdlSyncGuard") + HAL_AFTER_ABI
    text += RESOURCES + ex.structure(NATIVE_ETH, "RegistrationInfo") + NATIVE_AFTER_REGISTRATION
    text += "\n".join(pure) + "\n};\n" + OWNER_SUPPORT + owner_decl + "\n};\n"
    text += "class AirportMtkAwdlInterface:public IO80211AWDLProtocol {\n OSDeclareDefaultStructors(AirportMtkAwdlInterface)\npublic:\n"
    text += selected + "\npublic: // host assertions expose, but do not rewrite, private declarations\n" + private + "\n};\n"
    module = ex.text(HAL)
    module_start = module.index("static uint32_t mwx_awdl_native_issued[3][4096];")
    module_end = module.index("static bool mwx_awdl_native_history_init", module_start)
    text += ex.span(HAL, module_start, module_end, "module-lifetime native history/session declarations")
    text += ex.function(HAL, "mwx_awdl_native_history_init") + ex.function(HAL, "mwx_awdl_native_new_session")
    text += ex.function(INTERFACE, "awdlNow") + ex.function(HAL, "mwx_awdl_now")
    text += "\nusing mwx_softc=MtkMwx::Com; using ieee80211com=MtkMwx::IC;\n"
    for name in ["mwx_awdl_infra_joining", "mwx_awdl_sync_live", "mwx_awdl_sync_status_locked", "mwx_awdl_sync_status"]:
        text += ex.function(HAL, name)
    for name in ["awdlSetReceiver", "awdlTxSession", "awdlTakeTxResult", "awdlInterfaceReady", "awdlSyncSnapshot", "awdlRocTimingSnapshot"]:
        text += ex.function(HAL, "MtkMwx::" + name)
    text += POST_TYPES
    for helper in ["awdlTickUS", "awdlScheduleService", "peerScheduleKnown", "awdlAmsduEnabled",
                   "awdlPeerGateMode", "awdlPeerGateSlackUS", "awdlRenewStationOnNoPeer", "amsduZero", "amsduWrite"]:
        text += ex.function(INTERFACE, helper)
    for name in names:
        text += ex.function(INTERFACE, "AirportMtkAwdlInterface::" + name)
    for name in ["NativeRxAllocation", "~NativeRxAllocation", "currentPeer"]:
        text += ex.function(INTERFACE, "AirportMtkAwdlInterface::NativeRxAllocation::" + name)
    text += "\n#undef super\n"
    for name in OWNER_NAMES:
        text += ex.function(OWNER, "AirportMtkWlan::" + name)
    text += '#line 1 "native-lifecycle-host-boundaries-and-assertions"\n' + FIXTURE + SYNC_TESTS + ORIGIN_TESTS + BSD_TESTS
    init = ex.text(INTERFACE)
    start = init.index("    for(unsigned i=0;i<16;++i) {", init.index("    state.sequence=state.ownSequence;"))
    end = balanced_end(init, init.index("{", start))
    text += PACKED_TEST_PREFIX + ex.span(INTERFACE, start, end, "packed-sequence regression: exact init loop") + PACKED_TEST_SUFFIX
    return text


HOST_MAIN = r'''
#include "extracted.cpp"
#line 1 "native-lifecycle-host-main"
int main(int argc,char **argv) {
 assert(argc==2);assert(mwx_awdl_native_history_init());
 std::string name=argv[1];
 if(name=="init-start")checkInitStart();else if(name=="datapath")checkDatapath();
 else if(name=="sync")checkSync();else if(name=="origins")checkOrigins();
 else if(name=="module-poll")checkModulePoll();
 else if(name=="bsd")checkBSD();
 else if(name=="packed-sequence"||name=="old-packed-reference")checkPackedSequence(name=="old-packed-reference");
 else assert(false);
 noResources();assert(!gateDepth&&!heldHalLocks);return 0;
}
'''


def original_program(ex: Extractor, kind: str) -> str:
    # Preserve the literal's real GLOBAL main (including implicit return 0).
    # The type alias affects ONLY assertions, never a production method body.
    fixture = "ControlFixture" if kind == "native-controls" else "NativeFixture"
    marker = "Tahoe KC" if kind == "native-controls" else "const uint8_t local[]"
    prefix = r'''
#include "extracted.cpp"
struct OriginalGateEntry {
 OriginalGateEntry() { assert(mwx_awdl_native_history_init());resetFaults();gate.mutex.lock();++gateDepth; }
 ~OriginalGateEntry() { noResources();assert(gateDepth==1&&!heldHalLocks);--gateDepth;gate.mutex.unlock(); }
} originalGateEntry;
'''
    # WCL's external shared-gate entry is an OS dispatch boundary. Original
    # timer calls still execute tick/service and public wrappers recurse normally.
    return prefix + "\n#define AirportMtkAwdlInterface " + fixture + "\n" + ex.assertions(VENDOR / ("tests/awdl/test-"+kind+".py"), marker) + "\n#undef AirportMtkAwdlInterface\n"


def rust_input_hashes() -> dict[str, str]:
    paths = set((ROOT / "rust").glob("*.toml")) | {ROOT / "rust/Cargo.lock"}
    for crate in (ROOT / "rust").glob("mtk-*"):
        paths.update(crate.rglob("*.rs"))
        paths.update(crate.rglob("*.toml"))
    for folder in [ROOT / "rust/targets", ROOT / "rust/.cargo"]:
        paths.update(p for p in folder.rglob("*") if p.is_file())
    return {str(p.relative_to(ROOT)): digest(p.read_bytes()) for p in sorted(paths)}


def protected_hashes() -> dict[str, str]:
    # Protect production/Rust inputs and upstream fixtures, including new headers
    # and untracked Rust source. Do not hash unrelated workers' local scripts.
    result = subprocess.run(["git", "-C", str(ROOT), "ls-files", "-z", "kext", "rust", "scripts/test-awdl.sh"],
                            check=True, stdout=subprocess.PIPE)
    paths = {ROOT / p.decode() for p in result.stdout.split(b"\0") if p}
    paths |= {p for p in (VENDOR / "tests/awdl").rglob("*") if p.is_file()}
    paths.update((VENDOR / "include/AWDL").glob("*.hpp"))
    paths.add(Path(__file__).resolve())
    paths.update(ROOT / p for p in rust_input_hashes())
    return {str(p.relative_to(ROOT)): digest(p.read_bytes()) for p in sorted(paths)}


def archive_provenance() -> dict:
    if digest(ARCHIVE.read_bytes()) != ARCHIVE_SHA256:
        raise ValueError("permitted retained Wi-Fi archive fingerprint changed")
    before = json.loads((RETAINED / "source-before.json").read_text())["files"]
    stability = json.loads((RETAINED / "source-stability.json").read_text())
    if not stability.get("stable"):
        raise ValueError("retained build source-stability check failed")
    rust = {k: v["sha256"] for k, v in before.items() if k.startswith("rust/")}
    if not rust or any(digest((ROOT / k).read_bytes()) != sha for k, sha in rust.items()):
        raise ValueError("Rust sources no longer match fresh retained build")
    deps = (ARCHIVE.with_suffix(".d")).read_text()
    source_dependencies = [p for p in deps.split(":", 1)[1].split() if p.endswith(".rs")]
    missing = [p for p in source_dependencies if str(Path(p).relative_to(ROOT)) not in rust]
    if missing:
        raise ValueError("archive dependency lacks retained source fingerprint: " + repr(missing))
    return {"archive": str(ARCHIVE), "sha256": ARCHIVE_SHA256, "size": ARCHIVE.stat().st_size,
            "source_snapshot": str(RETAINED / "source-before.json"),
            "source_stability": stability, "rust_sources": rust,
            "archive_rs_dependencies": len(source_dependencies),
            "rust_sanitizer_instrumented": False}


class CommandLog:
    """Small shared runner; commands, complete output and exit codes are retained."""
    def __init__(self, out: Path, env: dict) -> None:
        self.out, self.env, self.commands = out, env, []

    def __call__(self, name: str, command: list[str], timeout: int = 120,
                 run_env: dict | None = None, *, cwd: Path = ROOT) -> int:
        command = [str(arg) for arg in command]
        started = time.monotonic()
        effective = run_env or self.env
        entry = {"name": name, "command": command, "shell_command": shlex.join(command), "cwd": str(cwd),
                 "started_at": utc(), "timeout_seconds": timeout,
                 "environment": {k: effective[k] for k in ["PATH", "ASAN_OPTIONS", "UBSAN_OPTIONS", "SDKROOT", "DEVELOPER_DIR",
                    "RUSTUP_TOOLCHAIN", "RUSTC", "RUSTC_WRAPPER", "RUSTC_WORKSPACE_WRAPPER", "CARGO_NET_OFFLINE"] if k in effective}}
        with (self.out / (name+".log")).open("wb") as log:
            log.write(("$ "+shlex.join(command)+"\nCWD "+str(cwd)+"\n"+json.dumps(entry["environment"])+"\n").encode())
            log.flush()
            try:
                proc = subprocess.run(command, cwd=cwd, env=effective, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
                status = proc.returncode
            except subprocess.TimeoutExpired:
                status = 124
                log.write(b"\nHOST TIMEOUT\n")
            except OSError as error:
                status = 127
                log.write((repr(error)+"\n").encode())
        entry.update({"exit_code": status, "ended_at": utc(), "elapsed_seconds": round(time.monotonic()-started, 6),
                      "log": str(self.out / (name+".log"))})
        self.commands.append(entry)
        (self.out / "commands.json").write_text(json.dumps(self.commands, indent=2)+"\n")
        return status

    def output(self, name: str) -> str:
        return (self.out / (name+".log")).read_text().split("\n", 3)[3]

    def checked(self, name: str, command: list[str], timeout: int = 120, *, cwd: Path = ROOT) -> str:
        status = self(name, command, timeout, cwd=cwd)
        if status:
            raise RuntimeError(f"{name} exited {status}; see {self.out / (name+'.log')}")
        return self.output(name)


def host_environment() -> dict:
    env = os.environ.copy()
    for key in ["CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "LIBRARY_PATH"]:
        env.pop(key, None)
    env.update(ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_stack_use_after_return=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    return env


def build_fresh_archive(out: Path, run: CommandLog) -> dict:
    """Build a new host-only wifi archive; no installs, network or shared target."""
    before = rust_input_hashes()
    (out / "rust-inputs.before.json").write_text(json.dumps(before, indent=2)+"\n")
    channel = re.search(r'^channel\s*=\s*"([^"]+)"',
                        (ROOT / "rust/rust-toolchain.toml").read_text(), re.MULTILINE).group(1)
    installed = run.checked("installed-toolchains", ["rustup", "toolchain", "list"])
    if not any(line.split()[0] == channel or line.split()[0].startswith(channel+"-")
               for line in installed.splitlines() if line.split()):
        raise RuntimeError("Pinned toolchain is not installed (will not download): "+channel)
    env = run.env
    if "DEVELOPER_DIR" not in env and "CommandLineTools" in run.checked("xcode-select", ["xcode-select", "-p"]):
        env["DEVELOPER_DIR"] = "/Applications/Xcode.app/Contents/Developer"
    env["RUSTUP_TOOLCHAIN"] = channel
    rustc = run.checked("pinned-rustc", ["rustup", "which", "--toolchain", channel, "rustc"]).strip()
    cargo = run.checked("pinned-cargo", ["rustup", "which", "--toolchain", channel, "cargo"]).strip()
    env.update(RUSTC=rustc, RUSTC_WRAPPER="", RUSTC_WORKSPACE_WRAPPER="", CARGO_NET_OFFLINE="true")
    for key in ["RUSTFLAGS", "CARGO_ENCODED_RUSTFLAGS", "CARGO_BUILD_TARGET"]:
        env.pop(key, None)
    run.checked("rustc-version", [rustc, "--version", "--verbose"])
    run.checked("cargo-version", [cargo, "--version", "--verbose"])
    host = out / "rust-host"
    host.mkdir(exist_ok=False)
    output = run.checked("cargo-build", [cargo, "build", "--offline", "--locked", "-p", "mtk-ffi",
                         "--no-default-features", "--features", "wifi", "--target-dir", str(host),
                         "--message-format=json-render-diagnostics"], 300, cwd=ROOT / "rust")
    rows = [json.loads(line) for line in output.splitlines() if line.startswith('{')]
    assert any(r.get("reason") == "build-finished" and r.get("success") for r in rows), "Cargo build did not finish"
    artifacts = {r["target"]["name"]: r for r in rows if r.get("reason") == "compiler-artifact"}
    ffi, wlan = artifacts["mtk_ffi"], artifacts["mtk_wlan"]
    assert not ffi["fresh"] and not wlan["fresh"], "new target unexpectedly reused compiled crates"
    assert "wifi" in ffi["features"] and "bt" not in ffi["features"] and "mtk_bt" not in artifacts
    assert Path(ffi["target"]["src_path"]).resolve() == ROOT / "rust/mtk-ffi/src/lib.rs"
    archive = next(Path(name).resolve() for name in ffi["filenames"] if name.endswith(".a"))
    assert archive.is_file() and host.resolve() in archive.parents
    dependency_line = archive.with_suffix(".d").read_text().splitlines()[0].split(": ", 1)[1]
    dependencies = {str(Path(p).resolve().relative_to(ROOT)) for p in shlex.split(dependency_line) if p.endswith(".rs")}
    assert dependencies and dependencies <= before.keys(), "archive dependency has no source fingerprint"
    after = rust_input_hashes()
    (out / "rust-inputs.after.json").write_text(json.dumps(after, indent=2)+"\n")
    assert before == after, "Rust inputs changed during the fresh build; rerun on stable sources"
    return {"mode": "fresh-offline-host", "archive": str(archive), "sha256": digest(archive.read_bytes()),
            "size": archive.stat().st_size, "toolchain": channel, "rustc": rustc, "cargo": cargo,
            "ffi_artifact": ffi, "wlan_artifact": wlan, "rust_sources": before,
            "archive_rs_dependencies": sorted(dependencies), "rust_sanitizer_instrumented": False}


def compare_head_upstream(out: Path, run: CommandLog, env: dict, results: dict) -> None:
    """Keep extractors unchanged; redirect only their three production C++ inputs."""
    folder = out / "head-source"
    folder.mkdir()
    mapping, sources = {}, {}
    commit = run.checked("head-commit", ["git", "-C", str(ROOT), "rev-parse", "HEAD"]).strip()
    for path in [INTERFACE, OWNER, HAL]:
        relative = str(path.relative_to(ROOT))
        text = run.checked("head-source-"+path.stem, ["git", "-C", str(ROOT), "show", commit+":"+relative])
        snapshot = folder / path.name
        snapshot.write_text(text)
        mapping[str(path.resolve())] = str(snapshot)
        sources[relative] = {"snapshot": str(snapshot), "sha256": digest(text.encode())}
    manifest = folder / "mapping.json"
    manifest.write_text(json.dumps(mapping, indent=2)+"\n")
    driver = out / "head-extractor.py"
    driver.write_text('''from pathlib import Path
import json, runpy, sys
script, manifest = Path(sys.argv[1]), Path(sys.argv[2])
mapping = json.loads(manifest.read_text())
original = Path.read_text
def read_source(path, *args, **kwargs):
    target = mapping.get(str(path.resolve()))
    return original(Path(target) if target else path, *args, **kwargs)
Path.read_text = read_source
sys.argv = [str(script)]
runpy.run_path(str(script), run_name="__main__")
''')
    results["head_upstream"] = {}
    shared = {}
    for fixture in sorted((VENDOR / "tests/awdl").glob("test-*.py")):
        name = "head-upstream-"+fixture.stem
        results["head_upstream"][fixture.name] = run(name, [sys.executable, "-I", "-B", str(driver),
                                                            str(fixture), str(manifest)], 120, env)
        current = set(re.findall(r"\berror: (.+)", run.output("upstream-"+fixture.stem)))
        baseline = set(re.findall(r"\berror: (.+)", run.output(name)))
        if current or baseline:
            shared[fixture.name] = {"shared": sorted(current & baseline),
                                    "current_only": sorted(current-baseline), "head_only": sorted(baseline-current)}
    current_failed = sorted(name for name, status in results["upstream"].items() if status)
    head_failed = sorted(name for name, status in results["head_upstream"].items() if status)
    results["head_comparison"] = {
        "commit": commit, "sources": sources, "current_failed": current_failed, "head_failed": head_failed,
        "same_failed_suites": current_failed == head_failed, "compiler_errors": shared,
        "scope": "Only production .cpp reads use HEAD. Untouched upstream extractors, current C++ headers and current wifi Rust archive; not a full HEAD build."}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--diagnostics", type=Path, help="new empty directory below build/diagnostics")
    parser.add_argument("--fresh-ffi", action="store_true", help="fresh offline wifi archive from the installed pinned nightly")
    parser.add_argument("--skip-upstream", action="store_true", help="only companion; default also reruns the 13 unchanged extractors")
    parser.add_argument("--compare-head-upstream", action="store_true", help="also supply git HEAD production sources to unchanged extractors")
    args = parser.parse_args()
    if args.skip_upstream and args.compare_head_upstream:
        parser.error("--compare-head-upstream requires the upstream runs")
    base = ROOT / "build/diagnostics"
    base.mkdir(parents=True, exist_ok=True)
    if args.diagnostics:
        out = args.diagnostics.resolve()
        if not out.is_relative_to(base.resolve()):
            parser.error("diagnostics must be below build/diagnostics")
        out.mkdir(parents=True, exist_ok=False)
    else:
        out = Path(tempfile.mkdtemp(prefix="awdl-native-lifecycle-"+datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")+"-", dir=base))
    before = protected_hashes()
    (out / "protected-before.json").write_text(json.dumps(before, indent=2)+"\n")
    env = host_environment()
    run = CommandLog(out, env)
    results: dict = {"diagnostics": str(out), "started_at": utc(), "companion": {}, "upstream": {}}
    archive, provenance = None, None
    print("AWDL native lifecycle evidence: "+str(out), flush=True)

    try:
        provenance = build_fresh_archive(out, run) if args.fresh_ffi else archive_provenance()
        archive = Path(provenance["archive"])
        (out / "archive-provenance.json").write_text(json.dumps(provenance, indent=2)+"\n")
        ex = Extractor()
        source = generate(ex)
        (out / "extracted.cpp").write_text(source)
        (out / "host-main.cpp").write_text(HOST_MAIN)
        for kind in ["native-controls", "native-windows"]:
            (out / (kind+".cpp")).write_text(original_program(ex, kind))
        (out / "extraction.json").write_text(json.dumps({"groups": INTERFACE_GROUPS, "inventory": ex.inventory,
                                                       "source_sha256": {str(p): digest(t.encode()) for p, t in ex.cache.items()}}, indent=2)+"\n")
        flags = ["/usr/bin/clang++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
                 "-Wno-unused-function", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 "-fno-omit-frame-pointer", "-pthread", "-I"+str(VENDOR / "include")]
        results["compile"] = {}
        results["negative_controls"] = {}
        for name in ["lifecycle", "native-controls", "native-windows"]:
            executable = out / name
            cpp = out / ("host-main.cpp" if name == "lifecycle" else name+".cpp")
            command = flags + [str(cpp), str(archive), "-o", str(executable)]
            results["compile"][name] = run("compile-"+name, command, 120)
            if results["compile"][name]:
                continue
            cases = CASES[:-2] if name == "lifecycle" else [name]
            for case in cases:
                invocation = [str(executable), case] if name == "lifecycle" else [str(executable)]
                results["companion"][case] = run("companion-"+case, invocation, 120)
                reports = (out / ("companion-"+case+".log")).read_text().count("runtime error:")
                if reports and results["companion"][case] == 0:
                    results["companion"][case] = 1
            if name == "lifecycle":
                status = run("negative-old-packed-reference", [str(executable), "old-packed-reference"], 120)
                log = (out / "negative-old-packed-reference.log").read_text()
                detected = status != 0 and "reference binding to misaligned address" in log and "apple80211_channel" in log
                results["negative_controls"]["old-packed-reference"] = {"exit_code": status, "expected_alignment_detected": detected}
        if not args.skip_upstream:
            bindir = out / "bin"
            bindir.mkdir()
            wrapper = bindir / "clang++"
            wrapper.write_text("#!/bin/sh\ncase \" $* \" in *\" -c \"*) exec /usr/bin/clang++ \"$@\" ;; esac\n"
                               "exec /usr/bin/clang++ \"$@\" "+shlex.quote(str(archive))+"\n")
            wrapper.chmod(0o700)
            upstream_env = env.copy()
            upstream_env["PATH"] = str(bindir)+os.pathsep+env.get("PATH", "")
            for fixture in sorted((VENDOR / "tests/awdl").glob("test-*.py")):
                results["upstream"][fixture.name] = run("upstream-"+fixture.stem, [sys.executable, "-I", "-B", str(fixture)], 120, upstream_env)
            if args.compare_head_upstream:
                compare_head_upstream(out, run, upstream_env, results)
    except Exception as error:
        results["error"] = repr(error)
        (out / "error.log").write_text(repr(error)+"\n")
    finally:
        after = protected_hashes()
        changes = {k: {"before": before.get(k), "after": after.get(k)} for k in before.keys() | after.keys() if before.get(k) != after.get(k)}
        results.update({"ended_at": utc(), "protected_files": len(before), "protected_changes": changes,
                        "archive_unchanged": bool(archive and archive.is_file() and digest(archive.read_bytes()) == provenance["sha256"]),
                        "archive_mode": provenance.get("mode", "validated-retained") if provenance else None,
                        "test_awdl_sh_executed": False,
                        "limitations": ["Not the complete unchanged MI-discovery fixture", "No replica or coverage claim for the UMI terminal-state table",
                                        "Non-KERNEL host build omits RX gate/lifetime/BPF scratch-pointer guards and kernel-only trace calls",
                                        "Modeled HAL awdlEnable includes its own epoch-cancellation rule; production HAL operation/quiesce/epoch correctness is not validated",
                                        "HAL awdlReserve is a boundary model with absent timing by default; awdlRocTimingSnapshot and the timing ABI are extracted verbatim",
                                        "Startup models an associated channel-6 link for the real first-sync readiness check, then NativeFixture drops it; GPU registry enumeration is not exercised",
                                        "Mbufs are single-segment host models; extracting buildAmsdu does not validate kernel chain allocation",
                                        "Preheld-gate upstream assertion companions do not validate original extractor compatibility or every original setup/dispatch assumption",
                                        "Private native ownership/copy/stop/reentrancy, firmware, RF and IOKit contracts remain unproved; host boundary models are not hardware verification",
                                        "ASan/UBSan do not establish race freedom; no TSan or native/kernel execution",
                                        "Only C++ is sanitizer-instrumented; the provenance-checked Rust archive is real but not instrumented",
                                        "Upstream extractors are unchanged; companion success does not make their extraction failures pass"]})
        (out / "protected-after.json").write_text(json.dumps(after, indent=2)+"\n")
        (out / "results.json").write_text(json.dumps(results, indent=2)+"\n")
    print(json.dumps(results, indent=2))
    failed = bool(results.get("error") or any(results.get("compile", {}).values()) or changes or not results["archive_unchanged"])
    failed |= any(status for status in results["companion"].values())
    failed |= any(not control["expected_alignment_detected"] for control in results.get("negative_controls", {}).values())
    failed |= any(status for status in results["upstream"].values())
    failed |= any(status for status in results.get("head_upstream", {}).values())
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
