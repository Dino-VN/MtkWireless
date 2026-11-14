#!/usr/bin/env python3
"""Production legacy getter bounds and closed-interface regressions (ASan/UBSan).

Uses actual Apple ABI structs and net80211 rate tables. HAL/IOKit ownership,
channel conversion and SGI are host boundaries, not native-runtime verification.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
V = ROOT / 'kext/Wifi/vendor'
CPP = V / 'AirportMtkWlan/AirportMtkSkywalkInterface.cpp'
VAR = V / 'include/Airport/apple80211_var.h'
IOC = VAR.with_name('apple80211_ioctl.h')
NODE = V / 'mtk80211/openbsd/net80211/ieee80211_node.h'
IEEE = NODE.with_name('ieee80211.c')
WCL = CPP.parent / 'WCL/WCL.cpp'


def function(path, signature):
    text = path.read_text()
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 3]


def structure(path, name):
    text = path.read_text()
    start = re.search(r'(?m)^struct ' + name + r'\s*\n?\{', text).start()
    end = text.index('\n}', start)
    return text[start:text.index(';', end) + 1]


def macros(path, pattern):
    return '\n'.join(line for line in path.read_text().splitlines()
                     if re.match(r'^#define\s+' + pattern + r'\b', line)) + '\n'


code = r'''
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/types.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define min(a,b) MIN(a,b)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define IEEE80211_S_RUN 4
#define IEEE80211_ADDR_LEN APPLE80211_ADDR_LEN
#define IEEE80211_MODE_11N 7
#define IEEE80211_MODE_11AC 8
#define IEEE80211_MODE_11AX 9
#define IEEE80211_CHAN_WIDTH_20 0
#define IEEE80211_CHAN_WIDTH_40 1
#define IEEE80211_CHAN_WIDTH_80 2
#define IEEE80211_CHAN_WIDTH_80P80 3
#define IEEE80211_CHAN_WIDTH_160 4
#define IEEE80211_CHAN_ANYC reinterpret_cast<ieee80211_channel *>(-1)
#define APPLE80211_UNIT_DBM 1
#define MTK_MIN_DBM (-100)
using IOReturn=int;
constexpr IOReturn kIOReturnSuccess=0,kIOReturnBadArgument=10,kIOReturnNotReady=11,kIOReturnUnsupported=12;
struct ether_addr {uint8_t octet[6];};
'''
code += macros(VAR, r'APPLE80211_(?:ADDR_LEN|MAX_RATES|MAX_SSID_LEN|MAX_RADIO)')
code += macros(IOC, 'APPLE80211_VERSION')
code += macros(NODE, r'IEEE80211_(?:HT|VHT|HE)_\w+')
for path, names in [(VAR, ['apple80211_channel', 'apple80211_rate', 'apple80211_scan_result']),
                    (IOC, ['apple80211_ssid_data', 'apple80211_channel_data', 'apple80211_bssid_data',
                           'apple80211_rssi_data', 'apple80211_rate_data', 'apple80211_rate_set_data']),
                    (NODE, ['ieee80211_ht_rateset', 'ieee80211_vht_rateset', 'ieee80211_he_rateset'])]:
    code += '\n'.join(structure(path, name) for name in names) + '\n'
for name in ['ieee80211_std_ratesets_11n', 'ieee80211_std_ratesets_11ac', 'ieee80211_std_ratesets_11ax']:
    text = IEEE.read_text()
    at = text.index(name + '[')
    start = text.rfind('const struct', 0, at)
    code += text[start:text.index('\n};', at) + 3] + '\n'
code += r'''
struct ieee80211_channel {unsigned ic_flags=1,number=149;};
struct ieee80211_rateset {uint8_t rs_nrates=0,rs_rates[15]{};};
struct ieee80211_node {
    ieee80211_channel *ni_chan=nullptr; unsigned ni_chw=0;
    uint8_t ni_esslen=0,ni_essid[32]{},ni_bssid[6]{},ni_txmcs=0,ni_vht_ss=0,ni_txrate=0;
    ieee80211_rateset ni_rates; uint8_t *ni_rsnie_tlv=nullptr; unsigned ni_rsnie_tlv_len=0;
    unsigned ni_intval=100,ni_age_ts=0,ni_capinfo=0,ni_rssi=25; bool sgi=false;
};
struct ieee80211com {int ic_state=IEEE80211_S_RUN,ic_curmode=IEEE80211_MODE_11AC; ieee80211_node *ic_bss=nullptr;};
struct DriverInfo {unsigned nss=2; unsigned getTxNSS(){return nss;} int getBSSNoise(){return 85;}};
struct MtkHalService {
    unsigned refs=1,queries=0; ieee80211com ic; DriverInfo info;
    void retain(){++refs;} void release(){assert(refs);--refs;}
    ieee80211com *get80211Controller(){++queries;return &ic;}
    DriverInfo *getDriverInfo(){return &info;}
};
struct Timer {unsigned refs=1;void retain(){++refs;}void release(){assert(refs);--refs;}};
#define OSSafeReleaseNULL(p) do {if(p){(p)->release();(p)=nullptr;}} while(0)
struct IO80211InfraProtocol {unsigned baseFrees=0;virtual void free(){++baseFrees;}};
unsigned airport_up_time(){return 1000;}
unsigned ieee80211_chan2ieee(ieee80211com *,ieee80211_channel *c){return c->number;}
unsigned ieeeChanFlag2appleScanFlagVentura(unsigned flags){return flags;}
unsigned ieeeChanFlag2apple(unsigned flags,unsigned){return flags;}
int ieee80211_node_supports_sgi(ieee80211_node *n){return n->sgi;}
struct apple80211_assoc_data; struct apple80211_scan_data;
struct AirportMtkSkywalkInterface:IO80211InfraProtocol {
    MtkHalService *fHalService=nullptr; Timer *scanSource=nullptr; void *instance=nullptr;
    bool wclStopping=false; unsigned stops=0;
    void stopWCL(){wclStopping=true;++stops;}
    void free() override;
    ieee80211com *current80211() const;
    IOReturn getSSID(apple80211_ssid_data *);
    IOReturn getCHANNEL(apple80211_channel_data *);
    IOReturn getBSSID(apple80211_bssid_data *);
    IOReturn getRSSI(apple80211_rssi_data *);
    IOReturn getRATE(apple80211_rate_data *);
    IOReturn getRATE_SET(apple80211_rate_set_data *);
    IOReturn getCURRENT_NETWORK(apple80211_scan_result *);
    IOReturn getSCAN_RESULT(apple80211_scan_result *);
    IOReturn setSCAN_REQ(apple80211_scan_data *);
    IOReturn setASSOCIATE(apple80211_assoc_data *);
};
'''
code += function(CPP, 'ieee80211com *AirportMtkSkywalkInterface::current80211() const')
code += function(CPP, 'static int convertNodeToScanResult(')
for name, arg in [('getSSID', 'ssid_data'), ('getCHANNEL', 'channel_data'), ('getBSSID', 'bssid_data'),
                  ('getRSSI', 'rssi_data'), ('getRATE', 'rate_data'), ('getRATE_SET', 'rate_set_data'),
                  ('getSCAN_RESULT', 'scan_result'), ('setSCAN_REQ', 'scan_data'), ('setASSOCIATE', 'assoc_data')]:
    code += function(CPP, 'IOReturn AirportMtkSkywalkInterface::\n' + name + '(struct apple80211_' + arg)
code += function(CPP, 'IOReturn AirportMtkSkywalkInterface::\ngetCURRENT_NETWORK(')
code += function(WCL, 'void AirportMtkSkywalkInterface::free()')
code += r'''
int main(){
    AirportMtkSkywalkInterface iface; MtkHalService hal; Timer timer;
    iface.fHalService=&hal; iface.scanSource=&timer; hal.retain();timer.retain();
    assert(iface.setASSOCIATE(reinterpret_cast<apple80211_assoc_data *>(1))==kIOReturnUnsupported);
    assert(iface.setSCAN_REQ(reinterpret_cast<apple80211_scan_data *>(1))==kIOReturnUnsupported);
    assert(iface.getSCAN_RESULT(reinterpret_cast<apple80211_scan_result *>(1))==kIOReturnUnsupported);
    apple80211_rate_data rate{}; apple80211_rate_set_data rates{}; apple80211_ssid_data ssid{};
    apple80211_channel_data channel{}; apple80211_bssid_data bssid{}; apple80211_rssi_data rssi{}; apple80211_scan_result scan{};
    assert(iface.getRATE(nullptr)==kIOReturnBadArgument&&iface.getSSID(nullptr)==kIOReturnBadArgument);
    assert(iface.getRATE(&rate)==kIOReturnNotReady&&iface.getCHANNEL(&channel)==kIOReturnNotReady);
    ieee80211_node node; ieee80211_channel ch; node.ni_chan=&ch;hal.ic.ic_bss=&node;
    uint8_t ies[1500];memset(ies,0x5a,sizeof(ies)); node.ni_rsnie_tlv=ies;node.ni_rsnie_tlv_len=sizeof(ies);
    node.ni_esslen=255;memset(node.ni_essid,'x',32);node.ni_rates.rs_nrates=255;
    for(unsigned i=0;i<15;++i)node.ni_rates.rs_rates[i]=i+2;
    assert(iface.getCURRENT_NETWORK(&scan)==0&&scan.asr_ie_len==1024&&scan.asr_nrates==15&&scan.asr_ssid_len==32);
    assert(scan.asr_rates[0]==2&&scan.asr_rates[14]==16&&scan.asr_ie_data[1023]==0x5a);
    assert(iface.getSSID(&ssid)==0&&ssid.ssid_len==32&&iface.getRATE_SET(&rates)==0&&rates.num_rates==15);
    for(int mode:{IEEE80211_MODE_11N,IEEE80211_MODE_11AC,IEEE80211_MODE_11AX})
      for(unsigned width=0;width<5;++width)for(unsigned streams:{0,1,2,3,8,255})for(unsigned mcs:{0,7,9,11,15,31,63,255})for(bool sgi:{false,true}){
        hal.ic.ic_curmode=mode;node.ni_chw=width;node.ni_vht_ss=streams;node.ni_txmcs=mcs;node.sgi=sgi;
        // Some real VHT table cells are 0 for unsupported MCS/bandwidth pairs.
        assert(iface.getRATE(&rate)==0);
      }
    node.ni_chan=IEEE80211_CHAN_ANYC; assert(iface.getCHANNEL(&channel)==kIOReturnNotReady);
    node.ni_chan=&ch;hal.ic.ic_curmode=0;node.ni_txrate=255;assert(iface.getRATE(&rate)==kIOReturnNotReady);
    iface.wclStopping=true;auto queries=hal.queries;
    assert(iface.getRATE(&rate)==kIOReturnNotReady&&iface.getSSID(&ssid)==kIOReturnNotReady);
    assert(iface.getBSSID(&bssid)==kIOReturnNotReady&&iface.getRSSI(&rssi)==kIOReturnNotReady&&iface.getCURRENT_NETWORK(&scan)==kIOReturnNotReady);
    assert(hal.queries==queries);iface.free();assert(!iface.fHalService&&!iface.scanSource&&hal.refs==1&&timer.refs==1&&iface.baseFrees==1);
    std::puts("Legacy safety: unsupported old setters/iterator, real rate-table bounds, clamped IE/rates/SSID and retained HAL/closed getters passed");
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-legacy-safety-') as directory:
    cpp = Path(directory) / 'legacy.cpp'
    binary = cpp.with_suffix('')
    cpp.write_text(code)
    subprocess.run(['/usr/bin/clang++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
