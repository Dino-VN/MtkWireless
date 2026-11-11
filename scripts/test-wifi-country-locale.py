#!/usr/bin/env python3
"""Offline country/locale reporting regression; never contacts or reconfigures Wi-Fi.

Compile the production getters/setter and both WCL channel writers with ASan/UBSan.
The HAL/IOKit boundaries are stubs, not a regulatory implementation or a test of
Tahoe's native locale dispatcher. The unchanged notification route has source
checks for its deferred, command-gated, off-workloop contract.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
vendor = root / 'kext/Wifi/vendor'
controller = (vendor / 'AirportMtkWlan/AirportMtkWlan.cpp').read_text()
interface = (vendor / 'AirportMtkWlan/AirportMtkSkywalkInterface.cpp').read_text()
wcl = (vendor / 'AirportMtkWlan/WCL/WCL.cpp').read_text()
apple = (vendor / 'include/Airport/apple80211_var.h').read_text()
ioctl = (vendor / 'include/Airport/apple80211_ioctl.h').read_text()
net = (vendor / 'mtk80211/openbsd/net80211/ieee80211_var.h').read_text()


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


def declaration(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n};', start) + 3]


def define(source, name):
    return re.search(r'^#define\s+' + name + r'\s+[^\n]+', source, re.M).group() + '\n'


country_copy = function(controller, 'static bool mtkCopyCountryCode(')
country_get = function(controller, 'IOReturn AirportMtkWlan::\ngetCOUNTRY_CODE(')
country_set = function(controller, 'IOReturn AirportMtkWlan::\nsetCOUNTRY_CODE(')
locale_get = function(interface, 'IOReturn AirportMtkSkywalkInterface::\ngetLOCALE(')
# A request cannot announce a policy change that this HAL cannot apply. Do not
# replace the existing real-change notification path with a synchronous post.
assert 'handleWCLEvent(' not in country_set and 'postMessage(' not in country_set
assert 'PE_parse_boot_argn(' not in country_get
assert 'geo_location_cc' not in country_get
schedule = function(wcl, 'void AirportMtkSkywalkInterface::deferWCLCountryUpdate(')
worker = function(wcl, 'void AirportMtkSkywalkInterface::wclCountryUpdateThread(')
gated = function(wcl, 'IOReturn AirportMtkSkywalkInterface::wclCountryUpdateGated(')
assert 'thread_call_allocate(wclCountryUpdateThread, update)' in schedule
assert 'thread_call_enter(update->call)' in schedule
assert 'update->gate->runAction(wclCountryUpdateGated, update)' in worker
assert 'update->interface->wclStopping' in gated
assert '!update->queue->inGate() || update->queue->onThread()' in gated
assert gated.index('return kIOReturnNotPermitted;') < gated.index('postMessage(')
assert 'postMessage(APPLE80211_M_COUNTRY_CODE_CHANGED, nullptr, 0, false)' in gated
assert re.search(r'case IEEE80211_EVT_COUNTRY_CODE_UPDATE:\s*deferWCLCountryUpdate\(\);\s*break;', wcl)

constants = define(apple, 'APPLE80211_MAX_CC_LEN') + define(ioctl, 'APPLE80211_VERSION')
for name in ('MAX', '6GHZ_BASE', '2GHZ', '5GHZ', '6GHZ', 'DFS', 'PASSIVE',
             'HT40U', 'HT40D', 'HT40', 'VHT40U', 'VHT40D', 'VHT40', 'VHT80'):
    constants += define(net, 'IEEE80211_CHAN_' + name)

code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <sys/types.h>
#include "WCL.hpp"
using IOReturn = int;
constexpr IOReturn kIOReturnSuccess=0, kIOReturnBadArgument=1,
    kIOReturnNotReady=2, kIOReturnUnsupported=3, kIOReturnError=4;
''' + constants + declaration(ioctl, 'struct apple80211_country_code_data\n') + '\n' + \
    declaration(ioctl, 'struct apple80211_locale_data\n') + '\n' + \
    declaration(apple, 'enum apple80211_locale\n') + r'''
static_assert(sizeof(apple80211_country_code_data)==8, "country wire size");
static_assert(offsetof(apple80211_country_code_data,cc)==4, "country wire offset");
static_assert(sizeof(apple80211_locale_data)==8, "locale wire size");
struct OSObject {};
struct ieee80211_channel { uint16_t ic_freq=0; uint32_t ic_flags=0; };
struct ieee80211com {
    ieee80211_channel ic_channels[IEEE80211_CHAN_MAX+1]{};
    uint8_t ic_chan_active[(IEEE80211_CHAN_MAX+1)/8]{};
};
#define isset(bits,n) ((bits)[(n)/8] & (1U<<((n)%8)))
#define setbit(bits,n) ((bits)[(n)/8] |= (1U<<((n)%8)))
#define IEEE80211_IS_CHAN_6GHZ(c) ((c)->ic_flags & IEEE80211_CHAN_6GHZ)
#define IEEE80211_IS_CHAN_5GHZ(c) ((c)->ic_flags & IEEE80211_CHAN_5GHZ)
#define IEEE80211_IS_CHAN_HT40(c) ((c)->ic_flags & IEEE80211_CHAN_HT40)
#define IEEE80211_IS_CHAN_VHT40(c) ((c)->ic_flags & IEEE80211_CHAN_VHT40)
#define IEEE80211_IS_CHAN_VHT80(c) ((c)->ic_flags & IEEE80211_CHAN_VHT80)
unsigned ieee80211_chan2ieee(ieee80211com *ic, const ieee80211_channel *c) {
    unsigned index=unsigned(c-ic->ic_channels);
    return IEEE80211_IS_CHAN_6GHZ(c) ? index-IEEE80211_CHAN_6GHZ_BASE : index;
}
struct DriverInfo {
    const char *country="00";
    unsigned queries=0;
    const char *getFirmwareCountryCode() { ++queries; return country; }
};
struct Hal {
    DriverInfo info;
    DriverInfo *availableInfo=&info;
    ieee80211com radio;
    DriverInfo *getDriverInfo() { return availableInfo; }
    ieee80211com *get80211Controller() { return &radio; }
};
struct AirportMtkWlan {
    Hal hal;
    Hal *fHalService=&hal;
    IOReturn getCOUNTRY_CODE(OSObject *, apple80211_country_code_data *);
    IOReturn setCOUNTRY_CODE(OSObject *, apple80211_country_code_data *);
};
struct apple80211ChannelInfo;
struct AirportMtkSkywalkInterface : OSObject {
    AirportMtkWlan *instance;
    Hal *fHalService;
    IOReturn getLOCALE(apple80211_locale_data *);
    IOReturn getWCL_CHANNELS_INFO(apple80211ChannelInfo *);
    IOReturn copyWCLChannels(void *);
};
''' + country_copy + country_get + country_set + locale_get + \
    function(wcl, 'static AirportWCL::Band wclBand(') + \
    function(wcl, 'IOReturn AirportMtkSkywalkInterface::getWCL_CHANNELS_INFO(') + \
    function(wcl, 'IOReturn AirportMtkSkywalkInterface::copyWCLChannels(') + r'''
apple80211_country_code_data request(const char *cc) {
    apple80211_country_code_data data{};
    data.version=APPLE80211_VERSION;
    assert(strlen(cc)==2);
    memcpy(data.cc,cc,3);
    return data;
}
void expectCountry(AirportMtkWlan &driver, const char *expected) {
    struct Guard {
        uint64_t before;
        apple80211_country_code_data data;
        uint64_t after;
    } out;
    memset(&out,0xa5,sizeof(out));
    assert(driver.getCOUNTRY_CODE(nullptr,&out.data)==kIOReturnSuccess);
    assert(out.before==UINT64_C(0xa5a5a5a5a5a5a5a5));
    assert(out.after==UINT64_C(0xa5a5a5a5a5a5a5a5));
    assert(out.data.version==APPLE80211_VERSION && !memcmp(out.data.cc,expected,3));
    // The structure's tail padding must not retain caller data either.
    assert(reinterpret_cast<const uint8_t *>(&out.data)[7]==0);
}
void channelReports(AirportMtkSkywalkInterface &iface, AirportWCL::ChannelInfo &wcl,
                    AirportWCL::NativeChannelInfo &native) {
    memset(&wcl,0xa5,sizeof(wcl)); memset(&native,0xa5,sizeof(native));
    assert(iface.getWCL_CHANNELS_INFO(reinterpret_cast<apple80211ChannelInfo *>(&wcl))==kIOReturnSuccess);
    assert(iface.copyWCLChannels(&native)==kIOReturnSuccess);
    assert(wcl.count==3 && native.count==3 && !wcl.supports6GHz);
    assert(native.country[3]==0);
    assert(wcl.channels[1].channel==52 && (wcl.channels[1].flags&6)==6);
    assert(native.channel[1]==52 && native.dfs[1] && native.passive[1]);
}
int main() {
    AirportMtkWlan driver;
    AirportMtkSkywalkInterface iface;
    iface.instance=&driver; iface.fHalService=&driver.hal;
    assert(driver.getCOUNTRY_CODE(nullptr,nullptr)==kIOReturnBadArgument);
    assert(driver.setCOUNTRY_CODE(nullptr,nullptr)==kIOReturnBadArgument);
    assert(driver.hal.info.queries==0);
    expectCountry(driver,"00");

    // Full two-letter syntax space (both cases) and Apple/world pseudo-codes.
    // These are test inputs, not evidence of the device's geographic country.
    for (char a='A'; a<='Z'; ++a) for (char b='A'; b<='Z'; ++b) {
        for (unsigned lowercase=0; lowercase<4; ++lowercase) {
            char input[]={char(a+((lowercase&1)?32:0)),char(b+((lowercase&2)?32:0)),0};
            uint8_t out[3]={0xa5,0xa5,0xa5};
            assert(mtkCopyCountryCode(input,out));
            assert(out[0]==a && out[1]==b && out[2]==0);
        }
    }
    for (const char *cc : {"00","XX","xx","ZZ","zz","X0","X1","X2","X3","XA","XB"}) {
        auto data=request(cc), saved=data;
        assert(driver.setCOUNTRY_CODE(nullptr,&data)==
               (!strcmp(cc,"00") ? kIOReturnSuccess : kIOReturnUnsupported));
        assert(!memcmp(&data,&saved,sizeof(data)));
        expectCountry(driver,"00"); // Reset/locale markers never select a country.
    }
    const uint8_t malformed[][3]={
        {0,0,0}, {'U',0,0}, {0,'S',0}, {'U','S','A'}, {'U','S',' '},
        {'U','S',0xff}, {'1','2',0}, {'0','U',0}, {'U','0',0}, {'X','4',0},
        {' ','S',0}, {'!','!',0}, {0x80,'S',0}, {'U',0xff,0}, {'x','!',0}
    };
    for (const auto &bytes : malformed) {
        auto data=request("US"); memcpy(data.cc,bytes,3); auto saved=data;
        uint8_t out[3]={0xa5,0xa5,0xa5};
        assert(!mtkCopyCountryCode(reinterpret_cast<const char *>(bytes),out));
        assert(out[0]==0xa5 && out[1]==0xa5 && out[2]==0xa5);
        assert(driver.setCOUNTRY_CODE(nullptr,&data)==kIOReturnBadArgument);
        assert(!memcmp(&data,&saved,sizeof(data)));
        expectCountry(driver,"00");
    }
    // C-string HAL inputs can also be null, empty or one byte. ASan checks that
    // rejection does not read past these short strings or the fixed cc[3] field.
    const char empty[]={0}, shortCode[]={'U',0};
    for (const char *cc : {static_cast<const char *>(nullptr),empty,shortCode,"U1","USX"}) {
        driver.hal.info.country=cc;
        apple80211_country_code_data out;
        memset(&out,0xa5,sizeof(out));
        assert(driver.getCOUNTRY_CODE(nullptr,&out)==kIOReturnNotReady);
        assert(out.version==APPLE80211_VERSION && !out.cc[0] && !out.cc[1] && !out.cc[2]);
        auto data=request("US");
        assert(driver.setCOUNTRY_CODE(nullptr,&data)==kIOReturnNotReady);
    }
    driver.hal.info.country="00";
    driver.hal.availableInfo=nullptr;
    apple80211_country_code_data out{};
    assert(driver.getCOUNTRY_CODE(nullptr,&out)==kIOReturnNotReady);
    driver.hal.availableInfo=&driver.hal.info;
    driver.fHalService=nullptr;
    assert(driver.getCOUNTRY_CODE(nullptr,&out)==kIOReturnNotReady);
    assert(driver.getCOUNTRY_CODE(nullptr,nullptr)==kIOReturnBadArgument);
    driver.fHalService=&driver.hal;

    auto &radio=driver.hal.radio;
    radio.ic_channels[1]={2412,IEEE80211_CHAN_2GHZ};
    radio.ic_channels[52]={5260,IEEE80211_CHAN_5GHZ|IEEE80211_CHAN_DFS|IEEE80211_CHAN_PASSIVE};
    radio.ic_channels[149]={5745,IEEE80211_CHAN_5GHZ|IEEE80211_CHAN_HT40U|IEEE80211_CHAN_VHT80};
    // Inactive channels, including 6 GHz, must stay inactive across every hint.
    radio.ic_channels[256+37]={6135,IEEE80211_CHAN_6GHZ|IEEE80211_CHAN_PASSIVE};
    for (unsigned n : {1u,52u,149u}) setbit(radio.ic_chan_active,n);
    uint8_t savedRadio[sizeof(radio)]; memcpy(savedRadio,&radio,sizeof(radio));
    for (const char *effective : {"00","US","DE","JP","ZZ","XX","X0","X2","XA"}) {
        driver.hal.info.country=effective;
        expectCountry(driver,effective);
        AirportWCL::ChannelInfo beforeWcl{},afterWcl{};
        AirportWCL::NativeChannelInfo beforeNative{},afterNative{};
        channelReports(iface,beforeWcl,beforeNative);
        assert(!memcmp(beforeWcl.country,effective,3));
        assert(!memcmp(beforeNative.country,effective,3));
        for (const char *hint : {"US","DE","00","XX","ZZ","X2"}) {
            auto data=request(hint);
            assert(driver.setCOUNTRY_CODE(nullptr,&data)==
                   (!strcmp(hint,effective) ? kIOReturnSuccess : kIOReturnUnsupported));
            expectCountry(driver,effective); // In particular, ZZ is not a hint fallback.
            channelReports(iface,afterWcl,afterNative);
            assert(!memcmp(&beforeWcl,&afterWcl,sizeof(beforeWcl)));
            assert(!memcmp(&beforeNative,&afterNative,sizeof(beforeNative)));
            assert(!memcmp(savedRadio,&radio,sizeof(radio)));
        }
        apple80211_locale_data locale{0xa5a5a5a5,0xa5a5a5a5};
        assert(iface.getLOCALE(&locale)==kIOReturnSuccess);
        assert(locale.version==APPLE80211_VERSION && locale.locale==APPLE80211_LOCALE_UNKNOWN);
    }
    driver.hal.info.country="uS"; expectCountry(driver,"US");
    auto same=request("us");
    assert(driver.setCOUNTRY_CODE(nullptr,&same)==kIOReturnSuccess);
    assert(!memcmp(same.cc,"us",3)); // Canonicalization does not mutate the request.
    assert(iface.getLOCALE(nullptr)==kIOReturnError);
    // Both channel writers zero their country on a failed HAL query, not a
    // fabricated country from a previous successful report.
    driver.hal.info.country=nullptr;
    AirportWCL::ChannelInfo wcl{}; AirportWCL::NativeChannelInfo native{};
    channelReports(iface,wcl,native);
    assert(!memcmp(wcl.country,"\0\0\0",3) && !memcmp(native.country,"\0\0\0\0",4));
    puts("PASS: country syntax, malformed/unknown/valid inputs, HAL-only reporting, no-op/refused requests");
    puts("PASS: both WCL country layouts; rejected hints preserve channel permissions and flags");
    puts("PASS: legacy locale UNKNOWN; null/unavailable HAL and output initialization");
}
'''

with tempfile.TemporaryDirectory(prefix='mtk-wifi-country-locale-') as directory:
    work = Path(directory)
    source = work / 'test.cpp'
    source.write_text(code)
    subprocess.run(['clang++', '-std=c++14', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-g',
                    '-I', str(vendor / 'AirportMtkWlan/WCL'),
                    '-I', str(root / 'kext/Wifi'), '-I', str(root / 'kext/include'),
                    str(source), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
print('PASS: no reporting-only boot override or hint fallback; deferred/gated notification source guards')
