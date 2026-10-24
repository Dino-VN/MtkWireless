"""Exercise production MI controls, scheduler and native RX with failure cases."""
from pathlib import Path
import subprocess, tempfile, sys
root=Path(__file__).resolve().parents[2]
s=(Path(sys.argv[1]) if len(sys.argv)>1 else root/'AirportMtkWlan/AirportMtkAwdlInterface.cpp').read_text()
def fn(sig):
 a=s.index(sig); return s[a:s.index('\n}\n',a)+3]
header=(root/'include/Airport/apple80211_ioctl.h').read_text()
structs=[]
for name in ['apple80211_awdl_af_tx_mode','apple80211_awdl_oob_request']:
 a=header.index('struct '+name+' {');b=header.index('} __attribute__((packed));',a)+len('} __attribute__((packed));')
 structs.append(header[a:b])
code=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <AWDL/AwdlTimeline.hpp>
#define __IO80211_TARGET __MAC_26_0
using IOReturn=unsigned;using UInt32=unsigned;
constexpr unsigned APPLE80211_VERSION=1,kIOReturnSuccess=0,kIOReturnBadArgument=1,
 kIOReturnUnsupported=2,kIOReturnNotReady=3,kIOReturnNoMemory=4,kIOReturnNoResources=5,kIOReturnNotFound=6,
 APPLE80211_C_FLAG_20MHZ=1,APPLE80211_C_FLAG_2GHZ=8,APPLE80211_C_FLAG_5GHZ=16;
constexpr unsigned MBUF_PKTHDR=1,kIOReturnOutputSuccess=0,kIOReturnOutputDropped=7;
uint64_t clockUS=10000000;uint64_t awdlNow(){return clockUS;}
struct apple80211_channel{uint32_t version,channel,flags;};
struct apple80211_peer_cache_control{uint8_t bytes[0x1d0];};
'''+ '\n'.join(structs)+r'''
struct Timer{unsigned ms=0;void setTimeoutMS(unsigned n){ms=n;}void setTimeoutUS(unsigned n){ms=(n+999)/1000;}void cancelTimeout(){}};
struct Packet{std::vector<uint8_t> bytes;};using mbuf_t=Packet*;
void mbuf_freem(mbuf_t m){delete m;}void *mbuf_data(mbuf_t m){return m->bytes.data();}
unsigned mbuf_flags(mbuf_t){return MBUF_PKTHDR;}
size_t mbuf_pkthdr_len(mbuf_t m){return m->bytes.size();}
int mbuf_copydata(mbuf_t m,size_t at,size_t n,void*out){if(at+n>m->bytes.size())return 1;memcpy(out,m->bytes.data()+at,n);return 0;}
struct IO80211Buffer{uint8_t bytes[2048]{};unsigned length=0,returned=0;
 void*getBytesNoCopy(){return bytes;}unsigned getCapacity(){return sizeof(bytes);}
 void setLength(unsigned n){length=n;}void returnBuffer(){++returned;}};
struct IO80211BufferPool{IO80211Buffer b;bool missing=false;IO80211Buffer*getBuffer(){return missing?nullptr:&b;}};
struct Hal{
 bool grant=true,on=true,missingStation=false;IOReturn txResult=0,peerResult=0;unsigned peerCalls=0;uint64_t observedAt=clockUS;unsigned transmitted=0;std::vector<std::vector<uint8_t>> frames;
 IOReturn awdlPrepareChannel(unsigned){return 0;}bool awdlIsEnabled(){return on;}void awdlAbort(){}bool awdlChannelAllowed(unsigned c){return c==6||c==149;} uint8_t infraChannel=0;uint8_t awdlInfraChannel(){return infraChannel;}uint8_t awdlOnlyChannel(){return infraChannel;}void awdlInfraSleep(unsigned){}void awdlInfraWakeIfIdle(){}bool awdlChannelReachable(unsigned c){return awdlChannelAllowed(c)&&(!infraChannel||c==infraChannel);}
 bool awdlGrantSnapshot(mtkawdl::Reservation &out){
  out={};out.granted=grant;out.channel=6;out.token=1;out.grantObservedAt=observedAt;
  out.deadline=clockUS+70000;return on;
 }
 bool awdlCanTransmit(unsigned){return grant;}IOReturn awdlReserve(unsigned,unsigned){return 0;}
 std::vector<bool> peerOperations;
 IOReturn awdlPeer(const uint8_t*,bool add){++peerCalls;peerOperations.push_back(add);return peerResult;}
 IOReturn awdlTransmit(mbuf_t m,unsigned){++transmitted;frames.push_back(m->bytes);delete m;return txResult;}
 IOReturn awdlTryTransmit(mbuf_t *m,unsigned){
  if(missingStation&&!((*m)->bytes[4]&1))return kIOReturnNotFound;
  if(txResult==kIOReturnNotReady||txResult==kIOReturnNoResources||txResult==kIOReturnNoMemory)return txResult;
  IOReturn result=awdlTransmit(*m,6);*m=nullptr;return result;
 }
};
struct AirportMtkAwdlInterface{
 Hal radio;Hal*hal=&radio;Timer t;Timer*timer=&t;IO80211BufferPool pool;
 void *owner=nullptr;void retain(){}void release(){}
 bool enabled=true,stopping=false,trafficPeerInstalled=false,trafficActive=false,allocationFail=false;
 uint8_t trafficPeer[6]{},installedTrafficPeer[6]{};
 uint8_t discoveryTxPeers[3][6]{};struct AddbaDecline{uint8_t request[24+9];bool valid;}addbaPending[4]{};uint64_t addbaRequests=0,addbaQueued=0,addbaSubmitted=0,addbaFailed=0,addbaExpired=0,addbaDeferred=0,addbaDropped=0,delbaReceived=0;
 mtkawdl::DataPeers dataPeers;mtkawdl::State state;mtkawdl::Timeline awTimeline;mtkawdl::Window requestedWindow{};
 struct NativePeerSample{mtkawdl::Advertisement advertisement;uint8_t peer[6];uint64_t received;bool valid;};
 NativePeerSample nativePeerSamples[16]{};unsigned nativePeerSampleNext=0;
 void notifyAvailabilityWindowStart(uint8_t,uint64_t){}
 void notifyAvailabilityWindowEnd(uint64_t){awTimeline.close();state.holdClock(0);state.releaseControls(clockUS);}
 apple80211_awdl_af_tx_mode afMode{};
 struct{unsigned synchronization_frame_period=440;}syncParams;
 struct{int peer_rssi_threshold=-70;}rssiThresholds;
 uint8_t miTemplate[2048]{},syncTemplate[2048]{},rejectedAction[2048]{};
 size_t miTemplateLength=0,templateLength=0,rejectedActionLength=0;
 uint64_t miStart=0,miSubmitted=0,miFailed=0;uint16_t miCounter=0;bool miCounterValid=false;
 uint64_t umiSubmitted=0,umiFailed=0,umiExpired=0,umiDeferred=0;
 uint64_t umiRequests=0,umiQueued=0;IOReturn umiStatus=0;
 uint64_t serviceTicks=0,reserveAttempts=0,grantObservations=0,actionSubmitted=0,actionFailed=0,
 actionReceived=0,actionHostAccepted=0,actionHostRejected=0,actionRSSIRejected=0,
 actionNativeDelivered=0,actionInvalid=0,actionNoBuffer=0,nextAction=0,requestDeadline=0;
 uint64_t reserveWaitUS=0,reserveMaxWaitUS=0,reserveInvalidations=0;uint32_t reserveInvalidReason=0;
 uint32_t reserveLeadUS=mtkawdl::rocLeadUs;mtkawdl::RocLeadTiming reserveTiming;uint64_t grantLatencyObservedAt=0;
 mtkawdl::RocChannelTiming reserveChannelTiming;
 int32_t lastActionRSSI=0;unsigned reserveDuration=0,requestedChannel=0,grantedChannel=0;
 IOReturn reserveStatus=0,actionStatus=0,miStatus=0;mbuf_t queue[64]{};
 uint64_t dataTxSubmitted=0,dataTxFailed=0,dataTxExpired=0,dataTxDeferred=0;IOReturn dataTxStatus=0;
 uint64_t dataRxFrames=0,dataRxInvalid=0,dataRxUnknownPeer=0,dataRxWrongDestination=0,dataRxReplay=0,dataRxDecoded=0;
 std::vector<std::vector<uint8_t>> received;
 uint64_t queueTime[64]{},queueDeadline[64]{};unsigned queueCount=0,queueHead=0,nativeCalls=0;
 void setProperty(const char*,uint64_t,unsigned){}void setProperty(const char*,bool){}
 void setProperty(const char*,void*,unsigned){}
 void publishScheduler(uint64_t){}void inputNative(const uint8_t*f,size_t n){received.emplace_back(f,f+n);}
 mbuf_t makeFrame(const uint8_t*f,size_t n){return allocationFail?nullptr:new Packet{{f,f+n}};}
 IO80211BufferPool*getMgmtFramePool(){return &pool;}
 void bpfTapInput(IO80211Buffer*b,unsigned type,void*meta,unsigned n){
   assert(type==12&&n==72);
   int32_t rssi;memcpy(&rssi,(uint8_t*)meta+44,4);assert(rssi==-40);
   ++nativeCalls;b->returnBuffer();
 }
 IOReturn setAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode*);
 IOReturn setAWDL_OOB_AUTO_REQUEST(apple80211_awdl_oob_request*);
 IOReturn setAWDL_OOB_REQUEST(apple80211_awdl_oob_request*);
 UInt32 output(mbuf_t);void serviceInGate();static bool receive(void*,const uint8_t*,size_t,uint8_t,int8_t);
 IOReturn setPEER_CACHE_CONTROL(apple80211_peer_cache_control*);
};
unsigned cacheCompletions=0;
void awdlControllerMessage(void*,AirportMtkAwdlInterface*,unsigned id,void*p,unsigned n,bool){
 assert(id==0x33&&n==0x1d0);auto*b=(uint8_t*)p;assert(mtkawdl::le32(b+4)==1||mtkawdl::le32(b+4)==3);++cacheCompletions;
}
'''+fn('UInt32 AirportMtkAwdlInterface::output(')+fn('IOReturn AirportMtkAwdlInterface::setAWDL_AF_TX_MODE(')+fn('IOReturn AirportMtkAwdlInterface::setAWDL_OOB_AUTO_REQUEST(')+(fn('IOReturn AirportMtkAwdlInterface::setAWDL_OOB_REQUEST(') if 'IOReturn AirportMtkAwdlInterface::setAWDL_OOB_REQUEST(' in s else 'IOReturn AirportMtkAwdlInterface::setAWDL_OOB_REQUEST(apple80211_awdl_oob_request*){return kIOReturnUnsupported;}')+fn('IOReturn AirportMtkAwdlInterface::setPEER_CACHE_CONTROL(')+fn('void AirportMtkAwdlInterface::serviceInGate(')+fn('bool AirportMtkAwdlInterface::receive(')+r'''
int main(){
 AirportMtkAwdlInterface d;const uint8_t local[]={2,1,2,3,4,5},peer[]={2,1,2,3,4,6};
 d.state.init(local,clockUS);
 static_assert(sizeof(apple80211_awdl_oob_request)==56,"Tahoe borrowed payload ABI");
 uint8_t service[]={6,9,0,0,0,0,0,0,0,0,0,0};
 uint8_t raw[56]{};mtkawdl::put32(raw,1);memset(raw+4,255,6);
 mtkawdl::put32(raw+12,24500);mtkawdl::put32(raw+16,0xffffffff);
 mtkawdl::put16(raw+40,sizeof(service));const uint8_t*ptr=service;memcpy(raw+48,&ptr,8);
 apple80211_awdl_oob_request req;memcpy(&req,raw,sizeof(req));
 assert(d.setAWDL_OOB_AUTO_REQUEST(&req)==0&&d.miTemplateLength==sizeof(service));
 assert(d.miStart==10024500);service[3]=99;assert(d.miTemplate[3]==0);service[3]=0;
 clockUS+=1000;assert(!d.setAWDL_OOB_AUTO_REQUEST(&req)&&d.miStart==10024500);
 for(unsigned bitmap:{1U,0x05050505U}){req.unk4=bitmap;assert(d.setAWDL_OOB_AUTO_REQUEST(&req)==kIOReturnUnsupported);}
 req.unk4=0xffffffff;req.data=nullptr;assert(d.setAWDL_OOB_AUTO_REQUEST(&req)==kIOReturnBadArgument);
 req.data=service;req.data_len=2049;assert(d.setAWDL_OOB_AUTO_REQUEST(&req)==kIOReturnBadArgument);
 req.data_len=sizeof(service);req.unk3=1000001;assert(d.setAWDL_OOB_AUTO_REQUEST(&req)==kIOReturnBadArgument);req.unk3=24500;
 apple80211_awdl_af_tx_mode mode{1,3};assert(!d.setAWDL_AF_TX_MODE(&mode));
 d.serviceInGate();assert(!d.radio.transmitted); // start delay, suppress PSF
 clockUS=d.miStart;d.radio.grant=false;d.serviceInGate();assert(!d.radio.transmitted);
 d.radio.grant=true;d.serviceInGate();assert(d.miSubmitted==1&&d.radio.transmitted==1);
 assert(d.radio.frames.back()[30]==3);mtkawdl::Advertisement a{};
 assert(mtkawdl::parseAction(d.radio.frames.back().data(),d.radio.frames.back().size(),a));
 d.serviceInGate();assert(d.miSubmitted==1);clockUS+=4*mtkawdl::awUs;d.serviceInGate();assert(d.miSubmitted==2);
 mode.mode=0;assert(!d.setAWDL_AF_TX_MODE(&mode));d.serviceInGate();assert(d.radio.frames.back()[30]==0);
 mode.mode=1;assert(d.setAWDL_AF_TX_MODE(&mode)==kIOReturnUnsupported&&d.afMode.mode==0);
 req.unk4=0;assert(!d.setAWDL_OOB_AUTO_REQUEST(&req)&&!d.miTemplateLength);
 d.enabled=false;unsigned before=d.radio.transmitted;d.serviceInGate();assert(d.radio.transmitted==before);
 d.enabled=true;
 mtkawdl::State remote;remote.init(peer,clockUS);uint8_t f[256];
 size_t n=mtkawdl::buildAction(remote,f,sizeof(f),clockUS);
 assert(d.receive(&d,f,n,6,-40)&&d.actionHostAccepted==1&&d.nativeCalls==1);
 // MIF supplement without sync must reach native even though host rejects it.
 memcpy(f+40,service,sizeof(service));n=40+sizeof(service);
 assert(d.receive(&d,f,n,6,-40)&&d.actionHostRejected==1&&d.nativeCalls==2);
 assert(d.rejectedActionLength==n&&d.pool.b.returned==2);
 assert(d.receive(&d,f,n,6,-90)&&d.actionRSSIRejected==1&&d.nativeCalls==2);
 f[41]=255;assert(d.receive(&d,f,n,6,-40)&&d.actionInvalid==1&&d.nativeCalls==2);
 f[41]=9;d.pool.missing=true;assert(d.receive(&d,f,n,6,-40)&&d.actionNoBuffer==1);
 // Exercise actual data RX, including hardware-deaggregated and aggregate
 // payloads, per-MSDU replay, malformed tails and destination/peer gates.
 uint8_t eth[55]{},wire[128]{};memcpy(eth,local,6);memcpy(eth+6,peer,6);
 eth[12]=0x86;eth[13]=0xdd;eth[14]=0x60;eth[19]=1;eth[54]=123;
 size_t wn=mtkawdl::encodeData(wire,sizeof(wire),eth,sizeof(eth),40);
 assert(d.receive(&d,wire,wn,6,-40)&&d.received.size()==1);
 assert(d.received.back()==std::vector<uint8_t>(eth,eth+sizeof(eth)));
 d.receive(&d,wire,wn,6,-40);assert(d.dataRxReplay==1&&d.received.size()==1);
 std::vector<uint8_t> aggregate(wire,wire+24);aggregate[0]=0x88;
 aggregate.push_back(0x80);aggregate.push_back(0);
 for(unsigned seq:{41U,42U}) {
  aggregate.insert(aggregate.end(),eth,eth+12);aggregate.push_back(0);aggregate.push_back(wn-24);
  size_t pos=aggregate.size();aggregate.insert(aggregate.end(),wire+24,wire+wn);
  mtkawdl::put16(aggregate.data()+pos+10,seq);
  if(seq==41)while((aggregate.size()-26)%4)aggregate.push_back(0);
 }
 d.receive(&d,aggregate.data(),aggregate.size(),6,-40);assert(d.received.size()==3);
 d.receive(&d,aggregate.data(),aggregate.size(),6,-40);assert(d.dataRxReplay==3);
 auto malformed=aggregate;
 // A truncated final IPv6 payload must not deliver the valid first subframe.
 malformed=aggregate;malformed.pop_back();d.receive(&d,malformed.data(),malformed.size(),6,-40);
 assert(d.dataRxInvalid==1&&d.received.size()==3);
 std::vector<uint8_t> deagg(aggregate.begin(),aggregate.begin()+26);
 deagg.push_back(0xcc);deagg.push_back(0xdd);
 deagg.insert(deagg.end(),eth,eth+12);deagg.push_back(0);deagg.push_back(wn-24);
 size_t dp=deagg.size();deagg.insert(deagg.end(),wire+24,wire+wn);
 mtkawdl::put16(deagg.data()+dp+10,43);
 assert(mtkawdl::relocateDeaggregatedHeader(deagg.data(),deagg.size()));
 deagg.erase(deagg.begin(),deagg.begin()+2);d.receive(&d,deagg.data(),deagg.size(),6,-40);
 assert(d.received.size()==4&&d.received.back()==std::vector<uint8_t>(eth,eth+sizeof(eth)));
 wire[4]^=4;d.receive(&d,wire,wn,6,-40);assert(d.dataRxWrongDestination==1);wire[4]^=4;
 wire[10]^=4;d.receive(&d,wire,wn,6,-40);assert(d.dataRxUnknownPeer==1);wire[10]^=4;
 // Successful submissions, firmware failures, expiry without any grant.
 d.afMode.mode=3;d.miTemplateLength=0;
 const uint64_t life=mtkawdl::dataQueueLifetimeUs(d.state,wire+4);
 auto enqueue=[&](uint64_t time){unsigned tail=(d.queueHead+d.queueCount)%64;
  d.queue[tail]=d.makeFrame(wire,wn);d.queueTime[tail]=time;d.queueDeadline[tail]=time+life;++d.queueCount;};
 d.state.peers[d.state.find(peer)].advertisement.sequence.channels[0]=6;
 memcpy(wire+4,peer,6);enqueue(clockUS);d.serviceInGate();assert(d.dataTxSubmitted==1&&!d.queueCount);
 assert(d.radio.peerCalls==1&&d.radio.peerOperations.back());
 enqueue(clockUS);d.serviceInGate();assert(d.radio.peerCalls==1&&d.dataTxSubmitted==2);
 // A peer ADDBA Request is declined through the same per-peer queue: never
 // handed to native, never rebuilt as an MI, sent unchanged on its channel.
 {uint8_t addba[33]={0xd0,0,0x30,0};memcpy(addba+4,local,6);memcpy(addba+10,peer,6);
  memcpy(addba+16,mtkawdl::bssid,6);addba[22]=0x10;addba[23]=0x3c;
  const uint8_t body[9]={3,0,0x3f,0x03,0x10,0,0,0x30,0};memcpy(addba+24,body,9);
  unsigned calls=d.nativeCalls,sent=d.radio.transmitted;uint64_t umi=d.umiSubmitted,invalid=d.actionInvalid;
  assert(d.receive(&d,addba,33,6,-90)&&d.addbaRequests==1&&d.nativeCalls==calls&&d.actionInvalid==invalid);
  assert(d.receive(&d,addba,33,6,-40)&&d.addbaRequests==2&&!d.addbaDropped);
  d.serviceInGate();assert(d.addbaQueued==1&&d.addbaSubmitted==1&&d.umiSubmitted==umi&&!d.queueCount);
  assert(d.radio.transmitted==sent+1);const auto &r=d.radio.frames.back();
  assert(r.size()==33&&r[24]==3&&r[25]==1&&r[26]==0x3f&&r[27]==37&&!r[28]);
  assert(!memcmp(r.data()+4,peer,6)&&!memcmp(r.data()+10,local,6)&&r[29]==0x03&&r[30]==0x10);
  // DELBA is counted; an unknown sender is dropped without a queued frame.
  addba[25]=2;assert(d.receive(&d,addba,33,6,-40)&&d.delbaReceived==1);
  addba[25]=0;addba[15]^=0x80;assert(d.receive(&d,addba,33,6,-40));d.serviceInGate();
  assert(d.addbaDropped==1&&d.addbaQueued==1&&!d.queueCount);}
 // Native cache owns the station after discovery; remove it only once all
 // membership expires. An unknown queued MAC cannot create a new station.
 assert(d.dataPeers.setCached(peer,true,clockUS));
 clockUS+=mtkawdl::peerLifetimeUs+1;d.serviceInGate();assert(d.radio.peerCalls==1);
 assert(d.dataPeers.setCached(peer,false,clockUS));d.serviceInGate();
 assert(d.radio.peerCalls==2&&!d.radio.peerOperations.back());
 for(auto &slot:d.discoveryTxPeers)assert(!slot[0]);
 enqueue(clockUS);d.serviceInGate();assert(d.radio.peerCalls==2);
 mbuf_freem(d.queue[d.queueHead]);d.queueCount=0;
 clockUS-=mtkawdl::peerLifetimeUs+1;
 assert(d.dataPeers.observe(peer,clockUS)>=0);
 d.state.init(local,clockUS);d.awTimeline.reset();
 remote.init(peer,clockUS);n=mtkawdl::buildAction(remote,f,sizeof(f),clockUS);
 assert(d.receive(&d,f,n,6,-40));
 // Pinned Tahoe sendUnicastMI ABI: ordinary option word 6, zero delay/
 // bitmap, protocol BSSID and synchronous borrowed supplemental TLV copy.
 apple80211_awdl_oob_request umi{};auto *u=(uint8_t*)&umi;
 mtkawdl::put32(u,1);memcpy(u+4,peer,6);mtkawdl::put16(u+20,6);
 memcpy(u+22,mtkawdl::bssid,6);umi.data=service;umi.data_len=sizeof(service);
 assert(d.setAWDL_OOB_REQUEST(&umi)==0&&d.queueCount==1);
 service[3]=99; // The queued request cannot retain native's borrowed pointer.
 auto originalUMI=d.queue[d.queueHead];uint64_t umiExpiry=d.queueDeadline[d.queueHead];
 d.radio.grant=false;unsigned umiTxBefore=d.radio.transmitted;d.serviceInGate();
 assert(d.queueCount==1&&d.queue[d.queueHead]==originalUMI&&d.radio.transmitted==umiTxBefore);
 clockUS+=1000;d.radio.grant=true;d.state.metric=d.state.masterMetric=123;
 d.radio.txResult=kIOReturnNoResources;d.serviceInGate();
 assert(d.queueCount==1&&d.umiDeferred==1&&d.queueDeadline[d.queueHead]==umiExpiry);
 d.radio.txResult=0;d.serviceInGate();assert(!d.queueCount&&d.umiSubmitted==1);
 auto sent=d.radio.frames.back();assert(sent[0]==0xd0&&sent[30]==3);
 assert(!memcmp(sent.data()+4,peer,6)&&!memcmp(sent.data()+10,local,6));
 assert(mtkawdl::le32(sent.data()+36)==uint32_t(clockUS));
 size_t tlv=43+mtkawdl::le16(sent.data()+41);assert(sent[tlv]==6&&sent[tlv+3]==0);
 assert(d.dataTxSubmitted==2); // UMI must not count as a BSD IPv6 submission.
 service[3]=0;umi.data=nullptr;assert(d.setAWDL_OOB_REQUEST(&umi)==kIOReturnBadArgument);umi.data=service;
 umi.data_len=2049;assert(d.setAWDL_OOB_REQUEST(&umi)==kIOReturnBadArgument);umi.data_len=sizeof(service);
 mtkawdl::put16(u+20,14);assert(d.setAWDL_OOB_REQUEST(&umi)==kIOReturnUnsupported);mtkawdl::put16(u+20,6);
 u[22]^=1;assert(d.setAWDL_OOB_REQUEST(&umi)==kIOReturnUnsupported);u[22]^=1;
 u[4]^=4;assert(d.setAWDL_OOB_REQUEST(&umi)==kIOReturnNotFound);u[4]^=4;
 d.enabled=false;assert(d.setAWDL_OOB_REQUEST(&umi)==kIOReturnNotReady);d.enabled=true;
 auto role=d.state.syncRole;d.state.syncRole=2;d.state.synchronized=false;
 assert(d.setAWDL_OOB_REQUEST(&umi)==kIOReturnNotReady);d.state.syncRole=role;
 d.queueCount=64;assert(d.setAWDL_OOB_REQUEST(&umi)==kIOReturnNoResources);d.queueCount=0;
 assert(!d.setAWDL_OOB_REQUEST(&umi));d.queueDeadline[d.queueHead]=clockUS;
 d.radio.grant=false;d.serviceInGate();assert(!d.queueCount&&d.umiExpired==1);d.radio.grant=true;
 // Independently exercise delayed UMI against schedule/election changes,
 // a missing station, allocation failure, fatal TX and transport reset.
 {
  AirportMtkAwdlInterface q;q.state.init(local,clockUS);q.state.nativeElectionManaged=true;remote.init(peer,clockUS);
  n=mtkawdl::buildAction(remote,f,sizeof(f),clockUS);assert(q.receive(&q,f,n,6,-40));
  q.state.sequence.count=8;q.state.ownSequence=q.state.sequence;
  uint8_t supplement[55]{};memcpy(supplement,service,sizeof(service));
  supplement[12]=24;mtkawdl::put16(supplement+13,40);
  auto request=umi;request.data=supplement;request.data_len=sizeof(supplement);
  assert(!q.setAWDL_OOB_REQUEST(&request));uint64_t queued=q.queueTime[q.queueHead],deadline=q.queueDeadline[q.queueHead];
  auto owned=q.queue[q.queueHead];auto ownedBytes=owned->bytes;
  for(unsigned i=0;i<q.state.peers[0].advertisement.sequence.count;++i)q.state.peers[0].advertisement.sequence.channels[i]=149;
  q.serviceInGate();assert(q.queueCount==1&&q.queue[q.queueHead]==owned&&q.queue[q.queueHead]->bytes==ownedBytes);
  for(unsigned i=0;i<q.state.peers[0].advertisement.sequence.count;++i)q.state.peers[0].advertisement.sequence.channels[i]=6;
  q.state.sequence.count=16;memset(q.state.sequence.channels,6,16);
  q.state.ownSequence=q.state.sequence;
  q.awTimeline.reset();q.state.releaseControls(clockUS); // Native schedule change closes the previous AW.
  q.state.metric=q.state.masterMetric=456;
  q.allocationFail=true;q.serviceInGate();q.allocationFail=false;
  assert(q.queueCount==1&&q.queue[q.queueHead]==owned&&q.umiDeferred==1);
  q.radio.missingStation=true;q.serviceInGate();
  assert(q.queueCount==1&&q.umiStatus==kIOReturnNotFound&&q.umiDeferred==2);
  assert(q.queueTime[q.queueHead]==queued&&q.queueDeadline[q.queueHead]==deadline);
  clockUS+=1000;q.radio.missingStation=false;q.serviceInGate();
  assert(!q.queueCount&&q.umiSubmitted==1&&q.dataTxSubmitted==0&&q.dataTxStatus==0);
  const auto &mi=q.radio.frames.back();mtkawdl::Advertisement refreshed{};
  assert(mtkawdl::parseAction(mi.data(),mi.size(),refreshed));
  assert(mtkawdl::le32(mi.data()+36)==uint32_t(clockUS)&&refreshed.sequence.count==16&&refreshed.metric==456);
  assert(!q.setAWDL_OOB_REQUEST(&request));q.radio.txResult=kIOReturnBadArgument;q.serviceInGate();
  assert(!q.queueCount&&q.umiFailed==1&&q.dataTxFailed==0);q.radio.txResult=0;
  assert(!q.setAWDL_OOB_REQUEST(&request));q.radio.txResult=kIOReturnNoResources;
  q.serviceInGate();auto expiry=q.queueDeadline[q.queueHead];clockUS=expiry;
  q.radio.grant=false;q.serviceInGate();assert(!q.queueCount&&q.umiExpired==1);
  // Use a fresh schedule epoch after the independent deadline check.
  clockUS=queued;q.state.init(local,clockUS);remote.init(peer,clockUS);
  n=mtkawdl::buildAction(remote,f,sizeof(f),clockUS);assert(q.receive(&q,f,n,6,-40));
  assert(!q.setAWDL_OOB_REQUEST(&request));q.radio.on=false;q.serviceInGate();
  assert(!q.queueCount&&!q.enabled);
 }
 // Transient grant/peer/busy/ring failures keep the exact original packet.
 unsigned submitted=d.dataTxSubmitted,attempts=d.radio.transmitted;
 for(unsigned status:{kIOReturnNotReady,kIOReturnNoResources,kIOReturnNoMemory}){
  d.radio.txResult=status;enqueue(clockUS);
  auto original=d.queue[d.queueHead];auto bytes=original->bytes;uint64_t queued=d.queueTime[d.queueHead],expiry=d.queueDeadline[d.queueHead];
  for(unsigned retry=0;retry<3;++retry)d.serviceInGate();
  assert(d.dataTxFailed==0&&d.dataTxStatus==status&&d.queueCount==1);
  assert(d.queue[d.queueHead]==original&&d.queue[d.queueHead]->bytes==bytes&&d.queueTime[d.queueHead]==queued&&d.queueDeadline[d.queueHead]==expiry);
  assert(d.radio.transmitted==attempts);
  d.radio.txResult=0;d.serviceInGate();++attempts;++submitted;
  assert(d.dataTxSubmitted==submitted&&!d.queueCount&&d.radio.frames.back()==bytes);
 }
 assert(d.dataTxDeferred==9);
 // A missing unicast station cannot block multicast behind it. Retain only
 // that peer's packet, then submit it once native station programming succeeds.
 clockUS+=16*1024; // Inside the multicast rendezvous guard.
 d.radio.missingStation=true;enqueue(clockUS);auto unicast=d.queue[d.queueHead];
 wire[4]=1;enqueue(clockUS);wire[4]=peer[0];
 d.serviceInGate();++submitted;
 assert(d.dataTxSubmitted==submitted&&d.queueCount==1&&d.queue[d.queueHead]==unicast);
 d.radio.missingStation=false;d.serviceInGate();++submitted;
 assert(d.dataTxSubmitted==submitted&&!d.queueCount);
 // Shared ring pressure retains FIFO and never refreshes the original age.
 d.radio.txResult=kIOReturnNoResources;enqueue(clockUS);enqueue(clockUS);
 auto first=d.queue[d.queueHead],second=d.queue[(d.queueHead+1)%64];
 unsigned attemptsBefore=d.radio.transmitted;d.serviceInGate();
 assert(d.queueCount==2&&d.queue[d.queueHead]==first&&d.queue[(d.queueHead+1)%64]==second);
 d.radio.txResult=0;d.serviceInGate();submitted+=2;
 assert(!d.queueCount&&d.dataTxSubmitted==submitted&&d.radio.transmitted==attemptsBefore+2);
 d.radio.txResult=kIOReturnNotReady;enqueue(clockUS-life+500);d.serviceInGate();
 clockUS+=1000;d.serviceInGate();assert(!d.queueCount&&d.dataTxExpired==1);
 // Return to the earlier schedule epoch for the independent expiry fixture.
 clockUS-=1000;d.dataTxExpired=0;
 d.radio.txResult=kIOReturnBadArgument;enqueue(clockUS);d.serviceInGate();assert(d.dataTxFailed==1&&!d.queueCount);
 d.radio.txResult=0;
 d.radio.grant=false;enqueue(clockUS-life);enqueue(clockUS);d.serviceInGate();
 assert(d.dataTxExpired==1&&d.queueCount==1);mbuf_freem(d.queue[d.queueHead]);d.queueCount=0;
 // A full queue with alternating expired/live packets preserves live order.
 for(unsigned i=0;i<64;++i){wire[wn-1]=i;enqueue(clockUS-(i%2 ? 0 : life));}
 d.serviceInGate();assert(d.queueCount==32&&d.dataTxExpired==33);
 for(unsigned i=0;i<32;++i)assert(d.queue[(d.queueHead+i)%64]->bytes.back()==i*2+1);
 clockUS+=life;d.serviceInGate();assert(!d.queueCount&&d.dataTxExpired==65);
 // Cache membership outlives the host's 3-second synchronization schedule.
 apple80211_peer_cache_control cache{};mtkawdl::put32(cache.bytes,1);memcpy(cache.bytes+8,peer,6);
 d.radio.peerResult=kIOReturnNotReady;
 unsigned completionBefore=cacheCompletions;
 assert(d.setPEER_CACHE_CONTROL(&cache)==kIOReturnNotReady&&cacheCompletions==completionBefore);
 int failedMember=d.dataPeers.find(peer,clockUS);
 assert(failedMember<0||!d.dataPeers.entries[failedMember].cached);
 d.radio.peerResult=0;assert(d.setPEER_CACHE_CONTROL(&cache)==0);
 int member=d.dataPeers.find(peer,clockUS);assert(member>=0&&d.dataPeers.entries[member].cached);
 d.state.nativeElectionManaged=true;d.state.nativeParentSelected=true;clockUS+=4000000;d.state.elect(clockUS);
 assert(d.state.find(peer)<0&&d.dataPeers.find(peer,clockUS)>=0);
 memcpy(eth,local,6);wn=mtkawdl::encodeData(wire,sizeof(wire),eth,sizeof(eth),80);
 unsigned receivedBefore=d.received.size();d.receive(&d,wire,wn,6,-40);assert(d.received.size()==receivedBefore+1);
 d.receive(&d,wire,wn,6,-40);assert(d.received.size()==receivedBefore+1);
 // Repeating cache add must keep the existing replay window.
 assert(d.setPEER_CACHE_CONTROL(&cache)==0);d.receive(&d,wire,wn,6,-40);assert(d.received.size()==receivedBefore+1);
 mtkawdl::put32(cache.bytes+4,2);assert(d.setPEER_CACHE_CONTROL(&cache)==0);
 assert(d.dataPeers.find(peer,clockUS)<0);unsigned unknownBefore=d.dataRxUnknownPeer;
 d.receive(&d,wire,wn,6,-40);assert(d.dataRxUnknownPeer==unknownBefore+1);
 // Trusted traffic registration needs no fabricated synchronization peer.
 assert(d.dataPeers.setTraffic(peer,true,clockUS));assert(d.state.find(peer)<0);
 clockUS+=5000000;assert(d.dataPeers.find(peer,clockUS)>=0);
 unsigned peerCallsBefore=d.radio.peerCalls;
 assert(d.setPEER_CACHE_CONTROL(&cache)==0&&d.radio.peerCalls==peerCallsBefore);
 assert(d.dataPeers.find(peer,clockUS)>=0); // Cache delete cannot tear down traffic's station.
 d.trafficActive=d.trafficPeerInstalled=true;memcpy(d.trafficPeer,peer,6);memcpy(d.installedTrafficPeer,peer,6);
 d.serviceInGate();assert(d.trafficPeerInstalled&&d.radio.peerCalls==peerCallsBefore);
 // Ending traffic must keep a station still owned by native cache.
 mtkawdl::put32(cache.bytes+4,0);assert(d.setPEER_CACHE_CONTROL(&cache)==0);
 d.trafficActive=false;peerCallsBefore=d.radio.peerCalls;d.serviceInGate();
 assert(!d.trafficPeerInstalled&&d.radio.peerCalls==peerCallsBefore);
 assert(d.dataPeers.setCached(peer,false,clockUS));
 assert(d.dataPeers.setTraffic(peer,false,clockUS)&&d.dataPeers.find(peer,clockUS)<0);
 mtkawdl::DataPeers full;for(unsigned i=0;i<mtkawdl::maxPeers;++i){uint8_t mac[6]={2,0,0,0,0,uint8_t(i)};assert(full.setCached(mac,true,clockUS));}
 assert(!full.canObserve(peer,clockUS)&&!full.setCached(peer,true,clockUS));
 d.dataPeers=full;peerCallsBefore=d.radio.peerCalls;
 assert(d.setPEER_CACHE_CONTROL(&cache)==kIOReturnNoResources&&d.radio.peerCalls==peerCallsBefore);
 d.dataPeers.reset();
 assert(d.dataPeers.observe(peer,clockUS)>=0);clockUS+=mtkawdl::peerLifetimeUs+1;assert(d.dataPeers.find(peer,clockUS)<0);
 // Execute production Ethernet output and deadline snapshots. A queued
 // multicast remains byte-for-byte intact beyond one second without ROC.
 AirportMtkAwdlInterface queued;queued.state.init(local,clockUS);queued.radio.grant=false;
 queued.afMode.mode=3;memset(eth,255,6);memcpy(eth+6,peer,6);
 uint64_t born=clockUS;
 assert(queued.output(queued.makeFrame(eth,sizeof(eth)))==kIOReturnOutputSuccess);
 auto original=queued.queue[queued.queueHead];auto originalBytes=original->bytes;
 uint64_t originalExpiry=queued.queueDeadline[queued.queueHead];
 assert(originalExpiry==born+5056576&&queued.queueTime[queued.queueHead]==born);
 assert(!memcmp(originalBytes.data()+10,local,6));
 clockUS+=1200000;queued.serviceInGate();
 assert(queued.queueCount==1&&!queued.dataTxExpired&&queued.queue[queued.queueHead]==original);
 assert(queued.queue[queued.queueHead]->bytes==originalBytes);
 // A slower schedule grants a larger budget only to subsequent packets.
 queued.state.sequence.step=queued.state.ownSequence.step=63;
 queued.state.presence=queued.state.ownPresence=64;
 assert(mtkawdl::dataQueueLifetimeUs(queued.state,eth)==20785216);
 assert(queued.output(queued.makeFrame(eth,sizeof(eth)))==kIOReturnOutputSuccess);
 uint64_t secondExpiry=queued.queueDeadline[(queued.queueHead+1)%64];
 assert(secondExpiry==clockUS+20785216);
 clockUS=originalExpiry-1;queued.serviceInGate();assert(queued.queueCount==2&&!queued.dataTxExpired);
 assert(queued.queueDeadline[queued.queueHead]==originalExpiry);
 clockUS=originalExpiry;queued.serviceInGate();assert(queued.queueCount==1&&queued.dataTxExpired==1);
 assert(queued.queueDeadline[queued.queueHead]==secondExpiry);
 clockUS=secondExpiry;queued.serviceInGate();assert(!queued.queueCount&&queued.dataTxExpired==2);
 // mDNS queued at an edge waits for the multicast guard. A packet queued
 // during discovery slot 2 survives unchanged until rendezvous slot 10.
 AirportMtkAwdlInterface rendezvous;rendezvous.state.init(local,clockUS);
 rendezvous.afMode.mode=3;uint64_t rendezvousEpoch=clockUS;
 rendezvous.radio.observedAt=clockUS-1;
 assert(rendezvous.output(rendezvous.makeFrame(eth,sizeof(eth)))==kIOReturnOutputSuccess);
 rendezvous.serviceInGate();assert(rendezvous.queueCount==1&&!rendezvous.dataTxSubmitted);
 clockUS=rendezvousEpoch+16*1024;rendezvous.serviceInGate();
 assert(rendezvous.dataTxSubmitted==1&&!rendezvous.queueCount);
 clockUS=rendezvousEpoch+2*4*mtkawdl::awUs+16*1024;
 assert(rendezvous.output(rendezvous.makeFrame(eth,sizeof(eth)))==kIOReturnOutputSuccess);
 auto waiting=rendezvous.queue[rendezvous.queueHead];auto waitingBytes=waiting->bytes;
 auto waitingExpiry=rendezvous.queueDeadline[rendezvous.queueHead];
 for(unsigned slot=2;slot<10;++slot) {
  clockUS=rendezvousEpoch+slot*4*mtkawdl::awUs+16*1024;
  rendezvous.serviceInGate();assert(rendezvous.queueCount==1&&rendezvous.dataTxSubmitted==1);
  assert(rendezvous.queue[rendezvous.queueHead]==waiting&&waiting->bytes==waitingBytes);
  assert(rendezvous.queueDeadline[rendezvous.queueHead]==waitingExpiry);
 }
 clockUS=rendezvousEpoch+10*4*mtkawdl::awUs+16*1024;rendezvous.serviceInGate();
 assert(rendezvous.dataTxSubmitted==2&&!rendezvous.queueCount);
 assert(rendezvous.radio.frames.back()==waitingBytes);
 // A known peer's longer sequence is included, with the same finite cap.
 mtkawdl::State budgets;budgets.init(local,clockUS);mtkawdl::State slow;slow.init(peer,clockUS);
 slow.sequence.step=slow.ownSequence.step=63;slow.presence=slow.ownPresence=64;
 n=mtkawdl::buildAction(slow,f,sizeof(f),clockUS);assert(budgets.receiveAction(f,n,clockUS)>=0);
 assert(mtkawdl::dataQueueLifetimeUs(budgets,peer)==20785216);
 puts("PASS data deadlines: production enqueue, >1s no-grant preservation, immutable per-packet budgets, slow local/peer sequences and exact bounded expiry.");
 puts("PASS unicast MI: native ioctl ABI, borrowed copy, peer/grant gates, fresh clock/election/schedule, immutable retries/deadlines, allocation/station/fatal failures, bounded expiry and transport-reset cleanup.");
 puts("PASS actual MI/control/RX: borrowed ABI/copy, bitmap/cancel, granted MIF, PSF modes, native discovery, data/aggregate RX, per-MSDU replay, peer/destination gates, atomic malformed-tail rejection, TX submission/failure and no-grant expiry.");
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-mi-discovery-') as d:
 p=Path(d)/'test.cpp';p.write_text(code);b=Path(d)/'test'
 subprocess.run(['clang++','-std=c++14','-Wall','-Wextra','-fsanitize=address,undefined','-I'+str(root/'include'),str(p),'-o',str(b)],check=True)
 subprocess.run([str(b)],check=True)
