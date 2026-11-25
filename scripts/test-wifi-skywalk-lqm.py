#!/usr/bin/env python3
"""Run the actual LQM gate body against mocked OS boundaries under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'kext/Wifi/vendor/AirportMtkWlan/WCL/WCL.cpp').read_text()
start = source.index('IOReturn AirportMtkSkywalkInterface::updateWCLLinkQualityGated(')
opening = source.index('{', start)
depth = 1
end = opening + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
body = source[start:end]
assert '__ZN25IOSkywalkNetworkInterface17reportLinkQualityEj' in source
owner = (ROOT / 'kext/Wifi/vendor/AirportMtkWlan/AirportMtkWlan.cpp').read_text()
assert 'static_cast<AirportMtkSkywalkInterface *>(self->fNetIf)->pollWCLLinkQuality();' in owner
assert 'PE_parse_boot_argn("amtk_lqm"' in source

code = r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include "WCL.hpp"
using IOReturn = int;
constexpr int kIOReturnSuccess=0,kIOReturnNotReady=1,kIOReturnNotPermitted=2;
constexpr int IFF_UP=1,IFF_RUNNING=2,IEEE80211_S_RUN=4,kIO80211NetworkLinkUp=1,MTK_MIN_DBM=-100;
struct OSObject {};
struct IOSkywalkNetworkInterface:OSObject {};
struct Node { int ni_rssi=45; uint8_t ni_bssid[6]{}; };
struct IC { int ic_state=IEEE80211_S_RUN; Node *ic_bss; };
struct HAL { IC *ic; IC *get80211Controller(){ return ic; } };
struct Ifnet { int flags=IFF_UP|IFF_RUNNING; };
using ifnet_t=Ifnet *;
int ifnet_flags(ifnet_t p){ return p->flags; }
struct Queue { bool gated=true; bool inGate(){return gated;} };
struct Controller {
 Queue queue; bool queuePresent=true; unsigned posts=0; int lastRSSI=0;
 std::map<std::string,uint64_t> properties;
 Queue *getWorkQueue(){return queuePresent?&queue:nullptr;}
 template<class T> void setProperty(const char *key,T value,unsigned){properties[key]=uint64_t(value);}
 void postMessage(void *,int kind,const void *p,unsigned length,bool sync){
  assert(kind==AirportWCL::LqmUpdate && sync && length==0x1dc);
  auto &u=*static_cast<const AirportWCL::LqmBeaconUpdate *>(p);
  assert(u.rssiValid==1 && !u.snapshotValid && !u.countersValid && !u.beaconValid);
  auto bytes=static_cast<const uint8_t *>(p);
  for(unsigned i=0;i<length;++i)if(i!=0 && (i<4 || i>=8))assert(!bytes[i]);
  ++posts; lastRSSI=u.rssi;
 }
};
unsigned reports=0; unsigned lastQuality=0; IOReturn reportResult=0;
IOReturn mtkReportLinkQuality(IOSkywalkNetworkInterface *,unsigned value){++reports;lastQuality=value;return reportResult;}
struct AirportMtkSkywalkInterface:IOSkywalkNetworkInterface {
 bool wclStopping=false,portAuthorized=true,roamPending=false,wclSkywalkLqmEnabled=true,matching=true;
 HAL *fHalService; Controller *instance; void *mExpansionData=this;
 Ifnet bsd; ifnet_t current=&bsd; int link=1,legacyCalls=0;
 unsigned wclLqmPollTicks=0; uint64_t wclLqmReportAttempts=0,wclLqmRSSIPosts=0;
 int wclReportedLinkQuality=-2; ifnet_t wclLinkQualityInterface=nullptr;
 ifnet_t getBSDInterface(){return current;}
 int linkState(){return link;}
 bool joinBssidMatches(const uint8_t *){return matching;}
 void setLQM(unsigned long long){++legacyCalls;}
 static IOReturn updateWCLLinkQualityGated(OSObject *,void *,void *,void *,void *);
 IOReturn update(bool force=false,bool periodic=false){return updateWCLLinkQualityGated(nullptr,this,
  reinterpret_cast<void *>(uintptr_t(force)),reinterpret_cast<void *>(uintptr_t(periodic)),nullptr);}
};
'''+body+r'''
int main(){
 Node node; IC ic{IEEE80211_S_RUN,&node}; HAL hal{&ic}; Controller owner;
 AirportMtkSkywalkInterface s; s.fHalService=&hal;s.instance=&owner;
 assert(s.update(true)==0 && reports==1 && lastQuality==100 && s.legacyCalls==1);
 assert(s.update()==0 && reports==1);
 s.update(false,true);assert(owner.posts==0);
 s.update(false,true);assert(owner.posts==1 && owner.lastRSSI==-55 && reports==1);
 node.ni_rssi=25;s.update();assert(lastQuality==50);
 node.ni_rssi=5;s.update();assert(lastQuality==20);
 // A failed exported call is not cached, and the next poll retries it.
 node.ni_rssi=45;reportResult=99;unsigned before=reports;
 assert(s.update()==99 && reports==before+1 && s.wclReportedLinkQuality==20);
 assert(s.update()==99 && reports==before+2);
 reportResult=0;s.update();assert(s.wclReportedLinkQuality==100);
 // Held roam preserves quality; explicit down (including stop) still resets.
 s.roamPending=true;before=reports;s.update(true);assert(reports==before);
 s.portAuthorized=false;s.update(true);assert(reports==before+1 && lastQuality==0xfffffffeU);
 unsigned posts=owner.posts;s.update(false,true);s.update(false,true);assert(owner.posts==posts);
 s.roamPending=false;s.portAuthorized=true;
 for(int invalid=0;invalid<4;++invalid){
  ic.ic_state=IEEE80211_S_RUN;s.matching=true;s.link=1;s.bsd.flags=IFF_UP|IFF_RUNNING;
  if(invalid==0)ic.ic_state=0;if(invalid==1)s.matching=false;
  if(invalid==2)s.link=0;if(invalid==3)s.bsd.flags=IFF_UP;
  s.update(true);assert(lastQuality==0xfffffffeU);
  posts=owner.posts;s.update(false,true);s.update(false,true);assert(owner.posts==posts);
 }
 ic.ic_state=IEEE80211_S_RUN;s.matching=true;s.link=1;s.bsd.flags=IFF_UP|IFF_RUNNING;
 // Interface replacement forces reporting even at the same RSSI.
 s.update(true);Ifnet replacement;before=reports;s.current=&replacement;s.update();assert(reports==before+1);
 // Partial-init, stopped and wrong-gate calls never call Skywalk or WCL.
 before=reports;s.current=nullptr;assert(s.update(true)==kIOReturnNotReady);
 s.current=&s.bsd;s.wclStopping=true;assert(s.update(true)==kIOReturnNotReady);
 s.wclStopping=false;owner.queue.gated=false;assert(s.update(true)==kIOReturnNotPermitted);
 owner.queue.gated=true;owner.queuePresent=false;assert(s.update(true)==kIOReturnNotPermitted);
 owner.queuePresent=true;assert(reports==before);
 // A/B opt-out retains legacy reports and suppresses all periodic posts.
 s.wclSkywalkLqmEnabled=false;posts=owner.posts;s.update(true);s.update(false,true);s.update(false,true);
 assert(reports==before && owner.posts==posts);
 // RSSI-only post must not advertise a placeholder/non-negative RSSI.
 s.wclSkywalkLqmEnabled=true;node.ni_rssi=100;s.update(false,true);s.update(false,true);assert(owner.posts==posts);
 AirportWCL::LqmBeaconUpdate beacon;
 assert(AirportWCL::makeBeaconUpdate(8,beacon));
 assert(beacon.snapshotValid && beacon.countersValid && beacon.beaconValid && !beacon.rssiValid);
 puts("PASS: production LQM gate, RSSI ABI/cadence, report retry, down/stop/roam, A/B opt-out and lifecycle guards");
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-skywalk-lqm-') as tmp:
    src = Path(tmp) / 'test.cpp'
    binary = Path(tmp) / 'test'
    src.write_text(code)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I', str(ROOT / 'kext/include'), '-I', str(ROOT / 'kext/Wifi'),
                    '-I', str(ROOT / 'kext/Wifi/vendor/AirportMtkWlan/WCL'),
                    '-Wl,-U,_mtk_wlan_scan_ssids_parse', str(src), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
