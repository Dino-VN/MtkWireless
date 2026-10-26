#!/usr/bin/env python3
"""Execute production native lifecycle and packet ownership with failure injection.

Mocks model the native contracts established from the pinned Tahoe KC. This
checks our control flow, not the private implementation executing in kernel.
"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'AirportMtkWlan/AirportMtkAwdlInterface.cpp').read_text()
controller=(root/'AirportMtkWlan/AirportMtkWlan.cpp').read_text()
def fn(text, signature):
    begin=text.index(signature)
    return text[begin:text.index('\n}\n',begin)+3]
functions='\n'.join(fn(source,s) for s in [
 'UInt AirportMtkAwdlInterface::getHardwareAssists(',
 'void AirportMtkAwdlInterface::setHardwareAssists(',
 'void AirportMtkAwdlInterface::publishBSDOffloadState(',
 'IOReturn AirportMtkAwdlInterface::prepareBSDInterface(',
 'bool AirportMtkAwdlInterface::start(',
 'bool AirportMtkAwdlInterface::createDataPath(',
 'void AirportMtkAwdlInterface::releaseDataPath(',
 'IOReturn AirportMtkAwdlInterface::enable(unsigned',
 'IOReturn AirportMtkAwdlInterface::disable(unsigned',
 'IOReturn AirportMtkAwdlInterface::dataPathGated(',
 'void AirportMtkAwdlInterface::enableDatapath(',
 'void AirportMtkAwdlInterface::disableDatapath(',
 'void AirportMtkAwdlInterface::setDataPathEnabled(',
 'unsigned AirportMtkAwdlInterface::submitNative(',
 'void AirportMtkAwdlInterface::drainCompletions(',
 'void AirportMtkAwdlInterface::inputNative(',
 'void AirportMtkAwdlInterface::stop(',
 'IOReturn AirportMtkAwdlInterface::setSync(bool',
 'IOReturn AirportMtkAwdlInterface::syncGated(',
 'IOReturn AirportMtkAwdlInterface::setSyncInGate(',
 'IOReturn AirportMtkAwdlInterface::setMacAddress(',
 'ether_addr AirportMtkAwdlInterface::getSelfMacAddr('])
functions+='\n'+fn(controller,'bool AirportMtkWlan::createAwdlInterface(ether_addr *address)')
functions+='\n'+fn(controller,'void AirportMtkWlan::stopAwdlInterface()')
functions+='\n'+fn(controller,'IOReturn AirportMtkWlan::createAwdlInterfaceRequest(')
functions+='\n'+fn(controller,'IO80211VirtualInterface *AirportMtkWlan::createVirtualInterface(')
stubs=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <functional>
#include <cerrno>
#include <map>
#include <string>
#define XYLog(...) ((void)0)
#define IOLog(...) ((void)0)
#define APPLE80211_VERSION 1
using IOReturn=unsigned;
using UInt=unsigned;
using ifnet_t=void*;
constexpr unsigned IFCAP_HWCSUM=3,IFCAP_TSO=0x60,IFCAP_LRO=0x80,IFCAP_CSUM_PARTIAL=0x2000,
 IFCAP_CSUM_ZERO_INVERT=0x4000,IFCAP_LRO_NUM_SEG=0x8000;
unsigned hwassist=0x37f,capabilities=0xffff,supported=0xffff;int offloadError=0,capsError=0,supportedError=0,bsdPrepareStatus=0;
bool ignoreCaps=false;
int ifnet_set_offload(ifnet_t,unsigned value){if(offloadError)return offloadError;hwassist=value;return 0;}
int ifnet_set_capabilities_enabled(ifnet_t,unsigned value,unsigned mask){if(capsError)return capsError;if(!ignoreCaps)capabilities=(capabilities&~mask)|(value&mask);return 0;}
int ifnet_set_capabilities_supported(ifnet_t,unsigned value,unsigned mask){if(supportedError)return supportedError;supported=(supported&~mask)|(value&mask);return 0;}
unsigned ifnet_offload(ifnet_t){return hwassist;}
unsigned ifnet_capabilities_supported(ifnet_t){return supported;}
unsigned ifnet_capabilities_enabled(ifnet_t){return capabilities;}
using IOThread=void*;using AbsoluteTime=uint64_t;
constexpr unsigned kIOReturnTimeout=10,kMillisecondScale=1,THREAD_UNINT=0;
IOThread IOThreadSelf(){static int identity;return &identity;}
void clock_interval_to_deadline(unsigned,unsigned,AbsoluteTime*p){*p=10;}
constexpr unsigned kIOReturnSuccess=0,kIOReturnNotReady=1,kIOReturnError=2,kIOReturnNoMemory=3,
 kIOReturnUnsupported=4,kIOReturnBadArgument=5,kIOReturnBusy=6,kIOReturnOutputSuccess=0;
int live=0,factory=0,failFactory=0,add=0,failAdd=0;
bool resourceAvailable=true,deferralFail=false,superFail=false,infoFail=false,registerFail=false,macFail=false,macMismatch=false;
int nativeStarts=0,publications=0,completionsDone=0,outstanding=0;
bool rejectCompletion=false,rejectRX=false,prepareFail=false,allocateFail=false;
bool badRXBuffers=false,badRXOffset=false,badRXLength=false,noRXAddress=false;
unsigned rxFinalized=0,rxReclaimed=0,rxPreparedCompleted=0;
IOReturn nativeEnableStatus=0;
std::function<void()> nativeDisableObserver;
struct OSObject { virtual ~OSObject(){}; virtual void release(){delete this;} };
struct Gate{IOReturn runAction(IOReturn(*fn)(OSObject*,void*,void*,void*,void*),void*a,void*b=nullptr){return fn(nullptr,a,b,nullptr,nullptr);}void commandSleep(void*,AbsoluteTime,unsigned){assert(false);}void commandWakeup(void*,bool){}}gate;
Gate*_fCommandGate=&gate;
struct OSDictionary:OSObject {};
struct IOService:OSObject {
 static OSDictionary *resourceMatching(const char*) { return new OSDictionary; }
 static IOService *copyMatchingService(OSDictionary*) { return resourceAvailable?new IOService:nullptr; }
};
#define OSSafeReleaseNULL(x) do { if(x) {(x)->release();(x)=nullptr;} } while(0)
#define OSDynamicCast(type, value) static_cast<type *>(value)
struct ether_addr {uint8_t bytes[6];};
struct apple80211_virt_if_create_data {unsigned version=1,role=6;uint8_t mac[6]={2,1,2,3,4,6};char bsd_name[16]{};};
size_t strlcpy(char *to,const char *from,size_t n){size_t length=strlen(from);if(n){size_t copy=std::min(n-1,length);memcpy(to,from,copy);to[copy]=0;}return length;}
namespace mtkawdl { struct DataPeers{void reset(){}}; bool unicast(const uint8_t *p) {return p[0] && !(p[0]&1);} }
#define IEEE80211_S_RUN 4
struct IC {uint8_t ic_myaddr[6]={2,1,2,3,4,5};int ic_state=IEEE80211_S_RUN;};
struct MtkMwx:IOService {IC ic; bool channels=true,ready=true; std::function<void()> wait;
 bool awdlInterfaceReady(){return ready;}
 IC *get80211Controller(){return &ic;}bool awdlChannelAllowed(unsigned){return channels;} uint8_t infraChannel=0;uint8_t awdlInfraChannel(){return infraChannel;}uint8_t awdlOnlyChannel(){return infraChannel;}void awdlInfraSleep(unsigned){}void awdlInfraWakeIfIdle(){}bool awdlChannelReachable(unsigned c){return awdlChannelAllowed(c)&&(!infraChannel||c==infraChannel);}
 IOReturn awdlEnable(const uint8_t*,bool){if(wait)wait();return 0;}};
struct IOSkywalkPacketQueue;
struct IOSkywalkPacket:OSObject {
 unsigned length=64,offset=0,buffers=1; bool prepared=false,tx=false;
 IOSkywalkPacketQueue *source=nullptr; uint8_t bytes[2048]={};
 IOSkywalkPacket(){++live;} ~IOSkywalkPacket(){--live;}
 unsigned getDataLength(){return length;} unsigned getDataOffset(){return offset;}
 unsigned getPacketBufferCount(){return buffers;}
 void *getDataVirtualAddress(){assert(prepared);return !tx&&noRXAddress?nullptr:bytes;}
 IOReturn setDataOffset(unsigned o){assert(prepared);offset=o;return badRXOffset?1:0;}
 IOReturn setDataLength(unsigned n){assert(prepared);length=n;return n<=2048&&!badRXLength?0:1;}
 IOReturn prepareWithQueue(IOSkywalkPacketQueue *q,unsigned direction,unsigned){assert(direction==2);source=q;prepared=true;buffers=badRXBuffers?2:1;return prepareFail?1:0;}
 void completeWithQueue(IOSkywalkPacketQueue*,unsigned);
};
struct IOSkywalkPacketBufferPool:OSObject {
 struct PoolOptions{unsigned packets,buffers,size,maxBuffers,segment,flags;uint64_t reserved;};
 IOSkywalkPacketBufferPool(){++live;} ~IOSkywalkPacketBufferPool(){--live;}
 static IOSkywalkPacketBufferPool *withName(const char*,OSObject*,unsigned,const PoolOptions *o){
  assert(o->size==2048&&o->maxBuffers==1);return ++factory==failFactory?nullptr:new IOSkywalkPacketBufferPool;}
 IOReturn allocatePacket(IOSkywalkPacket **p,unsigned flags){assert(flags==1);if(allocateFail)return 1;*p=new IOSkywalkPacket;return 0;}
 void deallocatePacket(IOSkywalkPacket *p){if(p->tx){assert(outstanding>0);--outstanding;++completionsDone;}
  else {++rxReclaimed;if(p->prepared){p->completeWithQueue(nullptr,2);++rxPreparedCompleted;}}
  delete p;}
};
struct IOSkywalkPacketQueue:OSObject {
 bool enabled=false,added=false;IOSkywalkPacketBufferPool *pool;
 IOSkywalkPacketQueue(IOSkywalkPacketBufferPool *p):pool(p){++live;}
 ~IOSkywalkPacketQueue(){assert(!added);--live;}
 void disable(){enabled=false;} void enable(){enabled=true;}
 virtual void packetCompletion(IOSkywalkPacket*,IOSkywalkPacketQueue*){assert(false&&"default packetCompletion(): completion queues cannot be RX sources");}
};
// Pinned Skywalk completeWithQueue finalizes RX and calls the recorded source
// if non-null. Completion queues inherit the panic implementation above.
void IOSkywalkPacket::completeWithQueue(IOSkywalkPacketQueue *destination,unsigned direction){
 assert(prepared&&direction==2);++rxFinalized;
 if(source)source->packetCompletion(this,destination);
 prepared=false;
}
struct IOSkywalkTxSubmissionQueue:IOSkywalkPacketQueue {
 using IOSkywalkPacketQueue::IOSkywalkPacketQueue;
 static IOSkywalkTxSubmissionQueue *withPool(IOSkywalkPacketBufferPool*p,unsigned,unsigned,OSObject*,
 unsigned(*)(OSObject*,IOSkywalkTxSubmissionQueue*,IOSkywalkPacket *const*,unsigned,void*),void*,unsigned){return ++factory==failFactory?nullptr:new IOSkywalkTxSubmissionQueue(p);}
};
struct IOSkywalkTxCompletionQueue:IOSkywalkPacketQueue {
 using IOSkywalkPacketQueue::IOSkywalkPacketQueue;
 static IOSkywalkTxCompletionQueue *withPool(IOSkywalkPacketBufferPool*p,unsigned,unsigned,OSObject*,void*,void*,unsigned){return ++factory==failFactory?nullptr:new IOSkywalkTxCompletionQueue(p);}
 IOReturn enqueuePackets(IOSkywalkPacket *const *p,unsigned count,unsigned){assert(count<128);if(rejectCompletion||!enabled)return 1;for(unsigned i=0;i<count;++i)pool->deallocatePacket(p[i]);return 0;}
};
struct IOSkywalkRxCompletionQueue:IOSkywalkPacketQueue {
 using IOSkywalkPacketQueue::IOSkywalkPacketQueue;
 static IOSkywalkRxCompletionQueue *withPool(IOSkywalkPacketBufferPool*p,unsigned,unsigned,OSObject*,void*,void*,unsigned){return ++factory==failFactory?nullptr:new IOSkywalkRxCompletionQueue(p);}
 IOReturn enqueuePackets(IOSkywalkPacket *const *p,unsigned count,unsigned){assert(count==1&&p[0]->prepared);if(rejectRX||!enabled)return 1;assert(p[0]->bytes[0]==42);p[0]->completeWithQueue(this,2);delete p[0];return 0;}
};
struct WorkLoop {
 IOReturn addEventSource(IOSkywalkPacketQueue*q){if(++add==failAdd)return 9;q->added=true;return 0;}
 void removeEventSource(IOSkywalkPacketQueue*q){assert(q->added);q->added=false;}
};
struct Timer {bool pending=false;void setTimeoutMS(unsigned){pending=true;} void cancelTimeout(){pending=false;}};
using mbuf_t=std::vector<uint8_t>*;
std::vector<uint8_t> lastFrame;
void mbuf_freem(mbuf_t m){delete m;}
uint64_t awdlNow(){return 123;}
void *bsdAddress=nullptr;
void ifnet_set_lladdr(void *bsd,const void *mac,unsigned length){assert(length==6);memcpy(bsd,mac,6);}
struct NativeProtocol:IOService {
 unsigned cachedAssists=0x37f;
 virtual unsigned getHardwareAssists(){return cachedAssists;}
 virtual void setHardwareAssists(unsigned value,unsigned mask){cachedAssists=(cachedAssists&~mask)|value;}
 virtual ifnet_t getBSDInterface(){return this;}
 virtual IOReturn prepareBSDInterface(ifnet_t,unsigned){return bsdPrepareStatus;}
 ether_addr agentMac={}; bool agentStarted=false; unsigned macDepth=0;
 virtual IOReturn setMacAddress(ether_addr&)=0;
 virtual ether_addr getSelfMacAddr(){return agentMac;}
 IOReturn setHardwareAddress(ether_addr *mac){assert(agentStarted&&++macDepth==1);if(macFail){--macDepth;return 7;}IOReturn status=setMacAddress(*mac);if(!status&&!macMismatch)agentMac=*mac;--macDepth;return status;}
 ether_addr nativeSelfMac(){return agentMac;}
 bool deferred=false,registered=false;
 void deferBSDAttach(bool value){deferred=value;if(!value){assert(registered);++publications;}}
 void *getProperty(const char*){return deferred&&!deferralFail?this:nullptr;}
 bool start(IOService*){++nativeStarts;assert(deferred);auto identity=getSelfMacAddr();assert(mtkawdl::unicast(identity.bytes));agentStarted=!superFail;return !superFail;}
 virtual IOReturn enable(unsigned options){assert(options==7);return nativeEnableStatus;}
 virtual IOReturn disable(unsigned options){assert(options==7);if(nativeDisableObserver)nativeDisableObserver();return 0;}
 void stop(IOService*){registered=false;}
};
using IO80211SkywalkInterface=NativeProtocol;
struct IOSkywalkEthernetInterface {struct RegistrationInfo {uint8_t bytes[0x130];};};
struct AirportMtkWlan;
struct AirportMtkAwdlInterface:NativeProtocol {
 AirportMtkWlan *owner=nullptr;MtkMwx *hal=nullptr;bool trafficActive=false,trafficPeerInstalled=false;uint8_t installedTrafficPeer[6]{};uint8_t discoveryTxPeers[3][6]{};struct AddbaDecline{uint8_t request[24+9];bool valid;}addbaPending[4]{};uint64_t addbaRequests=0,addbaQueued=0,addbaSubmitted=0,addbaFailed=0,addbaExpired=0,addbaDeferred=0,addbaDropped=0,delbaReceived=0;mtkawdl::DataPeers dataPeers;bool stopping=false,nativeStarted=false,nativeRegistered=false,dataPathEnabled=false,enabled=false;
 IOSkywalkPacketBufferPool *txPool=nullptr,*rxPool=nullptr;
 IOSkywalkTxSubmissionQueue *txSubmit=nullptr;IOSkywalkTxCompletionQueue *txComplete=nullptr;IOSkywalkRxCompletionQueue *rxComplete=nullptr;
 unsigned queuesAdded=0,completionCount=0,queueCount=0;IOSkywalkPacket *completions[128]={};
 struct State {uint8_t mac[6]={2,1,2,3,4,5};bool nativeElectionManaged=false;int ownSequence=9,sequence=9;void holdClock(uint64_t){}void discardControls(){}void init(const uint8_t *m,uint64_t){memcpy(mac,m,6);ownSequence=sequence=0;}} state;
 struct {bool open=false;void reset(){open=false;}} awTimeline;void notifyAvailabilityWindowEnd(uint64_t){awTimeline.open=false;}
 uint8_t nativePeerSamples[16]{};
 struct {uint8_t master[6]={};} syncState;
 void *bsd=nullptr;IOThread syncOwner=nullptr;uint64_t syncRequests=0,syncWaits=0,syncFailures=0;
 IOReturn setMacAddress(ether_addr&) override;ether_addr getSelfMacAddr() override;
 unsigned queueHead=0,requestedChannel=0,grantedChannel=0;uint64_t nextAction=0,requestDeadline=0;static constexpr unsigned kDataQueueSize=256;mbuf_t queue[kDataQueueSize]={};
 unsigned refs=1;void retain(){++refs;} void release() override {if(!--refs)delete this;} IOReturn setSync(bool);IOReturn setSyncInGate(bool);static IOReturn syncGated(OSObject*,void*,void*,void*,void*);
 void finishAvailabilityWindowForDisable(){if(awTimeline.open)notifyAvailabilityWindowEnd(awdlNow());}
 uint64_t nativeTx=0,nativeRx=0,nativeDrops=0;Timer clock;Timer *timer=&clock;
 ~AirportMtkAwdlInterface(){releaseDataPath();assert(!nativeStarted);}
 static bool nativeDataPathAvailable(){return true;}
 bool initWithController(AirportMtkWlan*c,ether_addr*,MtkMwx*h){owner=c;hal=h;return true;}
 bool attach(IOService*){return true;}void detach(IOService*){}void setInterfaceRole(unsigned){}void setInterfaceId(unsigned){}
 const char *getBSDNamePrefix(){return "awdl";}
 const char *getBSDName(){return "awdl0";}
 std::map<std::string,uint64_t> properties;
 void setProperty(const char*,bool){}void setProperty(const char*,const char*){}void setProperty(const char*,const void*,unsigned){}void setProperty(const char*k,uint64_t v,unsigned){properties[k]=v;}
 bool initRegistrationInfo(IOSkywalkEthernetInterface::RegistrationInfo*,unsigned,unsigned){return !infoFail;}
 IOReturn registerEthernetInterface(IOSkywalkEthernetInterface::RegistrationInfo *info,IOSkywalkPacketQueue**,unsigned count,IOSkywalkPacketBufferPool*,IOSkywalkPacketBufferPool*,unsigned){const char *prefix=nullptr;uint32_t unit=1;memcpy(&prefix,info->bytes+0x30,sizeof(prefix));memcpy(&unit,info->bytes+0x38,sizeof(unit));assert(!strcmp(prefix,"awdl")&&unit==0);uint32_t subfamily=0,media=0;memcpy(&subfamily,info->bytes+0x0c,4);memcpy(&media,info->bytes+0x28,4);assert(subfamily==3&&media==0x80);assert(count==3&&queuesAdded==3&&deferred);registered=!registerFail;return registerFail?7:0;}
 unsigned getHardwareAssists() override;
 void setHardwareAssists(unsigned,unsigned) override;
 void publishBSDOffloadState(ifnet_t);
 IOReturn prepareBSDInterface(ifnet_t,unsigned) override;
 bool start(IOService*);bool createDataPath();void releaseDataPath();void drainCompletions();void inputNative(const uint8_t*,size_t);void setDataPathEnabled(bool);void stop(IOService*);
 IOReturn enable(unsigned) override;IOReturn disable(unsigned) override;
 void enableDatapath();void disableDatapath();static IOReturn dataPathGated(OSObject*,void*,void*,void*,void*);
 void shutdown(){stopping=true;setDataPathEnabled(false);}
 mbuf_t makeFrame(const uint8_t*p,size_t n){return new std::vector<uint8_t>(p,p+n);}
 unsigned output(mbuf_t m){lastFrame=*m;delete m;++queueCount;return 0;}
 static unsigned submitNative(OSObject*,IOSkywalkTxSubmissionQueue*,IOSkywalkPacket *const*,unsigned,void*);
};
using IO80211VirtualInterface=AirportMtkAwdlInterface;
struct AirportMtkWlan:IOService {
 bool awdlExperimental=true,awdlCreating=false,awdlStopping=false,awdlStarted=false,awdlAttached=false,creatingAwdl=false;
 AirportMtkAwdlInterface *awdlInterface=nullptr;void *awdlBSD=nullptr;
 MtkMwx hal;IOService net;MtkMwx *fHalService=&hal;IOService *fNetIf=&net,*bsdInterface=&net;
 IOReturn awdlCreateStatus=kIOReturnNotReady;const char *stage="";WorkLoop loop;
 WorkLoop *getWorkLoop(){return &loop;}
 void recordAwdlCreation(const char*s,IOReturn r){stage=s;awdlCreateStatus=r;}
 void setProperty(const char*,uint64_t,unsigned){}void setProperty(const char*,bool){}void removeProperty(const char*){}
 void setProperty(const char*,const char*){}
 bool createAwdlInterface(ether_addr *address=nullptr);void stopAwdlInterface();
 IOReturn createAwdlInterfaceRequest(apple80211_virt_if_create_data*);
 IO80211VirtualInterface *createVirtualInterface(ether_addr*,unsigned);
};
#define super NativeProtocol
'''
tests=r'''
void reset(){assert(live==0&&outstanding==0);factory=add=nativeStarts=publications=0;failFactory=failAdd=0;resourceAvailable=true;deferralFail=superFail=infoFail=registerFail=macFail=macMismatch=false;}
void failure(){AirportMtkWlan c;assert(!c.createAwdlInterface());assert(!c.awdlInterface&&!c.awdlCreating&&!c.awdlStarted&&!c.awdlAttached);assert(publications==0&&live==0);c.stopAwdlInterface();}
IOSkywalkPacket *tx(){auto p=new IOSkywalkPacket;p->tx=p->prepared=true;return p;}
int main(){
 AirportMtkAwdlInterface offload;NativeProtocol *offloadBase=&offload;
 assert(offloadBase->getHardwareAssists()==0);
 assert(!offloadBase->prepareBSDInterface(&offload,0)&&!hwassist);
 assert(!(capabilities&(IFCAP_HWCSUM|IFCAP_TSO|IFCAP_LRO|IFCAP_CSUM_PARTIAL|IFCAP_CSUM_ZERO_INVERT|IFCAP_LRO_NUM_SEG)));
 assert(capabilities&0x400); // Preserve Skywalk support.
 assert(supported&0x400);
 assert(!(supported&(IFCAP_HWCSUM|IFCAP_TSO|IFCAP_LRO|IFCAP_CSUM_PARTIAL|IFCAP_CSUM_ZERO_INVERT|IFCAP_LRO_NUM_SEG)));
 assert(offload.cachedAssists==0);
 offloadBase->setHardwareAssists(0x37f,0x37f);assert(offload.cachedAssists==0);
 assert(offload.properties["MTKAWDLBSDPreparedOffload"]==0);
 // Model native-host activation after preparation: Skywalk software assists
 // are observed and must not be removed by diagnostics or BSD enable.
 hwassist=0x703000;capabilities|=0x6060;supported|=0x6060;
 offload.publishBSDOffloadState(&offload);
 assert(hwassist==0x703000&&offload.properties["MTKAWDLBSDCurrentOffload"]==hwassist);
 assert(offload.properties["MTKAWDLBSDPreparedOffload"]==0);
 bsdPrepareStatus=kIOReturnNotReady;hwassist=123;
 assert(offloadBase->prepareBSDInterface(&offload,0)==kIOReturnNotReady&&hwassist==123);bsdPrepareStatus=0;
 offloadError=1;assert(offloadBase->prepareBSDInterface(&offload,0)==kIOReturnError);offloadError=0;
 capsError=1;assert(offloadBase->prepareBSDInterface(&offload,0)==kIOReturnError);capsError=0;
 supportedError=1;assert(offloadBase->prepareBSDInterface(&offload,0)==kIOReturnError);supportedError=0;
 capabilities|=IFCAP_TSO;ignoreCaps=true;
 assert(offloadBase->prepareBSDInterface(&offload,0)==kIOReturnError);
 assert(offload.properties["MTKAWDLBSDOffloadStatus"]==EIO);ignoreCaps=false;
 assert(offloadBase->prepareBSDInterface(&offload,0)==0);
 for(int n=1;n<=5;++n){reset();failFactory=n;failure();}
 for(int n=1;n<=3;++n){reset();failAdd=n;failure();}
 reset();resourceAvailable=false;failure();assert(factory==0&&nativeStarts==0);
 reset();deferralFail=true;failure();assert(factory==0&&nativeStarts==0);
 reset();superFail=true;failure();
 reset();macFail=true;failure();
 reset();macMismatch=true;failure();
 reset();infoFail=true;failure();
 reset();registerFail=true;failure();
 reset();AirportMtkWlan c;assert(c.createAwdlInterface());auto p=c.awdlInterface;
 assert(publications==1&&p->nativeRegistered&&p->queuesAdded==3&&live==5);
 assert(c.createAwdlInterface()&&publications==1);
 ether_addr changed={{2,9,8,7,6,5}},invalid={{1,0,0,0,0,0}};
 uint8_t bsdMac[6]={};p->bsd=bsdMac;
 assert(p->setHardwareAddress(&changed)==0&&memcmp(p->state.mac,changed.bytes,6)==0);
 assert(memcmp(p->agentMac.bytes,changed.bytes,6)==0&&memcmp(bsdMac,changed.bytes,6)==0);
 assert(p->state.sequence==9&&p->state.ownSequence==9);
 assert(p->setMacAddress(invalid)==kIOReturnBadArgument);
 p->enabled=true;assert(p->setMacAddress(changed)==kIOReturnBusy);p->enabled=false;
 p->stopping=true;assert(p->setMacAddress(changed)==kIOReturnNotReady);p->stopping=false;
 p->bsd=nullptr;p->enabled=true;
 // BSD's native enable entry must activate the queues; base enable does not.
 NativeProtocol *bsdProvider=p;
 assert(!p->dataPathEnabled&&!p->txSubmit->enabled&&!p->rxComplete->enabled);
 nativeEnableStatus=kIOReturnError;
 assert(bsdProvider->enable(7)==kIOReturnError&&!p->dataPathEnabled);
 nativeEnableStatus=0;
 p->nativeRegistered=false;assert(bsdProvider->enable(7)==kIOReturnNotReady);p->nativeRegistered=true;
 p->queuesAdded=2;assert(bsdProvider->enable(7)==kIOReturnNotReady);p->queuesAdded=3;
 _fCommandGate=nullptr;assert(bsdProvider->enable(7)==kIOReturnNotReady&&!p->dataPathEnabled);_fCommandGate=&gate;
 p->stopping=true;assert(bsdProvider->enable(7)==kIOReturnNotReady);p->stopping=false;
 assert(bsdProvider->enable(7)==0&&p->dataPathEnabled&&p->txSubmit->enabled&&p->txComplete->enabled&&p->rxComplete->enabled);
 assert(bsdProvider->enable(7)==0); // Repeated enable remains usable.
 auto packet=tx();unsigned consumed=p->submitNative(p,p->txSubmit,&packet,1,nullptr);
 assert(consumed==1&&p->nativeTx==1&&p->completionCount==1&&outstanding==0&&p->timer->pending);
 outstanding+=consumed;p->drainCompletions();assert(outstanding==0&&p->completionCount==0);
 // Native virtual address is the buffer base; data offset is applied once.
 packet=tx();packet->offset=37;packet->bytes[37]=42;packet->bytes[100]=91;
 consumed=p->submitNative(p,p->txSubmit,&packet,1,nullptr);outstanding+=consumed;
 assert(consumed==1&&lastFrame.size()==64&&lastFrame.front()==42&&lastFrame.back()==91);
 p->drainCompletions();assert(outstanding==0);
 // A full deferred buffer applies backpressure without disposing packets.
 std::vector<IOSkywalkPacket*> packets;for(unsigned i=0;i<129;++i)packets.push_back(tx());
 consumed=p->submitNative(p,p->txSubmit,packets.data(),129,nullptr);assert(consumed==128&&p->completionCount==128);
 outstanding+=consumed;delete packets[128];p->drainCompletions();assert(outstanding==0);
 // Disabled/protocol-full TX is consumed and completed as a drop.
 p->enabled=false;packet=tx();consumed=p->submitNative(p,p->txSubmit,&packet,1,nullptr);outstanding+=consumed;
 rejectCompletion=true;p->drainCompletions();rejectCompletion=false;assert(outstanding==0);
 p->enabled=true;packet=tx();consumed=p->submitNative(p,p->txSubmit,&packet,1,nullptr);outstanding+=consumed;
 nativeDisableObserver=[&](){assert(!p->dataPathEnabled&&!p->txSubmit->enabled&&!p->rxComplete->enabled&&p->completionCount==0&&outstanding==0);};
 assert(bsdProvider->disable(7)==0);assert(p->completionCount==0&&outstanding==0&&!p->dataPathEnabled&&!p->txSubmit->enabled&&!p->txComplete->enabled&&!p->rxComplete->enabled);
 uint8_t disabledData[64]={42};auto rxBefore=p->nativeRx;p->inputNative(disabledData,64);assert(p->nativeRx==rxBefore);
 assert(bsdProvider->disable(7)==0&&bsdProvider->enable(7)==0&&p->dataPathEnabled);
 nativeDisableObserver=nullptr;
 uint8_t data[1514]={42};int before=live;
 unsigned finalizedBefore=rxFinalized,reclaimedBefore=rxReclaimed;
 p->inputNative(data,64);assert(live==before&&p->nativeRx==1);
 assert(rxFinalized==finalizedBefore+1&&rxReclaimed==reclaimedBefore);
 rejectRX=true;p->inputNative(data,64);rejectRX=false;assert(live==before&&p->nativeRx==1);
 prepareFail=true;p->inputNative(data,64);prepareFail=false;assert(live==before);
 allocateFail=true;p->inputNative(data,64);allocateFail=false;assert(live==before);
 for(bool *fault:{&badRXBuffers,&badRXOffset,&badRXLength,&noRXAddress}){
  *fault=true;p->inputNative(data,64);*fault=false;assert(live==before&&p->nativeRx==1);
 }
 p->rxComplete->disable();p->inputNative(data,64);p->rxComplete->enable();assert(live==before&&p->nativeRx==1);
 assert(rxReclaimed==reclaimedBefore+7&&rxPreparedCompleted==7);
 assert(rxFinalized==finalizedBefore+8);
 p->inputNative(nullptr,64);p->inputNative(data,1515);p->inputNative(data,13);assert(live==before);
 // Firmware wait allows gated submissions between first drain and timer cancel.
 c.hal.wait=[&](){auto packet=tx();unsigned n=p->submitNative(p,p->txSubmit,&packet,1,nullptr);outstanding+=n;assert(p->completionCount==1);};
 assert(p->setSync(false)==0);assert(p->completionCount==0&&outstanding==0&&!p->timer->pending);
 c.hal.wait=nullptr;
 c.stopAwdlInterface();c.stopAwdlInterface();assert(live==0&&outstanding==0);
 reset();c.awdlExperimental=false;assert(!c.createAwdlInterface()&&factory==0);
 c.awdlExperimental=true;c.hal.ready=false;c.hal.ic.ic_state=0;
 assert(!c.createAwdlInterface()&&!c.awdlInterface&&factory==0);
 assert(!strcmp(c.stage,"firmware-not-ready"));c.hal.ready=true;
 c.hal.ic.ic_state=IEEE80211_S_RUN;c.hal.channels=false;
 assert(!c.createAwdlInterface()&&!c.awdlInterface&&factory==0&&!strcmp(c.stage,"no-social-channel"));
 c.hal.channels=true;
 c.awdlExperimental=true;c.fNetIf=nullptr;assert(!c.createAwdlInterface()&&factory==0);
 // Automatic native role-6 requests must work before any AP association.
 for(int state=0;state<=IEEE80211_S_RUN;++state){
  reset();AirportMtkWlan automatic;automatic.hal.ic.ic_state=state;
  apple80211_virt_if_create_data request;request.role=5;
  assert(automatic.createAwdlInterfaceRequest(&request)==kIOReturnUnsupported&&factory==0);
  request.role=6;request.version=2;
  assert(automatic.createAwdlInterfaceRequest(&request)==kIOReturnBadArgument&&factory==0);
  request.version=1;request.mac[0]=1;
  assert(automatic.createAwdlInterfaceRequest(&request)==kIOReturnBadArgument&&factory==0);
  request.mac[0]=2;automatic.hal.ready=false;
  assert(automatic.createAwdlInterfaceRequest(&request)==kIOReturnNotReady&&factory==0);
  automatic.hal.ready=true;
  assert(automatic.createAwdlInterfaceRequest(&request)==0);
  auto iface=automatic.awdlInterface;
  assert(iface&&publications==1&&!strcmp(request.bsd_name,"awdl0"));
  assert(automatic.hal.ic.ic_state==state); // No reset/association as a side effect.
  automatic.hal.ic.ic_state=0;automatic.hal.ready=false;
  assert(automatic.createAwdlInterfaceRequest(&request)==EEXIST);
  assert(automatic.createAwdlInterface()&&automatic.awdlInterface==iface&&publications==1);
  assert(!automatic.createVirtualInterface(nullptr,5));
  assert(automatic.createVirtualInterface(nullptr,6)==iface&&publications==1);
  automatic.stopAwdlInterface();assert(live==0);
 }
 reset();AirportMtkWlan legacy;legacy.hal.ic.ic_state=1;
 assert(legacy.createVirtualInterface(nullptr,6)==legacy.awdlInterface&&publications==1);
 legacy.stopAwdlInterface();assert(live==0);
 puts("PASS native backend: allocation/add/start/MAC/registration failures; Wi-Fi subfamily/media; nonrecursive MAC agent callback and startup identity; deferred BSD publication; BSD enable/disable/re-enable queue activation and failure guards; TX data offset, deferred completion, full buffer, drop/disable/fallback; RX ownership transfer and error cleanup.");
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-native-backend-') as temp:
    cpp=Path(temp)/'test.cpp';binary=Path(temp)/'test'
    cpp.write_text(stubs+functions+tests)
    subprocess.run(['clang++','-std=c++14','-Wall','-Wextra','-fsanitize=address,undefined',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=15)
