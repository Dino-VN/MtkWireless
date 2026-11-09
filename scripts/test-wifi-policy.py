#!/usr/bin/env python3
"""Exercise production WCL compatibility admission and wake capability methods."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
vendor = root / 'kext/Wifi/vendor'
net = vendor / 'mtk80211/openbsd/net80211'
wcl = (vendor / 'AirportMtkWlan/WCL/WCL.cpp').read_text()
interface = (vendor / 'AirportMtkWlan/AirportMtkSkywalkInterface.hpp').read_text()
controller = (vendor / 'AirportMtkWlan/AirportMtkWlan.cpp').read_text()
apple = (vendor / 'include/Airport/apple80211_var.h').read_text()
crypto = (net / 'ieee80211_crypto.h').read_text()


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


def enum(source, name):
    start = source.index('enum ' + name)
    return source[start:source.index('\n};', start) + 3]


join = function(wcl, 'IOReturn AirportMtkSkywalkInterface::startWCLJoin(')
admission = join[join.index('{') + 2:join.index('    WCL_LOG(')]
binding = join[join.index('    ic->ic_des_esslen = join.ssidLength;'):
               join.index('    // Same-ESS roaming')]
compat_call = re.search(r'^    applyJoinCompat\([^;]*\);$', join, re.M).group()
compat_decl = re.search(r'void applyJoinCompat\([^;]*\);', interface).group()
deferral = join[join.index('    if (supported && !joinPending && ic->ic_wcl_mac_reconfig &&'):
                join.index('    // A user join outranks a scan:')]
scan_admission = join[join.index('    if (supported && !joinPending && scanPending) {'):
                      join.index('    if (!supported ||')]
guard = join[join.index('    if (!supported ||'):join.index('    clearWCLRoam();')]
# Convert only boundary return values to this policy fixture's bool result.
deferral = deferral.replace('return kIOReturnSuccess;', 'return true;')
guard = guard.replace('return !supported ? kIOReturnUnsupported : kIOReturnBusy;', 'return false;')
newstate_failure = join[join.index('        WCL_LOG("join rejected: newstate SCAN failed'):
                        join.index('        return kIOReturnError;')]
assert join.index('    restoreJoinCompat();') > join.index('    if (!supported ||')
assert join.index('    restoreJoinCompat();') < join.index('    ieee80211_disable_rsn(ic);')
# Include the real post-erasure call, not a call invented by the test.
erase = '    explicit_bzero(&join, sizeof(join));'
assert join.rindex(erase) < join.index(compat_call)
assert join.index(compat_call) < join.index('    if (ic->ic_newstate(')
constants = '\n'.join(line for line in (net / 'ieee80211_var.h').read_text().splitlines()
                      if re.match(r'#define\s+IEEE80211_(?:F_HEON|F_DESBSSID|F_BGSCAN|C_MFP)\s', line))
diag = (net / 'mtk_join_diag.h').read_text()
diag = diag[:diag.index('/* MtkWireless: a failed attempt.')] + '#endif\n'
code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <net/if.h>
#include "WCL.hpp"
#define explicit_bzero(p,n) memset(p,0,n)
#define WCL_LOG(...) ((void)0)
constexpr int kIOReturnAborted=3;
uint64_t mtk_join_now(){return 100;}
''' + constants + '\n' + enum(apple, 'apple80211_authtype_upper') + '\n' + \
    enum(crypto, 'ieee80211_akm') + '\n' + diag + r'''
struct ieee80211com {
    uint32_t ic_caps=IEEE80211_C_MFP|0x100,ic_flags=IEEE80211_F_HEON|IEEE80211_F_BGSCAN;
    uint32_t ic_rsnakms=IEEE80211_AKM_PSK;
    uint8_t ic_wcl_compat_phy=0;
    unsigned ic_des_esslen=0;
    uint8_t ic_des_essid[32]{},ic_des_bssid[6]{};
    mtk_join_diag ic_join_diag{};
    struct { unsigned if_flags=IFF_UP|IFF_RUNNING; } ic_if;
    bool ic_wcl_mac_reconfig=false,ic_wcl_join_requested=false,ic_wcl_enterprise=false;
    uint8_t ic_psk[32]{};bool saeForgotten=false;
};
void ieee80211_sae_forget(ieee80211com *ic){ic->saeForgotten=true;}
struct Timer {
    unsigned armed=0,canceled=0;int duration=0;
    void setTimeoutMS(int ms){++armed;duration=ms;}
    void cancelTimeout(){++canceled;duration=0;}
};
struct Hal {
    ieee80211com radio;
    ieee80211com *get80211Controller(){return &radio;}
};
struct AirportMtkSkywalkInterface {
    Hal hal;Hal *fHalService=&hal;
    bool joinCompatEnabled=true,joinCompatActive=false;
    bool joinCompatClearedHE=false,joinCompatClearedMFP=false;
    uint8_t joinCompatSsid[32]{},joinCompatSsidLength=0,joinCompatLevel=0;
    uint8_t joinHotspotLevel=3,joinBSSID[6]{};
    bool joinBssidSpecified=false,joinPending=false,classifiedSae=false;
    bool joinDeferred=false,scanPending=false;
    AirportWCL::JoinRequest deferredJoin{};
    Timer timer;Timer *joinTimer=&timer;unsigned scansAborted=0,pmksaCleared=0;
    ''' + compat_decl + r'''
    void restoreJoinCompat();
    void finishWCLScan(int status){assert(status==kIOReturnAborted);scanPending=false;++scansAborted;}
    void clearWCLPMKSA(){++pmksaCleared;}
    // Admission, deferred/busy guards, binding and fallback are production
    // excerpts; RSN/leave/scan scheduling is not modeled as a full join.
    bool beginPolicy(AirportWCL::JoinRequest join){
''' + admission + r'''
        (void)enterprise;(void)pskSide;classifiedSae=sae;
''' + deferral + scan_admission + guard + binding + erase + '\n' + compat_call + r'''
        return true;
    }
    // Exercise the real immediate-SCAN-failure cleanup after profile application.
    void failSCAN(){auto *ic=fHalService->get80211Controller();
''' + newstate_failure + r'''
    }
};
''' + function(wcl, 'void AirportMtkSkywalkInterface::restoreJoinCompat(') + \
    function(wcl, 'void AirportMtkSkywalkInterface::applyJoinCompat(') + r'''
using IOReturn=int;
using UInt32=uint32_t;
constexpr IOReturn kIOReturnSuccess=0,kIOReturnUnsupported=1;
constexpr UInt32 kIOEthernetWakeOnMagicPacket=1;
constexpr UInt32 kIOPacketFilterMulticast=0x10,kIOPacketFilterPromiscuous=0x20;
struct OSSymbol {};
const OSSymbol networkGroup{},wakeGroup{},otherGroup{};
const OSSymbol *gIONetworkFilterGroup=&networkGroup;
const OSSymbol *gIOEthernetWakeOnLANFilterGroup=&wakeGroup;
struct IOEthernetController {
    mutable unsigned baseCalls=0;
    IOReturn getPacketFilters(const OSSymbol *group,UInt32 *filters)const{
        ++baseCalls;
        // MacKernelSDK's documented default: other filter groups report zero.
        *filters=group==gIONetworkFilterGroup ? 0xff : 0;
        return kIOReturnSuccess;
    }
};
struct AirportMtkWlan : IOEthernetController {
    bool magicPacketSupported=false,magicPacketEnabled=false;
    IOReturn getPacketFilters(const OSSymbol *,UInt32 *)const;
    IOReturn setWakeOnMagicPacket(bool);
};
''' + function(controller, 'IOReturn AirportMtkWlan::getPacketFilters(') + \
    function(controller, 'IOReturn AirportMtkWlan::setWakeOnMagicPacket(') + r'''
AirportWCL::JoinRequest request(uint32_t auth,bool password=false){
    AirportWCL::JoinRequest j{};
    j.lowerAuth=1;j.upperAuth=auth;j.ssidLength=4;memcpy(j.ssid,"test",4);
    j.bssid[0]=2;j.bssid[5]=1;
    if(auth){
        j.keyCipher=password?AirportWCL::CipherPassword:6;
        j.keyLength=password?12:32;
        memset(j.key,'x',j.keyLength);
    }
    return j;
}
void pristine(const AirportMtkSkywalkInterface &s,uint32_t flags,uint32_t caps){
    const auto &ic=s.hal.radio;
    assert(!s.joinCompatActive && !s.joinCompatClearedHE && !s.joinCompatClearedMFP);
    assert(!ic.ic_wcl_compat_phy && ic.ic_flags==flags && ic.ic_caps==caps);
}
void compatible(const AirportMtkSkywalkInterface &s,unsigned level,bool hadMfp=true){
    const auto &ic=s.hal.radio;
    assert(s.joinCompatActive && ic.ic_wcl_compat_phy==level);
    assert(!(ic.ic_flags&IEEE80211_F_HEON) && !(ic.ic_caps&IEEE80211_C_MFP));
    assert(ic.ic_flags&IEEE80211_F_BGSCAN);assert(ic.ic_caps&0x100);
    assert(ic.ic_join_diag.Compat==(1u|2u|(hadMfp?4u:0u)|(level>=2?8u:0u)|(level>=3?16u:0u)));
}
void testCompat(){
    // A local BSSID is not proof of a PSK hotspot. Open, WEP and 802.1X
    // must retain HE/PMF and must not inherit an old PSK compatibility level.
    for(uint32_t auth:{0u,uint32_t(APPLE80211_AUTHTYPE_WPA),uint32_t(APPLE80211_AUTHTYPE_WPA2),
                      uint32_t(APPLE80211_AUTHTYPE_SHA256_8021X),
                      uint32_t(APPLE80211_AUTHTYPE_WPA2|APPLE80211_AUTHTYPE_SHA256_8021X)}){
        for(bool sticky:{false,true}){
            AirportMtkSkywalkInterface s;auto &ic=s.hal.radio;
            ic.ic_rsnakms=auth?IEEE80211_AKM_8021X|IEEE80211_AKM_SHA256_8021X:0;
            if(sticky){s.joinCompatSsidLength=4;memcpy(s.joinCompatSsid,"test",4);s.joinCompatLevel=2;}
            auto j=request(auth);
            const auto flags=ic.ic_flags|IEEE80211_F_DESBSSID,caps=ic.ic_caps;
            assert(s.beginPolicy(j));pristine(s,flags,caps);
            assert(ic.ic_join_diag.Compat==0);
        }
    }
    {AirportMtkSkywalkInterface s;auto j=request(0);j.keyCipher=1;j.keyLength=5;j.keyIndex=0;
     assert(s.beginPolicy(j));assert(!s.joinCompatActive && !s.hal.radio.ic_wcl_compat_phy);}
    // All valid configured PSK masks remain eligible, including 0x0a;
    // comparing ic_rsnakms with a single enum value would break this case.
    for(uint32_t auth:{uint32_t(APPLE80211_AUTHTYPE_WPA_PSK),uint32_t(APPLE80211_AUTHTYPE_WPA2_PSK),
                      uint32_t(APPLE80211_AUTHTYPE_SHA256_PSK),
                      uint32_t(APPLE80211_AUTHTYPE_WPA2_PSK|APPLE80211_AUTHTYPE_SHA256_PSK)}){
        for(unsigned level=1;level<=3;++level){
            AirportMtkSkywalkInterface s;s.joinHotspotLevel=uint8_t(level);
            s.hal.radio.ic_rsnakms=IEEE80211_AKM_PSK|IEEE80211_AKM_SHA256_PSK;
            assert(s.beginPolicy(request(auth)));compatible(s,level);
        }
    }
    // A transition request carrying a PMK chooses PSK, not SAE. A password
    // may enable SAE; its configured mask must still veto the fallback.
    const uint32_t transition=APPLE80211_AUTHTYPE_WPA3_SAE|APPLE80211_AUTHTYPE_WPA2_PSK;
    {AirportMtkSkywalkInterface s;assert(s.beginPolicy(request(transition)));compatible(s,3);}
    for(uint32_t auth:{transition,uint32_t(APPLE80211_AUTHTYPE_WPA3_SAE)}){
        AirportMtkSkywalkInterface s;auto &ic=s.hal.radio;
        ic.ic_rsnakms=IEEE80211_AKM_SAE|IEEE80211_AKM_PSK;
        const auto flags=ic.ic_flags|IEEE80211_F_DESBSSID,caps=ic.ic_caps;
        assert(s.beginPolicy(request(auth,true)));pristine(s,flags,caps);
    }
    {AirportMtkSkywalkInterface s;s.hal.radio.ic_caps&=~IEEE80211_C_MFP;
     assert(s.beginPolicy(request(transition,true)));compatible(s,3,false);}
    // Classify the next SAE join from restored capabilities, not the previous
    // PSK fallback's temporary C_MFP clear. The transition must select SAE too.
    for(uint32_t auth:{transition,uint32_t(APPLE80211_AUTHTYPE_WPA3_SAE)}){
        AirportMtkSkywalkInterface s;auto &ic=s.hal.radio;
        const auto flags=ic.ic_flags|IEEE80211_F_DESBSSID,caps=ic.ic_caps;
        assert(s.beginPolicy(request(APPLE80211_AUTHTYPE_WPA2_PSK)));compatible(s,3);
        ic.ic_rsnakms=IEEE80211_AKM_SAE;ic.ic_join_diag.Compat=0;
        assert(s.beginPolicy(request(auth,true)) && s.classifiedSae);
        pristine(s,flags,caps);
    }
    // A second request rejected as busy must not restore an in-flight join's
    // deliberately cleared capabilities or change its active fallback level.
    {AirportMtkSkywalkInterface s;
     assert(s.beginPolicy(request(APPLE80211_AUTHTYPE_WPA2_PSK)));compatible(s,3);
     s.joinPending=true;
     assert(!s.beginPolicy(request(APPLE80211_AUTHTYPE_WPA3_SAE,true)));compatible(s,3);}
    // Busy/deferred rejection must preserve the existing profile, timer and
    // stored request. Deferring an admitted replacement restores only on resume.
    for(bool deferred:{false,true}){
        AirportMtkSkywalkInterface s;auto &ic=s.hal.radio;
        assert(s.beginPolicy(request(APPLE80211_AUTHTYPE_WPA2_PSK)));compatible(s,3);
        s.joinPending=true;s.joinDeferred=deferred;ic.ic_wcl_mac_reconfig=true;
        s.deferredJoin=request(APPLE80211_AUTHTYPE_WPA2_PSK);
        s.timer.setTimeoutMS(30000);const auto armed=s.timer.armed;
        assert(!s.beginPolicy(request(transition,true)));compatible(s,3);
        assert(s.joinPending && s.joinDeferred==deferred && s.timer.armed==armed && !s.timer.canceled);
        assert(s.deferredJoin.upperAuth==APPLE80211_AUTHTYPE_WPA2_PSK && s.deferredJoin.keyLength==32);
    }
    {AirportMtkSkywalkInterface s;auto &ic=s.hal.radio;
     assert(s.beginPolicy(request(APPLE80211_AUTHTYPE_WPA2_PSK)));compatible(s,3);
     ic.ic_if.if_flags&=~IFF_RUNNING;
     assert(!s.beginPolicy(request(transition,true)));compatible(s,3);}
    {AirportMtkSkywalkInterface s;auto &ic=s.hal.radio;
     assert(s.beginPolicy(request(APPLE80211_AUTHTYPE_WPA2_PSK)));compatible(s,3);
     ic.ic_wcl_mac_reconfig=true;
     assert(s.beginPolicy(request(transition,true)) && s.classifiedSae);compatible(s,3);
     assert(s.joinDeferred && s.joinPending && s.timer.duration==30000);
     assert(s.deferredJoin.upperAuth==transition && s.deferredJoin.keyCipher==AirportWCL::CipherPassword);
     ic.ic_wcl_mac_reconfig=false;s.joinDeferred=s.joinPending=false;
     ic.ic_rsnakms=IEEE80211_AKM_SAE;ic.ic_join_diag.Compat=0;
     assert(s.beginPolicy(s.deferredJoin) && s.classifiedSae);
     assert(!s.joinCompatActive && (ic.ic_caps&IEEE80211_C_MFP));}
    // An originally disabled PMF capability is not reconstructed from a
    // profile that never cleared it, nor from inactive stale bookkeeping.
    {AirportMtkSkywalkInterface s;auto &ic=s.hal.radio;ic.ic_caps&=~IEEE80211_C_MFP;
     assert(s.beginPolicy(request(APPLE80211_AUTHTYPE_WPA2_PSK)));compatible(s,3,false);
     assert(!s.beginPolicy(request(APPLE80211_AUTHTYPE_WPA3_SAE,true)) && !s.classifiedSae);
     compatible(s,3,false);}
    {AirportMtkSkywalkInterface s;s.hal.radio.ic_caps&=~IEEE80211_C_MFP;s.joinCompatClearedMFP=true;
     assert(!s.beginPolicy(request(APPLE80211_AUTHTYPE_WPA3_SAE,true)) && !s.classifiedSae);}
    // A scan can be preempted by an admitted join, but never by a busy request.
    {AirportMtkSkywalkInterface s;s.scanPending=true;
     assert(s.beginPolicy(request(APPLE80211_AUTHTYPE_WPA2_PSK)) && s.scansAborted==1);}
    {AirportMtkSkywalkInterface s;s.scanPending=s.joinPending=true;
     assert(!s.beginPolicy(request(APPLE80211_AUTHTYPE_WPA2_PSK)) && !s.scansAborted && s.scanPending);}
    // The immediate newstate error abandons and restores the temporary profile.
    {AirportMtkSkywalkInterface s;auto &ic=s.hal.radio;
     const auto flags=ic.ic_flags|IEEE80211_F_DESBSSID,caps=ic.ic_caps;
     assert(s.beginPolicy(request(APPLE80211_AUTHTYPE_WPA2_PSK)));compatible(s,3);
     s.joinPending=ic.ic_wcl_join_requested=ic.ic_wcl_enterprise=true;
     memset(ic.ic_psk,0xa5,sizeof(ic.ic_psk));s.timer.setTimeoutMS(30000);s.failSCAN();
     pristine(s,flags,caps);assert(!s.joinPending && !ic.ic_wcl_join_requested && !ic.ic_wcl_enterprise);
     assert(s.timer.canceled==1 && s.pmksaCleared==1 && ic.saeForgotten);
     for(auto byte:ic.ic_psk)assert(!byte);}
    // Same SSID learning remains effective even with a globally administered
    // BSSID; different length/content, a wildcard or disabled knobs do not.
    for(unsigned level=1;level<=3;++level){
        AirportMtkSkywalkInterface s;s.joinHotspotLevel=0;s.joinCompatSsidLength=4;
        memcpy(s.joinCompatSsid,"test",4);s.joinCompatLevel=uint8_t(level);
        auto j=request(APPLE80211_AUTHTYPE_WPA2_PSK);j.bssid[0]=0;
        assert(s.beginPolicy(j));compatible(s,level);
        s.restoreJoinCompat();s.hal.radio.ic_join_diag.Compat=0;j.ssid[0]='n';
        assert(s.beginPolicy(j));assert(!s.joinCompatActive);
        j.ssid[0]='t';j.ssidLength=3;
        assert(s.beginPolicy(j));assert(!s.joinCompatActive);
    }
    {AirportMtkSkywalkInterface s;auto j=request(APPLE80211_AUTHTYPE_WPA2_PSK);
     memset(j.bssid,0,6);assert(s.beginPolicy(j));assert(!s.joinCompatActive);}
    {AirportMtkSkywalkInterface s;s.joinCompatEnabled=false;
     assert(s.beginPolicy(request(APPLE80211_AUTHTYPE_WPA2_PSK)));assert(!s.joinCompatActive);}
    {AirportMtkSkywalkInterface s;s.joinCompatSsidLength=4;memcpy(s.joinCompatSsid,"test",4);
     s.joinCompatLevel=3;s.joinHotspotLevel=1;
     assert(s.beginPolicy(request(APPLE80211_AUTHTYPE_WPA2_PSK)));compatible(s,3);}
    // Admitted next joins restore the prior profile, including a disabled
    // fallback knob. Originally disabled capabilities must never be enabled.
    for(unsigned bits=0;bits<4;++bits){
        for(bool disable:{false,true}){
            AirportMtkSkywalkInterface s;auto &ic=s.hal.radio;
            if(!(bits&1))ic.ic_flags&=~IEEE80211_F_HEON;
            if(!(bits&2))ic.ic_caps&=~IEEE80211_C_MFP;
            const auto flags=ic.ic_flags|IEEE80211_F_DESBSSID,caps=ic.ic_caps;
            assert(s.beginPolicy(request(APPLE80211_AUTHTYPE_WPA2_PSK)));
            assert(s.joinCompatActive);s.joinCompatEnabled=!disable;
            ic.ic_rsnakms=IEEE80211_AKM_8021X;ic.ic_join_diag.Compat=0;
            assert(s.beginPolicy(request(APPLE80211_AUTHTYPE_WPA2)));pristine(s,flags,caps);
            assert(ic.ic_join_diag.Compat==0);
        }
    }
    puts("PASS: production PSK-only compatibility, Enterprise/Open/SAE preservation and restoration");
}
void testWake(){
    for(bool pciPme:{false,true}){
        AirportMtkWlan d;d.magicPacketSupported=pciPme;
        UInt32 mask=0xffffffff;
        assert(d.getPacketFilters(gIOEthernetWakeOnLANFilterGroup,&mask)==kIOReturnSuccess);
        assert(mask==0 && !(mask&kIOEthernetWakeOnMagicPacket) && d.baseCalls==1);
        assert(d.setWakeOnMagicPacket(true)==kIOReturnUnsupported && !d.magicPacketEnabled);
        assert(d.setWakeOnMagicPacket(false)==kIOReturnSuccess && !d.magicPacketEnabled);
        d.magicPacketEnabled=true;
        assert(d.setWakeOnMagicPacket(false)==kIOReturnSuccess && !d.magicPacketEnabled);
        const auto calls=d.baseCalls;
        assert(d.getPacketFilters(gIONetworkFilterGroup,&mask)==kIOReturnSuccess);
        assert(mask==(kIOPacketFilterMulticast|kIOPacketFilterPromiscuous) && d.baseCalls==calls);
        assert(d.getPacketFilters(&otherGroup,&mask)==kIOReturnSuccess && mask==0);
        assert(d.baseCalls==calls+1);
    }
    puts("PASS: wake capability is not inferred from PCI PME; unsupported enable and idempotent disable");
}
int main(int argc,char **argv){
    assert(argc==2);
    if(!strcmp(argv[1],"compat")||!strcmp(argv[1],"all"))testCompat();
    if(!strcmp(argv[1],"wake")||!strcmp(argv[1],"all"))testWake();
}
'''
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--case', choices=['all', 'compat', 'wake'], default='all')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='mtk-wifi-policy-') as tmp:
    work = Path(tmp)
    (work / 'test.cpp').write_text(code)
    subprocess.run(['clang++', '-std=c++14', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined',
                    '-I', str(vendor / 'AirportMtkWlan/WCL'),
                    '-I', str(root / 'kext/Wifi'), '-I', str(root / 'kext/include'),
                    str(work / 'test.cpp'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test'), args.case], check=True)
