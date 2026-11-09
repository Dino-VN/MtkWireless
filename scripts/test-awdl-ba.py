#!/usr/bin/env python3
"""Exercise production DELBA queue/service excerpts against the real Rust FFI."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
vendor = root / 'kext/Wifi/vendor'
source = (vendor / 'AirportMtkWlan/AirportMtkAwdlInterface.cpp').read_text()
header = (vendor / 'AirportMtkWlan/AirportMtkAwdlInterface.hpp').read_text()
a = source.index('bool AirportMtkAwdlInterface::receive(')
receive = source[a:source.index('        if(!mtkawdl::validActionEnvelope(', a)]
receive = receive.replace('    uint64_t now=awdlNow();', '    (void)channel;\n    (void)awdlNow();')
receive += '        return false;\n    }\n    return false;\n}\n'
a = source.index('    for(auto &pending:addbaPending) {', source.index('void AirportMtkAwdlInterface::serviceInGate('))
service = source[a:source.index('        if(dataPeers.find(peer,now)<0', a)] + '        (void)ssn; (void)window;\n    }\n'
pending = re.search(r'    struct AddbaDecline[^\n]+', header).group()
assert 'setProperty("MTKAWDLDELBADropped",delbaDropped,64);' in source
assert 'uint64_t delbaDropped = 0;' in header
code = r'''
#include <AWDL/AwdlProtocol.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
using IOReturn=unsigned;
uint64_t awdlNow(){return 100;}
struct Hal {
    bool sessions[8]{true,true,true,true,true,true,true,true};
    unsigned teardowns=0;uint8_t lastTid=0xff;
    uint8_t peer[6]{2,1,2,3,4,6};
    bool stopAfter=false;bool *stopping=nullptr;
    IOReturn awdlRxBlockAck(const uint8_t *address,uint8_t tid,uint16_t ssn,uint16_t window,bool enable){
        assert(!memcmp(address,peer,6) && tid<8 && !ssn && !window && !enable);
        sessions[tid]=false;lastTid=tid;++teardowns;
        if(stopAfter)*stopping=true;
        return 0;
    }
};
struct AirportMtkAwdlInterface {
    bool enabled=true,stopping=false;mtkawdl::State state{};
    Hal boundary;Hal *hal=&boundary;
    uint64_t actionReceived=0,addbaRequests=0,addbaDropped=0,delbaReceived=0,delbaDropped=0;
    int8_t lastActionRSSI=0;
''' + pending + r'''
    AirportMtkAwdlInterface(){
        const uint8_t own[6]={2,1,2,3,4,5};state.init(own,100);boundary.stopping=&stopping;
    }
    static bool receive(void *,const uint8_t *,size_t,uint8_t,int8_t);
    // Only the DELBA service arm is extracted. Radio scheduling and ADDBA
    // firmware programming are deliberately outside this fixture's scope.
    void serviceDELBA(){uint64_t now=awdlNow();
''' + service + r'''
        (void)now;
    }
};
''' + receive + r'''
std::array<uint8_t,30> frame(const AirportMtkAwdlInterface &d,uint8_t tid,bool originator){
    std::array<uint8_t,30> f{};f[0]=0xd0;
    memcpy(f.data()+4,d.state.mac,6);memcpy(f.data()+10,d.boundary.peer,6);
    memcpy(f.data()+16,mtkawdl::bssid,6);f[24]=mtkawdl::blockAckCategory;f[25]=mtkawdl::delba;
    const uint16_t params=uint16_t(tid)<<12|(originator?0x0800:0);
    f[26]=uint8_t(params);f[27]=uint8_t(params>>8);f[28]=37;
    return f;
}
void noPending(const AirportMtkAwdlInterface &d){for(const auto &p:d.addbaPending)assert(!p.valid);}
int main(){
    // Full four-bit wire TIDs must survive the ABI: unsupported 8..15
    // cannot alias any 0..7 recipient session; recipient DELBA is not RX.
    for(uint8_t tid=0;tid<16;++tid)for(bool originator:{false,true}){
        AirportMtkAwdlInterface d;auto f=frame(d,tid,originator);uint8_t out=0xa5;
        const bool expected=originator && tid<8;
        assert(mtk_awdl_delba_rx_tid(d.state.mac,f.data(),f.size(),&out)==expected);
        assert(out==(expected?tid:0xa5));
        assert(d.receive(&d,f.data(),f.size(),6,-40));d.serviceDELBA();
        assert(d.boundary.teardowns==unsigned(expected));
        for(unsigned i=0;i<8;++i)assert(d.boundary.sessions[i]==!(expected && i==tid));
        noPending(d);d.serviceDELBA();assert(d.boundary.teardowns==unsigned(expected));
    }
    {AirportMtkAwdlInterface d;auto f=frame(d,13,true);
     assert(d.receive(&d,f.data(),f.size(),6,-40));d.serviceDELBA();
     assert(d.boundary.sessions[5] && !d.boundary.teardowns);}
    // A queued frame must retain the parser's original length boundary.
    // Zero-padding a 28/29-byte frame must not fabricate its missing reason.
    for(size_t n=0;n<30;++n){
        AirportMtkAwdlInterface d;auto f=frame(d,5,true);uint8_t out=0xa5;
        assert(!mtk_awdl_delba_rx_tid(d.state.mac,f.data(),n,&out) && out==0xa5);
        d.receive(&d,f.data(),n,6,-40);d.serviceDELBA();
        assert(!d.boundary.teardowns && d.boundary.sessions[5]);noPending(d);
    }
    // Overflow remains bounded best-effort admission, but it is observable.
    // Queue-full receive=true means handled, not successful firmware teardown.
    {AirportMtkAwdlInterface d;
     for(uint8_t tid=0;tid<4;++tid){auto f=frame(d,tid,true);assert(d.receive(&d,f.data(),f.size(),6,-40));}
     auto fifth=frame(d,5,true);assert(d.receive(&d,fifth.data(),fifth.size(),6,-40));
     assert(d.delbaReceived==5 && d.delbaDropped==1 && !d.addbaDropped);
     assert(!d.boundary.teardowns);d.serviceDELBA();noPending(d);
     assert(d.boundary.teardowns==4 && d.boundary.sessions[5]);
     assert(d.receive(&d,fifth.data(),fifth.size(),6,-40));d.serviceDELBA();
     assert(d.boundary.teardowns==5 && !d.boundary.sessions[5] && d.delbaDropped==1);}
    // ADDBA requests share the same four slots; they can also fill admission.
    {AirportMtkAwdlInterface d;std::array<uint8_t,33> request{};auto base=frame(d,0,true);
     memcpy(request.data(),base.data(),base.size());request[25]=mtkawdl::addbaRequest;
     request[27]=2;request[28]=0x10;request[29]=request[30]=request[31]=request[32]=0;
     for(uint8_t token=1;token<=4;++token){request[26]=token;
         assert(d.receive(&d,request.data(),request.size(),6,-40));}
     auto f=frame(d,5,true);assert(d.receive(&d,f.data(),f.size(),6,-40));
     assert(d.addbaRequests==4 && !d.addbaDropped && d.delbaReceived==1 && d.delbaDropped==1);
     d.serviceDELBA();noPending(d);assert(!d.boundary.teardowns && d.boundary.sessions[5]);}
    // Preserve the action envelope, address, category and direction checks.
    for(unsigned offset:{0u,1u,4u,10u,16u,24u,25u}){
        AirportMtkAwdlInterface d;auto f=frame(d,5,true);
        f[offset]^=offset==1?0x40:1;uint8_t out=0xa5;
        assert(!mtk_awdl_delba_rx_tid(d.state.mac,f.data(),f.size(),&out) && out==0xa5);
        d.receive(&d,f.data(),f.size(),6,-40);d.serviceDELBA();
        assert(!d.boundary.teardowns);
    }
    {AirportMtkAwdlInterface d;auto f=frame(d,5,true);uint8_t out=0xa5;
     assert(!mtk_awdl_delba_rx_tid(nullptr,f.data(),f.size(),&out) && out==0xa5);
     assert(!mtk_awdl_delba_rx_tid(d.state.mac,nullptr,1,&out) && out==0xa5);
     assert(!mtk_awdl_delba_rx_tid(d.state.mac,f.data(),f.size(),nullptr));}
    for(bool stopping:{false,true}){
        AirportMtkAwdlInterface d;auto f=frame(d,5,true);
        if(stopping)d.stopping=true;else d.enabled=false;
        assert(!d.receive(&d,f.data(),f.size(),6,-40));noPending(d);
    }
    {AirportMtkAwdlInterface d;auto f=frame(d,5,true);d.boundary.stopAfter=true;
     assert(d.receive(&d,f.data(),f.size(),6,-40));d.serviceDELBA();
     assert(d.stopping && d.boundary.teardowns==1);noPending(d);}
    puts("PASS: DELBA Rust FFI/production queue/service, TID range, truncation, envelopes and overflow telemetry");
}
'''
host = root / 'build/rust-host'
subprocess.run(['cargo', 'build', '-q', '-p', 'mtk-ffi', '--features', 'wifi',
                '--target-dir', str(host)], cwd=root / 'rust', check=True)
with tempfile.TemporaryDirectory(prefix='mtk-awdl-ba-') as tmp:
    work = Path(tmp)
    (work / 'test.cpp').write_text(code)
    subprocess.run(['clang++', '-std=c++14', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(vendor / 'include'),
                    str(work / 'test.cpp'), str(host / 'debug/libmtk_ffi.a'),
                    '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
