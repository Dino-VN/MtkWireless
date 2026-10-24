"""Run real init/free/shutdown against the null-unsafe native free contract.

Models the verified Tahoe Skywalk free dereference and failure stages. This
regression test does not execute private Apple code or establish kernel safety.
"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'AirportMtkWlan/AirportMtkAwdlInterface.cpp').read_text()
def fn(sig):
 a=s.index(sig);return s[a:s.index('\n}\n',a)+3]
code=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <AWDL/AwdlTimeline.hpp>
#define XYLog(...) ((void)0)
#define bzero(p,n) memset(p,0,n)
#define OSSafeReleaseNULL(x) do{if(x){(x)->release();(x)=nullptr;}}while(0)
using IOReturn=unsigned;
constexpr unsigned APPLE80211_VERSION=1,kIOReturnSuccess=0,kIOReturnNotReady=1,kIOReturnNoMemory=2,kIOReturnError=3,
 APPLE80211_C_FLAG_20MHZ=1,APPLE80211_C_FLAG_2GHZ=8,APPLE80211_C_FLAG_5GHZ=16;
struct ether_addr{uint8_t bytes[6];};
unsigned baseFreed=0,lowerFreed=0,receiverSet=0,transportStopped=0,added=0,removed=0;
bool social=true,baseFail=false,virtualFail=false,timerFail=false,addFail=false;
struct OSObject{};
struct IOSkywalkEthernetInterface:OSObject{void free(){++lowerFreed;}};
struct IO80211SkywalkInterface:IOSkywalkEthernetInterface{
 bool ivars=false;
 bool init(){ivars=!baseFail;return ivars;}
 void free(){assert(ivars);ivars=false;++baseFreed;IOSkywalkEthernetInterface::free();}
};
struct Native:IO80211SkywalkInterface{
 bool init(OSObject*,ether_addr*,unsigned,const char*){assert(ivars);return !virtualFail;}
 void free(){IO80211SkywalkInterface::free();}
};
struct IOTimerEventSource{
 static IOTimerEventSource*timerEventSource(OSObject*,void(*)(OSObject*,IOTimerEventSource*)){return timerFail?nullptr:new IOTimerEventSource;}
 void cancelTimeout(){}void disable(){}void release(){delete this;}
};
struct WorkLoop{unsigned addEventSource(IOTimerEventSource*){if(addFail)return 3;++added;return 0;}void removeEventSource(IOTimerEventSource*){++removed;}};
struct AirportMtkWlan:OSObject{WorkLoop loop;const char*stage=nullptr;WorkLoop*getWorkLoop(){return &loop;}void recordAwdlCreation(const char*s,unsigned){stage=s;}};
struct MtkMwx{bool awdlChannelAllowed(unsigned){return social;} uint8_t infraChannel=0;uint8_t awdlInfraChannel(){return infraChannel;}uint8_t awdlOnlyChannel(){return infraChannel;}void awdlInfraSleep(unsigned){}void awdlInfraWakeIfIdle(){}bool awdlChannelReachable(unsigned c){return awdlChannelAllowed(c)&&(!infraChannel||c==infraChannel);}void awdlSetReceiver(void*o,bool(*)(void*,const uint8_t*,size_t,uint8_t,int8_t)){if(o)++receiverSet;}
 void awdlEnable(void*,bool){++transportStopped;}};
struct apple80211_channel{uint32_t version,channel,flags;};
struct AirportMtkAwdlInterface:Native{
 AirportMtkWlan*owner=nullptr;MtkMwx*hal=nullptr;IOTimerEventSource*timer=nullptr;
 void*bsd=nullptr;bool timerAdded=false,stopping=false,enabled=false,nativeBaseInitialized=false,receiverRegistered=false,trafficActive=false,trafficPeerInstalled=false;
 unsigned queueHead=0,queueCount=0,templateLength=0,requestedChannel=0,grantedChannel=0;uint64_t nextAction=0,requestDeadline=0;
 void*queue[64]{};uint8_t trafficPeer[6]{},installedTrafficPeer[6]{};uint8_t discoveryTxPeers[3][6]{};struct AddbaDecline{uint8_t request[24+9];bool valid;}addbaPending[4]{};uint64_t addbaRequests=0,addbaQueued=0,addbaSubmitted=0,addbaFailed=0,addbaExpired=0,addbaDeferred=0,addbaDropped=0,delbaReceived=0;
 mtkawdl::DataPeers dataPeers;mtkawdl::State state;mtkawdl::Timeline awTimeline;
 unsigned windowEnds=0;
 void notifyAvailabilityWindowEnd(uint64_t){++windowEnds;awTimeline.close();}
 struct {unsigned version,availability_window_length,availability_window_period,extension_length,synchronization_frame_period,non_election_master;}syncParams{};
 struct{unsigned version,unk1,unk2,unk3,unk4;}extension{};
 struct{unsigned version,election_id;}electionId{};
 struct{unsigned version,mode;}presence{},afMode{};
 struct{unsigned version,state;uint8_t master[6];}syncState{};
 struct{unsigned version;int a,b,c,threshold;}rssiThresholds{};
 struct{unsigned version,rate;}minRate{};
 struct{unsigned version,length,step_count;apple80211_channel seqs[32];}channelSequence{};
 static void tick(OSObject*,IOTimerEventSource*){}
 static bool receive(void*,const uint8_t*,size_t,uint8_t,int8_t){return true;}
 void setInterfaceId(unsigned){}void setDataPathEnabled(bool){}void releaseDataPath(){}
 void shutdown(){shutdownInGate();}
 bool initWithController(AirportMtkWlan*,ether_addr*,MtkMwx*);
 void shutdownInGate();void free();
};
uint64_t awdlNow(){return 1000000;}void mbuf_freem(void*){assert(false);}
#define super Native
'''+fn('bool AirportMtkAwdlInterface::initWithController(')+fn('void AirportMtkAwdlInterface::shutdownInGate(')+fn('void AirportMtkAwdlInterface::free(')+r'''
int main(){
 AirportMtkWlan owner;MtkMwx radio;ether_addr mac{{2,1,2,3,4,5}};
 for(unsigned failure=0;failure<6;++failure){
 social=failure!=0;baseFail=failure==1;virtualFail=failure==2;timerFail=failure==3;addFail=failure==4;
 baseFreed=lowerFreed=receiverSet=transportStopped=added=removed=0;
 AirportMtkAwdlInterface obj;
 bool ok=obj.initWithController(&owner,&mac,&radio);assert(ok==(failure==5));
 if(ok)obj.awTimeline.open=true;
 obj.shutdown();obj.free();
 assert(obj.windowEnds==(ok?1U:0U));
 assert(lowerFreed==1&&baseFreed==(failure>=2?1U:0U));
 assert(receiverSet==(ok?1U:0U)&&transportStopped==(ok?1U:0U));
 assert(added==removed);
 }
 puts("PASS actual partial init/free: no channels, base allocation failure, virtual init failure, timer allocation/add failure and complete init; no null native free, no unowned HAL disable, balanced event sources.");
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-partial-init-') as d:
 p=Path(d)/'test.cpp';p.write_text(code);b=Path(d)/'test'
 subprocess.run(['clang++','-std=c++14','-Wall','-Wextra','-fsanitize=address,undefined','-I'+str(root/'include'),str(p),'-o',str(b)],check=True)
 subprocess.run([str(b)],check=True)
