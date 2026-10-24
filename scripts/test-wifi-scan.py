#!/usr/bin/env python3
"""Exercise production WCL scan admission/completion and idle retry policy."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
vendor = root / 'kext/Wifi/vendor'
wcl = (vendor / 'AirportMtkWlan/WCL/WCL.cpp').read_text()
node = (vendor / 'mtk80211/openbsd/net80211/ieee80211_node.c').read_text()
hal = (vendor / 'SimpleMtkWlan/hal_mwx/MtkMwx.cpp').read_text()

def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]

# Run the actual completion handler followed by the production end_scan
# preamble. Reaching BSS selection represents continuing the BSD scan loop.
start = node.index('void\nieee80211_end_scan(')
end_scan = node[start:node.index('    ni = RB_MIN', start)]
end_scan += '    ++bssSelections;\n}\n'
start = wcl.index('        case IEEE80211_EVT_SCAN_DONE:')
completion = wcl[start:wcl.index('        case IEEE80211_EVT_STA_ASSOC_DONE:', start)]
start = hal.index('int\nmt7921_mcu_hw_scan(')
admission = hal[start:hal.index('\n\tm = mwx_mcu_alloc_msg', start)]
admission += '\n    return 99; /* admitted to firmware submission */\n}\n'
code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
struct mtk_wlan_scan_ssids {uint8_t count=0;uint8_t lengths[10]{};uint8_t ssids[10][32]{};bool wildcard=false;};
#define AIRPORT_WCL 1
#define IEEE80211_STA_ONLY 1
#define XYLog(...) ((void)0)
#define IFF_RUNNING 1
#define IFF_DEBUG 2
#define IEEE80211_S_SCAN 1
#define IEEE80211_S_AUTH 2
#define IEEE80211_S_ASSOC 3
#define IEEE80211_S_RUN 4
#define IEEE80211_M_STA 0
#define IEEE80211_F_BGSCAN 1
#define IEEE80211_F_DISABLE_BG_AUTO_CONNECT 2
#define IEEE80211_F_ASCAN 4
#define IEEE80211_INACT_SCAN 1
#define IEEE80211_CHAN_MAX 196
#define IEEE80211_EVT_SCAN_DONE 1
#define MWX_FLAG_SCANNING 1
#define MWX_FLAG_BGSCAN 2
#define DEVNAME(sc) "test"
constexpr int EBUSY=16;
using IOReturn=int;
enum {kIOReturnSuccess, kIOReturnBadArgument, kIOReturnNotReady,
      kIOReturnBusy, kIOReturnUnsupported, kIOReturnError, kIOReturnTimeout};
struct _ifnet {int if_flags=IFF_RUNNING; const char*if_xname="test";};
struct ieee80211_channel {int ic_flags=1;};
struct ieee80211_node {};
struct ieee80211com : _ifnet {
    void*ic_softc=nullptr;
    _ifnet ic_if;
    int ic_state=IEEE80211_S_SCAN, ic_flags=0, ic_opmode=0, ic_scan_count=0;
    bool ic_wcl_scan_requested=false, ic_wcl_scan_inflight=false;
    bool ic_wcl_scan_active=false, ic_wcl_scan_restricted=false;
    bool ic_wcl_join_requested=false;
    unsigned ic_wcl_scan_ssid_length=0, ic_des_esslen=0;
    uint8_t ic_wcl_scan_ssid[32]{}, ic_wcl_scan_channels[32]{};
    mtk_wlan_scan_ssids ic_wcl_scan_targets;
    ieee80211_channel ic_channels[197];
    int(*ic_newstate)(ieee80211com*,int,int)=nullptr;
    void(*ic_event_handler)(ieee80211com*,int,void*)=nullptr;
};
struct Timer {int delay=-1; void setTimeoutMS(int n){delay=n;}
    void cancelTimeout(){delay=-1;}};
struct apple80211ScanRequest {int type=1; unsigned ssidLength=0;
    unsigned channelCount=0; uint8_t ssid[32]{}, channels[32]{};
    mtk_wlan_scan_ssids targets;};
namespace AirportWCL {
using ScanRequest=apple80211ScanRequest;
bool decodeScan(apple80211ScanRequest*p,unsigned,ScanRequest&out){out=*p;return true;}
bool scanChannelSelected(const uint8_t*,bool,unsigned){return true;}
}
struct Hal {ieee80211com ic; ieee80211com*get80211Controller(){return &ic;}};
void ieee80211_begin_cache_bgscan(_ifnet*p){
    // The production routine sets BGSCAN before calling the HAL.
    auto ic=reinterpret_cast<ieee80211com*>(reinterpret_cast<char*>(p)-sizeof(_ifnet));
    ic->ic_flags |= IEEE80211_F_BGSCAN;
}
void ieee80211_clean_inactive_nodes(ieee80211com*,int){}
unsigned starts=0,bssSelections=0;
int requestScan(ieee80211com*ic,int,int){++starts;ic->ic_wcl_scan_inflight=true;return 0;}
struct AirportMtkSkywalkInterface {
    Hal radio; Hal*fHalService=&radio; Timer timer; Timer*scanSource=&timer;
    bool scanPending=false,joinPending=false,scanCompleted=false,scanSkipCompletion=false;
    IOReturn beginWCLScan(apple80211ScanRequest*);
    void scanDone(){auto ic=&radio.ic;switch(IEEE80211_EVT_SCAN_DONE){
''' + completion + r'''
    }}
};
AirportMtkSkywalkInterface*current;
void event(ieee80211com*,int,void*){current->scanDone();}
''' + function(wcl, 'IOReturn AirportMtkSkywalkInterface::beginWCLScan(') + end_scan + r'''
struct mbuf {};
struct mt76_connac_hw_scan_req {};
struct mwx_softc {
    ieee80211com sc_ic; bool sc_awdl_enabled=true,sc_awdl_scan_suspended=false;
    int sc_ns_state=IEEE80211_S_RUN;
    unsigned sc_flags=0, sc_scan_requests=0; int sc_scan_to=0;
    unsigned sc_awdl_scan_preempted=0;
};
unsigned aborts=0;
void mwx_awdl_abort(mwx_softc*){++aborts;}
void timeout_add_sec(int*,int){}
''' + admission + function(hal, 'int\nmwx_bgscan(') + r'''
int main(){
    AirportMtkSkywalkInterface d; current=&d; auto&ic=d.radio.ic;
    ic.ic_event_handler=event; ic.ic_newstate=requestScan;
    apple80211ScanRequest request;
    // Idle logical SCAN must stop retrying with no airportd client.
    ieee80211_end_scan(reinterpret_cast<_ifnet*>(&ic));
    assert(bssSelections==0);
    // Directed scans from idle SCAN start immediately, despite stale SSID
    // metadata. Their first completion is valid, so no empty-list timeout.
    ic.ic_wcl_scan_ssid_length=4;request.ssidLength=4;
    assert(d.beginWCLScan(&request)==0 && starts==1 && !d.scanSkipCompletion);
    ic.ic_wcl_scan_inflight=false;
    ieee80211_end_scan(reinterpret_cast<_ifnet*>(&ic));
    assert(d.scanCompleted && !ic.ic_wcl_scan_requested && bssSelections==0);
    // A directed request arriving during another hardware scan must wait
    // for its own iteration, rather than reporting the old scan's result.
    d.scanPending=false;ic.ic_wcl_scan_inflight=true;
    assert(d.beginWCLScan(&request)==0 && d.scanSkipCompletion);
    ieee80211_end_scan(reinterpret_cast<_ifnet*>(&ic));
    assert(!d.scanCompleted && ic.ic_wcl_scan_requested && bssSelections==1);
    ieee80211_end_scan(reinterpret_cast<_ifnet*>(&ic));
    assert(d.scanCompleted && !ic.ic_wcl_scan_requested && bssSelections==1);
    // A list target must also defer an unrelated firmware completion.
    d.scanPending=false;ic.ic_wcl_scan_inflight=true;
    request.ssidLength=0;request.targets.count=1;request.targets.lengths[0]=4;
    memcpy(request.targets.ssids[0],"Dino",4);
    assert(d.beginWCLScan(&request)==0 && d.scanSkipCompletion);
    assert(ic.ic_wcl_scan_targets.count==1);
    ieee80211_end_scan(reinterpret_cast<_ifnet*>(&ic));
    assert(!d.scanCompleted && ic.ic_wcl_scan_requested);
    ieee80211_end_scan(reinterpret_cast<_ifnet*>(&ic));
    assert(d.scanCompleted && !ic.ic_wcl_scan_requested);
    bssSelections=1;
    // Passive scans clear directed targets inherited from an earlier request.
    d.scanPending=false;ic.ic_wcl_scan_inflight=false;request.type=2;
    assert(d.beginWCLScan(&request)==0 && !ic.ic_wcl_scan_targets.count);
    ieee80211_end_scan(reinterpret_cast<_ifnet*>(&ic));
    request.type=1;
    // Joining keeps candidate selection/retries, even after scan completion.
    ic.ic_wcl_join_requested=true;
    ieee80211_end_scan(reinterpret_cast<_ifnet*>(&ic));
    assert(bssSelections==2);
    // Authentication and association reject competing scans.
    d.scanPending=false;ic.ic_state=IEEE80211_S_AUTH;
    assert(d.beginWCLScan(&request)==kIOReturnBusy);
    ic.ic_state=IEEE80211_S_ASSOC;
    assert(d.beginWCLScan(&request)==kIOReturnBusy);
    // AWDL may suspend an unsolicited associated scan, but requested scans
    // and joins must take the radio and finish instead of timing out.
    mwx_softc sc;sc.sc_ic.ic_state=IEEE80211_S_RUN;sc.sc_ic.ic_softc=&sc;
    assert(mt7921_mcu_hw_scan(&sc,1)==0 && sc.sc_awdl_scan_suspended && !aborts);
    // A rejected autonomous scan must not tell net80211 it submitted work:
    // otherwise F_BGSCAN blocks every subsequent network-list request.
    assert(mwx_bgscan(&sc.sc_ic)==EBUSY && !sc.sc_flags && !sc.sc_scan_requests);
    sc.sc_ic.ic_wcl_scan_requested=true;
    assert(mt7921_mcu_hw_scan(&sc,1)==99 && !sc.sc_awdl_scan_suspended && aborts==1);
    sc.sc_ic.ic_wcl_scan_requested=false;sc.sc_ic.ic_wcl_join_requested=true;
    // WCL retains join_requested on a successful link; it must not turn
    // every autonomous roam scan into an explicit join request.
    assert(mt7921_mcu_hw_scan(&sc,1)==0 && sc.sc_awdl_scan_suspended && aborts==1);
    sc.sc_ns_state=IEEE80211_S_SCAN;
    assert(mt7921_mcu_hw_scan(&sc,1)==99 && aborts==2);
    sc.sc_ic.ic_state=IEEE80211_S_SCAN;sc.sc_ic.ic_wcl_join_requested=false;
    assert(mt7921_mcu_hw_scan(&sc,0)==99 && aborts==3);
    puts("PASS: idle scan release, explicit scan restart/completion, stale completion, join priority, AWDL scan admission");
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-wifi-scan-') as directory:
    work = Path(directory)
    (work / 'test.cpp').write_text(code)
    subprocess.run(['clang++', '-std=c++14', '-fsanitize=address,undefined',
                    str(work / 'test.cpp'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
