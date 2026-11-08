#!/usr/bin/env python3
"""Exercise the production native RX branch and Rust ABI with ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kext/Wifi/vendor/AirportMtkWlan/AirportMtkAwdlInterface.cpp').read_text()
start = source.index('        uint64_t nativeTag=mtkawdl::nativeRxTag(')
branch = source[start:source.index('\n#ifdef KERNEL', start)]
subprocess.run(['cargo', 'build', '-q', '-p', 'mtk-ffi', '--features', 'wifi',
                '--target-dir', str(root/'build/rust-host')], cwd=root/'rust', check=True)
code = r'''
#include <AWDL/AwdlProtocol.hpp>
#include <cassert>
#include <cstring>
#include <vector>
struct Rx {
    uint64_t nativeIPv6Tags[128]={},nativeIPv6TagTimes[128]={};
    unsigned nativeIPv6TagNext=0;
    uint64_t nativeIPv6Decoded=0,nativeIPv6Duplicate=0,dataRxDecoded=0;
    std::vector<uint8_t> decoded;
    void inputNativeView(const mtkawdl::DataView &view) {
        decoded.resize(mtkawdl::dataEthernetLength(view));
        assert(decoded.size()>=54 && decoded.size()<=1514);
        assert(mtkawdl::decodeDataView(decoded.data(),decoded.size(),view)==decoded.size());
    }
    void receive(const std::vector<uint8_t> &frame,uint64_t now) {
        const uint8_t *f=frame.data();size_t n=frame.size();
        assert(mtkawdl::validDataFrame(f,n));
        mtkawdl::DataView view={};
        size_t at=mtkawdl::dataHeaderLength(f,n);
        auto self=this;
        while(at<n) {
            auto used=mtkawdl::dataView(f,n,at,view);assert(used);at+=used;
''' + branch + r'''
        }
    }
};
int main() {
    // Synthetic complete RFC1042 IPv6 frame matching the captured native
    // header format. The retained hardware records contain prefixes only.
    uint8_t eth[96]={2,1,2,3,4,5,2,1,2,3,4,6,0x86,0xdd,0x60};
    eth[18]=0;eth[19]=42;
    uint8_t wire[122]={};assert(mtkawdl::encodeData(wire,sizeof(wire),eth,sizeof(eth),42)==sizeof(wire));
    std::vector<uint8_t> native(wire,wire+24);native[0]=0x88;
    native.push_back(0x87);native.push_back(0);
    native.insert(native.end(),eth,eth+12);native.push_back(0);native.push_back(90);
    const uint8_t snap[]={0xaa,0xaa,3,0,0,0,0x86,0xdd};
    native.insert(native.end(),snap,snap+8);native.insert(native.end(),eth+14,eth+96);
    Rx rx;rx.receive(native,100);assert(rx.nativeIPv6Decoded==1);
    assert(rx.decoded.size()==96 && !memcmp(rx.decoded.data(),eth,96));
    auto retry=native;retry[1]|=8;rx.receive(retry,101);
    assert(rx.nativeIPv6Duplicate==1 && rx.nativeIPv6Decoded==1);
    auto split=native;split.back()=1;rx.receive(split,102);
    assert(rx.nativeIPv6Decoded==2 && rx.decoded.back()==1);
    rx.receive(native,3000100);assert(rx.nativeIPv6Decoded==3);
    for(unsigned i=0;i<130;++i) {
        auto next=native;next[next.size()-2]=i;next.back()=2;
        rx.receive(next,3000200+i);
    }
    assert(rx.nativeIPv6Decoded==133 && rx.nativeIPv6Duplicate==1);
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-native-rx-') as directory:
    directory = Path(directory)
    (directory/'test.cpp').write_text(code)
    subprocess.run(['clang++', '-std=c++14', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I'+str(root/'kext/Wifi/vendor/include'),
                    str(directory/'test.cpp'), str(root/'build/rust-host/debug/libmtk_ffi.a'),
                    '-o', str(directory/'test')], check=True)
    subprocess.run([str(directory/'test')], check=True)
print('Native Mirroring RX: production retry cache, hardware splits, expiry, wrap and ABI decode PASS')
