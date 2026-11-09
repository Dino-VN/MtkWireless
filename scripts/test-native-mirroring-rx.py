#!/usr/bin/env python3
"""Run production canonical/native RX with the real Rust FFI under ASan/UBSan.

Canonical preflight is ALLOCATION ONLY: error/NULL and success/NULL allocation
must not consume replay. Preparation/decode/enqueue faults intentionally do.
The packet/pool/queue boundaries below are explicit test models, not evidence
of private Skywalk allocation, preparation, transfer or finalize ownership.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
base = root / 'kext/Wifi/vendor/AirportMtkWlan/AirportMtkAwdlInterface'
source = base.with_suffix('.cpp').read_text()
header = base.with_suffix('.hpp').read_text()


def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


methods = [function(signature) for signature in (
    'void AirportMtkAwdlInterface::invalidateDataRxAdmission(',
    'void AirportMtkAwdlInterface::resetDataPeers(',
    'int AirportMtkAwdlInterface::observeDataPeer(',
    'bool AirportMtkAwdlInterface::cacheDataPeer(',
    'bool AirportMtkAwdlInterface::trafficDataPeer(',
    'IOReturn AirportMtkAwdlInterface::programDataPeer(',
    'AirportMtkAwdlInterface::NativeRxAllocation::NativeRxAllocation(',
    'AirportMtkAwdlInterface::NativeRxAllocation::~NativeRxAllocation(',
    'int AirportMtkAwdlInterface::NativeRxAllocation::currentPeer(',
    'bool AirportMtkAwdlInterface::admitDataRx(',
    'uint64_t AirportMtkAwdlInterface::inputCanonicalView(',
    'bool AirportMtkAwdlInterface::deliverNativeView(',
    'bool AirportMtkAwdlInterface::inputNativeView(',
    'void AirportMtkAwdlInterface::recordDataRx(',
    'void AirportMtkAwdlInterface::recordInvalidDataRx(',
    'bool AirportMtkAwdlInterface::createDataPath(',
    'void AirportMtkAwdlInterface::releaseDataPath(',
    'IOReturn AirportMtkAwdlInterface::releaseDataPathGated(',
    'void AirportMtkAwdlInterface::releaseDataPathInGate(',
    'IOReturn AirportMtkAwdlInterface::dataPathGated(',
    'void AirportMtkAwdlInterface::enableDatapath(',
    'void AirportMtkAwdlInterface::disableDatapath(',
    'void AirportMtkAwdlInterface::setDataPathEnabled(',
    'IOReturn AirportMtkAwdlInterface::enable(unsigned',
    'IOReturn AirportMtkAwdlInterface::enableGated(',
    'IOReturn AirportMtkAwdlInterface::enableInGate(',
    'IOReturn AirportMtkAwdlInterface::disable(unsigned',
    'IOReturn AirportMtkAwdlInterface::disableGated(',
    'IOReturn AirportMtkAwdlInterface::disableInGate(',
    'void AirportMtkAwdlInterface::shutdown(',
    'IOReturn AirportMtkAwdlInterface::shutdownGated(',
    'void AirportMtkAwdlInterface::shutdownInGate(',
    'void AirportMtkAwdlInterface::stop(',
    'IOReturn AirportMtkAwdlInterface::stopGated(',
    'void AirportMtkAwdlInterface::stopInGate(',
    'IOReturn AirportMtkAwdlInterface::setMacAddress(',
    'IOReturn AirportMtkAwdlInterface::macAddressGated(',
    'IOReturn AirportMtkAwdlInterface::setMacAddressInGate(',
    'IOReturn AirportMtkAwdlInterface::setInterfaceEnable(',
    'IOReturn AirportMtkAwdlInterface::interfaceEnableGated(',
    'IOReturn AirportMtkAwdlInterface::setInterfaceEnableInGate(',
    'bool AirportMtkAwdlInterface::setLinkState(',
    'IOReturn AirportMtkAwdlInterface::linkStateGated(',
    'bool AirportMtkAwdlInterface::setLinkStateInGate(',
    'IOReturn AirportMtkAwdlInterface::setSync(bool',
    'IOReturn AirportMtkAwdlInterface::syncGated(',
    'void AirportMtkAwdlInterface::finishAvailabilityWindowForDisable(',
    'IOReturn AirportMtkAwdlInterface::setSyncInGate(',
    'IOReturn AirportMtkAwdlInterface::recordCacheControlResult(',
    'IOReturn AirportMtkAwdlInterface::setPEER_CACHE_CONTROL(',
    'IOReturn AirportMtkAwdlInterface::peerCacheGated(',
    'IOReturn AirportMtkAwdlInterface::setPeerCacheInGate(',
    'IOReturn AirportMtkAwdlInterface::setAWDL_PEER_TRAFFIC_REGISTRATION(',
    'IOReturn AirportMtkAwdlInterface::peerTrafficGated(',
    'IOReturn AirportMtkAwdlInterface::setPeerTrafficInGate(',
)]
start = header.index('    struct NativeRxAllocation {')
allocation_fields = header[start:header.index('    static unsigned submitNative(', start)]
start = header.index('    struct __attribute__((packed)) DataRxSample {')
trace_fields = header[start:header.index('    IOReturn dataTxStatus', start)]
start = header.index('    struct CacheDiagnostic {')
cache_fields = header[start:header.index('    CacheDiagnostic cacheDiagnostic;', start)
                     + len('    CacheDiagnostic cacheDiagnostic;')]
# Keep the declarations coupled to the extracted production definitions. The
# allocation/trace declarations themselves are taken from the real header.
declarations = []
for method in methods:
    signature = method[:method.index('\n{')].strip()
    if '::NativeRxAllocation::' in signature:
        continue
    signature = signature.replace('AirportMtkAwdlInterface::', '', 1)
    name = re.search(r'(\w+)\(', signature).group(1)
    if name in {'admitDataRx', 'inputCanonicalView', 'deliverNativeView',
                'recordDataRx', 'recordInvalidDataRx'}:
        continue
    if name.endswith('Gated'):
        signature = 'static ' + signature
    declarations.append(signature + ';')
# No authoritative table mutation or station programming may bypass the
# invalidation wrappers, including advertisement refreshes and rejections.
for operation in ('reset', 'observe', 'setCached', 'setTraffic', 'acceptData'):
    assert len(re.findall(r'\bdataPeers\.' + operation + r'\(', source)) == 1, operation
assert len(re.findall(r'hal->awdlPeer\(', source)) == 1
receive = function('bool AirportMtkAwdlInterface::receive(')
prefix = receive[:receive.index('    if(f[0]==0xd0)')]
data_arm = receive[receive.index('    ++self->dataRxFrames;'):]
host = Path(os.environ.get('MTK_RX_HOST_TARGET', root / 'build/rust-host'))
subprocess.run(['cargo', 'build', '--offline', '-q', '-p', 'mtk-ffi', '--features', 'wifi',
                '--target-dir', str(host)], cwd=root / 'rust', check=True)
code = r'''
#define KERNEL 1
#include <AWDL/AwdlLayout.hpp>
#include <AWDL/AwdlTimeline.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <vector>
using IOReturn=unsigned;
constexpr IOReturn kIOReturnSuccess=0,kIOReturnError=1,kIOReturnNotReady=2,
    kIOReturnBadArgument=3,kIOReturnBusy=4,kIOReturnNoResources=5,
    kIOReturnUnsupported=6,kIOReturnTimeout=7,kIOReturnNoMemory=8;
constexpr unsigned APPLE80211_VERSION=1,THREAD_UNINT=0,THREAD_AWAKENED=0,
    THREAD_TIMED_OUT=9,kMillisecondScale=1;
using IOThread=void*;using AbsoluteTime=uint64_t;using ifnet_t=void*;using mbuf_t=void*;
struct ether_addr { uint8_t bytes[6]; };
enum IO80211LinkState { kIO80211NetworkLinkDown=0,kIO80211NetworkLinkUp=1 };
enum class Fault { None, Allocate, AllocateNull, Prepare, Buffers, Offset, Length, Address, Decode, Enqueue };
Fault fault=Fault::None;
unsigned livePackets=0,livePools=0,liveQueues=0,nextPoolID=0,preparedPackets=0;
uint64_t clockNow=100;
uint64_t awdlNow(){return clockNow;}
IOThread IOThreadSelf(){return reinterpret_cast<void*>(uintptr_t(1));}
void clock_interval_to_deadline(unsigned duration,unsigned,AbsoluteTime *wake){*wake=clockNow+duration*1000;}
void mbuf_freem(mbuf_t p){assert(p);}
void invoke(std::function<void()> &hook){auto call=std::move(hook);hook=nullptr;if(call)call();}
std::function<void()> allocationHook,prepareHook,addressHook,enqueueHook,nativeHook;
#define XYLog(...) ((void)0)
struct OSObject {
    unsigned refs=1;
    virtual ~OSObject(){assert(refs==0 || refs==1);}
    void retain(){assert(refs);++refs;}
    void release(){assert(refs);if(!--refs)delete this;}
};
#define OSSafeReleaseNULL(p) do { if(p) { (p)->release();(p)=nullptr; } } while(0)
struct IOSkywalkPacketQueue;
struct WorkLoop : OSObject {
    unsigned depth=0,adds=0,removes=0;
    bool inGate() const{return depth!=0;}
    IOReturn addEventSource(OSObject*){assert(inGate());++adds;return 0;}
    void removeEventSource(OSObject*){assert(inGate());++removes;}
};
WorkLoop controllerLoop;
struct GateScope { GateScope(){++controllerLoop.depth;}~GateScope(){--controllerLoop.depth;} };
struct IOCommandGate {
    using Action=IOReturn(*)(OSObject*,void*,void*,void*,void*);
    unsigned calls=0;
    WorkLoop *getWorkLoop(){return &controllerLoop;}
    IOReturn runAction(Action action,void *a=nullptr,void *b=nullptr,void *c=nullptr,void *d=nullptr){
        ++calls;GateScope gate;return action(nullptr,a,b,c,d);
    }
    IOReturn commandSleep(void*,AbsoluteTime wake,unsigned){clockNow=wake;return THREAD_TIMED_OUT;}
    void commandWakeup(void*,bool){}
} commandGate;
IOCommandGate *_fCommandGate=&commandGate;
struct OSDictionary : OSObject {};
struct IOService : OSObject {
    static OSDictionary *resourceMatching(const char*){return new OSDictionary;}
    static IOService *copyMatchingService(OSDictionary*){return new IOService;}
};
struct AirportMtkWlan : IOService {
    WorkLoop *getWorkLoop(){return &controllerLoop;}
    void recordAwdlCreation(const char*,IOReturn){assert(controllerLoop.inGate());}
};
struct IOTimerEventSource : OSObject {
    unsigned ticks=0;
    void setTimeoutMS(unsigned){assert(controllerLoop.inGate());++ticks;}
    void cancelTimeout(){assert(controllerLoop.inGate());}
    void disable(){assert(controllerLoop.inGate());}
};
struct PoolStats { unsigned id=++nextPoolID,allocations=0,reclaims=0,deletes=0; };
struct QueueStats {
    unsigned calls=0,accepted=0,deletes=0;
    std::vector<uint8_t> decoded;
    std::vector<std::vector<uint8_t>> frames;
};
struct IOSkywalkPacket {
    std::vector<uint8_t> bytes=std::vector<uint8_t>(2048);
    unsigned length=0,origin;bool prepared=false;
    explicit IOSkywalkPacket(unsigned id):origin(id){++livePackets;}
    ~IOSkywalkPacket(){--livePackets;}
    IOReturn prepareWithQueue(void *source,unsigned direction,unsigned flags){
        assert(!source && direction==2 && flags==0 && controllerLoop.inGate());
        prepared=true;++preparedPackets;invoke(prepareHook);
        return fault==Fault::Prepare?kIOReturnError:kIOReturnSuccess;
    }
    unsigned getPacketBufferCount(){assert(prepared);return fault==Fault::Buffers?2:1;}
    IOReturn setDataOffset(unsigned offset){assert(prepared && offset==0);return fault==Fault::Offset?1:0;}
    IOReturn setDataLength(unsigned value){
        assert(prepared && value<=bytes.size());length=value;return fault==Fault::Length?1:0;
    }
    void *getDataVirtualAddress(){
        assert(prepared);invoke(addressHook);return fault==Fault::Address?nullptr:bytes.data();
    }
};
struct IOSkywalkPacketBufferPool : OSObject {
    struct PoolOptions {unsigned a,b,c,d,e,f,g;};
    std::shared_ptr<PoolStats> stats=std::make_shared<PoolStats>();
    std::deque<Fault> results;
    IOSkywalkPacketBufferPool(){++livePools;}
    ~IOSkywalkPacketBufferPool(){++stats->deletes;--livePools;}
    static IOSkywalkPacketBufferPool *withName(const char*,OSObject*,unsigned,PoolOptions*){
        assert(controllerLoop.inGate());return new IOSkywalkPacketBufferPool;
    }
    IOReturn allocatePacket(IOSkywalkPacket **packet,unsigned flags){
        assert(flags==1 && !*packet && controllerLoop.inGate());++stats->allocations;
        Fault result=fault;
        if(!results.empty()){result=results.front();results.pop_front();}
        // Reentry happens inside allocation, before any packet is returned.
        invoke(allocationHook);
        if(result==Fault::Allocate)return kIOReturnError; // error/NULL only
        if(result==Fault::AllocateNull)return kIOReturnSuccess; // success/NULL
        *packet=new IOSkywalkPacket(stats->id);return kIOReturnSuccess;
    }
    void deallocatePacket(IOSkywalkPacket *packet){
        assert(packet && packet->origin==stats->id && controllerLoop.inGate());
        ++stats->reclaims;delete packet;
    }
};
struct IOSkywalkPacketQueue : OSObject {
    unsigned origin;bool active=false;
    std::shared_ptr<QueueStats> stats=std::make_shared<QueueStats>();
    explicit IOSkywalkPacketQueue(IOSkywalkPacketBufferPool *p):origin(p->stats->id){++liveQueues;}
    ~IOSkywalkPacketQueue(){++stats->deletes;--liveQueues;}
    void enable(){assert(controllerLoop.inGate());active=true;}
    void disable(){assert(controllerLoop.inGate());active=false;}
};
struct IOSkywalkTxSubmissionQueue : IOSkywalkPacketQueue {
    using IOSkywalkPacketQueue::IOSkywalkPacketQueue;
    template<class... A> static IOSkywalkTxSubmissionQueue *withPool(IOSkywalkPacketBufferPool *p,A...){
        return new IOSkywalkTxSubmissionQueue(p);
    }
};
struct IOSkywalkTxCompletionQueue : IOSkywalkPacketQueue {
    using IOSkywalkPacketQueue::IOSkywalkPacketQueue;
    template<class... A> static IOSkywalkTxCompletionQueue *withPool(IOSkywalkPacketBufferPool *p,A...){
        return new IOSkywalkTxCompletionQueue(p);
    }
};
struct IOSkywalkRxCompletionQueue : IOSkywalkPacketQueue {
    using IOSkywalkPacketQueue::IOSkywalkPacketQueue;
    template<class... A> static IOSkywalkRxCompletionQueue *withPool(IOSkywalkPacketBufferPool *p,A...){
        return new IOSkywalkRxCompletionQueue(p);
    }
    IOReturn enqueuePackets(IOSkywalkPacket *const *packets,unsigned count,unsigned flags){
        assert(count==1 && flags==0 && packets[0]->prepared && packets[0]->origin==origin);
        ++stats->calls;invoke(enqueueHook);
        // This model rejects before transfer and consumes only on success.
        // It does not establish the private native contract on either branch.
        if(fault==Fault::Enqueue)return kIOReturnError;
        auto packet=packets[0];
        stats->decoded.assign(packet->bytes.begin(),packet->bytes.begin()+packet->length);
        stats->frames.push_back(stats->decoded);
        ++stats->accepted;delete packet;return kIOReturnSuccess;
    }
};
struct MtkMwx {
    unsigned peerCalls=0,enables=0,wakes=0;bool firmwareEnabled=true;
    IOReturn peerResult=kIOReturnSuccess;
    std::function<void()> peerHook,enableHook;
    bool awdlChannelAllowed(unsigned) const{return true;}
    bool awdlIsEnabled() const{return firmwareEnabled;}
    bool awdlGrantSnapshot(mtkawdl::Reservation&){return false;}
    void awdlSetReceiver(void*,bool(*)(void*,const uint8_t*,size_t,uint8_t,int8_t)){}
    IOReturn awdlEnable(const uint8_t*,bool value){
        assert(controllerLoop.inGate());++enables;invoke(enableHook);firmwareEnabled=value;return 0;
    }
    uint64_t awdlTxSession(){return 123;}
    void awdlInfraWakeIfIdle(){assert(controllerLoop.inGate());++wakes;}
    IOReturn awdlPeer(const uint8_t*,bool){
        assert(controllerLoop.inGate());++peerCalls;invoke(peerHook);return peerResult;
    }
};
struct apple80211_peer_cache_control { uint8_t bytes[0x1d0]{}; };
struct apple80211_awdl_peer_traffic_registration {
    unsigned version=1,active=0,name_len=0;uint8_t name[64]{},addr[6]{};
};
struct NativeInterface : IOService {
    IOReturn enable(unsigned){assert(controllerLoop.inGate());invoke(nativeHook);return 0;}
    IOReturn disable(unsigned){assert(controllerLoop.inGate());invoke(nativeHook);return 0;}
    IOReturn setInterfaceEnable(bool){assert(controllerLoop.inGate());invoke(nativeHook);return 0;}
    bool setLinkState(IO80211LinkState,unsigned){assert(controllerLoop.inGate());invoke(nativeHook);return true;}
    void stop(IOService*){assert(controllerLoop.inGate());invoke(nativeHook);}
};
#define super NativeInterface
struct AirportMtkAwdlInterface;
IOReturn awdlControllerMessage(AirportMtkWlan*,AirportMtkAwdlInterface*,unsigned m,void*,unsigned long n,bool sync){
    assert(controllerLoop.inGate() && m==0x33 && n==0x1d0 && sync);return 0;
}
int ifnet_set_lladdr(ifnet_t,const uint8_t*,unsigned){return 0;}
struct AirportMtkAwdlInterface : NativeInterface {
    bool enabled=true,stopping=false,dataPathEnabled=false;
    bool nativeStarted=true,nativeRegistered=true,timerAdded=true,receiverRegistered=true;
    mtkawdl::State state={};mtkawdl::DataPeers dataPeers;
    uint64_t dataRxAdmissionGeneration=1;unsigned dataRxPeerProgramming=0;
    IOThread syncOwner=nullptr;
    uint64_t syncRequests=0,syncWaits=0,syncFailures=0,txSession=123;
    AirportMtkWlan controller;AirportMtkWlan *owner=&controller;
    MtkMwx radio;MtkMwx *hal=&radio;
    IOTimerEventSource *timer=new IOTimerEventSource;
    IOSkywalkPacketBufferPool *txPool=nullptr,*rxPool=nullptr;
    IOSkywalkTxSubmissionQueue *txSubmit=nullptr;
    IOSkywalkTxCompletionQueue *txComplete=nullptr;
    IOSkywalkRxCompletionQueue *rxComplete=nullptr;
    unsigned queuesAdded=0,completionCount=0;
    IOSkywalkPacket *completions[128]{};
    std::shared_ptr<PoolStats> pool;
    std::shared_ptr<QueueStats> completion;
    uint64_t dataRxFrames=0,dataRxInvalid=0,dataRxUnknownPeer=0;
    uint64_t dataRxWrongDestination=0,dataRxReplay=0,dataRxDecoded=0;
    uint64_t nativeRx=0,nativeDrops=0;
    uint8_t discoveryTxPeers[3][6]{},addbaPending[4][33]{},nativePeerSamples[16][8]{};
    uint8_t trafficPeer[6]{},installedTrafficPeer[6]{};
    bool trafficActive=false,trafficPeerInstalled=false;
    struct {uint8_t master[6]{};} syncState;
    ifnet_t bsd=nullptr;
    mtkawdl::Timeline awTimeline;
    static constexpr unsigned kDataQueueSize=256;
    mbuf_t queue[kDataQueueSize]{};unsigned queueHead=0,queueCount=0;
    uint8_t requestedChannel=0,grantedChannel=0;
    uint64_t requestedDeadline=0,requestDeadline=0,nextAction=0;
''' + allocation_fields + trace_fields + cache_fields + '\n'.join(declarations) + r'''
    static bool receive(void*,const uint8_t*,size_t,uint8_t,int8_t);
    static unsigned submitNative(OSObject*,IOSkywalkTxSubmissionQueue*,IOSkywalkPacket *const*,unsigned,void*){return 0;}
    void drainCompletions(){assert(controllerLoop.inGate() && !completionCount);}
    void drainTxStatus(){assert(controllerLoop.inGate());} // unrelated HAL polling excluded
    void notifyAvailabilityWindowEnd(uint64_t){assert(controllerLoop.inGate());awTimeline.open=false;}
    void publishBSDOffloadState(ifnet_t){assert(controllerLoop.inGate());}
    ifnet_t getBSDInterface(){return nullptr;}
    IOReturn reportLinkStatus(unsigned,unsigned){assert(controllerLoop.inGate());return 0;}
    template<class... A> void setProperty(const char*,A...){}
    AirportMtkAwdlInterface(){
        GateScope gate;clockNow=100;
        const uint8_t own[]={2,1,2,3,4,5};state.init(own,100);resetDataPeers();
        assert(createDataPath());setDataPathEnabled(true);
        pool=rxPool->stats;completion=rxComplete->stats;
    }
    ~AirportMtkAwdlInterface(){
        GateScope gate;releaseDataPathInGate();OSSafeReleaseNULL(timer);
        assert(refs==1 && !completionCount && !livePackets);
    }
    void authorize(const std::vector<uint8_t> &frame){
        GateScope gate;assert(cacheDataPeer(frame.data()+10,true,100));
    }
    void refresh(const std::vector<uint8_t> &frame,uint64_t now){
        GateScope gate;assert(observeDataPeer(frame.data()+10,now)>=0);
    }
    IOReturn cache(const uint8_t *peer,unsigned operation){
        apple80211_peer_cache_control request;mtkawdl::put32(request.bytes,1);
        mtkawdl::put32(request.bytes+4,operation);memcpy(request.bytes+8,peer,6);
        return setPEER_CACHE_CONTROL(&request);
    }
    IOReturn traffic(const uint8_t *peer,bool active){
        apple80211_awdl_peer_traffic_registration request;
        memcpy(request.addr,peer,6);request.active=active;
        return setAWDL_PEER_TRAFFIC_REGISTRATION(&request);
    }
    bool ingest(const std::vector<uint8_t> &frame,uint64_t now){
        // Match the production HAL's controller-workloop receive invocation.
        GateScope gate;clockNow=now;return receive(this,frame.data(),frame.size(),149,-35);
    }
};
''' + '\n'.join(methods) + prefix + data_arm + r'''
const uint8_t ownMac[6]={2,1,2,3,4,5},peerMac[6]={2,1,2,3,4,6};
std::vector<uint8_t> canonical(uint16_t sequence,unsigned stream=5,const uint8_t *destination=ownMac,size_t size=96){
    std::vector<uint8_t> eth(size,0),wire(size+26,0);
    memcpy(eth.data(),destination,6);memcpy(eth.data()+6,peerMac,6);
    eth[12]=0x86;eth[13]=0xdd;eth[14]=0x60;
    eth[18]=uint8_t((size-54)>>8);eth[19]=uint8_t(size-54);
    assert(mtkawdl::encodeData(wire.data(),wire.size(),eth.data(),eth.size(),sequence)==wire.size());
    if(stream!=16){wire[0]=0x88;wire.insert(wire.begin()+24,{uint8_t(stream),0});}
    assert(mtkawdl::validDataFrame(wire.data(),wire.size()));return wire;
}
std::vector<uint8_t> aggregate(const std::vector<std::vector<uint8_t>> &frames,bool envelope=false){
    assert(!frames.empty() && frames[0][0]==0x88);
    std::vector<uint8_t> out(frames[0].begin(),frames[0].begin()+26);out[24]|=0x80;
    for(size_t i=0;i<frames.size();++i){
        const auto &frame=frames[i];assert(frame[0]==0x88);
        if(i)out.insert(out.end(),(4-(out.size()-26)%4)%4,0);
        out.insert(out.end(),frame.begin()+4,frame.begin()+16);
        const auto length=frame.size()-26+(envelope?8:0);
        out.push_back(uint8_t(length>>8));out.push_back(uint8_t(length));
        if(envelope){
            const uint8_t llc[6]={0xaa,0xaa,3,0,0,0};out.insert(out.end(),llc,llc+6);
            out.push_back(uint8_t((frame.size()-26)>>8));out.push_back(uint8_t(frame.size()-26));
        }
        out.insert(out.end(),frame.begin()+26,frame.end());
    }
    assert(mtkawdl::validDataFrame(out.data(),out.size()));return out;
}
mtkawdl::DataReplayWindow window(const AirportMtkAwdlInterface &rx,unsigned stream=5){
    return rx.dataPeers.entries[0].windows[stream];
}
void sameWindow(const mtkawdl::DataReplayWindow &a,const mtkawdl::DataReplayWindow &b){
    assert(a.bitmap==b.bitmap && a.lastRx==b.lastRx && a.sequence==b.sequence &&
           a.active==b.active && a.sequenceBits==b.sequenceBits);
}
void unchanged(const mtkawdl::DataPeers &a,const mtkawdl::DataPeers &b){
    // Host-only fixture snapshot. Production never snapshots this table.
    assert(!memcmp(&a,&b,sizeof(a)));
}
void pureFFITests(){
    assert(sizeof(mtkawdl::DataReplayWindow)==24 && mtkawdl::layoutMismatch()==-1);
    mtkawdl::DataPeers peers;peers.reset();assert(peers.setCached(peerMac,true,100));
    assert(peers.acceptData(0,5,42,100));const auto before=peers;
    assert(!mtk_awdl_data_peers_probe(nullptr,0,5,42,101));
    for(auto bad:{size_t(mtkawdl::maxPeers),SIZE_MAX})assert(!peers.probeData(bad,5,43,101));
    for(auto bad:{size_t(17),SIZE_MAX})assert(!peers.probeData(0,bad,43,101));
    assert(!peers.probeData(0,5,42,101) && peers.probeData(0,5,43,101));
    assert(peers.probeData(0,5,43,101));unchanged(peers,before);
    puts("PASS: real Rust FFI pure probe, null/bounds and shared layout");
}
void canonicalAllocationTests(){
    auto frame=canonical(42);
    for(Fault failure:{Fault::Allocate,Fault::AllocateNull}){
        AirportMtkAwdlInterface rx;rx.authorize(frame);
        const auto before=rx.dataPeers;const auto generation=rx.dataRxAdmissionGeneration;
        fault=failure;assert(rx.ingest(frame,100));
        unchanged(rx.dataPeers,before);
        assert(rx.dataRxAdmissionGeneration==generation && !rx.dataRxTraceCount);
        assert(rx.nativeDrops==1 && !rx.nativeRx && !rx.dataRxDecoded && rx.pool->allocations==1);
        assert(!rx.pool->reclaims && !livePackets && !rx.completion->calls && !preparedPackets);
        fault=Fault::None;assert(rx.ingest(frame,101));
        assert(rx.nativeRx==1 && rx.dataRxDecoded==1 && window(rx).lastRx==101);
        const auto allocations=rx.pool->allocations;assert(rx.ingest(frame,102));
        assert(rx.pool->allocations==allocations && rx.nativeRx==1 && rx.dataRxReplay==1);
        assert(window(rx).lastRx==102 && rx.dataRxTraceCount==2 && rx.dataRxDuplicate==1);
        // LastRx refresh and trace stay ordinary even when no pool is ready.
        rx.disableDatapath();assert(rx.ingest(frame,103));
        assert(rx.pool->allocations==allocations && window(rx).lastRx==103);
        assert(rx.dataRxReplay==2 && rx.dataRxLastRejected.timeUS==103);
        preparedPackets=0;
    }
    for(unsigned unavailable=0;unavailable<4;++unavailable){
        AirportMtkAwdlInterface rx;rx.authorize(frame);const auto before=rx.dataPeers;
        auto p=rx.rxPool;auto q=rx.rxComplete;
        if(unavailable==0)rx.disableDatapath();
        if(unavailable==1)rx.rxPool=nullptr;
        if(unavailable==2)rx.rxComplete=nullptr;
        if(unavailable==3)rx.nativeRegistered=false;
        assert(rx.ingest(frame,100));unchanged(rx.dataPeers,before);assert(!rx.pool->allocations);
        rx.rxPool=p;rx.rxComplete=q;rx.nativeRegistered=true;rx.enableDatapath();
        assert(rx.ingest(frame,101) && rx.nativeRx==1);
    }
    puts("PASS: canonical error/NULL and success/NULL allocation, identical retry once, ordinary duplicate refresh");
}
void canonicalConsumedFaultTests(){
    const auto frame=canonical(42);
    for(Fault failure:{Fault::Prepare,Fault::Buffers,Fault::Offset,Fault::Length,
                       Fault::Address,Fault::Decode,Fault::Enqueue}){
        AirportMtkAwdlInterface rx;rx.authorize(frame);fault=failure;
        auto offered=frame;
        if(failure==Fault::Decode){
            mtkawdl::DataView view{};assert(mtkawdl::dataView(offered.data(),offered.size(),26,view));
            const auto offset=size_t(view.payload-offered.data());
            // Only to reach the actual Rust decoder's error branch after full
            // validation/admission: inject an invalid RFC1042 prefix in place.
            addressHook=[&,offset]{const uint8_t bad[8]={0xaa,0xaa,3,0,0,0,0x86,0xdd};
                                  memcpy(offered.data()+offset,bad,8);};
        }
        assert(rx.ingest(offered,100));
        assert(window(rx).active && window(rx).sequence==42 && window(rx).lastRx==100);
        assert(rx.dataRxDecoded==1 && rx.dataRxTraceCount==1 && !rx.nativeRx && rx.nativeDrops==1);
        assert(rx.pool->allocations==1 && rx.pool->reclaims==1 && !livePackets);
        fault=Fault::None;assert(rx.ingest(frame,101));
        assert(rx.dataRxReplay==1 && rx.pool->allocations==1 && !rx.nativeRx);
        assert(window(rx).lastRx==101 && rx.dataRxLastRejected.result==2);
    }
    puts("PASS: canonical preparation/decode/enqueue failure intentionally remains replay-consumed");
}
void canonicalEquivalenceTests(){
    fault=Fault::None;
    for(unsigned stream=0;stream<17;++stream)for(unsigned domain=0;domain<16;++domain){
        AirportMtkAwdlInterface rx;auto first=canonical(uint16_t(domain<<12|0xffe),stream);rx.authorize(first);
        auto ordinary=rx.dataPeers;
        uint64_t now=100;
        // Domain-local 12-bit rollover, reordered/too-old/foreign-domain data,
        // full 16-bit rollover/domain crossing, idle boundary and clock restart.
        std::vector<std::pair<uint16_t,uint64_t>> events;
        for(uint16_t low:{0xffe,0xfff,0,1,0xfff,0xfc0,0x19e,0x19d,0x19e})
            events.push_back({uint16_t(domain<<12|low),now++});
        for(uint16_t seq:{uint16_t((domain+1)<<12),uint16_t(65534),uint16_t(65535),uint16_t(0),uint16_t(63),uint16_t(1)})
            events.push_back({seq,now++});
        events.push_back({1,now-1+mtkawdl::peerLifetimeUs});
        events.push_back({1,now+2*mtkawdl::peerLifetimeUs});
        events.push_back({0,1});events.push_back({0,2});
        for(const auto &event:events){
            const auto allocations=rx.pool->allocations;const auto delivered=rx.nativeRx;
            const bool accepted=ordinary.acceptData(0,stream,event.first,event.second);
            assert(rx.ingest(canonical(event.first,stream),event.second));
            assert(rx.pool->allocations==allocations+unsigned(accepted) && rx.nativeRx==delivered+unsigned(accepted));
            for(unsigned s=0;s<17;++s)sameWindow(window(rx,s),ordinary.entries[0].windows[s]);
        }
    }
    // Equality of two streams' sequences must not couple their windows.
    AirportMtkAwdlInterface rx;rx.authorize(canonical(42));
    for(unsigned stream=0;stream<17;++stream)assert(rx.ingest(canonical(42,stream),100+stream));
    assert(rx.nativeRx==17);
    for(unsigned stream=0;stream<17;++stream)assert(rx.ingest(canonical(42,stream),200+stream));
    assert(rx.nativeRx==17 && rx.dataRxReplay==17 && rx.pool->allocations==17);
    puts("PASS: production canonical/ordinary Rust equivalence, 17 streams, 16 domains, rollover/reorder/expiry/backward clocks");
}
void canonicalNestedTests(){
    fault=Fault::None;
    for(bool rejection:{false,true}){
        AirportMtkAwdlInterface rx;auto seed=canonical(10);rx.authorize(seed);assert(rx.ingest(seed,100));
        auto outer=canonical(11),newer=canonical(rejection?10:12);
        auto ordinary=rx.dataPeers;const auto original=rx.dataRxAdmissionGeneration;
        allocationHook=[&]{
            assert(rx.refs>=3 && rx.rxPool->refs>=2 && rx.rxComplete->refs>=2);
            assert(rx.ingest(newer,200));
            assert(ordinary.acceptData(0,5,rejection?10:12,200)==!rejection);
            assert(rx.dataRxAdmissionGeneration!=original);
        };
        assert(rx.ingest(outer,101));unchanged(rx.dataPeers,ordinary);
        assert(window(rx).lastRx==200 && rx.pool->reclaims==1);
        assert(rx.dataRxTraceCount==2 && rx.nativeRx==(rejection?1u:2u));
        assert(rx.pool->allocations==(rejection?2u:3u)); // nested rejection allocates nothing
        assert(rx.ingest(outer,201));
        assert(ordinary.acceptData(0,5,11,201));unchanged(rx.dataPeers,ordinary);
        const auto count=rx.pool->allocations;assert(rx.ingest(outer,202));
        assert(rx.pool->allocations==count && window(rx).lastRx==202);
    }
    // A later RX admitted by preparation/enqueue must also cancel the REST
    // of an old aggregate. Its first subframe was already committed by design.
    for(bool duringEnqueue:{false,true}){
        AirportMtkAwdlInterface rx;auto seed=canonical(10);rx.authorize(seed);assert(rx.ingest(seed,100));
        auto outer=aggregate({canonical(11),canonical(12)}),newer=canonical(20);
        auto ordinary=rx.dataPeers;assert(ordinary.acceptData(0,5,11,101));
        auto callback=[&]{assert(rx.ingest(newer,200));assert(ordinary.acceptData(0,5,20,200));};
        if(duringEnqueue)enqueueHook=callback;else prepareHook=callback;
        assert(rx.ingest(outer,101));unchanged(rx.dataPeers,ordinary);
        assert(rx.pool->allocations==3 && rx.nativeRx==3 && rx.dataRxTraceCount==3);
        assert(window(rx).lastRx==200 && window(rx).sequence==20);
        assert(rx.ingest(outer,201));assert(rx.nativeRx==4); // 11 duplicate, 12 new
    }
    puts("PASS: nested newer canonical acceptance AND rejection cancel old allocation; later callbacks cannot reset remaining old aggregate");
}
void canonicalMutationTests(){
    fault=Fault::None;const auto frame=canonical(42);
    for(unsigned mutation=0;mutation<9;++mutation){
        AirportMtkAwdlInterface rx;rx.authorize(frame);const auto original=rx.dataRxAdmissionGeneration;
        const auto before=window(rx);const auto pool=rx.pool;
        allocationHook=[&,mutation]{
            clockNow=200;
            switch(mutation){
            case 0: assert(!rx.cache(peerMac,2) && !rx.cache(peerMac,0));break; // revoke/re-add same MAC
            case 1: rx.refresh(frame,200);break; // live observation mutates seen
            case 2: rx.resetDataPeers();assert(rx.cacheDataPeer(peerMac,true,200));break;
            case 3: assert(!rx.traffic(peerMac,true));assert(rx.cacheDataPeer(peerMac,false,200));
                    assert(!rx.traffic(peerMac,false) && !rx.traffic(peerMac,true));break;
            case 4: assert(!rx.disable(0) && !rx.enable(0));break; // readiness ABA
            case 5: assert(!rx.setSync(false) && !rx.setSync(true));rx.authorize(frame);break;
            case 6: assert(!rx.setSync(false));
                    {ether_addr changed{{2,9,8,7,6,5}},same{{2,1,2,3,4,5}};
                     assert(!rx.setMacAddress(changed) && !rx.setMacAddress(same));}
                    assert(!rx.setSync(true));rx.authorize(frame);break; // MAC ABA
            case 7: assert(!rx.setInterfaceEnable(false) && !rx.setSync(true));rx.authorize(frame);break;
            case 8: assert(rx.setLinkState(kIO80211NetworkLinkDown,0) && !rx.setSync(true));rx.authorize(frame);break;
            }
            assert(rx.dataRxAdmissionGeneration!=original && !pool->deletes);
        };
        assert(rx.ingest(frame,100));sameWindow(window(rx),before);
        assert(!rx.nativeRx && !rx.dataRxDecoded && !rx.dataRxTraceCount && pool->reclaims==1);
        assert(rx.ingest(frame,201) && rx.nativeRx==1);const auto attempts=pool->allocations;
        assert(rx.ingest(frame,202) && rx.nativeRx==1 && pool->allocations==attempts);
    }
    // Reuse a slot that expired during allocation, including the same MAC.
    {AirportMtkAwdlInterface rx;{GateScope gate;assert(rx.observeDataPeer(peerMac,100)>=0);}
     allocationHook=[&]{clockNow=3000101;rx.refresh(frame,clockNow);};
     assert(rx.ingest(frame,101));assert(!window(rx).active && !rx.nativeRx && rx.pool->reclaims==1);
     assert(rx.ingest(frame,3000102) && rx.nativeRx==1);}
    // Fresh liveness lookup is essential even WITHOUT a generation mutation.
    {AirportMtkAwdlInterface rx;{GateScope gate;assert(rx.observeDataPeer(peerMac,100)>=0);}
     const auto before=rx.dataPeers;allocationHook=[]{clockNow=3000101;};
     assert(rx.ingest(frame,101));unchanged(rx.dataPeers,before);assert(!rx.nativeRx && rx.pool->reclaims==1);
     rx.refresh(frame,3000102);assert(rx.ingest(frame,3000103) && rx.nativeRx==1);}
    // A permanent identity change rejects the old destination, and admits a
    // matching new one only after renewed source authorization.
    {AirportMtkAwdlInterface rx;rx.authorize(frame);const uint8_t next[6]={2,9,8,7,6,5};
     allocationHook=[&]{assert(!rx.setSync(false));ether_addr mac;memcpy(&mac,next,6);
                       assert(!rx.setMacAddress(mac) && !rx.setSync(true));rx.authorize(frame);};
     assert(rx.ingest(frame,100));assert(!rx.nativeRx && !window(rx).active && rx.pool->reclaims==1);
     const auto attempts=rx.pool->allocations;assert(rx.ingest(frame,201));assert(rx.pool->allocations==attempts);
     assert(rx.ingest(canonical(42,5,next),202) && rx.nativeRx==1);}
    puts("PASS: canonical allocation canceled by revoke/re-add, reset/observe/traffic, resource-ready ABA, MAC/disable/sync/link transitions and expiry");
}
void canonicalOriginTests(){
    fault=Fault::None;const auto frame=canonical(42);
    // Actual release/create helpers detach metadata before their callbacks;
    // the allocating pool/queue remain alive only through the RX retains.
    {AirportMtkAwdlInterface rx;rx.authorize(frame);const auto before=rx.dataPeers;
     const auto pool=rx.pool;const auto queue=rx.completion;
     allocationHook=[&]{rx.releaseDataPath();assert(!rx.rxPool && !rx.rxComplete);
                       assert(!pool->deletes && !queue->deletes);assert(rx.createDataPath());rx.enableDatapath();};
     assert(rx.ingest(frame,100));unchanged(rx.dataPeers,before);
     assert(pool->reclaims==1 && pool->deletes==1 && queue->deletes==1 && !rx.nativeRx);
     assert(rx.rxPool->stats->allocations==0 && rx.rxPool->stats->reclaims==0);
     assert(rx.ingest(frame,101) && rx.nativeRx==1);}
    // Resource identity itself is rechecked, not just the generation. These
    // deliberately omitted notifications are injected faults, not production
    // mutation entrypoints or a proposed teardown protocol.
    for(bool poolOnly:{false,true}){
        AirportMtkAwdlInterface rx;rx.authorize(frame);const auto before=rx.dataPeers;
        auto oldPool=rx.rxPool;auto oldQueue=rx.rxComplete;
        const auto pool=rx.pool;const auto queue=rx.completion;
        allocationHook=[&]{
            if(poolOnly)rx.rxPool=new IOSkywalkPacketBufferPool;
            else rx.rxComplete=new IOSkywalkRxCompletionQueue(rx.rxPool);
        };
        assert(rx.ingest(frame,100));unchanged(rx.dataPeers,before);assert(!rx.nativeRx && pool->reclaims==1);
        // Restore the deliberately inconsistent test resources before retry.
        if(poolOnly){rx.rxPool->release();rx.rxPool=oldPool;}
        else {rx.rxComplete->release();rx.rxComplete=oldQueue;}
        assert(!pool->deletes && !queue->deletes && rx.ingest(frame,101) && rx.nativeRx==1);
    }
    for(bool stop:{false,true}){
        AirportMtkAwdlInterface rx;rx.authorize(frame);const auto pool=rx.pool;const auto queue=rx.completion;
        allocationHook=[&]{assert(rx.refs>=3);if(stop)rx.stop(rx.owner);else rx.shutdown();
                          assert(rx.stopping && !pool->deletes && !queue->deletes && rx.refs>=3);};
        assert(rx.ingest(frame,100));assert(!window(rx).active && !rx.nativeRx && !rx.dataRxTraceCount);
        assert(pool->reclaims==1 && rx.refs==1 && !livePackets);
        if(stop)assert(pool->deletes==1 && queue->deletes==1 && !rx.rxPool && !rx.rxComplete);
        else assert(!pool->deletes && !queue->deletes);
        assert(!rx.ingest(frame,101));
    }
    puts("PASS: canonical replacement/stop/shutdown during allocation use retained self and correct origin pool cleanup");
}
void canonicalOrderingTests(){
    fault=Fault::None;const auto frame=canonical(42);
    AirportMtkAwdlInterface rx;rx.authorize(frame);
    assert(!AirportMtkAwdlInterface::receive(&rx,frame.data(),frame.size(),149,-35)); // off gate
    assert(!rx.dataRxFrames && !rx.pool->allocations && !rx.dataRxTraceCount);
    const auto calls=commandGate.calls;assert(rx.cache(peerMac,6)==0 && commandGate.calls==calls+1 && rx.refs==1);
    assert(rx.ingest(frame,100));auto prospective=canonical(43);
    const auto allocations=rx.pool->allocations;const auto before=rx.dataRxAdmissionGeneration;
    rx.radio.peerHook=[&]{assert(controllerLoop.inGate() && rx.dataRxPeerProgramming && rx.dataRxAdmissionGeneration!=before);
                         assert(rx.ingest(prospective,200));assert(rx.pool->allocations==allocations);};
    assert(!rx.cache(peerMac,6) && !rx.dataRxPeerProgramming);
    rx.radio.enableHook=[&]{assert(controllerLoop.inGate() && rx.syncOwner);
                           assert(rx.ingest(prospective,201));assert(rx.pool->allocations==allocations);};
    assert(!rx.setSync(true) && !rx.syncOwner);
    assert(rx.ingest(prospective,202) && rx.nativeRx==2);
    // An unsuccessful firmware mutation still invalidates BEFORE its side
    // effects; no table state changed, but its outer preflight is canceled.
    allocationHook=[&]{rx.radio.peerResult=kIOReturnError;
                       assert(rx.cache(peerMac,6)==kIOReturnError);rx.radio.peerResult=0;};
    assert(rx.ingest(canonical(44),203));assert(rx.nativeRx==2 && window(rx).sequence==43);
    assert(rx.ingest(canonical(44),204) && rx.nativeRx==3);
    {AirportMtkAwdlInterface exhausted;exhausted.authorize(frame);const auto table=exhausted.dataPeers;
     exhausted.dataRxAdmissionGeneration=UINT64_MAX;assert(exhausted.ingest(frame,100));
     assert(!exhausted.dataRxAdmissionGeneration && !exhausted.pool->allocations);unchanged(exhausted.dataPeers,table);
     {GateScope gate;exhausted.resetDataPeers();assert(exhausted.cacheDataPeer(peerMac,true,100));}
     assert(exhausted.ingest(frame,101) && !exhausted.pool->allocations && !exhausted.dataRxAdmissionGeneration);}
    {AirportMtkAwdlInterface exhausted;exhausted.authorize(frame);const auto calls=exhausted.radio.peerCalls;
     {GateScope gate;exhausted.dataRxPeerProgramming=~0u;assert(exhausted.programDataPeer(peerMac,true)==kIOReturnNotReady);}
     assert(!exhausted.dataRxAdmissionGeneration && exhausted.radio.peerCalls==calls);
     assert(exhausted.ingest(frame,100) && !exhausted.pool->allocations);}
    puts("PASS: explicit public gate ordering, firmware/sync admission gaps, failed mutation invalidation and permanent exhaustion");
}
void canonicalAggregateTests(){
    fault=Fault::None;
    for(bool envelope:{false,true})for(Fault failure:{Fault::Allocate,Fault::AllocateNull}){
        auto a=canonical(100,5,ownMac,95),b=canonical(101,5,ownMac,72),c=canonical(102);
        a.back()=0xa;b.back()=0xb;c.back()=0xc;auto frames=aggregate({a,b,c},envelope);
        AirportMtkAwdlInterface rx;rx.authorize(frames);
        rx.rxPool->results={Fault::None,failure,Fault::None};
        assert(rx.ingest(frames,100));
        assert(window(rx).sequence==102 && window(rx).bitmap==5 && window(rx).lastRx==100);
        assert(rx.nativeRx==2 && rx.pool->allocations==3 && rx.completion->frames.size()==2);
        assert(rx.completion->frames[0].back()==0xa && rx.completion->frames[1].back()==0xc);
        assert(rx.ingest(frames,101));
        assert(window(rx).bitmap==7 && rx.nativeRx==3 && rx.pool->allocations==4);
        assert(rx.completion->frames.back().back()==0xb && rx.dataRxReplay==2);
        assert(rx.ingest(frames,102) && rx.nativeRx==3 && rx.pool->allocations==4 && rx.dataRxReplay==5);
    }
    auto frames=aggregate({canonical(100,5,ownMac,95),canonical(101)},true);
    mtkawdl::DataView first{},second{};
    size_t used=mtkawdl::dataView(frames.data(),frames.size(),26,first);
    assert(used && mtkawdl::dataView(frames.data(),frames.size(),26+used,second));
    const auto subframe=26+used,payload=size_t(second.payload-frames.data());
    std::vector<std::vector<uint8_t>> malformed;
    for(size_t offset:{size_t(0),size_t(1),size_t(16),size_t(22),size_t(25),
                       subframe+6,subframe+12,subframe+14+6,payload,payload+16,payload+20}){
        // Flip IPv6's VERSION, not its legal traffic-class low nibble.
        auto bad=frames;bad[offset]^=offset==1?0x40:(offset==payload+16?0x10:1);
        if(offset==subframe+12)bad[offset]=0xff;
        assert(!mtkawdl::validDataFrame(bad.data(),bad.size()));malformed.push_back(bad);
    }
    auto extra=frames;extra.push_back(0);malformed.push_back(extra);
    auto partial=frames;partial.pop_back();malformed.push_back(partial);
    for(size_t n=0;n<frames.size();++n){
        // A cut exactly at a complete subframe boundary can be a valid shorter
        // aggregate. Do not pretend the wire format declares a subframe count.
        if(!mtkawdl::validDataFrame(frames.data(),n))malformed.emplace_back(frames.begin(),frames.begin()+n);
    }
    for(const auto &bad:malformed){
        AirportMtkAwdlInterface rx;rx.authorize(frames);const auto before=rx.dataPeers;
        rx.ingest(bad,100);unchanged(rx.dataPeers,before);
        assert(!rx.nativeRx && !rx.pool->allocations && !rx.dataRxTraceCount);
    }
    // Per-subframe destinations remain independent admission checks.
    const uint8_t wrongMac[6]={2,9,9,9,9,9},group[6]={0x33,0x33,0,0,0,1};
    AirportMtkAwdlInterface rx;auto destinations=aggregate({canonical(100),canonical(101,5,wrongMac),canonical(102,5,group)});
    rx.authorize(destinations);assert(rx.ingest(destinations,100));
    assert(rx.dataRxWrongDestination==1 && rx.nativeRx==2 && window(rx).bitmap==5);
    auto unknown=canonical(103);unknown[15]^=1;
    const auto count=rx.pool->allocations;assert(rx.ingest(unknown,101));assert(rx.dataRxUnknownPeer==1 && rx.pool->allocations==count);
    auto wrong=canonical(103,5,wrongMac);assert(rx.ingest(wrong,102));assert(rx.pool->allocations==count);
    puts("PASS: canonical mixed allocation aggregates remember only successful allocations; full malformed aggregate/source/IPv6/address checks precede delivery");
}
void canonicalPostAllocationViewTests(){
    fault=Fault::None;
    for(unsigned change=0;change<3;++change){
        auto clean=change==2?canonical(42):aggregate({canonical(42)});auto offered=clean;
        AirportMtkAwdlInterface rx;rx.authorize(clean);const auto before=rx.dataPeers;
        mtkawdl::DataView view{};assert(mtkawdl::dataView(offered.data(),offered.size(),26,view));
        const auto destination=size_t(view.destination-offered.data()),source=size_t(view.source-offered.data());
        allocationHook=[&,change]{
            // Inject changed borrowed bytes strictly to exercise post-allocation
            // destination/source/lookup checks, not a native ownership claim.
            if(change==0)offered[destination+5]^=1;
            else offered[source+5]^=1;
        };
        assert(rx.ingest(offered,100));unchanged(rx.dataPeers,before);
        assert(!rx.nativeRx && !rx.dataRxTraceCount && rx.pool->reclaims==1);
        assert(rx.ingest(clean,101) && rx.nativeRx==1);
    }
    puts("PASS: post-allocation fresh transmitter and each subframe destination are rechecked before canonical replay mutation");
}
void nativeTests() {
    // Synthetic complete RFC1042 IPv6 frame matching the captured native
    // header format. Retained hardware records contain prefixes only.
    uint8_t eth[96]={2,1,2,3,4,5,2,1,2,3,4,6,0x86,0xdd,0x60};
    eth[18]=0;eth[19]=42;
    uint8_t wire[122]={};assert(mtkawdl::encodeData(wire,sizeof(wire),eth,sizeof(eth),42)==sizeof(wire));
    std::vector<uint8_t> native(wire,wire+24);native[0]=0x88;
    native.push_back(0x87);native.push_back(0);
    native.insert(native.end(),eth,eth+12);native.push_back(0);native.push_back(90);
    const uint8_t snap[]={0xaa,0xaa,3,0,0,0,0x86,0xdd};
    native.insert(native.end(),snap,snap+8);native.insert(native.end(),eth+14,eth+96);
    auto retry=native;retry[1]|=8;
    // Failed delivery must not poison the cache or advance its eviction cursor.
    // Allocation with NULL proves the regression independently of queue ownership.
    for(Fault failure:{Fault::Allocate,Fault::AllocateNull,Fault::Prepare,Fault::Buffers,Fault::Offset,
                       Fault::Length,Fault::Address,Fault::Decode,Fault::Enqueue}){
        AirportMtkAwdlInterface rx;rx.authorize(native);fault=failure;
        auto offered=native;
        if(failure==Fault::Decode){
            mtkawdl::DataView view{};assert(mtkawdl::dataView(offered.data(),offered.size(),26,view));
            const auto version=size_t(view.payload-offered.data())+8;
            // Fault injection after validation, not a mock decoder: the real
            // Rust FFI rejects RFC1042 data with an invalid IPv6 version.
            addressHook=[&,version]{offered[version]^=0x10;};
        }
        assert(rx.ingest(offered,100));
        assert(rx.nativeRx==0 && rx.nativeDrops==1 && rx.completion->accepted==0);
        assert(rx.nativeIPv6TagNext==0);
        for(auto tag:rx.nativeIPv6Tags)assert(tag==0);
        assert(rx.completion->calls==(failure==Fault::Enqueue?1u:0u));
        assert(rx.pool->reclaims==((failure==Fault::Allocate || failure==Fault::AllocateNull)?0u:1u));
        assert(livePackets==0);
        fault=Fault::None;assert(rx.ingest(retry,101));
        assert(rx.nativeRx==1 && rx.completion->accepted==1 && rx.nativeIPv6TagNext==1);
        assert(rx.nativeIPv6Duplicate==0);
        assert(rx.completion->decoded.size()==96 && !memcmp(rx.completion->decoded.data(),eth,96));
        const auto attempts=rx.pool->allocations;assert(rx.ingest(retry,102));
        assert(rx.nativeIPv6Duplicate==1 && rx.nativeRx==1 && rx.pool->allocations==attempts);
        assert(livePackets==0);
    }
    // An unavailable data path likewise leaves the native cache unchanged.
    for(unsigned unavailable=0;unavailable<3;++unavailable){
        AirportMtkAwdlInterface rx;rx.authorize(native);
        auto poolObject=rx.rxPool;auto completionObject=rx.rxComplete;
        if(unavailable==0)rx.dataPathEnabled=false;
        if(unavailable==1)rx.rxPool=nullptr;
        if(unavailable==2)rx.rxComplete=nullptr;
        assert(rx.ingest(native,100));assert(rx.pool->allocations==0 && rx.nativeIPv6TagNext==0);
        rx.dataPathEnabled=true;rx.rxPool=poolObject;rx.rxComplete=completionObject;
        assert(rx.ingest(retry,101));assert(rx.nativeRx==1 && rx.nativeIPv6Duplicate==0);
    }
    AirportMtkAwdlInterface rx;rx.authorize(native);assert(rx.ingest(native,100));
    assert(rx.nativeIPv6Decoded==1 && rx.nativeRx==1);
    assert(rx.completion->decoded.size()==96 && !memcmp(rx.completion->decoded.data(),eth,96));
    assert(rx.ingest(retry,101));assert(rx.nativeIPv6Duplicate==1 && rx.nativeIPv6Decoded==1);
    auto split=native;split.back()=1;assert(rx.ingest(split,102));
    assert(rx.nativeIPv6Decoded==2 && rx.completion->decoded.back()==1);
    // Keep the peer authorized while the native retry entry expires.
    rx.refresh(native,3000100);
    assert(rx.ingest(native,3000100));assert(rx.nativeIPv6Decoded==3);
    for(unsigned i=0;i<130;++i){
        auto next=native;next[next.size()-2]=i;next.back()=2;
        assert(rx.ingest(next,3000200+i));
    }
    assert(rx.nativeIPv6Decoded==133 && rx.nativeIPv6Duplicate==1 && rx.nativeRx==133);
    // Whole-aggregate, peer, address and frame admission still precede allocation/cache.
    AirportMtkAwdlInterface blocked;assert(blocked.ingest(native,100));
    assert(blocked.dataRxUnknownPeer==1 && blocked.pool->allocations==0);
    blocked.authorize(native);
    auto invalid=native;invalid.pop_back();assert(blocked.ingest(invalid,101));
    assert(blocked.dataRxInvalid==1 && blocked.pool->allocations==0);
    auto wrong=native;wrong[9]^=1;wrong[31]^=1;assert(blocked.ingest(wrong,102));
    assert(blocked.dataRxWrongDestination==1 && blocked.pool->allocations==0);
    auto badSecond=native;
    badSecond.insert(badSecond.end(),2,0); // inter-subframe padding
    badSecond.insert(badSecond.end(),native.begin()+26,native.end());
    badSecond.pop_back();assert(blocked.ingest(badSecond,103));
    assert(blocked.dataRxInvalid==2 && blocked.pool->allocations==0);
    assert(blocked.nativeIPv6TagNext==0);
    blocked.enabled=false;assert(!blocked.ingest(native,104));blocked.enabled=true;
    blocked.stopping=true;assert(!blocked.ingest(native,105));
    assert(blocked.pool->allocations==0 && livePackets==0);
    puts("PASS: existing native-tag NULL/fault retry, duplicates, splits, expiry, capacity and structural admission behavior");
}
int main(){
    pureFFITests();canonicalAllocationTests();canonicalConsumedFaultTests();canonicalEquivalenceTests();
    canonicalNestedTests();canonicalMutationTests();canonicalOriginTests();canonicalOrderingTests();
    canonicalAggregateTests();canonicalPostAllocationViewTests();nativeTests();
    assert(!livePackets && !livePools && !liveQueues && !controllerLoop.depth);
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-native-rx-') as directory:
    directory = Path(os.environ.get('MTK_RX_TEST_WORK', directory))
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'test.cpp').write_text(code)
    subprocess.run(['clang++', '-std=c++14', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I' + str(root / 'kext/Wifi/vendor/include'),
                    str(directory / 'test.cpp'), str(host / 'debug/libmtk_ffi.a'),
                    '-o', str(directory / 'test')], check=True)
    subprocess.run([str(directory / 'test')], check=True)
print('AWDL RX: production extraction + real Rust FFI ASan/UBSan PASS (canonical allocation-only; prepare/decode/enqueue failures remain consumed)')
