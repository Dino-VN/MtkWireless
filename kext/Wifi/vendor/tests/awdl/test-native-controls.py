"""Execute the native template/capability/traffic handlers with Tahoe payloads."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'AirportMtkWlan/AirportMtkAwdlInterface.cpp').read_text()
functions = []
for name in ['setAWDL_SYNC_FRAME_TEMPLATE', 'getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES',
             'setAWDL_PEER_TRAFFIC_REGISTRATION', 'setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE',
             'setAWDL_SYNC_PARAMS', 'setAWDL_SECONDARY_MASTER_CHANNEL']:
    start = source.index('IOReturn AirportMtkAwdlInterface::' + name + '(')
    functions.append(source[start:source.index('\n}\n', start) + 3])
header = (root / 'include/Airport/apple80211_ioctl.h').read_text()
structs = []
for name in ['apple80211_awdl_sync_channel_sequence', 'apple80211_awdl_sync_params',
             'apple80211_awdl_secondary_master_channel']:
    start = header.index('struct '+name+' {')
    structs.append(header[start:header.index('} __attribute__((packed));',start)+len('} __attribute__((packed));')])
code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <initializer_list>
#include <AWDL/AwdlProtocol.hpp>
using IOReturn = unsigned;
constexpr unsigned APPLE80211_VERSION = 1, kIOReturnSuccess = 0,
                   kIOReturnBadArgument = 1, kIOReturnUnsupported = 2,kIOReturnNoResources=7;
#define __IO80211_TARGET __MAC_26_0
struct apple80211_channel { uint32_t version,channel,flags; };
'''+'\n'.join(structs)+r'''
uint64_t awdlNow() { return 1000000; }
struct apple80211_awdl_sync_frame_template {
    uint32_t version, payload_len; void *payload;
} __attribute__((packed));
struct apple80211_awdl_cap {
    uint32_t version; uint8_t cap;
} __attribute__((packed));
struct apple80211_awdl_peer_traffic_registration {
    uint32_t version; uint8_t addr[6], pad[2]; uint32_t name_len;
    char name[152]; uint32_t active;
};
struct Timer { unsigned calls = 0; void setTimeoutMS(unsigned n) { assert(n==1); ++calls; } };
struct AirportMtkAwdlInterface {
    struct Hal { bool awdlChannelAllowed(unsigned c) { return c==6 || c==11 || c==44 || c==149; } uint8_t infraChannel=0;uint8_t awdlInfraChannel(){return infraChannel;}uint8_t awdlOnlyChannel(){return infraChannel;}void awdlInfraSleep(unsigned){}void awdlInfraWakeIfIdle(){}bool awdlChannelReachable(unsigned c){return awdlChannelAllowed(c)&&(!infraChannel||c==infraChannel);} } radio;
    Hal *hal = &radio;
    mtkawdl::DataPeers dataPeers;mtkawdl::State state;
    apple80211_awdl_sync_channel_sequence channelSequence{};
    apple80211_awdl_sync_params syncParams{};
    uint64_t nextAction = 0; uint32_t secondaryMasterChannel = 0;
    uint8_t syncTemplate[2048]{}, trafficPeer[6]{};
    unsigned templateLength = 0; bool enabled = true, trafficActive = false;
    Timer clock; Timer *timer = &clock;
    void setProperty(const char*,uint64_t,unsigned) {}
    void setProperty(const char*,const void*,unsigned) {}
    IOReturn setAWDL_SYNC_FRAME_TEMPLATE(apple80211_awdl_sync_frame_template*);
    IOReturn getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES(apple80211_awdl_cap*);
    IOReturn setAWDL_PEER_TRAFFIC_REGISTRATION(apple80211_awdl_peer_traffic_registration*);
    IOReturn setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence*);
    IOReturn setAWDL_SYNC_PARAMS(apple80211_awdl_sync_params*);
    IOReturn setAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel*);
};
''' + '\n'.join(functions) + r'''
int main() {
    AirportMtkAwdlInterface interface;
    const uint8_t local[6]={2,1,2,3,4,5}; interface.state.init(local,awdlNow());
    // Construct the request using offsets observed in the shipped Tahoe KC,
    // independently of C++ field names. Flags 0x8/0x10 expose the old +4 bug.
    uint8_t raw[0x190]={};
    mtkawdl::put32(raw,1);raw[5]=16;raw[6]=1;raw[7]=3;raw[9]=0xff;
    for(unsigned i=0;i<16;++i) {
        mtkawdl::put32(raw+12+i*12,1);
        mtkawdl::put32(raw+16+i*12,i<8?149:6);
        mtkawdl::put32(raw+20+i*12,i<8?0x10:0x8);
    }
    static_assert(sizeof(apple80211_awdl_sync_channel_sequence)==0x190,"native payload size");
    apple80211_awdl_sync_channel_sequence sequence; memcpy(&sequence,raw,sizeof(sequence));
    assert(!interface.setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(&sequence));
    assert(interface.state.sequence.channels[0]==149&&interface.state.sequence.channels[8]==6);
    sequence.seqs[2].channel=166;
    auto old=interface.state.sequence;
    assert(interface.setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(&sequence)==kIOReturnBadArgument);
    assert(!memcmp(&old,&interface.state.sequence,sizeof(old)));
    apple80211_awdl_sync_params params{1,16,16,16,440,0};
    assert(!interface.setAWDL_SYNC_PARAMS(&params)&&interface.state.psfPeriod==440);
    params.synchronization_frame_period=110;
    assert(!interface.setAWDL_SYNC_PARAMS(&params)&&interface.state.psfPeriod==110);
    params.synchronization_frame_period=0;
    assert(interface.setAWDL_SYNC_PARAMS(&params)==kIOReturnUnsupported&&interface.state.psfPeriod==110);
    uint8_t master[6]={2,9,8,7,6,5};assert(interface.state.setSyncRole(2,master));
    params.synchronization_frame_period=440;
    assert(!interface.setAWDL_SYNC_PARAMS(&params)&&interface.state.syncRole==2);
    assert(!memcmp(interface.state.forcedSync,master,6));
    // Native owns the programmed sequence even while following a peer whose
    // advertised sequence differs. RX and timer election must preserve it.
    interface.state.nativeElectionManaged=true;
    interface.state.nativeParentSelected=true;
    memcpy(interface.state.nativeParent,master,6);
    auto &p=interface.state.peers[0];p.used=true;p.seen=p.epoch=awdlNow();
    memcpy(p.mac,master,6);p.advertisement.sequence=interface.state.sequence;
    p.advertisement.sequence.channels[0]=44;p.advertisement.presence=4;
    memcpy(p.advertisement.master,master,6);memcpy(p.advertisement.sync,master,6);
    interface.state.elect(awdlNow());assert(interface.state.synchronized);
    sequence.seqs[2].channel=149;
    assert(!interface.setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(&sequence));
    interface.state.elect(awdlNow());
    assert(interface.state.sequence.channels[0]==149&&interface.state.channel(awdlNow())==149);
    // Native mode 1 can synchronize to a distinct immediate master (KC
    // programChipWithNewMaster). It is not restricted to the local MAC.
    assert(interface.state.setSyncRole(1,master));
    apple80211_awdl_secondary_master_channel secondary{1,149};
    assert(!interface.setAWDL_SECONDARY_MASTER_CHANNEL(&secondary)&&interface.secondaryMasterChannel==149);
    secondary.secondary_master_channel=166;
    assert(interface.setAWDL_SECONDARY_MASTER_CHANNEL(&secondary)==kIOReturnBadArgument);
    // A service TLV alone is a valid native supplement: no vendor header/sync.
    uint8_t service[] = {6,9,0,0,0,0,0,0,0,0,0,0};
    apple80211_awdl_sync_frame_template d{0,sizeof(service),service};
    assert(!interface.setAWDL_SYNC_FRAME_TEMPLATE(&d));
    assert(interface.templateLength == sizeof(service));
    assert(!memcmp(interface.syncTemplate,service,sizeof(service)));
    uint8_t before[2048]; memcpy(before,interface.syncTemplate,sizeof(before));
    for (unsigned length : {0U,1U,2U,4U,11U,2049U}) {
        d.payload_len = length;
        assert(interface.setAWDL_SYNC_FRAME_TEMPLATE(&d)==kIOReturnBadArgument);
        assert(interface.templateLength==sizeof(service));
        assert(!memcmp(interface.syncTemplate,before,sizeof(before)));
    }
    d.payload_len=sizeof(service);d.payload=nullptr;
    assert(interface.setAWDL_SYNC_FRAME_TEMPLATE(&d)==kIOReturnBadArgument);
    assert(interface.setAWDL_SYNC_FRAME_TEMPLATE(nullptr)==kIOReturnBadArgument);
    apple80211_awdl_cap cap{99,255};
    assert(!interface.getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES(&cap));
    assert(cap.version==1&&cap.cap==0);
    assert(interface.getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES(nullptr)==kIOReturnBadArgument);
    apple80211_awdl_peer_traffic_registration traffic{};
    // Native disable has an all-zero peer and must not trigger the timer.
    assert(!interface.setAWDL_PEER_TRAFFIC_REGISTRATION(&traffic));
    assert(!interface.trafficActive&&interface.clock.calls==0);
    traffic.active=1;
    assert(interface.setAWDL_PEER_TRAFFIC_REGISTRATION(&traffic)==kIOReturnBadArgument);
    traffic.addr[0]=2;traffic.addr[5]=1;
    assert(!interface.setAWDL_PEER_TRAFFIC_REGISTRATION(&traffic));
    assert(interface.trafficActive&&interface.clock.calls==1);
    assert(!memcmp(interface.trafficPeer,traffic.addr,6));
    traffic.name_len=153;
    assert(interface.setAWDL_PEER_TRAFFIC_REGISTRATION(&traffic)==kIOReturnBadArgument);
    puts("PASS real native control handlers: Tahoe raw +0xc channel descriptors; channel validation, 110/440 PSF and retained sync peer, secondary social channel, native TLV template and traffic validation.");
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-awdl-controls-') as tmp:
    cpp = Path(tmp) / 'test.cpp'
    binary = Path(tmp) / 'test'
    cpp.write_text(code)
    subprocess.run(['clang++','-std=c++14','-Wall','-Wextra',
                    '-fsanitize=address,undefined','-I'+str(root/'include'),
                    str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
