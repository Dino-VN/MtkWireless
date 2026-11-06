#!/usr/bin/env python3
"""Exercise production cache transactions and native event forwarding with ASan/UBSan."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
base = root / 'kext/Wifi/vendor/AirportMtkWlan/AirportMtkAwdlInterface'
source = base.with_suffix('.cpp').read_text()
header = base.with_suffix('.hpp').read_text()
start = header.index('    struct CacheDiagnostic {')
fields = header[start:header.index('    CacheDiagnostic cacheDiagnostic;', start)
                + len('    CacheDiagnostic cacheDiagnostic;')]

def method(name):
    start = source.index('IOReturn AirportMtkAwdlInterface::' + name + '(')
    return source[start:source.index('\n}\n', start)+3]

code = r'''
#include <AWDL/AwdlProtocol.hpp>
#include <cassert>
#include <cstring>
#include <functional>
using IOReturn = unsigned;
constexpr unsigned APPLE80211_VERSION=1, kIOReturnSuccess=0,
    kIOReturnNotReady=0xe00002d8, kIOReturnBadArgument=0xe00002c2,
    kIOReturnUnsupported=0xe00002c7, kIOReturnNoResources=0xe00002be,
    kIOReturnIOError=0xe00002ca;
static uint64_t now=1000000;
uint64_t awdlNow() { return now; }
struct apple80211_peer_cache_control { uint8_t bytes[0x1d0]{}; };
struct NativeInterface {
    unsigned calls=0, message=0; void *data=nullptr; unsigned long length=0; bool sync=false;
    IOReturn result=kIOReturnSuccess;
    IOReturn reportDataPathEvents(unsigned m,void *p,unsigned long n,bool s) {
        ++calls; message=m; data=p; length=n; sync=s; return result;
    }
};
#define super NativeInterface
struct AirportMtkAwdlInterface : NativeInterface {
''' + fields + r'''
    mtkawdl::DataPeers dataPeers;
    uint8_t discoveryTxPeers[3][6]{};
    bool stopping=false, enabled=true; int retains=0;
    void *owner=this;
    void retain() { ++retains; }
    void release() { --retains; }
    struct Hal {
        unsigned calls=0; bool lastAdd=false; IOReturn result=0;
        std::function<void()> duringWait;
        IOReturn awdlPeer(const uint8_t*,bool add) {
            ++calls; lastAdd=add; now+=20000;
            if(duringWait) duringWait();
            return result;
        }
    } radio;
    Hal *hal=&radio;
    IOReturn reportDataPathEvents(unsigned,void*,unsigned long,bool);
    IOReturn recordCacheControlResult(IOReturn,uint64_t);
    IOReturn setPEER_CACHE_CONTROL(apple80211_peer_cache_control*);
};
static unsigned posts=0;
static IOReturn postResult=0;
static uint8_t savedMessage[0x1d0];
IOReturn awdlControllerMessage(void*,AirportMtkAwdlInterface*,unsigned message,void *data,unsigned long n,bool sync) {
    assert(message==0x33 && n==sizeof(savedMessage) && sync);
    ++posts; memcpy(savedMessage,data,n); return postResult;
}
''' + '\n'.join(method(name) for name in ('reportDataPathEvents', 'recordCacheControlResult',
                                         'setPEER_CACHE_CONTROL')) + r'''
int main() {
    AirportMtkAwdlInterface d;
    apple80211_peer_cache_control request;
    uint8_t peer[6]={2,1,2,3,4,5};
    memcpy(request.bytes+8,peer,6); request.bytes[14]=0x34; request.bytes[15]=0x12;
    assert(d.setPEER_CACHE_CONTROL(nullptr)==kIOReturnNotReady);
    d.radio.result=kIOReturnIOError;
    assert(d.setPEER_CACHE_CONTROL(&request)==kIOReturnIOError);
    assert(!posts && d.dataPeers.find(peer,now)<0 && !d.retains);
    assert(d.cacheDiagnostic.firmwareFailures==1 && d.cacheDiagnostic.failures==2);
    assert(d.cacheDiagnostic.durationUS==20000 && d.cacheDiagnostic.firmwareDurationUS==20000);
    d.radio.result=0;
    assert(!d.setPEER_CACHE_CONTROL(&request) && posts==1 && !d.retains);
    assert(mtkawdl::le32(savedMessage+4)==1 && !memcmp(savedMessage+8,peer,6));
    // A copied message is delivered later, after stack storage has expired.
    // Native dispatch's arguments and non-success result must be preserved.
    d.result=kIOReturnUnsupported;
    assert(d.reportDataPathEvents(0x33,savedMessage,sizeof(savedMessage),false)==d.result);
    assert(d.data==savedMessage && d.length==sizeof(savedMessage) && !d.sync);
    assert(d.cacheDiagnostic.events==1 && !d.cacheDiagnostic.invalidEvents);
    assert(d.cacheDiagnostic.eventStatus==d.result);
    auto count=d.cacheDiagnostic.events;
    assert(d.reportDataPathEvents(0x35,nullptr,0,true)==d.result);
    assert(d.cacheDiagnostic.events==count && d.message==0x35 && d.sync);
    // ASan proves the observer doesn't read a short/null prefix. The native
    // handler still receives the original malformed message unchanged.
    for(unsigned n=0;n<16;++n) {
        auto bytes=new uint8_t[n];
        assert(d.reportDataPathEvents(0x33,bytes,n,false)==d.result);
        assert(d.data==bytes && d.length==n); delete[] bytes;
    }
    assert(d.reportDataPathEvents(0x33,nullptr,0x1d0,true)==d.result);
    assert(d.cacheDiagnostic.invalidEvents==17);
    // Submission failure must be observable without inventing a delivery or
    // changing the cache handler's pre-existing successful return contract.
    postResult=kIOReturnNoResources;
    mtkawdl::put32(request.bytes+4,6);
    assert(!d.setPEER_CACHE_CONTROL(&request));
    assert(d.cacheDiagnostic.postFailures==1 && d.cacheDiagnostic.postStatus==postResult);
    assert(d.cacheDiagnostic.events==count+17);
    postResult=0;
    assert(d.dataPeers.setTraffic(peer,true,now));
    auto calls=d.radio.calls;
    mtkawdl::put32(request.bytes+4,2);
    assert(!d.setPEER_CACHE_CONTROL(&request));
    assert(d.radio.calls==calls && mtkawdl::le32(savedMessage+4)==3);
    int member=d.dataPeers.find(peer,now);
    assert(member>=0 && d.dataPeers.entries[member].traffic && !d.dataPeers.entries[member].cached);
    // A teardown during the firmware wait cannot post a false completion.
    mtkawdl::put32(request.bytes+4,0);
    auto before=posts;
    d.radio.duringWait=[&] { d.stopping=true; };
    assert(d.setPEER_CACHE_CONTROL(&request)==kIOReturnNotReady);
    assert(posts==before && !d.retains);
    assert(d.cacheDiagnostic.result==kIOReturnNotReady);
    assert(d.cacheDiagnostic.maxDurationUS==20000);
}
'''
env = os.environ.copy()
if 'DEVELOPER_DIR' not in env and 'CommandLineTools' in subprocess.check_output(
        ['xcode-select', '-p'], text=True):
    env['DEVELOPER_DIR'] = '/Applications/Xcode.app/Contents/Developer'
lib = root / 'build/rust-host/debug/libmtk_ffi.a'
subprocess.run(['cargo', 'build', '-q', '-p', 'mtk-ffi', '--features', 'wifi',
                '--target-dir', str(root/'build/rust-host')], cwd=root/'rust', env=env, check=True)
with tempfile.TemporaryDirectory(prefix='mtk-awdl-cache-') as work:
    work = Path(work)
    (work/'test.cpp').write_text(code)
    subprocess.run(['/usr/bin/clang++', '-std=c++14', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I'+str(root/'kext/Wifi/vendor/include'),
                    str(work/'test.cpp'), str(lib), '-o', str(work/'test')], env=env, check=True)
    subprocess.run([str(work/'test')], check=True)
print('AWDL cache: failed firmware/teardown, copied completion, traffic ownership and bounded native forwarding passed')
