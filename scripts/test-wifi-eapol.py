#!/usr/bin/env python3
"""Exercise BSD EAPOL transport before RSN authorization using production code."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
vendor = root / 'kext/Wifi/vendor'
controller = (vendor / 'AirportMtkWlan/AirportMtkWlan.cpp').read_text()
wcl = (vendor / 'AirportMtkWlan/WCL/WCL.cpp').read_text()
rx = (vendor / 'mtk80211/openbsd/net80211/ieee80211_input.c').read_text()
tx = (vendor / 'mtk80211/openbsd/net80211/ieee80211_output.c').read_text()
proto = (vendor / 'mtk80211/openbsd/net80211/ieee80211_proto.c').read_text()


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


# The early AssocComplete event precedes HAL setup. Transport must follow
# the committed state instead, including transitions back out of RUN.
newstate = function(proto, 'int\nieee80211_newstate(')
notify = newstate[newstate.index('\t/* MtkWireless: user-space EAP'):]
notify = notify[:notify.index('\treturn 0;')]
start = wcl.index('        case IEEE80211_EVT_STATE_CHANGED:')
state_event = wcl[start:wcl.index('        case IEEE80211_EVT_STA_ROAM_START:', start)]
assert 'setEAPOLTransport(false)' in function(wcl, 'IOReturn AirportMtkSkywalkInterface::leaveWCLNetwork(')
assert 'setEAPOLTransport(false)' in function(wcl, 'IOReturn AirportMtkSkywalkInterface::stopWCLGated(')

# Run the actual pre-authorization guard from the TX encapsulation path.
start = tx.index('    if ((ic->ic_flags & IEEE80211_F_RSNON) &&\n        !ni->ni_port_valid')
tx_guard = tx[start:tx.index('    if ((ic->ic_flags & IEEE80211_F_COUNTERM)', start)]

code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#define __PRIVATE_SPI__ 1
#define IEEE80211_STA_ONLY 1
#define NBPFILTER 0
#define XYLog(...) ((void)0)
#define DPRINTF(...) ((void)0)
#define mtk_printf(...) ((void)0)
#undef htons
#define htons(x) (x)
#define ic_ac ac
#define ETHERTYPE_PAE 0x888e
#define IEEE80211_F_RSNON 1
#define IFF_RUNNING 1
#define IEEE80211_S_INIT 0
#define IEEE80211_S_SCAN 1
#define IEEE80211_S_AUTH 2
#define IEEE80211_S_ASSOC 3
#define IEEE80211_S_RUN 4
#define IEEE80211_EVT_STATE_CHANGED 11
#define kIONetworkLinkValid 1
#define kIONetworkLinkActive 2
#define kIONetworkLinkNoNetworkChange 4
#define kIO80211NetworkLinkDown 0
#define kIO80211NetworkLinkUp 1
#define MBUF_DONTWAIT 0
#define mtod(m,type) reinterpret_cast<type>(&((m)->header))
#define OSDynamicCast(type,p) static_cast<type*>(p)
using IOReturn=int;
using UInt32=uint32_t;
using UInt64=uint64_t;
using IO80211LinkState=int;
constexpr int kIOReturnSuccess=0;
struct OSObject {};
struct OSData {};
struct IONetworkMedium {};
struct ether_header {uint8_t ether_dhost[6]{},ether_shost[6]{};uint16_t ether_type;};
struct mbuf {ether_header header;bool freed=false;};
using mbuf_t=mbuf*;
struct mbuf_list {std::vector<mbuf_t> packets;};
void ml_enqueue(mbuf_list*ml,mbuf_t m){ml->packets.push_back(m);}
void mbuf_freem(mbuf_t m){assert(!m->freed);m->freed=true;}
int mbuf_dup(mbuf_t m,int,mbuf_t*out){*out=new mbuf(*m);return 0;}
struct ieee80211_node {bool ni_port_valid=false;uint8_t ni_11d_cc[2]={};};
struct Queue {int flushes=0;};
void ifq_flush(Queue*q){++q->flushes;}
void mq_purge(Queue*q){++q->flushes;}
struct Stats {unsigned inputPackets=0,is_tx_noauth=0,is_rx_unauth=0;};
struct _ifnet {int if_flags=IFF_RUNNING;Stats stat;Stats*netStat=&stat;Queue if_snd;};
struct ieee80211com {bool ic_wcl_enterprise=false;int ic_state=IEEE80211_S_INIT;
    uint8_t ic_11d_bss_cc[2]={};
    int ic_flags=IEEE80211_F_RSNON;uintptr_t ic_deauth_reason=0;Stats ic_stats;
    _ifnet ic_if;struct { _ifnet&ac_if; } ac{ic_if};
    Queue ic_mgtq;ieee80211_node bss;ieee80211_node*ic_bss=&bss;
    void(*ic_event_handler)(ieee80211com*,int,void*)=nullptr;};
unsigned kernelEapol=0;
void ieee80211_eapol_key_input(ieee80211com*,mbuf_t m,ieee80211_node*){
    ++kernelEapol;mbuf_freem(m);
}
struct Hal {ieee80211com ic;ieee80211com*get80211Controller(){return &ic;}};
struct Bsd {bool running=false,output=false;unsigned starts=0,stops=0,flushes=0;
    bool setLinkState(int s){running=s==kIO80211NetworkLinkUp;return true;}
    void startOutputThread(){output=true;++starts;}
    void stopOutputThread(){output=false;++stops;}
    void flushOutputQueue(){++flushes;}};
struct AirportMtkSkywalkInterface {bool authorized=false;unsigned joins=0;
    bool holdWCLRoamLink(bool){return false;}
    void handleWCLLink(bool up){authorized=up;if(up)++joins;}};
struct Base {bool setLinkStatus(UInt32,const IONetworkMedium*,UInt64,OSData*){return true;}};
struct AirportMtkWlan;
struct Gate {AirportMtkWlan*owner=nullptr;
    using Action=IOReturn(*)(OSObject*,void*,void*,void*,void*);
    IOReturn runAction(Action a,void*p=nullptr,void*q=nullptr);};
struct AirportMtkWlan : OSObject,Base {
    using super=Base;Hal hal;Hal*fHalService=&hal;Bsd bsd;Bsd*bsdInterface=&bsd;
    AirportMtkSkywalkInterface native;AirportMtkSkywalkInterface*fNetIf=&native;
    Gate gate{this};UInt32 currentStatus=kIONetworkLinkValid;
    bool eapolTransportActive=false;
    bool countryCodeAnnounceNeeded(){return false;}
    Gate*getCommandGate(){return &gate;}
    void setEAPOLTransport(bool);
    static IOReturn setEAPOLTransportGated(OSObject*,void*,void*,void*,void*);
    static IOReturn setLinkStateGated(OSObject*,void*,void*,void*,void*);
    bool setLinkStatus(UInt32,const IONetworkMedium* =nullptr,UInt64=0,OSData* =nullptr);
};
IOReturn Gate::runAction(Action a,void*p,void*q){return a(owner,p,q,nullptr,nullptr);}
''' + function(controller, 'void AirportMtkWlan::setEAPOLTransport(') + \
    function(controller, 'IOReturn AirportMtkWlan::setEAPOLTransportGated(') + \
    function(controller, 'IOReturn AirportMtkWlan::\nsetLinkStateGated(') + \
    function(controller, 'bool AirportMtkWlan::\nsetLinkStatus(') + \
    function(rx, 'void\nieee80211_enqueue_data(') + r'''
bool txAllowed(ieee80211com*ic,ieee80211_node*ni,ether_header eh){
''' + tx_guard + r'''
    return true;
bad:
    return false;
}
AirportMtkWlan*current;
void stateEvent(ieee80211com*ic,int code,void*){
    auto instance=current;bool wclStopping=false;
    auto deferWCLCountryUpdate=[]{};
    switch(code){
''' + state_event + r'''
    }
}
void commitState(ieee80211com*ic,int state){
    ic->ic_state=state;
''' + notify + r'''
}
int main(){
    AirportMtkWlan d;current=&d;auto&ic=d.hal.ic;
    ic.ic_event_handler=stateEvent;
    // Open and PSK retain their existing link/output behavior.
    commitState(&ic,IEEE80211_S_RUN);
    assert(!d.bsd.output && !d.eapolTransportActive);
    d.setLinkStatus(kIONetworkLinkValid|kIONetworkLinkActive);
    assert(d.bsd.output && d.native.authorized && d.native.joins==1);
    d.setLinkStatus(kIONetworkLinkValid);
    assert(!d.bsd.output && !d.bsd.running);
    ic.ic_wcl_enterprise=true;
    for(int state : {IEEE80211_S_INIT,IEEE80211_S_SCAN,IEEE80211_S_AUTH,IEEE80211_S_ASSOC}){
        commitState(&ic,state);assert(!d.eapolTransportActive && !d.bsd.output);
    }
    commitState(&ic,IEEE80211_S_RUN);
    assert(d.eapolTransportActive && d.bsd.running && d.bsd.output);
    assert(!d.native.authorized && d.native.joins==1 && !ic.bss.ni_port_valid);
    unsigned starts=d.bsd.starts;
    commitState(&ic,IEEE80211_S_RUN);assert(d.bsd.starts==starts);
    // The first EAP request reaches the BSD list with its Ethernet header,
    // and its response passes TX while ordinary IPv4/IPv6 are still denied.
    for(uint16_t type : {uint16_t(ETHERTYPE_PAE),uint16_t(0x0800),uint16_t(0x86dd)}){
        mbuf m;m.header.ether_type=type;mbuf_list ml;
        ieee80211_enqueue_data(&ic,&m,&ic.bss,0,&ml);
        assert(m.freed);
        assert(ml.packets.size()==(type==ETHERTYPE_PAE?1u:0u));
        for(auto copy:ml.packets){assert(copy->header.ether_type==type);delete copy;}
        assert(txAllowed(&ic,&ic.bss,m.header)==(type==ETHERTYPE_PAE));
    }
    assert(kernelEapol==1);
    // Port authorization retains output and publishes the normal link.
    ic.bss.ni_port_valid=true;
    d.setLinkStatus(kIONetworkLinkValid|kIONetworkLinkActive);
    assert(d.bsd.output && d.native.authorized && d.native.joins==2);
    ether_header data{};data.ether_type=0x86dd;assert(txAllowed(&ic,&ic.bss,data));
    d.setEAPOLTransport(false);assert(d.bsd.output); // authorized link owns it
    d.setLinkStatus(kIONetworkLinkValid);
    // Timeout, reauthentication, and reset close a still unauthorized port.
    ic.bss.ni_port_valid=false;
    for(int state : {IEEE80211_S_SCAN,IEEE80211_S_AUTH,IEEE80211_S_INIT}){
        commitState(&ic,IEEE80211_S_RUN);assert(d.bsd.output);
        commitState(&ic,state);
        assert(!d.eapolTransportActive && !d.bsd.output && !d.bsd.running);
    }
    commitState(&ic,IEEE80211_S_RUN);
    d.setEAPOLTransport(false);assert(!d.bsd.output && !d.bsd.running); // abort/stop
    ic.ic_if.if_flags=0;commitState(&ic,IEEE80211_S_RUN);assert(!d.bsd.output);
    ic.ic_if.if_flags=IFF_RUNNING;d.bsdInterface=nullptr;
    commitState(&ic,IEEE80211_S_RUN);assert(!d.eapolTransportActive);
    puts("PASS: Enterprise EAPOL BSD transport, RX delivery, TX/RX port gates, authorization and teardown");
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-wifi-eapol-') as tmp:
    work = Path(tmp)
    (work / 'test.cpp').write_text(code)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra',
                    '-Wno-unused-parameter', '-Wno-unused-variable',
                    '-Wno-unused-but-set-variable',
                    '-fsanitize=address,undefined', str(work / 'test.cpp'),
                    '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
