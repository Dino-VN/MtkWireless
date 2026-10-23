"""Validate the Tahoe AWDL_ADVERTISERS 44-byte ABI decoder."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <AWDL/AwdlAdvertisers.hpp>
int main() {
    const uint8_t raw[] = {
        0x64,0x00,0x20,0x00,0x02,0x00,0x6e,0x00,0x00,0x00,0x10,0x00,
        0x10,0x00,0xc4,0x00,0x04,0x00,0x01,0x00,0x12,0x34,0x56,0x78,
        0x04,0x03,0x02,0x01,0xd4,0xc3,0xb2,0xa1,0x02,0x11,0x22,0x33,
        0x44,0x55,0x02,0xaa,0xbb,0xcc,0xdd,0xee
    };
    static_assert(sizeof(mtkawdl::AdvertiserRecord)==44, "ABI size");
    mtkawdl::AdvertiserRecord record{};
    assert(mtkawdl::decodeAdvertiserRecord(raw,sizeof(raw),record));
    assert(record.awCounter==100 && record.downCounter==32);
    assert(record.syncContention==2 && record.afPeriod==110);
    assert(record.raw8==0 && record.raw10==16 && record.awPeriod==16);
    assert(record.rssi==-60 && record.presence==4 && record.raw18==1);
    assert(record.fwAfRxTime==0x12345678);
    assert(record.peerSelfMetric==0x01020304 && record.rootMetric==0xa1b2c3d4);
    assert(record.rootMaster[0]==0x02 && record.rootMaster[5]==0x55);
    assert(record.immediatePeer[0]==0x02 && record.immediatePeer[5]==0xee);
    assert(!mtkawdl::decodeAdvertiserRecord(raw,43,record));
    assert(!mtkawdl::decodeAdvertiserRecord(raw,45,record));
    uint8_t mutated[sizeof(raw)]; memcpy(mutated,raw,sizeof(raw));
    mutated[20]=0xa5; mutated[21]=0xa4; mutated[22]=0xa3; mutated[23]=0xa2;
    assert(mtkawdl::decodeAdvertiserRecord(mutated,sizeof(mutated),record));
    assert(record.fwAfRxTime==0xa5a4a3a2);
    puts("PASS AWDL_ADVERTISERS 44-byte offsets/endian decoder");
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-awdl-advertisers-') as tmp:
    cpp = Path(tmp) / 'test.cpp'
    binary = Path(tmp) / 'test'
    cpp.write_text(code)
    subprocess.run(['clang++','-std=c++14','-Wall','-Wextra','-fsanitize=address,undefined',
                    '-I'+str(root/'include'),str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
