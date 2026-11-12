#!/usr/bin/env python3
"""Source-bound SyncEnable recovery and HAL context lifetime tests (host only).

Uses the complete production frontend extraction and the REAL awdlEnable,
MwxAwdlCall/Operation, quiesce, context/station removal, filter restoration and
admission helpers. MCU transport, registers, native queues, task dispatch and
mbuf allocation are explicit host boundaries. Production bodies are not patched.
The real Wi-Fi Rust State/Reservation/Timeline archive is built offline, or reused
only with matching source/archive provenance. C++ runs under strict ASan/UBSan.

GPU registry nodes, power/scan/state changes and firmware wait outcomes below
are injected test inputs, not observations of hardware. Task admission tests run
the real queueing function and task prologue, not the whole primary state task.
No claim of IOKit/RF correctness, race freedom, Rust sanitizer instrumentation,
installation or native-service execution. Every run retains evidence locally.

Run: python3 -I -B scripts/test-awdl-sync-recovery.py
"""
from pathlib import Path
import argparse
import importlib.util
import json
import re
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("awdl_lifecycle", ROOT / "scripts/test-awdl-native-lifecycle.py")
L = importlib.util.module_from_spec(spec)
spec.loader.exec_module(L)
REG = L.HAL.parent / "if_mwxreg.h"
CASES = ["admission", "task-admission", "boot", "stale-session", "epoch", "deadline",
         "errors", "filter-retry", "cancel", "supersession", "limits", "quiesce",
         "off-reentry", "snapshot-reset", "late-publication", "completion-cancel"]


def one(text, old, new):
    if text.count(old) != 1:
        raise ValueError("host boundary drift: " + old[:100])
    return text.replace(old, new, 1)


def definition(ex, name):
    text = ex.text(L.HAL)
    start = text.index("class " + name + " {")
    end = text.index(";", L.balanced_end(text, text.index("{", start))) + 1
    return ex.span(L.HAL, start, end, "class " + name)


def function(ex, name):
    # These C definitions put the return type on its own source line. Include
    # that line verbatim rather than synthesizing a host-only declaration.
    returns = {"mwx_append_len": "void *", "mwx_append_tlv": "void *",
               "mwx_fill_sta_req_hdr": "struct mbuf *", "mwx_newstate": "int"}
    if name not in returns:
        return ex.function(L.HAL, name)
    text = ex.text(L.HAL)
    signature = returns[name]+"\n"+name+"("
    if text.count(signature) != 1:
        raise ValueError("split return-type boundary drift: "+name)
    start = text.index(signature)
    params_end = L.balanced_end(text, start+len(signature)-1)
    opening = text.index("{", params_end)
    if text[params_end:opening].strip():
        raise ValueError("function signature boundary drift: "+name)
    return ex.span(L.HAL, start, L.balanced_end(text, opening), name)


REGISTRY = r'''
struct OSIterator:OSObject {
 std::vector<OSObject *> objects; size_t index=0;
 OSObject *getNextObject() { return index<objects.size()?objects[index++]:nullptr; }
};
struct OSData:OSObject {
 uint8_t value[4]{}; unsigned length=4;
 unsigned getLength() { return length; } void *getBytesNoCopy() { return value; }
};
std::vector<OSObject *> registryNodes;
bool registryMatchUnavailable=false,registryIteratorUnavailable=false;
'''
REGISTRY_NODE = r'''
struct RegistryNode:IOService {
 OSData vendor; bool profile=false;
 explicit RegistryNode(uint16_t id) { vendor.value[0]=id; vendor.value[1]=id>>8; }
 OSObject *getProperty(const char *key) override {
  if(!strcmp(key,"vendor-id"))return &vendor;
  if(!strcmp(key,"nvrm-gpu-profile"))return profile?this:nullptr;
  return IOService::getProperty(key);
 }
};
'''
SUPPORT = r'''
#define __packed __attribute__((packed))
#define IEEE80211_ADDR_LEN 6
#define MWX_DEV_LOG(...) ((void)0)
#define DPRINTF(...) ((void)0)
#define KASSERT(x) assert(x)
#define mtod(m,t) reinterpret_cast<t>(mbuf_data(m))
#define mbuf Packet
#define M_DONTWAIT 0
#define MSEC_TO_NSEC(x) (uint64_t(x)*1000000)
#define htole16(x) uint16_t(x)
#define htole32(x) uint32_t(x)
#define MT_BAND_ADDR(b,o) (uint32_t(o)) // only band 0 is used by this fixture
constexpr unsigned IEEE80211_F_BGSCAN=2;
struct ieee80211_node { uint8_t ni_bssid[6]={2,9,9,9,9,9},ni_macaddr[6]{}; bool ni_port_valid=true; };
struct task { unsigned queued=0; }; struct taskq {};
taskq systemQueue; taskq *systq=&systemQueue;
void task_add(taskq *,task *t) { assert(!heldHalLocks); ++t->queued; }
uint64_t mtk_join_now() { return clockUS.load(); }
struct TraceBoundary {
 unsigned ended=0,changed=0;
 void contextEnded(uint64_t,uint32_t,uint64_t) { assert(heldHalLocks); ++ended; }
 void peerChanged(unsigned) { assert(heldHalLocks); ++changed; }
};
struct FirmwareBoundary {
 std::vector<std::string> steps;
 std::function<void(const std::string &)> hook;
 std::map<std::string,int> results;
 uint32_t response=0; bool failAllocation=false,failPrepend=false;
 uint32_t filter=0,originalFilter=0;
 unsigned mbufs=0,rateResets=0,aborts=0;
 void reset() { assert(!mbufs); steps.clear(); hook=nullptr; results.clear(); response=0;
  failAllocation=failPrepend=false; filter=originalFilter=0; rateResets=aborts=0; }
 unsigned count(const std::string &s) { return std::count(steps.begin(),steps.end(),s); }
} firmware;
'''
FW_FUNCTIONS = r'''
static int boundary(mwx_softc *sc,const char *name) {
 assert(sc&&!heldHalLocks&&gateDepth); firmware.steps.emplace_back(name);
 auto hook=firmware.hook; if(hook)hook(name);
 auto result=firmware.results.find(name); return result==firmware.results.end()?0:result->second;
}
static mbuf_t mwx_alloc_sta_req_tlv(int) {
 if(firmware.failAllocation)return nullptr;
 auto p=new Packet; p->bytes.reserve(2048); ++firmware.mbufs; return p;
}
static size_t m_trailingspace(mbuf_t p) { return p->bytes.capacity()-p->bytes.size(); }
static size_t m_leadingspace(mbuf_t) { return 64; }
static mbuf_t m_prepend(mbuf_t p,unsigned n,unsigned) {
 if(firmware.failPrepend){assert(firmware.mbufs); --firmware.mbufs; delete p; return nullptr;}
 p->bytes.insert(p->bytes.begin(),n,0); return p;
}
static int mt7921_mcu_uni_add_dev(mwx_softc *sc,mwx_vif *,mwx_node *,int add) {
 return boundary(sc,add?"device-add":"device-remove");
}
static void mwx_mac_wtbl_update(mwx_softc *,unsigned) { assert(!heldHalLocks); }
static int mwx_mcu_send_mbuf(mwx_softc *sc,unsigned,mbuf_t p,int *seq) {
 assert(p&&p->bytes.size()>=sizeof(sta_req_hdr)&&firmware.mbufs);
 sta_req_hdr header{};memcpy(&header,p->bytes.data(),sizeof(header));
 assert(header.bss_idx==1&&header.is_tlv_append==1&&header.tlv_num==2);
 sta_rec_basic basic{};memcpy(&basic,p->bytes.data()+sizeof(header),sizeof(basic));
 const char *step=basic.conn_state==CONN_STATE_DISCONNECT?"station-remove-send":"station-send";
 *seq=basic.conn_state==CONN_STATE_DISCONNECT?2:1;
 --firmware.mbufs; delete p; return boundary(sc,step);
}
static int mwx_mcu_wait_resp_int(mwx_softc *sc,unsigned,int seq,uint32_t *status) {
 *status=seq==1?firmware.response:0;
 return boundary(sc,seq==1?"station-response":"station-remove-response");
}
static int mt7921_mcu_set_tx(mwx_softc *sc,mwx_vif *) { return boundary(sc,"tx-queues"); }
static int mt7921_mcu_set_rxfilter(mwx_softc *sc,uint32_t flags,uint8_t operation,uint32_t bits) {
 const char *step=operation==mwx_fif_bit_clr?"filter-clear":
  operation==mwx_fif_bit_set?"filter-reapply":(flags&mwx_fif_other_bss)?"filter-enable":"filter-restore";
 int status=boundary(sc,step);
 if(!status&&operation==mwx_fif_bit_clr)firmware.filter&=~bits;
 if(!status&&operation==mwx_fif_bit_set)firmware.filter|=bits;
 return status;
}
static uint32_t mwx_read(mwx_softc *,unsigned) { return firmware.filter; }
static void mwx_rmw(mwx_softc *,unsigned,uint32_t set,uint32_t clear) { firmware.filter=(firmware.filter&~clear)|set; }
static void mwx_clear(mwx_softc *,unsigned,uint32_t bits) { firmware.filter&=~bits; }
static void mwx_awdl_reset_rate(mwx_softc *,unsigned) { assert(heldHalLocks); ++firmware.rateResets; }
static int mwx_awdl_abort(mwx_softc *sc) { assert(!heldHalLocks); ++firmware.aborts;sc->sc_awdl_roc_abort_pending=false;return 0; }
static int tsleep_nsec(void *event,int,const char *,uint64_t nsec) {
 assert(!heldHalLocks&&gateDepth); return gate.commandSleep(event,clockUS.load()+nsec/1000,THREAD_UNINT);
}
'''


def generate(ex):
    text = L.generate(ex)
    # Keep the shared OS/native fixture, not the older modeled-HAL assertions.
    marker = '#line 1 "native-lifecycle-host-boundaries-and-assertions"\n'
    if text.count(marker) != 1:
        raise ValueError("shared fixture boundary drift")
    text = text.split(marker)[0] + L.FIXTURE.split("void checkInitStart() {")[0]
    text = one(text, "constexpr int IEEE80211_M_STA=1,IEEE80211_S_SCAN=0,IEEE80211_S_AUTH=2,IEEE80211_S_ASSOC=3,IEEE80211_S_RUN=4;",
               "constexpr int IEEE80211_M_STA=1; enum ieee80211_state { IEEE80211_S_INIT=-1,IEEE80211_S_SCAN=0,IEEE80211_S_AUTH=2,IEEE80211_S_ASSOC=3,IEEE80211_S_RUN=4 };")
    text = one(text, "int sc_ns_state=IEEE80211_S_RUN;", "ieee80211_state sc_ns_state=IEEE80211_S_RUN;")
    text = one(text, "int ic_state=4,ic_opmode=IEEE80211_M_STA;", "ieee80211_state ic_state=IEEE80211_S_RUN; int ic_opmode=IEEE80211_M_STA;")
    # Elaborated `struct` names in verbatim task bodies need real class names,
    # not typedef aliases. These mappings are explicit host structure boundaries.
    text = one(text, "using mwx_softc=MtkMwx::Com; using ieee80211com=MtkMwx::IC;",
               "#define mwx_softc MtkMwx::Com\n#define ieee80211com MtkMwx::IC")
    text = one(text, "struct OSIterator:OSObject { OSObject *getNextObject() { return nullptr; } };\n"
               "struct OSData:OSObject { unsigned getLength() { return 0; } void *getBytesNoCopy() { return nullptr; } };", REGISTRY)
    # OS boundaries inject reset immediately after releasing a snapshot lock,
    # and time passing after the HAL's last guarded commit. No production body
    # is rewritten to create either interleaving.
    text = one(text, "std::function<void(void *)> publishHook;",
               "std::function<void(void *)> publishHook;\nstd::function<void(const char *,const char *)> stringPropertyHook;")
    text = one(text, " void setProperty(const char *key,const char *value) { stringProperties[key]=value; }",
               " void setProperty(const char *key,const char *value) { stringProperties[key]=value; auto hook=stringPropertyHook; if(hook)hook(key,value); }")
    text = one(text, "void IOLockUnlock(IOLock *l) { assert(heldHalLocks); --heldHalLocks; l->mutex.unlock(); }",
               "std::function<void(IOLock *)> halUnlockHook;\nvoid IOLockUnlock(IOLock *l) { assert(heldHalLocks); --heldHalLocks; l->mutex.unlock(); auto hook=halUnlockHook; if(hook)hook(l); }")
    text = one(text, " OSObject *getProperty(const char *key) {", " virtual OSObject *getProperty(const char *key) {")
    text = one(text, ' static OSDictionary *serviceMatching(const char *name) { assert(!strcmp(name,"IOPCIDevice")); return nullptr; }',
               ' static OSDictionary *serviceMatching(const char *name) { assert(!strcmp(name,"IOPCIDevice")); return registryMatchUnavailable?nullptr:new OSDictionary; }')
    text = one(text, " static OSIterator *getMatchingServices(OSDictionary *) { return nullptr; }",
               " static OSIterator *getMatchingServices(OSDictionary *) { if(registryIteratorUnavailable)return nullptr; auto p=new OSIterator;p->objects=registryNodes;return p; }")
    text = one(text, "struct IOSkywalkPacketQueue;", REGISTRY_NODE+"\nstruct IOSkywalkPacketQueue;")
    constants = ["MWX_MAX_INTERFACES", "MWX_WTBL_SIZE", "MWX_WTBL_RESERVED", "MWX_WTBL_STA", "MT_WF_RFCR",
                 "MT_WF_RFCR_DROP_OTHER_BSS", "MT_WF_RFCR_DROP_A3_BSSID", "MT_WF_RFCR_DROP_A2_BSSID",
                 "MT_WF_RFCR_DROP_OTHER_UC", "MT_WF_RFCR_DROP_MCAST", "MT_WF_RFCR_DROP_MCAST_FILTERED",
                 "MCU_UNI_CMD_STA_REC_UPDATE", "STA_REC_BASIC", "STA_REC_WTBL", "WTBL_RESET_AND_SET",
                 "WTBL_GENERIC", "WTBL_RX", "STA_TYPE_BC", "STA_TYPE_STA", "NETWORK_INFRA",
                 "CONN_STATE_PORT_SECURE", "CONN_STATE_DISCONNECT"]
    regs = ex.text(REG)
    wire = SUPPORT
    for name in constants:
        matches = list(re.finditer(r"(?m)^#define[ \t]+"+re.escape(name)+r"(?:\([^\n]*?\))?[ \t]+[^\n]+", regs))
        if len(matches) != 1:
            raise ValueError("register constant drift: "+name)
        wire += ex.span(REG, matches[0].start(), matches[0].end(), "constant "+name)
    for name in ["sta_req_hdr", "sta_rec_basic", "sta_rec_wtbl", "wtbl_generic", "wtbl_rx"]:
        wire += ex.structure(REG, name)
    wire += ex.structure(L.HAL, "mwx_node")+ex.structure(L.HAL, "mwx_vif")
    text = one(text, "struct MtkMwx:IOService {", wire+"\nstruct MtkMwx:IOService {")
    text = one(text, "  struct { uint64_t Attempt=0; } ic_join_diag;", "  void *ic_softc=nullptr;\n  struct { uint64_t Attempt=0,RequestedState=0,AuthRequestedAtUS=0,AssocRequestedAtUS=0,RunRequestedAtUS=0; bool Active=false; } ic_join_diag;")
    text = one(text, "  struct Node { uint8_t ni_bssid[6]={2,9,9,9,9,9}; bool ni_port_valid=true; } modelBss; Node *ic_bss=&modelBss;",
               "  using Node=ieee80211_node; Node modelBss; Node *ic_bss=&modelBss;")
    text = one(text, "  uint32_t sc_awdl_epoch=0; AwdlRocTiming sc_awdl_roc_timing{};", r'''
  uint32_t sc_awdl_epoch=0; AwdlRocTiming sc_awdl_roc_timing{}; mtkawdl::Reservation sc_awdl_roc{};
  unsigned sc_awdl_calls=0; uint64_t sc_awdl_token_used[256]{};
  bool sc_awdl_roc_abort_pending=false,sc_awdl_configured=false,sc_awdl_filter_active=false,sc_awdl_scan_suspended=false;
  uint8_t sc_awdl_roc_abort_token=0,sc_awdl_channel=0,sc_awdl_address[6]{};
  uint64_t sc_awdl_roc_abort_retry_at=0; uint32_t sc_awdl_filter=0;
  mwx_vif sc_awdl_vif{}; mwx_node sc_awdl_peers[3]{};
  bool sc_awdl_peer_used[3]{},sc_awdl_station_pending[4]{};
  TraceBoundary *sc_awdl_tx_trace=nullptr;
  struct { bool ofdmOnly=false; } sc_awdl_tx_status;
  taskq modelQueue; taskq *sc_nswq=&modelQueue; task sc_newstate_task,sc_scan_task; int sc_ns_arg=0;
''')
    text = one(text, "  bool take(AwdlTxResult &out)", "  void cancel() { assert(heldHalLocks); injected.clear(); }\n  bool take(AwdlTxResult &out)")
    text = one(text, " uint32_t duration=0; mtkawdl::Reservation grant{};", " uint32_t duration=0; mtkawdl::Reservation &grant=storage.sc_awdl_roc;")
    text = one(text, " MtkMwx() { storage.sc_awdl_lock=IOLockAlloc(); assert(storage.sc_awdl_lock); }",
               " MtkMwx() { storage.sc_awdl_lock=IOLockAlloc(); assert(storage.sc_awdl_lock); storage.sc_ic.ic_softc=&storage; }\n IOService *mwxController() { return this; }")
    text = one(text, " bool awdlIsEnabled() { return on; }", " bool awdlIsEnabled();")
    text = one(text, " bool awdlGrantSnapshot(mtkawdl::Reservation &out) { out=grant; return on; }", " bool awdlGrantSnapshot(mtkawdl::Reservation &);")
    body = ex.text(L.HAL)
    start = body.index("static const uint32_t mwx_awdl_filter_mask=")
    end = body.index("void mwx_awdl_restore_filter", start)
    actual = ex.span(L.HAL, start, end, "HAL filter ownership constants")
    actual += FW_FUNCTIONS
    actual += "static bool mwx_awdl_current(mwx_softc *,uint32_t);\n"
    actual += definition(ex, "MwxAwdlInfraOperation")+definition(ex, "MwxAwdlCall")+definition(ex, "MwxAwdlOperation")
    for name in ["mwx_awdl_current", "mwx_append_len", "mwx_append_tlv", "mwx_fill_sta_req_hdr",
                 "mwx_awdl_restore_filter", "mwx_awdl_quiesce", "mwx_awdl_remove_station", "mwx_awdl_remove_context",
                 "mwx_awdl_ht_enabled", "MtkMwx::awdlEnable", "MtkMwx::awdlIsEnabled", "MtkMwx::awdlGrantSnapshot"]:
        actual += function(ex, name)
    # Replace the modeled HAL while retaining the native message boundary.
    # No statement in the production extraction is rewritten.
    message_boundary = L.POST_TYPES.split("IOReturn MtkMwx::awdlEnable(")[0]
    text = one(text, L.POST_TYPES, message_boundary+actual+'\n#define super IO80211AWDLProtocol\n')
    text += function(ex, "mwx_newstate")
    at = body.index("mwx_newstate_task(void *ptr)")
    opening = body.index("{", at)
    end = body.index("\tint s = splnet();", opening)
    text += "\nstatic void capturedStateTask(void *ptr,const std::function<void(mwx_softc *)> &observe) {\n"
    text += ex.span(L.HAL, opening+1, end, "VERBATIM primary state task admission/capture prologue")
    text += "(void)ostate;(void)nstate;(void)arg; observe(sc);\n}\n"
    return text


MAIN = r'''
#include "extracted.cpp"
static bool live(NativeFixture &d) {
 apple80211_awdl_sync_enabled value{};assert(d.getAWDL_SYNC_ENABLED(&value)==0);return value.enabled!=0;
}
static void resetModel() { registryNodes.clear();registryMatchUnavailable=registryIteratorUnavailable=false;
 stringPropertyHook=nullptr;halUnlockHook=nullptr;firmware.reset();resetFaults(); }
struct RecoveryFixture:NativeFixture {
 RecoveryFixture() { assert(setSync(false)==0); firmware.steps.clear(); }
 ~RecoveryFixture() override { firmware.hook=nullptr;firmware.results.clear();firmware.failAllocation=firmware.failPrepend=false;registryNodes.clear(); }
};
static void pending(NativeFixture &d) {
 assert(!d.enabled&&!d.txSession&&!live(d)&&d.syncEnable.phase!=AirportMtkAwdlInterface::SyncEnable::None&&d.timer->pending);
 assert(!d.properties["MTKAWDLSyncEnabled"]&&d.properties["MTKAWDLSyncRecoveryPending"]);
 assert(d.properties["MTKAWDLSyncRequestStatus"]==kIOReturnSuccess);
}
static void recovered(NativeFixture &d,uint64_t generation,uint64_t accepted,uint64_t deadline) {
 assert(d.enabled&&live(d)&&d.txSession&&d.syncEnable.phase==AirportMtkAwdlInterface::SyncEnable::None);
 assert(d.syncEnable.generation==generation&&d.syncEnable.acceptedAtUS==accepted&&d.syncEnable.deadlineUS==deadline);
 assert(d.syncRecovered==1&&!d.properties["MTKAWDLSyncRecoveryPending"]&&d.properties["MTKAWDLSyncEnabled"]);
}
static void admission() {
 for(unsigned kind=0;kind<12;++kind) {
  resetModel(); RecoveryFixture d; auto &sc=d.radio.storage;
  auto state=sc.sc_ic.ic_state,ns=sc.sc_ns_state;
  switch(kind) {
   case 0:sc.sc_ic.ic_if.if_flags=IFF_UP;break;
   case 1:sc.sc_power_req=1;break;
   case 2:sc.sc_awdl_stopping=true;break;
   case 3:sc.sc_resetting=true;break;
   case 4:sc.sc_ic.ic_wcl_mac_reconfig=true;break;
   case 5:sc.sc_awdl_busy=true;break;
   case 6:sc.sc_flags=MWX_FLAG_SCANNING;break;
   case 7:sc.sc_flags=MWX_FLAG_BGSCAN;break;
   case 8:sc.sc_ns_state=IEEE80211_S_AUTH;break;
   case 9:sc.sc_ns_state=IEEE80211_S_ASSOC;break;
   case 10:sc.sc_ic.ic_state=IEEE80211_S_SCAN;break;
   case 11:sc.sc_ic.modelBss.ni_port_valid=false;break;
  }
  assert(d.setSync(true)==0); pending(d);assert(firmware.count("device-add")==0);
  auto generation=d.syncEnable.generation,accepted=d.syncEnable.acceptedAtUS,deadline=d.syncEnable.deadlineUS;
  clockUS+=100000;d.serviceInGate();pending(d);assert(d.syncEnable.generation==generation&&d.syncEnable.deadlineUS==deadline);
  sc.sc_ic.ic_if.if_flags=IFF_UP|IFF_RUNNING;sc.sc_power_done=sc.sc_power_req;
  sc.sc_awdl_stopping=sc.sc_resetting=sc.sc_awdl_busy=sc.sc_ic.ic_wcl_mac_reconfig=false;
  sc.sc_flags=0;sc.sc_ns_state=ns;sc.sc_ic.ic_state=state;sc.sc_ic.modelBss.ni_port_valid=true;
  d.serviceInGate();recovered(d,generation,accepted,deadline);assert(firmware.count("device-add")==1);
 }
 resetModel(); {RecoveryFixture d;auto &sc=d.radio.storage;
  sc.sc_ic.ic_state=sc.sc_ns_state=IEEE80211_S_SCAN;d.radio.infraChannel=0;
  assert(d.setSync(true)==0&&live(d)); // idle SCAN is not an AP requirement
  unsigned added=firmware.count("device-add");sc.sc_flags=MWX_FLAG_SCANNING;
  assert(d.setSync(true)==0&&live(d)&&firmware.count("device-add")==added); // live duplicate has no setup
 }
 for(bool detach:{false,true}) {resetModel();RecoveryFixture d;auto &sc=d.radio.storage;
  if(detach)sc.sc_detaching=true;else sc.sc_ic.ic_if.if_flags=IFF_RUNNING;
  assert(d.setSync(true)==kIOReturnNotReady&&!live(d)&&d.syncEnable.phase==AirportMtkAwdlInterface::SyncEnable::None);
 }
}
static void taskAdmission() {
 resetModel(); RecoveryFixture d;auto &sc=d.radio.storage;
 sc.sc_ic.ic_state=sc.sc_ns_state=IEEE80211_S_SCAN;
 assert(mwx_newstate(&sc.sc_ic,IEEE80211_S_SCAN,-1)==0&&sc.sc_awdl_ns_pending&&sc.sc_newstate_task.queued==1);
 assert(d.setSync(true)==0);pending(d);assert(firmware.count("device-add")==0);
 auto generation=d.syncEnable.generation,accepted=d.syncEnable.acceptedAtUS,deadline=d.syncEnable.deadlineUS;
 capturedStateTask(&sc,[&](mwx_softc *) {
  assert(!sc.sc_awdl_ns_pending&&sc.sc_awdl_infra_busy==1&&!heldHalLocks);
  d.serviceInGate();pending(d);
  assert(mwx_newstate(&sc.sc_ic,IEEE80211_S_SCAN,-2)==0&&sc.sc_awdl_ns_pending);
 });
 assert(sc.sc_awdl_ns_pending&&!sc.sc_awdl_infra_busy);d.serviceInGate();pending(d);
 capturedStateTask(&sc,[&](mwx_softc *) {d.serviceInGate();pending(d);});
 assert(!sc.sc_awdl_ns_pending&&!sc.sc_awdl_infra_busy);
 {
  MwxAwdlInfraOperation init(&sc,true);MwxAwdlInfraOperation scan(&sc);
  assert(sc.sc_awdl_infra_busy==2&&sc.sc_awdl_init_busy==1);
  d.serviceInGate();pending(d);
 }
 d.serviceInGate();recovered(d,generation,accepted,deadline);
 // Primary scan work vetoes context setup without erasing an existing session.
 {MwxAwdlInfraOperation scan(&sc);MtkMwx::AwdlSyncSnapshot s{};assert(d.radio.awdlSyncSnapshot(s)&&!s.ready&&s.session==d.txSession);}
}
static void boot() {
 for(unsigned kind=0;kind<5;++kind) {
  resetModel();RecoveryFixture d;d.readyGateBypassed=false;
  auto &sc=d.radio.storage;sc.sc_ic.ic_state=sc.sc_ns_state=IEEE80211_S_SCAN;d.radio.infraChannel=0;
  RegistryNode node(kind==4?0x8086:0x10de);registryNodes={&node};
  if(kind==2)node.profile=true;
  if(kind==3)registryIteratorUnavailable=true;
  uint64_t begin=clockUS;
  assert(d.setSync(true)==0);
  if(kind>=2) {assert(live(d)&&d.readyGateBypassed&&d.syncEnable.deadlineUS==begin+15000000);continue;}
  pending(d);assert(d.syncEnable.phase==AirportMtkAwdlInterface::SyncEnable::Boot&&d.syncEnable.deadlineUS==begin+195000000&&d.timer->us==500000);
  uint64_t generation=d.syncEnable.generation;
  clockUS=begin+(kind==0?23000000:180000000);
  if(kind==0)node.profile=true;
  d.serviceInGate();recovered(d,generation,begin,clockUS.load()+15000000);
  assert(d.readyGateBypassCount==(kind==1?1U:0U)&&firmware.count("device-add")==1);
 }
}
static void stale() {
 resetModel();NativeFixture d;uint64_t old=d.txSession;
 gate.enter([&]{mwx_awdl_quiesce(&d.radio.storage,false);return 0;});
 assert(!live(d));assert(d.setSync(true)==0);pending(d);
 auto generation=d.syncEnable.generation,accepted=d.syncEnable.acceptedAtUS,deadline=d.syncEnable.deadlineUS;
 assert(d.events.empty()); // no fabricated native AW for a deferred enable
 d.radio.storage.sc_awdl_stopping=false;d.serviceInGate();recovered(d,generation,accepted,deadline);
 assert(d.txSession!=old&&d.events.empty());
}
static const char *stages[]={"device-add","station-send","station-response","tx-queues","filter-enable","filter-clear"};
static void epoch() {
 for(const char *at:stages) {
  resetModel();RecoveryFixture d;auto &sc=d.radio.storage;bool injected=false;
  const auto attemptsBefore=d.syncAttempts;
  firmware.hook=[&](const std::string &step) {if(step==at&&!injected){injected=true;IOLockLock(sc.sc_awdl_lock);++sc.sc_awdl_epoch;sc.sc_awdl_stopping=true;IOLockUnlock(sc.sc_awdl_lock);}};
  assert(d.setSync(true)==0&&injected);pending(d);
  auto generation=d.syncEnable.generation,accepted=d.syncEnable.acceptedAtUS,deadline=d.syncEnable.deadlineUS;
  firmware.hook=nullptr;sc.sc_awdl_stopping=false;clockUS+=100000;d.serviceInGate();recovered(d,generation,accepted,deadline);
  assert(d.syncEpochRebinds==1&&d.syncAttempts==attemptsBefore+2&&d.syncEnable.attempts==2&&sc.sc_awdl_calls==0&&!sc.sc_awdl_busy);
 }
}
static void deadline() {
 for(const char *at:stages) {
  resetModel();RecoveryFixture d;bool injected=false;
  firmware.hook=[&](const std::string &step){if(step==at&&!injected){injected=true;clockUS=d.syncEnable.deadlineUS;}};
  assert(d.setSync(true)==kIOReturnTimeout&&injected&&!live(d)&&!d.radio.storage.sc_awdl_enabled);
  assert(d.syncEnable.phase==AirportMtkAwdlInterface::SyncEnable::None&&d.syncExpired==1&&!d.timer->pending);
  firmware.hook=nullptr;assert(d.setSync(false)==0&&!d.radio.storage.sc_awdl_configured);
 }
 resetModel();RecoveryFixture d;d.radio.storage.sc_power_req=1;
 assert(d.setSync(true)==0);pending(d);auto deadline=d.syncEnable.deadlineUS;
 clockUS=deadline;d.serviceInGate();assert(d.syncEnable.phase==AirportMtkAwdlInterface::SyncEnable::None&&d.syncExpired==1&&!live(d)&&!d.timer->pending);
 d.radio.storage.sc_power_done=1;clockUS+=100000;d.serviceInGate();assert(!live(d)&&firmware.count("device-add")==0);
}
static void errors() {
 for(const char *at:{"device-add","station-send","station-response","tx-queues"}) {
  resetModel();RecoveryFixture d;firmware.results[at]=EIO;
  assert(d.setSync(true)==kIOReturnIOError&&!live(d)&&d.syncEnable.phase==AirportMtkAwdlInterface::SyncEnable::None&&!d.syncDeferred);
  firmware.results.clear();assert(d.setSync(false)==0);
 }
 resetModel();{RecoveryFixture d;firmware.response=0x10003;assert(d.setSync(true)==kIOReturnIOError&&!live(d));firmware.response=0;}
 resetModel();{RecoveryFixture d;firmware.failAllocation=true;assert(d.setSync(true)==kIOReturnNoMemory&&!live(d));firmware.failAllocation=false;}
 resetModel();{RecoveryFixture d;firmware.failPrepend=true;assert(d.setSync(true)==kIOReturnIOError&&!live(d));firmware.failPrepend=false;}
 // Keep baseline nonfatal filter-command diagnostics, not fabricated transport success.
 resetModel();{RecoveryFixture d;firmware.results["filter-enable"]=EIO;assert(d.setSync(true)==0&&live(d));assert(d.radio.properties["MTKAWDLRxFilterStatus"]==EIO);}
}
static void filterRetry() {
 resetModel();RecoveryFixture d;auto &sc=d.radio.storage;
 const uint32_t original=MT_WF_RFCR_DROP_OTHER_BSS|MT_WF_RFCR_DROP_A2_BSSID|MT_WF_RFCR_DROP_MCAST;
 firmware.filter=original|8;bool once=false;
 firmware.hook=[&](const std::string &step){if(step=="filter-clear"&&!once){once=true;sc.sc_flags=MWX_FLAG_SCANNING;}};
 assert(d.setSync(true)==0);pending(d);assert(sc.sc_awdl_filter_active&&sc.sc_awdl_filter==original&&firmware.filter==8);
 firmware.hook=nullptr;sc.sc_flags=0;d.serviceInGate();assert(live(d)&&sc.sc_awdl_filter==original);
 assert(firmware.count("filter-restore")==1&&firmware.count("filter-reapply")==1);
 assert(d.setSync(false)==0&&!sc.sc_awdl_filter_active&&firmware.filter==(original|8));
 // An epoch replacement during restoration is never cleared by the old OFF.
 assert(d.setSync(true)==0);uint32_t replacement=MT_WF_RFCR_DROP_OTHER_UC;
 firmware.hook=[&](const std::string &step){if(step=="filter-restore"){
  IOLockLock(sc.sc_awdl_lock);++sc.sc_awdl_epoch;sc.sc_awdl_filter=replacement;sc.sc_awdl_filter_active=true;IOLockUnlock(sc.sc_awdl_lock);
 }};
 assert(d.setSync(false)==kIOReturnAborted&&sc.sc_awdl_filter_active&&sc.sc_awdl_filter==replacement);
 firmware.hook=nullptr;
}
static void cancel() {
 for(unsigned kind=0;kind<5;++kind) {
  resetModel();RecoveryFixture d;d.radio.storage.sc_power_req=1;assert(d.setSync(true)==0);pending(d);
  auto generation=d.syncEnable.generation;ether_addr invalid{{1,0,0,0,0,0}},mac{{2,6,5,4,3,2}};
  assert(d.setMacAddress(invalid)==kIOReturnBadArgument&&d.syncEnable.generation==generation);
  if(kind==0)assert(d.setSync(false)==0);
  if(kind==1)assert(d.disable(0)==0);
  if(kind==2)assert(d.setInterfaceEnable(false)==0);
  if(kind==3)assert(d.setLinkState(kIO80211NetworkLinkDown,0));
  if(kind==4)assert(d.setMacAddress(mac)==0);
  assert(d.syncEnable.phase==AirportMtkAwdlInterface::SyncEnable::None&&d.syncEnable.generation>generation&&!live(d));
  d.radio.storage.sc_power_done=1;d.serviceInGate();assert(!live(d)&&firmware.count("device-add")==0);
 }
 resetModel();RecoveryFixture d;d.radio.storage.sc_power_req=1;assert(d.setSync(true)==0);d.shutdown();
 assert(d.syncEnable.phase==AirportMtkAwdlInterface::SyncEnable::None&&d.stopping&&!live(d)&&!d.timer->pending);
}
static void supersession() {
 resetModel();RecoveryFixture d;
 std::mutex mutex;std::condition_variable cv;bool firstWaiting=false,secondWaiting=false,thirdWaiting=false;
 bool releaseFirst=false,releaseSecond=false,releaseThird=false;int event;std::atomic<unsigned> stage{0};
 thread_local unsigned role=0;
 gate.sleepHook=[&](void *ident,uint64_t,unsigned){assert(!gateDepth&&!heldHalLocks);std::unique_lock<std::mutex> lock(mutex);
  bool *ready=role==1?&firstWaiting:role==2?&secondWaiting:&thirdWaiting;
  bool *release=role==1?&releaseFirst:role==2?&releaseSecond:&releaseThird;
  if(role==1)assert(ident==&event);*ready=true;cv.notify_all();
  assert(cv.wait_for(lock,std::chrono::seconds(4),[&]{return *release;}));return THREAD_AWAKENED;
 };
 firmware.hook=[&](const std::string &step){if(step=="device-add"&&stage++==0)gate.commandSleep(&event,clockUS.load()+1000,THREAD_UNINT);};
 IOReturn a=99,b=99,c=99;
 std::thread first([&]{role=1;a=d.setSync(true);});
 {std::unique_lock<std::mutex> lock(mutex);assert(cv.wait_for(lock,std::chrono::seconds(4),[&]{return firstWaiting;}));}
 std::thread second([&]{role=2;b=d.setSync(true);});
 {std::unique_lock<std::mutex> lock(mutex);assert(cv.wait_for(lock,std::chrono::seconds(4),[&]{return secondWaiting;}));}
 std::thread third([&]{role=3;c=d.setSync(false);});
 {std::unique_lock<std::mutex> lock(mutex);assert(cv.wait_for(lock,std::chrono::seconds(4),[&]{return thirdWaiting;}));releaseFirst=true;cv.notify_all();}
 first.join();{std::lock_guard<std::mutex> lock(mutex);releaseThird=true;cv.notify_all();}
 third.join();{std::lock_guard<std::mutex> lock(mutex);releaseSecond=true;cv.notify_all();}
 second.join();firmware.hook=nullptr;gate.sleepHook=nullptr;
 assert(a==kIOReturnAborted&&b==kIOReturnAborted&&c==0&&!live(d)&&!d.txSession&&!d.syncOwner);
 assert(firmware.count("device-add")==1&&!d.timer->pending&&d.events.empty());
}
static void limits() {
 resetModel();{RecoveryFixture d;d.radio.storage.sc_power_req=1;assert(d.setSync(true)==0);auto deadline=d.syncEnable.deadlineUS;
  for(unsigned i=0;i<8;++i){++d.radio.storage.sc_awdl_epoch;clockUS+=100000;d.serviceInGate();pending(d);assert(d.syncEnable.deadlineUS==deadline);}
  ++d.radio.storage.sc_awdl_epoch;d.serviceInGate();assert(d.syncEnable.phase==AirportMtkAwdlInterface::SyncEnable::None&&d.syncEpochRebinds==8&&!live(d));
 }
 resetModel();{RecoveryFixture d;auto &sc=d.radio.storage;const auto attemptsBefore=d.syncAttempts;
  firmware.hook=[&](const std::string &step){if(step=="device-add")sc.sc_flags=MWX_FLAG_SCANNING;};
  assert(d.setSync(true)==0);auto deadline=d.syncEnable.deadlineUS;
  for(unsigned i=1;i<16;++i){sc.sc_flags=0;clockUS+=100000;d.serviceInGate();pending(d);assert(d.syncEnable.deadlineUS==deadline);}
  sc.sc_flags=0;d.serviceInGate();assert(d.syncEnable.phase==AirportMtkAwdlInterface::SyncEnable::None&&d.syncAttempts==attemptsBefore+16&&d.syncEnable.attempts==16&&d.syncExpired==1&&!live(d));firmware.hook=nullptr;
 }
 resetModel();{NativeFixture d;d.syncEnable.generation=UINT64_MAX-1;
  assert(d.setSync(true)==kIOReturnNoResources&&d.syncEnable.generation==UINT64_MAX);
  assert(d.setSync(false)==0&&!live(d)&&!d.radio.storage.sc_awdl_enabled&&d.syncEnable.generation==UINT64_MAX);
 }
}
static void quiesce() {
 resetModel();RecoveryFixture d;auto &sc=d.radio.storage;
 std::mutex mutex;std::condition_variable cv;bool begun=false,release=false;int event;
 gate.sleepHook=[&](void *ident,uint64_t,unsigned){assert(!gateDepth&&!heldHalLocks);std::unique_lock<std::mutex> lock(mutex);
  if(ident==&event){begun=true;cv.notify_all();assert(cv.wait_for(lock,std::chrono::seconds(4),[&]{return release;}));}
  else {release=true;cv.notify_all();cv.wait_for(lock,std::chrono::milliseconds(1));}
  return THREAD_AWAKENED;
 };
 firmware.hook=[&](const std::string &step){if(step=="station-response")gate.commandSleep(&event,clockUS.load()+1000,THREAD_UNINT);};
 IOReturn status=99;std::thread request([&]{status=d.setSync(true);});
 {std::unique_lock<std::mutex> lock(mutex);assert(cv.wait_for(lock,std::chrono::seconds(4),[&]{return begun;}));}
 gate.enter([&]{mwx_awdl_quiesce(&sc,true);return 0;});request.join();firmware.hook=nullptr;gate.sleepHook=nullptr;
 assert(status==0&&!live(d)&&sc.sc_awdl_stopping&&!sc.sc_awdl_busy&&!sc.sc_awdl_calls&&sc.sc_awdl_native_session==0);
 pending(d); // same intent survived reset; the quiesce really drained its operation/call
 sc.sc_awdl_stopping=false;d.serviceInGate();assert(live(d)&&d.syncEpochRebinds==1);
}
static void openWindow(NativeFixture &d) {
 d.state.init((const uint8_t *)"\x02\x01\x02\x03\x04\x05",clockUS.load());
 d.radio.give(1,6,70);d.serviceInGate();assert(d.awTimeline.open&&d.events.size()==1&&d.events[0].id==41);
}
static void offReentry() {
 for(unsigned kind=0;kind<2;++kind) {
  resetModel();NativeFixture d;openWindow(d);clockUS=d.awTimeline.lastStart+41*1024;
  d.onEnd=[&]{assert(d.setLinkState(kIO80211NetworkLinkDown,0));};
  const auto wakes=d.radio.wakes;
  if(kind==0)assert(d.setSync(false)==kIOReturnSuccess);
  else assert(d.disable(0)==kIOReturnSuccess);
  assert(!d.enabled&&!d.txSession&&!live(d)&&!d.radio.storage.sc_awdl_enabled&&!d.radio.storage.sc_awdl_configured);
  assert(!d.awTimeline.open&&!d.queueCount&&!d.timer->pending&&d.events.size()==2&&d.events[1].id==43);
  assert(d.radio.wakes>wakes); // actual PM handshake is outside this OS boundary model
  d.onEnd=nullptr;
 }
 // Unlike cancellation-only recursion, a genuinely newer native ON replaces
 // this OFF's ownership while the synchronous end callback drops the gate.
 resetModel();NativeFixture d;openWindow(d);clockUS=d.awTimeline.radioEnd;
 const auto removes=firmware.count("device-remove");
 std::mutex mutex;std::condition_variable cv;bool endWaiting=false,onWaiting=false,release=false;int event;
 gate.sleepHook=[&](void *ident,uint64_t,unsigned){assert(!gateDepth&&!heldHalLocks);std::unique_lock<std::mutex> lock(mutex);
  if(ident==&event){endWaiting=true;cv.notify_all();assert(cv.wait_for(lock,std::chrono::seconds(4),[&]{return release;}));}
  else {onWaiting=true;cv.notify_all();cv.wait_for(lock,std::chrono::milliseconds(1));}
  return THREAD_AWAKENED;
 };
 d.onEnd=[&]{gate.commandSleep(&event,clockUS.load()+1000,THREAD_UNINT);};
 IOReturn off=99,on=99;std::thread first([&]{off=d.setSync(false);});
 {std::unique_lock<std::mutex> lock(mutex);assert(cv.wait_for(lock,std::chrono::seconds(4),[&]{return endWaiting;}));}
 std::thread next([&]{on=d.setSync(true);});
 {std::unique_lock<std::mutex> lock(mutex);assert(cv.wait_for(lock,std::chrono::seconds(4),[&]{return onWaiting;}));release=true;cv.notify_all();}
 first.join();next.join();d.onEnd=nullptr;gate.sleepHook=nullptr;
 assert(off==kIOReturnAborted&&on==kIOReturnSuccess&&live(d)&&d.txSession&&!d.syncOwner);
 assert(firmware.count("device-remove")==removes&&d.events.size()==2);
}
static void snapshotReset() {
 for(unsigned kind=0;kind<3;++kind) {
  resetModel();NativeFixture d;auto &sc=d.radio.storage;
  if(kind==2)d.state.controlsHeld=true;else openWindow(d);
  auto p=new Packet;p->bytes.resize(64);d.queue[d.queueHead]=p;d.queueCount=1;
  if(kind==1)d.onEnd=[&]{assert(d.setLinkState(kIO80211NetworkLinkDown,0));};
  bool injected=false;
  halUnlockHook=[&](IOLock *lock){if(lock==sc.sc_awdl_lock&&!injected){
   injected=true;halUnlockHook=nullptr;assert(!heldHalLocks);mwx_awdl_quiesce(&sc,false);
  }};
  // The first snapshot still names the old session; the next sees the reset.
  assert(d.setSync(true)==(kind==1?kIOReturnAborted:kIOReturnSuccess)&&injected);
  assert(!d.enabled&&!d.txSession&&!d.awTimeline.open&&!d.queueCount&&!d.state.controlsHeld);
  assert(d.events.size()==(kind==2?0U:2U));if(kind!=2)assert(d.events[1].id==43);
  if(kind==1){assert(d.syncEnable.phase==AirportMtkAwdlInterface::SyncEnable::None&&!d.timer->pending);continue;}
  pending(d);auto generation=d.syncEnable.generation,accepted=d.syncEnable.acceptedAtUS,deadline=d.syncEnable.deadlineUS;
  sc.sc_awdl_stopping=false;d.serviceInGate();recovered(d,generation,accepted,deadline);
  assert(d.events.size()==(kind==2?0U:2U)); // a new ON is not itself a native AW start
 }
}
static void latePublication() {
 resetModel();RecoveryFixture d;bool injected=false;
 stringPropertyHook=[&](const char *key,const char *value){
  if(!strcmp(key,"MTKAWDLFirmwareStage")&&!strcmp(value,"enabled")&&!injected){
   injected=true;assert(d.radio.storage.sc_awdl_enabled);clockUS=d.syncEnable.deadlineUS;
  }
 };
 assert(d.setSync(true)==kIOReturnTimeout&&injected);
 stringPropertyHook=nullptr;
 assert(!live(d)&&!d.enabled&&!d.txSession&&!d.radio.storage.sc_awdl_enabled&&!d.radio.storage.sc_awdl_native_session);
 assert(!d.radio.storage.sc_awdl_configured&&!d.radio.storage.sc_awdl_filter_active);
 assert(d.syncEnable.phase==AirportMtkAwdlInterface::SyncEnable::None&&d.syncExpired==1&&!d.timer->pending);
 assert(firmware.count("device-add")==1&&firmware.count("device-remove")==1&&d.events.empty());
}
static void completionCancel() {
 for(bool deferred:{false,true}) {
  resetModel();RecoveryFixture d;
  if(deferred){d.radio.storage.sc_power_req=1;assert(d.setSync(true)==0);pending(d);}
  std::vector<IOSkywalkPacket *> packets;
  for(unsigned i=0;i<129;++i){auto p=new IOSkywalkPacket(d.txPool);p->tx=p->prepared=true;packets.push_back(p);}
  const auto done=txCompleted;
  assert(gate.enter([&]{return d.submitNative(&d,d.txSubmit,packets.data(),128,nullptr);})==128);
  assert(d.completionCount==128&&d.timer->pending&&txCompleted==done);
  ether_addr mac{{2,6,5,4,3,2}};assert(d.setMacAddress(mac)==0);
  auto extra=packets.back();
  assert(gate.enter([&]{return d.submitNative(&d,d.txSubmit,&extra,1,nullptr);})==0);
  extra->release();
  assert(d.completionCount==128&&d.timer->pending); // no unrelated submission can repair a lost wake
  d.serviceInGate();assert(!d.completionCount&&txCompleted==done+128&&!packetsLive&&!d.timer->pending&&!live(d));
 }
}
int main(int argc,char **argv) {
 assert(argc==2&&mwx_awdl_native_history_init());std::string name=argv[1];
 if(name=="admission")admission();else if(name=="task-admission")taskAdmission();else if(name=="boot")boot();
 else if(name=="stale-session")stale();else if(name=="epoch")epoch();else if(name=="deadline")deadline();
 else if(name=="errors")errors();else if(name=="filter-retry")filterRetry();else if(name=="cancel")cancel();
 else if(name=="supersession")supersession();else if(name=="limits")limits();else if(name=="quiesce")quiesce();
 else if(name=="off-reentry")offReentry();else if(name=="snapshot-reset")snapshotReset();
 else if(name=="late-publication")latePublication();else if(name=="completion-cancel")completionCancel();else assert(false);
 noResources();assert(!firmware.mbufs&&!heldHalLocks&&!gateDepth);
 std::printf("PASS sync recovery: %s (real frontend + HAL context lifecycle; modeled OS/MCU/register boundaries)\n",name.c_str());
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive-provenance", type=Path)
    parser.add_argument("--cases", nargs="+", choices=CASES, default=CASES)
    args = parser.parse_args()
    diagnostics = ROOT / "build/diagnostics"
    diagnostics.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="awdl-sync-recovery-", dir=diagnostics))
    before = L.protected_hashes()
    before[str(Path(__file__).relative_to(ROOT))] = L.digest(Path(__file__).read_bytes())
    run = L.CommandLog(out, L.host_environment())
    result = {"diagnostics": str(out), "started_at": L.utc(), "cases": {}}
    print("AWDL sync recovery evidence: "+str(out), flush=True)
    try:
        if args.archive_provenance:
            archive = json.loads(args.archive_provenance.read_text())
            if archive["rust_sources"] != L.rust_input_hashes() or L.digest(Path(archive["archive"]).read_bytes()) != archive["sha256"]:
                raise ValueError("archive/source provenance mismatch")
            result["archive_reused_from"] = str(args.archive_provenance.resolve())
        else:
            archive = L.build_fresh_archive(out, run)
        (out / "archive-provenance.json").write_text(json.dumps(archive, indent=2)+"\n")
        if "DEVELOPER_DIR" not in run.env and "CommandLineTools" in run.checked("compiler-selection", ["xcode-select", "-p"]):
            run.env["DEVELOPER_DIR"] = "/Applications/Xcode.app/Contents/Developer"
        sdk = run.checked("sdk-path", ["xcrun", "--sdk", "macosx", "--show-sdk-path"]).strip()
        ex = L.Extractor()
        (out / "extracted.cpp").write_text(generate(ex))
        (out / "main.cpp").write_text(MAIN)
        (out / "extraction.json").write_text(json.dumps({"inventory": ex.inventory,
            "source_sha256": {str(p): L.digest(t.encode()) for p, t in ex.cache.items()}}, indent=2)+"\n")
        flags = ["/usr/bin/clang++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-function",
                 "-Wno-unused-parameter", "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
                 "-fno-sanitize-recover=all", "-pthread", "-isysroot", sdk, "-I"+str(L.VENDOR / "include")]
        result["compile"] = run("compile", flags+[str(out / "main.cpp"), archive["archive"], "-o", str(out / "recovery")], 120)
        if not result["compile"]:
            for case in args.cases:
                result["cases"][case] = run(case, [str(out / "recovery"), case], 60)
                print(run.output(case), end="", flush=True)
        result["archive_unchanged"] = L.digest(Path(archive["archive"]).read_bytes()) == archive["sha256"]
    except Exception as error:
        result["error"] = repr(error)
    after = L.protected_hashes()
    after[str(Path(__file__).relative_to(ROOT))] = L.digest(Path(__file__).read_bytes())
    result["source_changes_during_test"] = sorted(k for k in before.keys() | after.keys() if before.get(k) != after.get(k))
    result["ended_at"] = L.utc()
    result["passed"] = (not result.get("error") and result.get("compile") == 0 and
                        len(result["cases"]) == len(args.cases) and not any(result["cases"].values()) and
                        result.get("archive_unchanged") and not result["source_changes_during_test"])
    result["limitations"] = ["No native/kernel/RF execution or race-freedom proof",
        "MCU/register/task/native queue/mbuf boundaries are models; reset/power flags are injected",
        "Primary state task prologue and queueing body are real, not the entire primary firmware task",
        "Only C++ is ASan/UBSan-instrumented; real Rust FFI is not sanitizer-instrumented"]
    (out / "results.json").write_text(json.dumps(result, indent=2)+"\n")
    print(json.dumps(result, indent=2), flush=True)
    return int(not result["passed"])


if __name__ == "__main__":
    raise SystemExit(main())
