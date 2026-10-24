#!/usr/bin/env python3
"""Check advertised scan support and the WCL directed-scan request contract."""
from pathlib import Path
import subprocess
import shutil
import tempfile

root = Path(__file__).resolve().parents[1]
vendor = root / 'kext/Wifi/vendor'
source = (vendor / 'AirportMtkWlan/AirportMtkWlan.cpp').read_text()
start = source.index('IOReturn AirportMtkWlan::\ngetCARD_CAPABILITIES(')
capabilities = source[start:source.index('\n}\n', start) + 3]
header = (vendor / 'include/Airport/apple80211_var.h').read_text()
start = header.index('enum apple80211_card_capability\n')
enum = header[start:header.index('\n};', start) + 3]
hal = (vendor / 'SimpleMtkWlan/hal_mwx/MtkMwx.cpp').read_text()
start = hal.index('\t/* MtkWireless: WCL lists carry Instant Hotspot')
firmware_targets = hal[start:hal.index('\n\tfor (nchan', start)]
code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <initializer_list>
#include "WCL.hpp"
#define MTK_NATIVE_AWDL 1
#define APPLE80211_VERSION 1
#define IEEE80211_C_WEP 1
#define IEEE80211_C_RSN 2
#define IEEE80211_C_SHSLOT 4
#define IEEE80211_C_SHPREAMBLE 8
#define IEEE80211_C_HE 16
#define IEEE80211_C_MFP 32
using IOReturn = int;
constexpr int kIOReturnSuccess = 0;
struct OSObject {};
struct apple80211_capability_data {uint32_t version;uint8_t capabilities[21];};
struct Radio {uint32_t ic_caps=0; bool band5=false, band6=false;
    bool is5GBandSupport(){return band5;}
    bool is6GBandSupport(){return band6;}};
struct Hal {Radio radio;Radio*get80211Controller(){return &radio;}
    Radio*getDriverInfo(){return &radio;}};
struct AirportMtkAwdlInterface {static bool nativeDataPathAvailable(){return true;}};
struct AirportMtkWlan {Hal hal;Hal*fHalService=&hal;bool awdlExperimental=false;
    IOReturn getCARD_CAPABILITIES(OSObject*,apple80211_capability_data*);};
''' + enum + capabilities + r'''
#define MIN(a,b) ((a)<(b)?(a):(b))
#define htole32(x) (x)
#define IEEE80211_NWID_LEN 32
struct ScanIc {bool ic_wcl_scan_requested=true,ic_wcl_scan_active=true;
    unsigned ic_wcl_scan_ssid_length=0;uint8_t ic_wcl_scan_ssid[32]{};
    mtk_wlan_scan_ssids ic_wcl_scan_targets{};};
struct FwSsid {uint32_t ssid_len=0;uint8_t ssid[32]{};};
struct FwScan {FwSsid ssids[4],ext_ssids[6];unsigned scan_type=0,probe_req_num=0,
    ssid_type=0,ssid_type_ext=0,ssids_num=0,ext_ssids_num=0;};
void encodeTargets(ScanIc*ic,FwScan*req){unsigned i,n_ssids=0;
''' + firmware_targets + r'''
}

int main(){
    AirportMtkWlan driver;
    // Apple80211Scan tests CARD_CAPABILITIES byte 9 bit 1 to encode
    // SCAN_SSID_LIST / SCAN_BSSID_LIST in the modern WCL SCAN_REQ.
    // Instant Hotspot supplies a list even when it contains only one SSID.
    for(unsigned flags=0;flags<128;++flags){
        driver.hal.radio.ic_caps=flags&63;
        driver.hal.radio.band5=flags&1;
        driver.hal.radio.band6=flags&64;
        driver.awdlExperimental=flags&2;
        apple80211_capability_data caps;
        memset(&caps,0xaa,sizeof(caps));
        assert(driver.getCARD_CAPABILITIES(nullptr,&caps)==0);
        assert(caps.version==1);
        assert(caps.capabilities[9]==0x02); // modern SSID/BSSID lists
        bool mfp=flags&IEEE80211_C_MFP;
        // WME, SGI20/40; no legacy IOC 86. PMF: _getMFPCaps tests 0x11.
        assert(caps.capabilities[2]==(0x0e|(mfp?0x01:0)));
        // scan-done notifications; SAE (Apple80211IsAuthSelAllowed bit 3)
        assert(caps.capabilities[5]==(0x40|(mfp?0x08:0)));
        assert(bool(caps.capabilities[3]&0x20)==driver.hal.radio.band5);
        assert(bool(caps.capabilities[4]&1)==driver.awdlExperimental);
        assert(bool(caps.capabilities[17]&0x80)==bool(flags&IEEE80211_C_HE));
        // WCLDeviceConfiguration::is6GSupported: capability byte 0xf, bit 6.
        assert(bool(caps.capabilities[11]&0x40)==driver.hal.radio.band6);
    }
    // The per-SSID WCL request keeps the hotspot's target and channel.
    // Offsets match apple80211ScanRequest consumed by the production decoder.
    uint8_t wire[0x64]{};
    auto put=[&](unsigned at,uint32_t n){memcpy(wire+at,&n,4);};
    put(0,123);put(0x1c,4);memcpy(wire+0x20,"Dino",4);put(0x40,1);
    put(0x54,1);put(0x58,1);put(0x5c,149);put(0x60,0x10);
    AirportWCL::ScanRequest request;
    assert(AirportWCL::decodeScan(wire,sizeof(wire),request));
    assert(request.identifier==123 && request.type==1 && request.ssidLength==4);
    assert(memcmp(request.ssid,"Dino",4)==0 && request.channelCount==1);
    assert(request.channels[149/8]&(1U<<(149%8)));
    // 6 GHz entries (APPLE80211_C_FLAG_6GHZ) land above the 2.4/5 GHz bits.
    put(0x5c,37);put(0x60,0x2000);
    assert(AirportWCL::decodeScan(wire,sizeof(wire),request));
    assert(!(request.channels[37/8]&(1U<<(37%8))));
    assert(request.channels[(256+37)/8]&(1U<<((256+37)%8)));
    assert(AirportWCL::scanChannelSelected(request.channels,true,256+37));
    assert(!AirportWCL::scanChannelSelected(request.channels,true,37));
    put(0x5c,237);assert(AirportWCL::decodeScan(wire,sizeof(wire),request));
    for(uint8_t b : request.channels) assert(!b);
    // AppleChannelSpec_t as IO80211Family decodes it.
    assert(AirportWCL::chanspec20(6,AirportWCL::Band::TwoGHz)==0x1006);
    assert(AirportWCL::chanspec20(149,AirportWCL::Band::FiveGHz)==0xd095);
    assert(AirportWCL::chanspec20(37,AirportWCL::Band::SixGHz)==0x5025);
    assert(AirportWCL::chanspecIndex(0x5025)==256+37 && AirportWCL::chanspecIndex(0xd095)==149);
    AirportWCL::NativeChannelInfo channelsInfo{};
    assert(AirportWCL::appendChannel(channelsInfo,37,AirportWCL::Band::SixGHz,false,true,true,true));
    assert(channelsInfo.channelSpec[0]==0x5025 && channelsInfo.channel[0]==37);
    assert(channelsInfo.channelBitmap[0]==(3U|(1U<<9)|(1U<<12)|32U));
    put(0x5c,149);put(0x60,0x10);
    put(0x1c,33);assert(!AirportWCL::decodeScan(wire,sizeof(wire),request));
    put(0x1c,4);assert(!AirportWCL::decodeScan(wire,sizeof(wire)-1,request));
    // Decode the modern list emitted by IO80211ScanRequest (including its
    // trailing wildcard), then execute the actual HAL encoder on that result.
    uint8_t modern[0x1550]{};
    auto set32=[&](unsigned at,uint32_t n){memcpy(modern+at,&n,4);};
    set32(0x40,1);set32(0x1318,2);set32(0x131c,1);set32(0x1320,4);
    memcpy(modern+0x1324,"Dino",4);
    assert(AirportWCL::decodeScan(modern,sizeof(modern),request));
    assert(request.targets.count==1 && request.targets.wildcard);
    ScanIc ic;ic.ic_wcl_scan_targets=request.targets;FwScan firmware;
    encodeTargets(&ic,&firmware);
    assert(firmware.ssids_num==1 && firmware.ext_ssids_num==0);
    assert(firmware.ssids[0].ssid_len==4 && memcmp(firmware.ssids[0].ssid,"Dino",4)==0);
    assert(firmware.scan_type==1 && firmware.probe_req_num==2 && firmware.ssid_type_ext==0);
    for(unsigned count : {1u,4u,5u,10u}){
        ic.ic_wcl_scan_targets={};ic.ic_wcl_scan_targets.count=count;
        for(unsigned i=0;i<count;++i){ic.ic_wcl_scan_targets.lengths[i]=32;
            memset(ic.ic_wcl_scan_targets.ssids[i],i+1,32);}
        firmware={};encodeTargets(&ic,&firmware);
        assert(firmware.ssids_num==(count<4?count:4));
        assert(firmware.ext_ssids_num==(count>4?count-4:0));
        assert(firmware.ssid_type_ext==1);
        for(unsigned i=0;i<count;++i){auto&p=i<4?firmware.ssids[i]:firmware.ext_ssids[i-4];
            assert(p.ssid_len==32 && p.ssid[0]==i+1 && p.ssid[31]==i+1);}
    }
    ic.ic_wcl_scan_active=false;firmware={};encodeTargets(&ic,&firmware);
    assert(!firmware.ssids_num && !firmware.ext_ssids_num && !firmware.scan_type);
    mtk_wlan_scan_ssids invalid{};invalid.count=7;
    assert(mtk_wlan_scan_ssids_parse(nullptr,1,&invalid)==22 && invalid.count==0);
    assert(mtk_wlan_scan_ssids_parse(modern,sizeof(modern),nullptr)==22);
    // WPA3: WCL join with the SAE password (cipher 10), transition mode,
    // and the PMK-only and SAE-only cases.
    uint8_t join[0x2cc]{};
    auto j32=[&](unsigned at,uint32_t n){memcpy(join+at,&n,4);};
    j32(0x10,1);j32(0x1c,4);memcpy(join+0x20,"home",4);
    const char *pw="correct horse battery staple, 40+ chars long";
    size_t pwlen=strlen(pw);
    j32(0x44,uint32_t(pwlen));j32(0x48,10);memcpy(join+0x50,pw,pwlen);
    j32(0x214,1);join[0x21c]=2;join[0x221]=1;
    AirportWCL::JoinRequest jr;
    j32(0x14,AirportWCL::AuthSAE);
    assert(AirportWCL::decodeJoin(join,sizeof(join),jr));
    assert(jr.keyLength==pwlen && memcmp(jr.key,pw,pwlen)==0);
    assert(AirportWCL::classifyLegacyAuth(jr)==AirportWCL::LegacyAuth::Sae);
    j32(0x14,AirportWCL::AuthSAE|(1u<<3));   // transition: SAE + WPA2-PSK
    assert(AirportWCL::decodeJoin(join,sizeof(join),jr));
    assert(AirportWCL::classifyLegacyAuth(jr)==AirportWCL::LegacyAuth::Sae);
    assert(AirportWCL::passphraseKey(jr));
    j32(0x14,1u<<3);                          // WPA2-PSK given a password
    assert(AirportWCL::decodeJoin(join,sizeof(join),jr));
    assert(AirportWCL::classifyLegacyAuth(jr)==AirportWCL::LegacyAuth::Personal);
    j32(0x44,64);memset(join+0x50,'a',64);j32(0x14,AirportWCL::AuthSAE);
    assert(AirportWCL::decodeJoin(join,sizeof(join),jr) && jr.keyLength==64);
    assert(AirportWCL::classifyLegacyAuth(jr)==AirportWCL::LegacyAuth::Sae);
    assert(!AirportWCL::passphraseKey(jr));
    j32(0x44,65);assert(!AirportWCL::decodeJoin(join,sizeof(join),jr));
    j32(0x44,32);j32(0x48,6);                // a PMK cannot start SAE
    assert(AirportWCL::decodeJoin(join,sizeof(join),jr));
    assert(AirportWCL::classifyLegacyAuth(jr)==AirportWCL::LegacyAuth::Invalid);
    j32(0x14,AirportWCL::AuthSAE|(1u<<3));   // ...but joins the PSK side
    assert(AirportWCL::decodeJoin(join,sizeof(join),jr));
    assert(AirportWCL::classifyLegacyAuth(jr)==AirportWCL::LegacyAuth::Personal);
    j32(0x14,AirportWCL::AuthSAE|(1u<<2));   // SAE with 802.1X: refused
    assert(AirportWCL::decodeJoin(join,sizeof(join),jr));
    assert(AirportWCL::classifyLegacyAuth(jr)==AirportWCL::LegacyAuth::Invalid);
    // Without PMF the scan copy of a PSK/SAE transition RSN IE is still
    // narrowed to PSK; SAE-only stays as it is.
    uint8_t rsn[]={48,24,1,0, 0,0x0f,0xac,4, 1,0,0,0x0f,0xac,4, 2,0,0,0x0f,0xac,2,0,0x0f,0xac,8, 0x80,0};
    size_t rl=AirportWCL::selectWPA2TransitionMode(rsn,sizeof(rsn));
    assert(rl==sizeof(rsn)-4 && rsn[1]==20 && rsn[14]==1 && rsn[19]==2 && rsn[20]==0x80);
    uint8_t only[]={48,20,1,0, 0,0x0f,0xac,4, 1,0,0,0x0f,0xac,4, 1,0,0,0x0f,0xac,8, 0xc0,0};
    assert(AirportWCL::selectWPA2TransitionMode(only,sizeof(only))==sizeof(only));
    puts("PASS: Instant Hotspot capability, WCL SSID lists, firmware primary/extended targets, passive scans, 6 GHz chanspecs, FFI validation, PMF/SAE capability, WPA3 join classification");
}
'''
cargo = shutil.which('cargo') or str(Path.home() / '.cargo/bin/cargo')
host = root / 'build/rust-host'
subprocess.run([cargo, 'build', '-q', '-p', 'mtk-ffi', '--features', 'wifi',
                '--target-dir', str(host)], cwd=root / 'rust', check=True)
with tempfile.TemporaryDirectory(prefix='mtk-wifi-capabilities-') as directory:
    work = Path(directory)
    (work / 'test.cpp').write_text(code)
    subprocess.run(['clang++', '-std=c++14', '-fsanitize=address,undefined',
                    '-I', str(vendor / 'AirportMtkWlan/WCL'),
                    '-I', str(root / 'kext/Wifi'), '-I', str(root / 'kext/include'),
                    str(work / 'test.cpp'), str(host / 'debug/libmtk_ffi.a'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
