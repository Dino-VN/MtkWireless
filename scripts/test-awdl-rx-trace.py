#!/usr/bin/env python3
"""Run the production bounded AWDL RX recorder under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
base = root / 'kext/Wifi/vendor/AirportMtkWlan/AirportMtkAwdlInterface'
header = base.with_suffix('.hpp').read_text()
source = base.with_suffix('.cpp').read_text()
start = header.index('    struct __attribute__((packed)) DataRxSample {')
end = header.index('    IOReturn dataTxStatus', start)
fields = header[start:end]
start = source.index('void AirportMtkAwdlInterface::recordDataRx(')
method = source[start:source.index('\n}\n', start)+3]
start = source.index('void AirportMtkAwdlInterface::recordInvalidDataRx(')
method += source[start:source.index('\n}\n', start)+3]
code = r'''
#include <AWDL/AwdlProtocol.hpp>
#include <cassert>
#include <cstring>
struct AirportMtkAwdlInterface {
''' + fields + '};\n' + method + r'''
int main() {
    AirportMtkAwdlInterface recorder;
    uint8_t frame[26]{}, source[6]={2,1,2,3,4,5}, destination[6]={2,6,7,8,9,10};
    uint8_t payload[1516]; memset(payload,0xa5,sizeof(payload));
    frame[22]=0x30; frame[23]=0x12;
    mtkawdl::DataView view{};
    view.source=source; view.destination=destination; view.payload=payload;
    view.length=sizeof(payload);
    for(unsigned i=0;i<70;++i) {
        view.sequence=i;
        recorder.recordDataRx(frame,view,100+i,6,-40,i-1,1,true);
    }
    assert(recorder.dataRxTraceCount==70);
    const auto &last=recorder.dataRxTrace[69%64];
    assert(last.sequence==69 && last.previousSequence==68 && last.mpduSequence==0x123);
    assert(last.prefixLength==80 && last.payloadLength==1516 && last.result==1);
    assert(!memcmp(last.source,source,6) && !memcmp(last.destination,destination,6));
    assert(!memcmp(last.prefix,payload,80) && !recorder.dataRxDuplicate && !recorder.dataRxTooOld);
    view.sequence=65; view.length=56;
    recorder.recordDataRx(frame,view,200,149,-70,69,0x11,false);
    assert(recorder.dataRxDuplicate==1 && recorder.dataRxLastRejected.result==2);
    assert(recorder.dataRxLastRejected.previousBitmap==0x11);
    assert(recorder.dataRxLastRejected.prefixLength==56 && !recorder.dataRxLastRejected.prefix[56]);
    view.sequence=1;
    recorder.recordDataRx(frame,view,201,6,-50,100,1,false);
    assert(recorder.dataRxTooOld==1 && recorder.dataRxLastRejected.result==3);
    view.sequence=0;
    recorder.recordDataRx(frame,view,202,6,-50,65535,1,true);
    assert(recorder.dataRxTooOld==1 && recorder.dataRxLastRejected.timeUS==201);
    view.sequence=4095;
    recorder.recordDataRx(frame,view,203,6,-50,0,3,false,0xfff);
    assert(recorder.dataRxDuplicate==2 && recorder.dataRxLastRejected.result==2);
    // Invalid frames may be shorter than a data header or larger than MTU.
    // Only the supplied bytes are read; ring wrap retains the original size.
    uint8_t shortFrame[1]={0xc4};
    recorder.recordInvalidDataRx(shortFrame,sizeof(shortFrame),300,6,-60);
    assert(recorder.dataRxInvalidTrace[0].prefixLength==1);
    assert(recorder.dataRxInvalidTrace[0].prefix[0]==0xc4);
    assert(!recorder.dataRxInvalidTrace[0].prefix[1]);
    for(unsigned i=0;i<35;++i) {
        payload[0]=i;
        recorder.recordInvalidDataRx(payload,sizeof(payload),301+i,149,-50);
    }
    assert(recorder.dataRxInvalidTraceCount==36);
    const auto &bad=recorder.dataRxInvalidTrace[35%32];
    assert(bad.timeUS==335 && bad.frameLength==1516 && bad.prefixLength==192);
    assert(bad.prefix[0]==34 && bad.channel==149 && bad.rssi==-50);
    recorder.recordInvalidDataRx(frame,sizeof(frame),400,6,-40,true);
    assert(recorder.dataRxInvalidTraceCount==36);
    assert(recorder.dataRxWrongDestinationLast.timeUS==400);
    assert(recorder.dataRxWrongDestinationLast.prefixLength==sizeof(frame));
    assert(!memcmp(recorder.dataRxWrongDestinationLast.prefix,frame,sizeof(frame)));
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-awdl-rx-trace-') as work:
    work = Path(work)
    (work / 'test.cpp').write_text(code)
    subprocess.run(['clang++', '-std=c++14', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I'+str(root/'kext/Wifi/vendor/include'),
                    str(work/'test.cpp'), '-o', str(work/'test')], check=True)
    subprocess.run([str(work/'test')], check=True)
print('AWDL RX trace: production recorder wrap, bounds, rejection counters and retained sample passed')
