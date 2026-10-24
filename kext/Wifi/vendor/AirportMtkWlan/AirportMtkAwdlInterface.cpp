// SPDX-License-Identifier: GPL-2.0-or-later
#include "AirportMtkWlan.hpp"
#include "AirportMtkAwdlInterface.hpp"
#ifdef MTK_NATIVE_AWDL
#include <IOKit/IOCommandGate.h>
#include <sys/kpi_mbuf.h>
#include <sys/errno.h>
#include <Airport/macOS26/IO80211Buffer.h>

#define super IO80211AWDLProtocol
OSDefineMetaClassAndStructors(AirportMtkAwdlInterface, IO80211AWDLProtocol)

extern IOCommandGate *_fCommandGate;
// The controller copies the message before returning (same API used by the
// native BCM driver's cache completion). No message data outlives this call.
extern void awdlControllerMessage(IO80211Controller*,IO80211SkywalkInterface*,unsigned int,void*,unsigned long,bool)
    __asm__("__ZN17IO80211Controller11postMessageEP23IO80211SkywalkInterfacejPvmb");

static uint64_t awdlNow()
{
    uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(),&ns); return ns/1000;
}

bool AirportMtkAwdlInterface::start(IOService *provider)
{
    if(!owner || provider!=owner || !hal || stopping || nativeStarted) return false;
    // The superclass publishes during start. BSD must not match until native
    // registration creates its logical-link set and packet queues.
    deferBSDAttach(true);
    if(!getProperty("IODeferBSDAttach")) {
        owner->recordAwdlCreation("bsd-deferral-failed",kIOReturnNoMemory);
        return false;
    }
    if(!createDataPath()) { releaseDataPath(); return false; }
    owner->recordAwdlCreation("native-super-start",kIOReturnNotReady);
    if(!super::start(provider)) { releaseDataPath(); return false; }
    nativeStarted=true;
    // The native MAC agent now exists. Its setter calls our setMacAddress
    // callback, then commits the address to the agent and IOMACAddress.
    ether_addr mac;
    memcpy(&mac,state.mac,6);
    owner->recordAwdlCreation("native-mac-address",kIOReturnNotReady);
    IOReturn macStatus=IO80211SkywalkInterface::setHardwareAddress(&mac);
    ether_addr nativeMac=IO80211SkywalkInterface::getSelfMacAddr();
    if(macStatus!=kIOReturnSuccess || memcmp(&nativeMac,state.mac,6)) {
        owner->recordAwdlCreation("native-mac-address-failed",
                                macStatus!=kIOReturnSuccess ? macStatus : kIOReturnError);
        stop(provider); return false;
    }
    setProperty("MTKAWDLNativeMAC",&nativeMac,6);
    setProperty("MTKAWDLNativeMACReady",true);
    IOSkywalkEthernetInterface::RegistrationInfo info={};
    owner->setProperty("MTKAWDLRegistrationVersion",static_cast<uint64_t>(1),32);
    owner->setProperty("MTKAWDLRegistrationSize",static_cast<uint64_t>(sizeof(info)),32);
    if(!initRegistrationInfo(&info,1,sizeof(info))) {
        owner->recordAwdlCreation("registration-info-failed",kIOReturnError);
        stop(provider); return false;
    }
    // Tahoe's helper calls Ethernet base getters directly and defaults to
    // Ethernet media / "en" / UINT_MAX; it bypasses Wi-Fi overrides. Set
    // the public registration fields verified against the Tahoe initializer:
    // subfamily 0x0c, initial media 0x28, BSD prefix/unit 0x30/0x38.
    const uint32_t subfamily=3; // IFNET_SUBFAMILY_WIFI (XNU kpi_interface.h)
    const uint32_t media=0x80; // IFM_IEEE80211 (net/if_media.h)
    const char *prefix=getBSDNamePrefix();
    const uint32_t unit=0;
    memcpy(info.bytes+0x0c,&subfamily,sizeof(subfamily));
    memcpy(info.bytes+0x28,&media,sizeof(media));
    memcpy(info.bytes+0x30,&prefix,sizeof(prefix));
    memcpy(info.bytes+0x38,&unit,sizeof(unit));
    owner->setProperty("MTKAWDLRegistrationReady",true);
    IOSkywalkPacketQueue *queues[]={txSubmit,txComplete,rxComplete};
    owner->recordAwdlCreation("native-register",kIOReturnNotReady);
    IOReturn status=registerEthernetInterface(&info,queues,3,txPool,rxPool,0);
    if(status!=kIOReturnSuccess) {
        owner->recordAwdlCreation("native-register-failed",status);
        stop(provider); return false;
    }
    nativeRegistered=true;
    setProperty("MTKAWDLConfigured",true);
    setProperty("MTKAWDLConfigStatus",static_cast<uint64_t>(kIOReturnSuccess),32);
    // Controller start marks the object ready before releasing BSD deferral.
    // Publish the verified agent address after registration and before BSD.
    setProperty("IOMACAddress",&nativeMac,6);
    setProperty("MTKAWDLRegistrationSubfamily",static_cast<uint64_t>(subfamily),32);
    setProperty("MTKAWDLRegistrationMedia",static_cast<uint64_t>(media),32);
    setProperty("MTKAWDLNativeQueuesRegistered",true);
    setProperty("MTKAWDLDataPathEnabled",false);
    setProperty("MTKAWDLNativeTxPackets",nativeTx,64);
    setProperty("MTKAWDLNativeRxPackets",nativeRx,64);
    setProperty("MTKAWDLNativeDroppedPackets",nativeDrops,64);
    // Controller start marks the object ready before releasing BSD deferral.
    return true;
}

bool AirportMtkAwdlInterface::createDataPath()
{
    // The native factory has an unbounded IOBSD dependency wait. A nonblocking
    // lookup prevents entering that wait when boot resources are unavailable.
    OSDictionary *matching=IOService::resourceMatching("IOBSD");
    IOService *resource=matching ? IOService::copyMatchingService(matching) : NULL;
    OSSafeReleaseNULL(matching);
    if(!resource) { owner->recordAwdlCreation("bsd-resource-unavailable",kIOReturnNotReady); return false; }
    resource->release();
    IOSkywalkPacketBufferPool::PoolOptions options={256,256,2048,1,4096,0,0};
    owner->recordAwdlCreation("tx-pool",kIOReturnNotReady);
    txPool=IOSkywalkPacketBufferPool::withName("MTK AWDL TX",this,0,&options);
    if(!txPool) { owner->recordAwdlCreation("tx-pool-failed",kIOReturnNoMemory); return false; }
    owner->recordAwdlCreation("rx-pool",kIOReturnNotReady);
    rxPool=IOSkywalkPacketBufferPool::withName("MTK AWDL RX",this,0,&options);
    if(!rxPool) { owner->recordAwdlCreation("rx-pool-failed",kIOReturnNoMemory); return false; }
    owner->recordAwdlCreation("native-queues",kIOReturnNotReady);
    txSubmit=IOSkywalkTxSubmissionQueue::withPool(txPool,64,0,this,submitNative,NULL,0);
    txComplete=IOSkywalkTxCompletionQueue::withPool(txPool,128,0,this,NULL,NULL,0);
    rxComplete=IOSkywalkRxCompletionQueue::withPool(rxPool,128,0,this,NULL,NULL,0);
    if(!txSubmit || !txComplete || !rxComplete) {
        owner->recordAwdlCreation("native-queues-failed",kIOReturnNoMemory); return false;
    }
    if(!owner->getWorkLoop()) {
        owner->recordAwdlCreation("native-workloop-unavailable",kIOReturnNotReady); return false;
    }
    IOSkywalkPacketQueue *queues[]={txSubmit,txComplete,rxComplete};
    for(auto q:queues) {
        IOReturn status=owner->getWorkLoop()->addEventSource(q);
        if(status!=kIOReturnSuccess) {
            owner->recordAwdlCreation("native-queue-add-failed",status); return false;
        }
        ++queuesAdded;
        q->disable();
    }
    return true;
}

void AirportMtkAwdlInterface::releaseDataPath()
{
    IOSkywalkPacketQueue *queues[]={txSubmit,txComplete,rxComplete};
    for(unsigned i=0;i<queuesAdded;++i) queues[i]->disable();
    // A rejected completion still owns its packet. Pool deallocation completes
    // the preparation and notifies the submission queue exactly once.
    for(unsigned i=0;i<completionCount;++i) txPool->deallocatePacket(completions[i]);
    completionCount=0;
    while(queuesAdded) owner->getWorkLoop()->removeEventSource(queues[--queuesAdded]);
    OSSafeReleaseNULL(txSubmit); OSSafeReleaseNULL(txComplete); OSSafeReleaseNULL(rxComplete);
    OSSafeReleaseNULL(txPool); OSSafeReleaseNULL(rxPool);
    dataPathEnabled=false;
}
void *AirportMtkAwdlInterface::getTxSubQueue(apple80211_wme_ac) { return txSubmit; }
void *AirportMtkAwdlInterface::getTxCompQueue() { return txComplete; }
void *AirportMtkAwdlInterface::getRxCompQueue() { return rxComplete; }
void *AirportMtkAwdlInterface::getTxPacketPool() { return txPool; }
void *AirportMtkAwdlInterface::getRxPacketPool() { return rxPool; }
UInt AirportMtkAwdlInterface::getHardwareAssists()
{
    // Native Ethernet preparation calls this virtual before ifnet_set_offload.
    // Raw AWDL TX neither segments TSO packets nor completes partial checksums.
    // Let networking produce complete, MTU-sized datagrams before submission.
    // Skywalk may advertise software GSO/copy checksumming on BSD later.
    return 0;
}
void AirportMtkAwdlInterface::setHardwareAssists(unsigned int,unsigned int)
{
    // Native registration/restore may update the cached assists directly.
    // Skywalk snapshots this cache before enabling its own software GSO and
    // copy/checksum path. Never advertise an unsupported physical assist.
    super::setHardwareAssists(0,~0u);
}
void AirportMtkAwdlInterface::publishBSDOffloadState(ifnet_t interface)
{
    setProperty("MTKAWDLDriverHardwareAssists",static_cast<uint64_t>(super::getHardwareAssists()),32);
    if(!interface) return;
    setProperty("MTKAWDLBSDCurrentOffload",static_cast<uint64_t>(ifnet_offload(interface)),32);
    setProperty("MTKAWDLBSDCurrentCapabilities",static_cast<uint64_t>(ifnet_capabilities_supported(interface)),32);
    setProperty("MTKAWDLBSDCurrentEnabled",static_cast<uint64_t>(ifnet_capabilities_enabled(interface)),32);
}
IOReturn AirportMtkAwdlInterface::prepareBSDInterface(ifnet_t interface,unsigned int options)
{
    setHardwareAssists(0,~0u);
    IOReturn status=super::prepareBSDInterface(interface,options);
    if(status!=kIOReturnSuccess) return status;
    // ifnet_set_offload(0) clears hwassist, but Tahoe skips updating enabled
    // capabilities when the translated mask is zero. Clear those explicitly
    // while retaining unrelated native capabilities such as Skywalk.
    const unsigned mask=IFCAP_HWCSUM | IFCAP_TSO | IFCAP_LRO | IFCAP_CSUM_PARTIAL |
        IFCAP_CSUM_ZERO_INVERT | 0x8000u; // Tahoe IFCAP_LRO_NUM_SEG; absent in build SDK.
    int error=ifnet_set_offload(interface,0);
    if(!error) error=ifnet_set_capabilities_enabled(interface,0,mask);
    if(!error) error=ifnet_set_capabilities_supported(interface,0,mask);
    unsigned offload=ifnet_offload(interface);
    unsigned supported=ifnet_capabilities_supported(interface), active=ifnet_capabilities_enabled(interface);
    // Success is a verified driver snapshot, not merely a successful setter.
    // Host activation may later advertise software GSO/copy checksumming.
    if(!error && (offload || ((supported|active)&mask))) error=EIO;
    setProperty("MTKAWDLBSDPreparedOffload",static_cast<uint64_t>(offload),32);
    setProperty("MTKAWDLBSDPreparedCapabilities",static_cast<uint64_t>(supported),32);
    setProperty("MTKAWDLBSDPreparedEnabled",static_cast<uint64_t>(active),32);
    setProperty("MTKAWDLBSDOffloadStatus",static_cast<uint64_t>(error),32);
    publishBSDOffloadState(interface);
    return error ? kIOReturnError : kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::enable(unsigned int options)
{
    if(stopping || !nativeRegistered || queuesAdded!=3 || !_fCommandGate)
        return kIOReturnNotReady;
    retain();
    // Tahoe's Wi-Fi base enable only calls setInterfaceEnable. The native
    // BCM proximity subclass explicitly starts its queues after that call.
    IOReturn status=super::enable(options);
    if(status==kIOReturnSuccess && !stopping) {
        publishBSDOffloadState(getBSDInterface());
        enableDatapath();
        if(!dataPathEnabled) status=kIOReturnNotReady;
    } else if(status==kIOReturnSuccess) status=kIOReturnNotReady;
    setProperty("MTKAWDLBSDEnableStatus",static_cast<uint64_t>(status),32);
    release(); return status;
}
IOReturn AirportMtkAwdlInterface::disable(unsigned int options)
{
    retain();
    // Stop submission before the native disable can tear down sync/peers.
    disableDatapath();
    IOReturn status=super::disable(options);
    setProperty("MTKAWDLBSDDisableStatus",static_cast<uint64_t>(status),32);
    release(); return status;
}
IOReturn AirportMtkAwdlInterface::dataPathGated(OSObject*,void *arg,void *enable,void*,void*)
{
    auto self=static_cast<AirportMtkAwdlInterface *>(arg);
    self->setDataPathEnabled(enable!=NULL); return kIOReturnSuccess;
}
void AirportMtkAwdlInterface::enableDatapath()
{
    if(_fCommandGate) _fCommandGate->runAction(dataPathGated,this,this);
}
void AirportMtkAwdlInterface::disableDatapath()
{
    if(_fCommandGate) _fCommandGate->runAction(dataPathGated,this,NULL);
}
void AirportMtkAwdlInterface::setDataPathEnabled(bool value)
{
    if(!nativeRegistered || !queuesAdded || (value && stopping)) return;
    if(value) {
        dataPathEnabled=true;
        txComplete->enable(); rxComplete->enable(); txSubmit->enable();
    } else {
        dataPathEnabled=false;
        txSubmit->disable(); rxComplete->disable();
        drainCompletions(); txComplete->disable();
    }
    setProperty("MTKAWDLDataPathEnabled",dataPathEnabled);
}
unsigned AirportMtkAwdlInterface::submitNative(OSObject *target,IOSkywalkTxSubmissionQueue*,
                                            IOSkywalkPacket *const *packets,unsigned count,void*)
{
    auto self=static_cast<AirportMtkAwdlInterface *>(target);
    // Native queues and the HAL share the controller workloop gate. Never
    // call firmware waits here; copy into the bounded protocol queue instead.
    unsigned consumed=0;
    for(;consumed<count && self->completionCount<128;++consumed) {
        IOSkywalkPacket *packet=packets[consumed];
        if(!packet) break;
        bool accepted=false;
        unsigned length=packet->getDataLength(), offset=packet->getDataOffset();
        if(self->enabled && self->dataPathEnabled && !self->stopping &&
           self->queueCount<64 && length>=14 && length<=1514 && offset+length<=2048 &&
           packet->getPacketBufferCount()==1) {
            auto bytes=static_cast<const uint8_t *>(packet->getDataVirtualAddress());
            mbuf_t frame=bytes ? self->makeFrame(bytes+offset,length) : NULL;
            if(frame) accepted=self->output(frame)==kIOReturnOutputSuccess;
        }
        if(accepted) ++self->nativeTx; else ++self->nativeDrops;
        // Completion must run after submission returns: native submission
        // updates its outstanding count only after invoking this callback.
        self->completions[self->completionCount++]=packet;
    }
    if(consumed && self->timer) self->timer->setTimeoutMS(1);
    return consumed;
}
void AirportMtkAwdlInterface::drainCompletions()
{
    // Native completion rejects count >= queue capacity (128). Clear our
    // pending count before notifications, which can reenter queue callbacks.
    unsigned pending=completionCount;
    if(!pending) return;
    IOSkywalkPacket *batch[128];
    memcpy(batch,completions,pending*sizeof(batch[0]));
    completionCount=0;
    for(unsigned offset=0;offset<pending;) {
        unsigned count=pending-offset;
        if(count>64) count=64;
        IOReturn status=txComplete->enqueuePackets(batch+offset,count,0);
        if(status!=kIOReturnSuccess) {
            for(unsigned i=0;i<count;++i) txPool->deallocatePacket(batch[offset+i]);
        }
        offset+=count;
    }
    setProperty("MTKAWDLNativeTxPackets",nativeTx,64);
    setProperty("MTKAWDLNativeDroppedPackets",nativeDrops,64);
}
void AirportMtkAwdlInterface::inputNative(const uint8_t *bytes,size_t length)
{
    if(!dataPathEnabled || stopping || !rxPool || !rxComplete) return;
    if(!bytes || length<14 || length>1514) { ++nativeDrops; return; }
    IOSkywalkPacket *packet=NULL;
    // Flags bit 0 uses kern_pbufpool_alloc_nosleep. Preparation materializes
    // the native buflets before accessing virtual address or packet length.
    if(rxPool->allocatePacket(&packet,1)!=kIOReturnSuccess || !packet) { ++nativeDrops; return; }
    // This packet was allocated directly from the pool, not borrowed from an
    // RX submission queue. Skywalk calls the recorded source's packetCompletion
    // while enqueuing RX; completion queues inherit a panic implementation.
    // A null source is supported by prepare/completeWithQueue and leaves the
    // pool-owned handle to be finalized and transferred to networking normally.
    if(packet->prepareWithQueue(NULL,2,0)!=kIOReturnSuccess ||
       packet->getPacketBufferCount()!=1 || packet->setDataOffset(0)!=kIOReturnSuccess ||
       packet->setDataLength(static_cast<unsigned>(length))!=kIOReturnSuccess) {
        rxPool->deallocatePacket(packet); ++nativeDrops; return;
    }
    void *destination=packet->getDataVirtualAddress();
    if(!destination || length>1514) { rxPool->deallocatePacket(packet); ++nativeDrops; return; }
    memcpy(destination,bytes,length);
    IOReturn status=rxComplete->enqueuePackets(&packet,1,0);
    if(status!=kIOReturnSuccess) { rxPool->deallocatePacket(packet); ++nativeDrops; }
    else { ++nativeRx; setProperty("MTKAWDLNativeRxPackets",nativeRx,64); }
    // Successful RX enqueue disposes the wrapper and transfers the packet
    // handle to networking. No release/deallocation after that transfer.
}

bool AirportMtkAwdlInterface::initWithController(AirportMtkWlan *controller,ether_addr *mac,MtkMwx *transport)
{
    owner=controller; hal=transport; timer=NULL; bsd=NULL; timerAdded=false;
    stopping=enabled=false; queueHead=queueCount=0; templateLength=0;
    // Configuration is tracked by the native peer manager; this backend keeps
    // the firmware context ready once registration succeeds.
    requestedChannel=grantedChannel=0; nextAction=requestDeadline=0;
    bzero(queue,sizeof(queue));
    bzero(trafficPeer,sizeof(trafficPeer)); trafficActive=false;
    bzero(installedTrafficPeer,sizeof(installedTrafficPeer)); trafficPeerInstalled=false;
    dataPeers.reset(); bzero(discoveryTxPeers,sizeof(discoveryTxPeers)); bzero(addbaPending,sizeof(addbaPending));
    state.init((const uint8_t *)mac,awdlNow());
    state.nativeElectionManaged=true;
    XYLog("AWDL native interface init\n");
    syncParams={APPLE80211_VERSION,16,16,16,110,0};
    extension={APPLE80211_VERSION,3,3,3,3};
    electionId={APPLE80211_VERSION,0}; presence={APPLE80211_VERSION,4};
    bzero(&syncState,sizeof(syncState)); syncState.version=APPLE80211_VERSION;
    syncState.state=1; memcpy(syncState.master,mac,6);
    rssiThresholds={APPLE80211_VERSION,0,0,0,-70}; afMode={APPLE80211_VERSION,0};
    minRate={APPLE80211_VERSION,12};
    bzero(&channelSequence,sizeof(channelSequence));
    channelSequence.version=APPLE80211_VERSION; channelSequence.length=16; channelSequence.step_count=3;
    for(unsigned i=0;i<16;++i) {
        channelSequence.seqs[i].version=APPLE80211_VERSION;
        channelSequence.seqs[i].channel=6;
        channelSequence.seqs[i].flags=APPLE80211_C_FLAG_20MHZ | APPLE80211_C_FLAG_2GHZ;
    }
    // Keep a stable sequence tied to the advertised AW clock.
    const uint8_t social[]={6,44,149}; unsigned count=0;
    for(unsigned i=0;i<3;++i) if(hal->awdlChannelAllowed(social[i]))
        state.ownSequence.channels[count++]=social[i];
    if(!count) {
        owner->recordAwdlCreation("no-social-channel",kIOReturnNotReady);
        return false;
    }
    for(unsigned i=count;i<16;++i) state.ownSequence.channels[i]=state.ownSequence.channels[i%count];
    state.sequence=state.ownSequence;
    for(unsigned i=0;i<16;++i) {
        auto &c=channelSequence.seqs[i]; c.channel=state.sequence.channels[i];
        c.flags=APPLE80211_C_FLAG_20MHZ | (c.channel<=14 ? APPLE80211_C_FLAG_2GHZ : APPLE80211_C_FLAG_5GHZ);
    }
    // Native initialization may call configuration getters through our vtable.
    // Skywalk::free dereferences its ivars before testing for NULL. Establish
    // the base allocation separately, so partial VirtualInterface init and
    // all later failures have a defined native cleanup path.
    owner->recordAwdlCreation("native-base-init",kIOReturnNotReady);
    if(!IO80211SkywalkInterface::init()) {
        owner->recordAwdlCreation("native-base-init-failed",kIOReturnNoMemory);
        return false;
    }
    nativeBaseInitialized=true;
    owner->recordAwdlCreation("native-super-init",kIOReturnNotReady);
    if(!super::init(controller,mac,6,"awdl")) {
        owner->recordAwdlCreation("native-super-init-failed",kIOReturnError);
        return false;
    }
    setInterfaceId(2);
    owner->recordAwdlCreation("protocol-timer",kIOReturnNotReady);
    timer=IOTimerEventSource::timerEventSource(this,tick);
    if(!timer) {
        owner->recordAwdlCreation("protocol-timer-failed",kIOReturnNoMemory);
        return false;
    }
    IOReturn timerStatus=owner->getWorkLoop()->addEventSource(timer);
    if(timerStatus!=kIOReturnSuccess) {
        owner->recordAwdlCreation("protocol-timer-add-failed",timerStatus);
        return false;
    }
    timerAdded=true;
    hal->awdlSetReceiver(this,receive);
    receiverRegistered=true;
    return true;
}

void AirportMtkAwdlInterface::setBSDInterface(ifnet_t interface) {
    bsd=interface;
    if(bsd) ifnet_set_lladdr(bsd,state.mac,6);
}
IOReturn AirportMtkAwdlInterface::getHardwareAddress(ether_addr *mac) {
    if(!mac) return kIOReturnBadArgument;
    memcpy(mac,state.mac,6); return kIOReturnSuccess;
}
ether_addr AirportMtkAwdlInterface::getSelfMacAddr()
{
    // Native startup constructs the peer manager before its MAC agent. The
    // native getter returns zero in that interval; use initialized identity.
    ether_addr mac;
    memcpy(&mac,state.mac,6);
    return mac;
}
bool AirportMtkAwdlInterface::isChipInterfaceReady() { return !stopping && hal; }

void AirportMtkAwdlInterface::shutdown()
{
    if(_fCommandGate && timerAdded) {
        _fCommandGate->runAction(shutdownGated,this); return;
    }
    shutdownGated(this,this,NULL,NULL,NULL);
}
IOReturn AirportMtkAwdlInterface::shutdownGated(OSObject*,void *arg,void*,void*,void*)
{
    auto self=static_cast<AirportMtkAwdlInterface *>(arg);
    self->shutdownInGate(); return kIOReturnSuccess;
}
void AirportMtkAwdlInterface::shutdownInGate()
{
    if(stopping) return;
    stopping=true;
    if(awTimeline.open) notifyAvailabilityWindowEnd(awdlNow());
    awTimeline.reset();
    setDataPathEnabled(false);
    if(timer) { timer->cancelTimeout(); timer->disable(); }
    if(hal && receiverRegistered) {
        hal->awdlSetReceiver(NULL,NULL); receiverRegistered=false;
        hal->awdlEnable(NULL,false);
    }
    enabled=false;
    dataPeers.reset(); bzero(discoveryTxPeers,sizeof(discoveryTxPeers)); bzero(addbaPending,sizeof(addbaPending)); trafficActive=false;
    trafficPeerInstalled=false; bzero(installedTrafficPeer,sizeof(installedTrafficPeer));
    while(queueCount) { mbuf_freem(queue[queueHead]); queueHead=(queueHead+1)%64; --queueCount; }
    if(timerAdded) { owner->getWorkLoop()->removeEventSource(timer); timerAdded=false; }
    bsd=NULL;
}
void AirportMtkAwdlInterface::stop(IOService *provider)
{
    shutdown();
    if(nativeStarted) { super::stop(provider); nativeStarted=false; }
    nativeRegistered=false;
    releaseDataPath();
}
void AirportMtkAwdlInterface::free()
{
    shutdown(); releaseDataPath(); OSSafeReleaseNULL(timer);
    if(nativeBaseInitialized) super::free();
    else {
        // Never enter IO80211SkywalkInterface::free on an object whose base
        // ivars were not allocated. The lower Skywalk free is NULL-safe.
        IOSkywalkEthernetInterface::free();
    }
}
IOReturn AirportMtkAwdlInterface::setMacAddress(ether_addr &mac)
{
    if(stopping) return kIOReturnNotReady;
    if(enabled || syncOwner) return kIOReturnBusy;
    if(!mtkawdl::unicast((uint8_t *)&mac)) return kIOReturnBadArgument;
    auto sequence=state.ownSequence;
    state.init((uint8_t *)&mac,awdlNow());
    state.nativeElectionManaged=true;
    awTimeline.reset();
    bzero(nativePeerSamples,sizeof(nativePeerSamples));
    dataPeers.reset(); bzero(discoveryTxPeers,sizeof(discoveryTxPeers)); bzero(addbaPending,sizeof(addbaPending)); trafficActive=false;
    state.sequence=state.ownSequence=sequence;
    memcpy(syncState.master,&mac,6);
    if(bsd) ifnet_set_lladdr(bsd,state.mac,6);
    // This is the native MAC agent's hardware callback. Calling
    // setHardwareAddress here reenters the agent and recurses indefinitely.
    setProperty("MTKAWDLProtocolMAC",state.mac,6);
    return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::setInterfaceEnable(bool value)
{
    if(stopping) return kIOReturnNotReady;
    // Native link transitions enable the Apple peer manager; sync is enabled
    // by the peer manager after the AWDL configuration transaction succeeds.
    retain();
    IOReturn status=super::setInterfaceEnable(value);
    if(status==kIOReturnSuccess && !stopping)
        status=reportLinkStatus(value ? 3 : 1,value ? 0x80 : 0x82);
    if(!value && !stopping) setSync(false);
    setProperty("MTKAWDLInterfaceEnableRequested",value);
    setProperty("MTKAWDLInterfaceEnableStatus",static_cast<uint64_t>(status),32);
    release(); return status;
}
bool AirportMtkAwdlInterface::setLinkState(IO80211LinkState value,unsigned int reason)
{
    if(stopping) return false;
    if(value==kIO80211NetworkLinkDown) setSync(false);
    setProperty("MTKAWDLNativeLinkState",static_cast<uint64_t>(value),32);
    return super::setLinkState(value,reason);
}
IOReturn AirportMtkAwdlInterface::setSync(bool value)
{
    retain();
    IOReturn status=_fCommandGate ? _fCommandGate->runAction(syncGated,this,
                            reinterpret_cast<void *>(uintptr_t(value))) : kIOReturnNotReady;
    release(); return status;
}
IOReturn AirportMtkAwdlInterface::syncGated(OSObject*,void *arg,void *value,void*,void*)
{
    auto self=static_cast<AirportMtkAwdlInterface *>(arg);
    return self->setSyncInGate(value!=NULL);
}
void AirportMtkAwdlInterface::finishAvailabilityWindowForDisable()
{
    if(!awTimeline.open) return;
    // Stop new traffic first, but retain an actually granted AW long enough
    // to end within native's 10-TU duration tolerance. This is a real wait,
    // never a future timestamp. Reset/detach/revocation still end immediately.
    uint64_t length=uint64_t(awTimeline.active.presence)*mtkawdl::awUs;
    uint64_t minimum=length>10*1024 ? length-10*1024 : 0;
    uint64_t earliest=awTimeline.lastStart+minimum;
    uint64_t waitedAt=awdlNow();
    while(!stopping && awTimeline.open && _fCommandGate &&
          awdlNow()<earliest && earliest<=awTimeline.radioEnd) {
        mtkawdl::Reservation live={};
        if(!hal || !hal->awdlGrantSnapshot(live) || !live.granted ||
           live.token!=awTimeline.token || live.channel!=awTimeline.active.channel ||
           live.grantObservedAt!=awTimeline.grantObservedAt || live.deadline<earliest ||
           awdlNow()>=live.deadline) break;
        uint64_t observed=awdlNow();
        if(observed>=earliest) break;
        uint64_t remaining=earliest-observed;
        if(remaining>10000) remaining=10000;
        AbsoluteTime wake;
        clock_interval_to_deadline(static_cast<unsigned>((remaining+999)/1000),kMillisecondScale,&wake);
        IOReturn slept=_fCommandGate->commandSleep(&syncOwner,wake,THREAD_UNINT);
        if(slept!=THREAD_AWAKENED && slept!=THREAD_TIMED_OUT) break;
    }
    uint64_t now=awdlNow();
    setProperty("MTKAWDLDisableAWWaitUS",now-waitedAt,64);
    if(awTimeline.open) notifyAvailabilityWindowEnd(now);
}
IOReturn AirportMtkAwdlInterface::setSyncInGate(bool value)
{
    if(stopping || !hal || !timer || !_fCommandGate) return kIOReturnNotReady;
    ++syncRequests;
    IOThread caller=IOThreadSelf();
    uint64_t deadline=awdlNow()+3000000;
    while(syncOwner) {
        // A synchronous callback cannot wait for its own stack to unwind.
        if(syncOwner==caller) return kIOReturnBusy;
        if(awdlNow()>=deadline) return kIOReturnTimeout;
        ++syncWaits;
        AbsoluteTime wake;
        clock_interval_to_deadline(10,kMillisecondScale,&wake);
        _fCommandGate->commandSleep(&syncOwner,wake,THREAD_UNINT);
        if(stopping) return kIOReturnNotReady;
    }
    syncOwner=caller;
    setProperty("MTKAWDLSyncTransition",value ? "enabling" : "disabling");
    if(!value) {
        enabled=false;
        finishAvailabilityWindowForDisable();
        state.discardControls(); state.holdClock(0);
        awTimeline.reset(); drainCompletions();
    }
    IOReturn status=stopping ? kIOReturnNotReady : hal->awdlEnable(state.mac,value);
    if(stopping) status=kIOReturnNotReady;
    if(status==kIOReturnSuccess) enabled=value;
    if(!enabled) {
        dataPeers.reset(); bzero(discoveryTxPeers,sizeof(discoveryTxPeers)); bzero(addbaPending,sizeof(addbaPending)); trafficActive=false; trafficPeerInstalled=false;
        bzero(installedTrafficPeer,sizeof(installedTrafficPeer));
        // Firmware commandSleep releases the gate. Submission can queue drop
        // completions during that wait, so drain again before canceling tick.
        drainCompletions();
        timer->cancelTimeout(); requestedChannel=grantedChannel=0; requestDeadline=0;
        while(queueCount) { mbuf_freem(queue[queueHead]); queueHead=(queueHead+1)%64; --queueCount; }
    } else if(!stopping) {
        nextAction=0; timer->setTimeoutMS(1);
    }
    if(status!=kIOReturnSuccess) ++syncFailures;
    setProperty("MTKAWDLSyncEnableStatus",static_cast<uint64_t>(status),32);
    setProperty("MTKAWDLSyncEnabled",enabled);
    setProperty("MTKAWDLSyncTransition",status==kIOReturnSuccess ? (enabled ? "enabled" : "disabled") : "failed");
    setProperty("MTKAWDLSyncRequests",syncRequests,64);
    setProperty("MTKAWDLSyncWaits",syncWaits,64);
    setProperty("MTKAWDLSyncFailures",syncFailures,64);
    XYLog("AWDL sync request=%d enabled=%d status=0x%x waits=%llu\n",value,enabled,status,syncWaits);
    syncOwner=NULL;
    _fCommandGate->commandWakeup(&syncOwner,false);
    return status;
}
mbuf_t AirportMtkAwdlInterface::makeFrame(const uint8_t *data,size_t length)
{
    mbuf_t m=NULL; unsigned chunks=1;
    if(mbuf_allocpacket(MBUF_DONTWAIT,length,&chunks,&m)) return NULL;
    memcpy(mbuf_data(m),data,length); mbuf_setlen(m,length); mbuf_pkthdr_setlen(m,length);
    return m;
}
UInt32 AirportMtkAwdlInterface::output(mbuf_t m)
{
    if(!m) return kIOReturnOutputDropped;
    if(!enabled || stopping || queueCount==64 || !(mbuf_flags(m)&MBUF_PKTHDR)) {
        mbuf_freem(m); return kIOReturnOutputDropped;
    }
    size_t n=mbuf_pkthdr_len(m); uint8_t eth[1514],frame[1542];
    if(n<14 || n>sizeof(eth) || mbuf_copydata(m,0,n,eth)) { mbuf_freem(m); return kIOReturnOutputDropped; }
    // The AWDL interface has its own locally administered address.
    memcpy(eth+6,state.mac,6);
    size_t size=mtkawdl::encodeData(frame,sizeof(frame),eth,n,state.dataCounter++);
    mbuf_freem(m);
    if(!size) return kIOReturnOutputDropped;
    m=makeFrame(frame,size);
    if(!m) return kIOReturnOutputDropped;
    unsigned pos=(queueHead+queueCount)%64;
    uint64_t now=awdlNow();
    queue[pos]=m; queueTime[pos]=now;
    queueDeadline[pos]=now+mtkawdl::dataQueueLifetimeUs(state,frame+4); ++queueCount;
    timer->setTimeoutMS(1);
    return kIOReturnOutputSuccess;
}
void AirportMtkAwdlInterface::tick(OSObject *o,IOTimerEventSource*)
{
    static_cast<AirportMtkAwdlInterface *>(o)->service();
}
void AirportMtkAwdlInterface::service()
{
    retain(); drainCompletions(); serviceInGate(); release();
}
void AirportMtkAwdlInterface::notifyAvailabilityWindowStart(uint8_t channel,uint64_t now)
{
    struct __attribute__((packed)) NativeAwStart {
        uint32_t awCounter;
        uint32_t nativeRole;
        uint8_t syncMaster[6];
        uint8_t topMaster[6];
        uint32_t fwStartUsBE;
        apple80211_channel channel;
    } payload={};
    static_assert(sizeof(apple80211_channel)==12,"AW channel ABI");
    static_assert(sizeof(NativeAwStart)==36,"AW-start ABI");
    static_assert(offsetof(NativeAwStart,awCounter)==0,"AW counter offset");
    static_assert(offsetof(NativeAwStart,nativeRole)==4,"AW role offset");
    static_assert(offsetof(NativeAwStart,syncMaster)==8,"AW sync offset");
    static_assert(offsetof(NativeAwStart,topMaster)==14,"AW root offset");
    static_assert(offsetof(NativeAwStart,fwStartUsBE)==20,"AW time offset");
    static_assert(offsetof(NativeAwStart,channel)==24,"AW channel offset");
    payload.awCounter=awTimeline.active.counter;
    // Native sync roles 1/2/3 map through BCM firmware 2/0/1 and back to
    // event roles 1/2/3 respectively. Report the effective elected role.
    payload.nativeRole=awTimeline.role;
    memcpy(payload.syncMaster,awTimeline.sync,6);
    memcpy(payload.topMaster,awTimeline.root,6);
    payload.fwStartUsBE=__builtin_bswap32(static_cast<uint32_t>(awTimeline.lastStart));
    payload.channel={APPLE80211_VERSION,channel,
        uint32_t(APPLE80211_C_FLAG_20MHZ | (channel<=14 ? APPLE80211_C_FLAG_2GHZ : APPLE80211_C_FLAG_5GHZ))};
    setProperty("MTKAWDLLastAWStartChannel",static_cast<uint64_t>(channel),32);
    setProperty("MTKAWDLLastAWStartCounter",static_cast<uint64_t>(payload.awCounter),32);
    setProperty("MTKAWDLLastAWStartUS",awTimeline.lastStart,64);
    setProperty("MTKAWDLLastAWStartDeliveryUS",now,64);
    setProperty("MTKAWDLLastAWStartDeliveryDelayUS",now-awTimeline.lastStart,64);
    ++awStartNotifications;
    setProperty("MTKAWDLAWStartNotifications",awStartNotifications,64);
    setProperty("MTKAWDLAWWindowOpen",true);
    setProperty("MTKAWDLAWRadioEndUS",awTimeline.radioEnd,64);
    postMessage(APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START,&payload,sizeof(payload),true);
}
void AirportMtkAwdlInterface::notifyAvailabilityWindowEnd(uint64_t now)
{
    if(!awTimeline.open) return;
    if(now>awTimeline.radioEnd) now=awTimeline.radioEnd;
    struct __attribute__((packed)) NativeAwEnd {
        uint32_t role;
        uint32_t endUsBE;
        uint8_t flags;
        uint8_t padding[3];
        uint32_t beaconCount;
        uint8_t stats[24];
    } payload={};
    static_assert(sizeof(NativeAwEnd)==40,"AW-end ABI");
    static_assert(offsetof(NativeAwEnd,endUsBE)==4,"AW end time offset");
    static_assert(offsetof(NativeAwEnd,beaconCount)==12,"AW beacon count offset");
    static_assert(offsetof(NativeAwEnd,stats)==16,"AW statistics offset");
    payload.endUsBE=__builtin_bswap32(static_cast<uint32_t>(now));
    setProperty("MTKAWDLLastAWEndUS",now,64);
    awTimeline.close();
    state.holdClock(0);
    setProperty("MTKAWDLAWEndNotifications",awTimeline.ends,64);
    setProperty("MTKAWDLAWWindowOpen",false);
    postMessage(APPLE80211_M_AWDL_AVAILABILITY_WINDOW_EXTENSIONS_END,&payload,sizeof(payload),true);
    // End callbacks may enqueue the last part of a control transaction.
    // Keep it deferred until native has consumed the preceding end event.
    if(enabled && !stopping) state.releaseControls(awdlNow());
    else state.discardControls();
}
void AirportMtkAwdlInterface::publishScheduler(uint64_t now)
{
    if(lastDiagnostic && now-lastDiagnostic<1000000) return;
    lastDiagnostic=now;
    hal->awdlPublishDiagnostics();
    publishBSDOffloadState(getBSDInterface());
    setProperty("MTKAWDLDataTxSubmitted",dataTxSubmitted,64);
    setProperty("MTKAWDLDataTxFailed",dataTxFailed,64);
    setProperty("MTKAWDLDataTxDeferred",dataTxDeferred,64);
    setProperty("MTKAWDLDataTxExpired",dataTxExpired,64);
    setProperty("MTKAWDLUMISubmitted",umiSubmitted,64);
    setProperty("MTKAWDLUMIFailed",umiFailed,64);
    setProperty("MTKAWDLUMIExpired",umiExpired,64);
    setProperty("MTKAWDLUMIDeferred",umiDeferred,64);
    setProperty("MTKAWDLUMIRequests",umiRequests,64);
    setProperty("MTKAWDLUMIQueued",umiQueued,64);
    setProperty("MTKAWDLUMIStatus",static_cast<uint64_t>(umiStatus),32);
    setProperty("MTKAWDLADDBARequests",addbaRequests,64);
    setProperty("MTKAWDLADDBAQueued",addbaQueued,64);
    setProperty("MTKAWDLADDBASubmitted",addbaSubmitted,64);
    setProperty("MTKAWDLADDBAFailed",addbaFailed,64);
    setProperty("MTKAWDLADDBAExpired",addbaExpired,64);
    setProperty("MTKAWDLADDBADeferred",addbaDeferred,64);
    setProperty("MTKAWDLADDBADropped",addbaDropped,64);
    setProperty("MTKAWDLDELBAReceived",delbaReceived,64);
    setProperty("MTKAWDLDataTxStatus",static_cast<uint64_t>(dataTxStatus),32);
    unsigned dataPending=0, umiPending=0;
    uint64_t oldestAge=0, nextExpiry=0;
    for(unsigned i=0;i<queueCount;++i) {
        unsigned index=(queueHead+i)%64;
        if(((const uint8_t *)mbuf_data(queue[index]))[0]==0xd0) { ++umiPending; continue; }
        ++dataPending;
        uint64_t age=now>=queueTime[index] ? now-queueTime[index] : 0;
        if(age>oldestAge) oldestAge=age;
        if(!nextExpiry || queueDeadline[index]<nextExpiry) nextExpiry=queueDeadline[index];
    }
    setProperty("MTKAWDLDataTxPending",static_cast<uint64_t>(dataPending),32);
    setProperty("MTKAWDLUMIPending",static_cast<uint64_t>(umiPending),32);
    setProperty("MTKAWDLDataTxOldestAgeUS",oldestAge,64);
    setProperty("MTKAWDLDataTxNextExpiryUS",nextExpiry,64);
    setProperty("MTKAWDLDataRxFrames",dataRxFrames,64);
    setProperty("MTKAWDLDataRxInvalid",dataRxInvalid,64);
    setProperty("MTKAWDLDataRxUnknownPeer",dataRxUnknownPeer,64);
    setProperty("MTKAWDLDataRxWrongDestination",dataRxWrongDestination,64);
    setProperty("MTKAWDLDataRxReplay",dataRxReplay,64);
    setProperty("MTKAWDLDataRxDecoded",dataRxDecoded,64);
    setProperty("MTKAWDLNativeDroppedPackets",nativeDrops,64);
    setProperty("MTKAWDLServiceTicks",serviceTicks,64);
    setProperty("MTKAWDLReserveAttempts",reserveAttempts,64);
    setProperty("MTKAWDLClockCorrections",state.clockCorrections,64);
    setProperty("MTKAWDLClockRejected",state.clockRejected,64);
    setProperty("MTKAWDLClockDeferred",state.clockDeferred,64);
    setProperty("MTKAWDLClockHoldUntilUS",state.clockHoldUntil,64);
    setProperty("MTKAWDLClockHoldover",state.nativeClockHoldover);
    setProperty("MTKAWDLClockHoldoverTransitions",state.nativeHoldoverTransitions,64);
    setProperty("MTKAWDLNativeParentSelected",state.nativeParentSelected);
    setProperty("MTKAWDLSelectedClockValidated",state.nativeClockValidated);
    setProperty("MTKAWDLClockAcquisitionPending",state.nativeClockAcquisitionPending);
    setProperty("MTKAWDLClockAcquisitions",state.nativeClockAcquisitions,64);
    setProperty("MTKAWDLClockParent",state.clockParent,6);
    setProperty("MTKAWDLSelectedParent",state.nativeParent,6);
    setProperty("MTKAWDLSelectedPeerLive",state.synchronized && !state.nativeClockHoldover);
    setProperty("MTKAWDLControlsDeferred",state.controlsDeferred,64);
    setProperty("MTKAWDLControlsPending",state.controlsDirty);
    setProperty("MTKAWDLReserveWaitUS",reserveWaitUS,64);
    setProperty("MTKAWDLReserveMaxWaitUS",reserveMaxWaitUS,64);
    setProperty("MTKAWDLReserveLeadUS",static_cast<uint64_t>(reserveLeadUS),32);
    setProperty("MTKAWDLReserveTimingSamples",reserveTiming.updates,64);
    setProperty("MTKAWDLReserveLeadDecreases",reserveTiming.decreases,64);
    setProperty("MTKAWDLReserveLeadChannel6US",static_cast<uint64_t>(reserveChannelTiming.lead(6)),32);
    setProperty("MTKAWDLReserveLeadChannel44US",static_cast<uint64_t>(reserveChannelTiming.lead(44)),32);
    setProperty("MTKAWDLReserveLeadChannel149US",static_cast<uint64_t>(reserveChannelTiming.lead(149)),32);
    setProperty("MTKAWDLReserveInvalidations",reserveInvalidations,64);
    setProperty("MTKAWDLReserveInvalidReason",static_cast<uint64_t>(reserveInvalidReason),32);
    setProperty("MTKAWDLRequestedAWChannel",static_cast<uint64_t>(requestedWindow.channel),32);
    setProperty("MTKAWDLRequestedAWCounter",static_cast<uint64_t>(requestedWindow.counter),32);
    setProperty("MTKAWDLRequestedAWStartUS",requestedWindow.start,64);
    setProperty("MTKAWDLRequestedAWEndUS",requestedWindow.end,64);
    setProperty("MTKAWDLReserveStatus",static_cast<uint64_t>(reserveStatus),32);
    setProperty("MTKAWDLReserveDurationMS",static_cast<uint64_t>(reserveDuration),32);
    setProperty("MTKAWDLGrantObservations",grantObservations,64);
    setProperty("MTKAWDLActionSubmitted",actionSubmitted,64);
    setProperty("MTKAWDLActionFailed",actionFailed,64);
    setProperty("MTKAWDLActionStatus",static_cast<uint64_t>(actionStatus),32);
    setProperty("MTKAWDLActionReceived",actionReceived,64);
    setProperty("MTKAWDLActionHostAccepted",actionHostAccepted,64);
    setProperty("MTKAWDLActionHostRejected",actionHostRejected,64);
    setProperty("MTKAWDLActionRSSIRejected",actionRSSIRejected,64);
    setProperty("MTKAWDLActionNativeDelivered",actionNativeDelivered,64);
    setProperty("MTKAWDLActionInvalid",actionInvalid,64);
    setProperty("MTKAWDLActionNoBuffer",actionNoBuffer,64);
    setProperty("MTKAWDLLastActionRSSI",static_cast<uint64_t>(lastActionRSSI),32);
    if(rejectedActionLength) setProperty("MTKAWDLRejectedAction",rejectedAction,static_cast<unsigned>(rejectedActionLength));
    setProperty("MTKAWDLMISubmitted",miSubmitted,64);
    setProperty("MTKAWDLAWStartNotifications",awStartNotifications,64);
    setProperty("MTKAWDLAWEndNotifications",awTimeline.ends,64);
    setProperty("MTKAWDLLateWindowObservations",awTimeline.late,64);
    setProperty("MTKAWDLShortGrantObservations",awTimeline.shortGrants,64);
    setProperty("MTKAWDLAWWindowOpen",awTimeline.open);
    setProperty("MTKAWDLAWRadioEndUS",awTimeline.radioEnd,64);
    setProperty("MTKAWDLMIFailed",miFailed,64);
    setProperty("MTKAWDLMIStatus",static_cast<uint64_t>(miStatus),32);
    setProperty("MTKAWDLHostChannel",static_cast<uint64_t>(state.channel(now)),32);
    setProperty("MTKAWDLHostCounter",static_cast<uint64_t>(state.counter(now)),32);
    setProperty("MTKAWDLHostChannelSequence",state.sequence.channels,state.sequence.count);
    setProperty("MTKAWDLHostPresence",static_cast<uint64_t>(state.presence),32);
    setProperty("MTKAWDLHostRole",static_cast<uint64_t>(state.syncRole),32);
    unsigned peers=0;
    for(unsigned i=0;i<mtkawdl::maxPeers;++i) if(state.peers[i].used) ++peers;
    setProperty("MTKAWDLHostPeerCount",static_cast<uint64_t>(peers),32);
    unsigned dataMembers=0;
    for(unsigned i=0;i<mtkawdl::maxPeers;++i) if(dataPeers.live(dataPeers.entries[i],now)) ++dataMembers;
    setProperty("MTKAWDLDataPeerCount",static_cast<uint64_t>(dataMembers),32);
}
void AirportMtkAwdlInterface::serviceInGate()
{
    if(!enabled || stopping) return;
    // Native end dispatch can reenter the timer/service path. Finish its
    // control transaction before opening another AW with the old snapshot.
    if(state.controlsHeld && !awTimeline.open) { timer->setTimeoutMS(1); return; }
    ++serviceTicks;
    // MtkWireless: back on the AP's channel (or AWDL off), say we are awake.
    hal->awdlInfraWakeIfIdle();
    if(!hal->awdlIsEnabled()) {
        // Transport reset invalidates the secondary BSS and all grants.
        state.holdClock(0);
        state.discardControls();
        enabled=false; dataPeers.reset(); bzero(discoveryTxPeers,sizeof(discoveryTxPeers)); bzero(addbaPending,sizeof(addbaPending)); trafficActive=false;
        trafficPeerInstalled=false; bzero(installedTrafficPeer,sizeof(installedTrafficPeer));
        setProperty("MTKAWDLSyncEnabled",false);
        setProperty("MTKAWDLSyncEnableStatus",static_cast<uint64_t>(kIOReturnNotReady),32);
        timer->cancelTimeout(); requestedChannel=grantedChannel=0; requestDeadline=0;
        // Native still owns the start even when firmware loses its context.
        if(awTimeline.open) notifyAvailabilityWindowEnd(awdlNow());
        awTimeline.reset();
        while(queueCount) { mbuf_freem(queue[queueHead]); queueHead=(queueHead+1)%64; --queueCount; }
        return;
    }
    uint64_t now=awdlNow();
    if(awTimeline.due(now)) {
        // The timestamp is the recorded radio/schedule edge, even if this
        // timer runs late. Administrative cancellation uses its observed time.
        notifyAvailabilityWindowEnd(awTimeline.radioEnd);
        if(stopping || !enabled) return;
        // Native EAW end is distinct from firmware ROC expiry. A grant can
        // cover several consecutive windows on the same channel.
        now=awdlNow();
    }
    if(awTimeline.open && awTimeline.matchesSchedule(state) && now<awTimeline.active.end) {
        state.holdClock(awTimeline.active.end);
        mtkawdl::Window next={}; mtkawdl::Reservation live={};
        bool snapshot=hal->awdlGrantSnapshot(live);
        bool nextValid=mtkawdl::currentWindow(state,awTimeline.active.end,next);
        // Use the native 10-TU end tolerance after accounting for this
        // start's lateness. The prior 8ms cap left usable switching time idle.
        uint64_t early=awTimeline.switchLeadUS();
        if(snapshot && nextValid && next.channel && hal->awdlChannelReachable(next.channel) &&
           !mtkawdl::grantCoversWindow(next,live,next.start) &&
           !mtkawdl::grantCoversReservedWindow(state,requestedWindow,requestedChannel,live,now) &&
           early && awTimeline.active.end-now<=early) {
            // Give the next ROC a bounded head start. Native permits +/-10TU
            // in actual AW duration, including lateness of this window's start.
            // Close before abort so callbacks never observe an unpaired start.
            notifyAvailabilityWindowEnd(now);
            if(stopping || !enabled) return;
            hal->awdlAbort(); requestedChannel=grantedChannel=0;
            now=awdlNow();
        }
    }
    // Expire queued data even while ROC requests fail or the slot is idle.
    // A full queue must not remain wedged waiting for a radio grant.
    for(unsigned pending=queueCount;pending--;) {
        mbuf_t m=queue[queueHead]; uint64_t time=queueTime[queueHead], expiry=queueDeadline[queueHead];
        queueHead=(queueHead+1)%64; --queueCount;
        if(now>=expiry) {
            const uint8_t *f=(const uint8_t *)mbuf_data(m);
            if(f[0]!=0xd0) ++dataTxExpired;
            else if(f[24]==mtkawdl::blockAckCategory) ++addbaExpired;
            else ++umiExpired;
            mbuf_freem(m);
        }
        else {
            unsigned tail=(queueHead+queueCount)%64;
            queue[tail]=m; queueTime[tail]=time; queueDeadline[tail]=expiry; ++queueCount;
        }
    }
    // Decline peer ADDBA requests through the per-peer queue, which waits
    // for the peer's channel and installs its firmware station.
    for(auto &pending:addbaPending) {
        if(!pending.valid) continue;
        pending.valid=false;
        const uint8_t *peer=pending.request+10;
        if(dataPeers.find(peer,now)<0 || state.find(peer)<0 || queueCount>=64) { ++addbaDropped; continue; }
        uint8_t frame[24+9];
        size_t size=mtkawdl::buildAddbaDecline(state,pending.request,sizeof(pending.request),frame,sizeof(frame));
        mbuf_t m=size ? makeFrame(frame,size) : NULL;
        if(!m) { ++addbaDropped; continue; }
        unsigned tail=(queueHead+queueCount)%64;
        queue[tail]=m; queueTime[tail]=now;
        queueDeadline[tail]=now+mtkawdl::dataQueueLifetimeUs(state,peer); ++queueCount; ++addbaQueued;
    }
    state.elect(now); publishScheduler(now);
    // Service discovery sends unicast (e.g. resolver replies) before native
    // traffic/cache registration. Install only destinations observed in valid
    // AWDL advertisements; the bounded membership lifetime also owns cleanup.
    for(unsigned i=0;i<3;++i) {
        if(mtkawdl::unicast(discoveryTxPeers[i]) && dataPeers.find(discoveryTxPeers[i],now)<0) {
            uint8_t address[6]; memcpy(address,discoveryTxPeers[i],6);
            IOReturn status=hal->awdlPeer(address,false);
            if(stopping || !enabled) return;
            // Membership can revive while the delete waits for firmware.
            // Keep tracking a restored station so its eventual expiry still
            // releases the WCID; a failed restore can be retried from the queue.
            bool clear=status==kIOReturnSuccess;
            if(clear && dataPeers.find(address,awdlNow())>=0)
                clear=hal->awdlPeer(address,true)!=kIOReturnSuccess;
            if(stopping || !enabled) return;
            if(clear && !memcmp(discoveryTxPeers[i],address,6))
                bzero(discoveryTxPeers[i],6);
            now=awdlNow();
        }
    }
    for(unsigned i=0;i<queueCount;++i) {
        const uint8_t *frame=(const uint8_t *)mbuf_data(queue[(queueHead+i)%64]);
        if(frame[4]&1) continue;
        uint8_t address[6]; memcpy(address,frame+4,6);
        int member=dataPeers.find(address,now);
        if(member<0 || dataPeers.entries[member].cached || dataPeers.entries[member].traffic) continue;
        int empty=-1; bool installed=false;
        for(unsigned j=0;j<3;++j) {
            if(!memcmp(discoveryTxPeers[j],address,6)) installed=true;
            if(!mtkawdl::unicast(discoveryTxPeers[j])) empty=j;
        }
        if(installed || empty<0) continue;
        IOReturn status=hal->awdlPeer(address,true);
        if(stopping || !enabled) return;
        if(status==kIOReturnSuccess) {
            memcpy(discoveryTxPeers[empty],address,6);
            if(dataPeers.find(address,awdlNow())<0) {
                bool clear=hal->awdlPeer(address,false)==kIOReturnSuccess;
                if(stopping || !enabled) return;
                if(clear && dataPeers.find(address,awdlNow())>=0)
                    clear=hal->awdlPeer(address,true)!=kIOReturnSuccess;
                if(stopping || !enabled) return;
                if(clear && !memcmp(discoveryTxPeers[empty],address,6))
                    bzero(discoveryTxPeers[empty],6);
            }
            if(stopping || !enabled) return;
        }
        // Firmware waits may release the gate. Recompute the RF window below,
        // and bound discovery station programming to one transaction per tick.
        break;
    }
    if(trafficPeerInstalled) {
        int peer=dataPeers.find(installedTrafficPeer,now);
        if(!trafficActive || peer<0 || memcmp(installedTrafficPeer,trafficPeer,6)) {
            // A native cache entry can still own this firmware station after
            // the single active traffic registration moves to another peer.
            bool cached=peer>=0 && dataPeers.entries[peer].cached;
            bool discovery=false;
            for(unsigned i=0;i<3;++i)
                if(!memcmp(discoveryTxPeers[i],installedTrafficPeer,6)) discovery=true;
            if(cached || discovery || hal->awdlPeer(installedTrafficPeer,false)==kIOReturnSuccess) {
                trafficPeerInstalled=false;
                bzero(installedTrafficPeer,sizeof(installedTrafficPeer));
            }
        }
    }
    if(trafficActive && !trafficPeerInstalled) {
        int peer=dataPeers.find(trafficPeer,now);
        if(peer>=0 && hal->awdlPeer(trafficPeer,true)==kIOReturnSuccess) {
            memcpy(installedTrafficPeer,trafficPeer,6); trafficPeerInstalled=true;
        }
    }
    // Peer programming can release the gate while waiting for firmware.
    if(stopping || !enabled) return;
    now=awdlNow();
    if(awTimeline.due(now)) {
        notifyAvailabilityWindowEnd(awTimeline.radioEnd);
        if(stopping || !enabled) return;
        now=awdlNow();
    }
    mtkawdl::Window window={};
    if(!mtkawdl::currentWindow(state,now,window)) { timer->setTimeoutMS(2); return; }
    // The notification describes the already granted radio interval. RX
    // clock corrections can move the computed slot without revoking it.
    // Keep that interval pinned; explicit setters and actual grant changes
    // still terminate it, rather than manufacturing a short AW-end.
    if(awTimeline.open && awTimeline.matchesSchedule(state)) window=awTimeline.active;
    uint8_t channel=window.channel;
    if(!channel || !hal->awdlChannelReachable(channel)) {
        if(awTimeline.open) notifyAvailabilityWindowEnd(now);
        if(stopping || !enabled) return;
        // An idle slot may already be preparing the following channel.
        // Preserve pending responses and live grants through idle slots.
        // Missing the original target does not expire firmware RF ownership.
        if(requestedChannel && requestedWindow.end<=now) {
            mtkawdl::Reservation pending={};
            if(!hal->awdlGrantSnapshot(pending) ||
               ((!pending.pending && !pending.granted) || now>=pending.deadline)) {
                state.holdClock(0);
                hal->awdlAbort(); requestedChannel=grantedChannel=0;
            }
        }
    }
    if(awTimeline.open && !awTimeline.matchesSchedule(state)) {
        notifyAvailabilityWindowEnd(now);
        if(stopping || !enabled) return;
        hal->awdlAbort(); requestedChannel=grantedChannel=0;
    }
    mtkawdl::Reservation grant={};
    bool grantReady=hal->awdlGrantSnapshot(grant) && grant.canTransmit(channel,now);
    if(grant.granted && grant.grantObservedAt && grant.grantObservedAt!=grantLatencyObservedAt) {
        grantLatencyObservedAt=grant.grantObservedAt;
        // Accepted response latency is measured from submission, independently
        // of the preceding synchronous RLM wait.
        reserveLeadUS=reserveTiming.observe(grant.responseDelayUS,reserveWaitUS);
        reserveChannelTiming.observe(grant.channel,grant.responseDelayUS,reserveWaitUS);
    }
    if(awTimeline.open && (!grant.granted || grant.channel!=awTimeline.active.channel ||
                         grant.token!=awTimeline.token ||
                         grant.grantObservedAt!=awTimeline.grantObservedAt)) {
        notifyAvailabilityWindowEnd(now<awTimeline.radioEnd ? now : awTimeline.radioEnd);
        if(stopping || !enabled) return;
    }
    if(!grantReady || (!awTimeline.open && now-window.start<=8000 &&
                       !mtkawdl::grantCoversWindow(window,grant,now))) {
        // Do not replace the last 2ms TX guard of a live grant with another
        // short ROC. Prepare the next EAW after closing this one.
        if(awTimeline.open) { timer->setTimeoutMS(1); return; }
        if(grantReady && now-window.start<=8000 &&
           !mtkawdl::grantCoversWindow(window,grant,now)) ++awTimeline.shortGrants;
        bool pending=grant.pending && now<grant.deadline;
        if(!pending) {
            if(mtkawdl::grantCoversReservedWindow(state,requestedWindow,requestedChannel,grant,now)) {
                // An accepted ROC already covers the original future AW.
                // Keep it through idle/different-channel slots instead of
                // moving the lookahead horizon and replacing its token.
                timer->setTimeoutMS(1); return;
            }
            mtkawdl::RadioPlan plan={};
            // Once an EAW ended early, reserve its successor even when a
            // short presence still leaves us inside the start tolerance.
            // Reacquiring the closed EAW wastes the switching head start.
            bool skipCurrent=awTimeline.emitted && now<awTimeline.active.end;
            // MtkWireless: with amtk_awdl_offchan=0, AWDL only uses the AP's
            // channel while associated. If no window is on it, back off
            // instead of polling every 2ms.
            uint8_t onlyChannel=hal->awdlOnlyChannel();
            if(!mtkawdl::planRadio(state,now,plan,reserveLeadUS,skipCurrent,&reserveChannelTiming,
                                   onlyChannel)) { timer->setTimeoutMS(onlyChannel ? 50 : 2); return; }
            mtkawdl::Window target=plan.window;
            if(!target.channel || !hal->awdlChannelReachable(target.channel)) {
                timer->setTimeoutMS(1); return;
            }
            if(target.start>now && mtkawdl::grantCoversWindow(target,grant,target.start)) {
                // An early grant is for the upcoming EAW. Do not repeatedly
                // abort/reacquire it just because the current EAW is late/idle.
                timer->setTimeoutMS(1); return;
            }
            if(grant.granted && now<grant.deadline &&
               ((grant.channel==channel && (!awTimeline.emitted || now>=awTimeline.active.end)) ||
                (grant.channel==target.channel && requestedChannel==target.channel &&
                 requestedWindow.start==target.start && grant.deadline>target.start))) {
                // Firmware may return a short grant. Replacing it each tick
                // wastes usable RF time and invalidates delayed grant tokens.
                // Keep a short grant overlapping this target, or usable
                // current-slot RF time. A grant expiring before its target
                // must be replaced at preparation time. An idle-gap grant
                // must not delay the next reservation beyond its requestAt.
                timer->setTimeoutMS(1); return;
            }
            // MtkWireless: tell the AP we sleep before an off-channel ROC;
            // awdlReserve waits ~2ms after it, so send it ahead of requestAt.
            if(plan.requestAt<=now+3000) hal->awdlInfraSleep(target.channel);
            if(plan.requestAt>now) {
                uint64_t delay=plan.requestAt-now;
                timer->setTimeoutUS(static_cast<uint32_t>(delay>2000 ? 2000 : delay)); return;
            }
            uint8_t parent[6],root[6];
            memcpy(parent,state.sync,6); memcpy(root,state.master,6);
            mtkawdl::Sequence sequence=state.sequence;
            uint8_t plannedRole=state.syncRole;
            // Include lead time and adjacent equal-channel EAWs in the dwell.
            // Firmware receipt still defines the only usable RF interval.
            uint32_t duration=plan.durationMs;
            reserveDuration=duration; ++reserveAttempts;
            requestedWindow=target;
            // RLM commandSleep releases the gate: RX may call elect() while
            // firmware prepares this channel. Freeze the shared event/wire
            // clock through the first requested EAW, before entering HAL.
            state.holdClock(target.end);
            uint64_t requestAt=now;
            IOReturn status=hal->awdlReserve(target.channel,duration);
            reserveStatus=status;
            if(stopping || !enabled) return;
            now=awdlNow();
            reserveWaitUS=now-requestAt;
            if(reserveWaitUS>reserveMaxWaitUS) reserveMaxWaitUS=reserveWaitUS;
            mtkawdl::Window after={};
            // Verify the original schedule coordinates even if RLM finished
            // after this EAW. A newly submitted pending ROC can still serve
            // following equal-channel windows; its first missed AW stays absent.
            uint64_t checkTime=target.start;
            reserveInvalidReason=0;
            if(plannedRole!=state.syncRole || memcmp(parent,state.sync,6) ||
               memcmp(root,state.master,6) || memcmp(&sequence,&state.sequence,sizeof(sequence)))
                reserveInvalidReason|=1;
            if(!mtkawdl::currentWindow(state,checkTime,after) ||
               after.start!=target.start || after.channel!=target.channel ||
               after.counter!=target.counter || after.presence!=target.presence)
                reserveInvalidReason|=2;
            if(now>=target.end) {
                mtkawdl::Reservation submitted={};
                bool usable=status==kIOReturnSuccess && hal->awdlGrantSnapshot(submitted) &&
                    submitted.channel==target.channel && now<submitted.deadline &&
                    (submitted.pending || submitted.granted);
                if(!usable) reserveInvalidReason|=4;
            }
            if(reserveInvalidReason) {
                ++reserveInvalidations; state.holdClock(0);
                hal->awdlAbort(); requestedChannel=grantedChannel=0;
                timer->setTimeoutMS(1); return;
            }
            if(status==kIOReturnSuccess) {
                requestedChannel=target.channel; requestDeadline=now+mtkawdl::rocResponseTimeoutUs;
            } else { requestedChannel=0; state.holdClock(0); }
        }
        timer->setTimeoutMS(1); return;
    }
    // A grant prepared for the next slot cannot produce TX or a native AW
    // during a slot whose channel is unavailable.
    if(!channel || !hal->awdlChannelReachable(channel)) { timer->setTimeoutMS(1); return; }
    if(grantedChannel!=channel) ++grantObservations;
    uint8_t nativeRole=state.syncRole;
    if(!awTimeline.open && awTimeline.begin(window,grant,now,state.sync,state.master,nativeRole,&state.sequence)) {
        state.nativeTimelineStarted=true;
        state.holdClock(window.end);
        state.holdControls();
        // Set lifecycle state before dispatch: native may synchronously call
        // back into sync/advertisers setters while receiving this event.
        notifyAvailabilityWindowStart(channel,now);
        if(stopping || !enabled || !awTimeline.open) return;
        mtkawdl::Window after={};
        mtkawdl::Reservation observed={};
        now=awdlNow();
        if(!mtkawdl::currentWindow(state,now,after) ||
           !awTimeline.matchesSchedule(state) ||
           !hal->awdlGrantSnapshot(observed) || observed.token!=grant.token ||
           observed.grantObservedAt!=grant.grantObservedAt || !observed.canTransmit(channel,now)) {
            if(awTimeline.open) notifyAvailabilityWindowEnd(now<awTimeline.radioEnd ? now : awTimeline.radioEnd);
            if(enabled && !stopping) timer->setTimeoutMS(1);
            return;
        }
    }
    if(afMode.mode==0 && (grantedChannel!=channel || now>=nextAction)) {
        uint8_t frame[2304]; size_t size;
        if(templateLength) {
            size=mtkawdl::buildActionWithTemplate(state,frame,sizeof(frame),now,
                                                 syncTemplate,templateLength);
        } else size=mtkawdl::buildAction(state,frame,sizeof(frame),now);
        if(size) frame[30]=0; // PSF; MIF (3) is scheduled separately below
        mbuf_t m=size ? makeFrame(frame,size) : NULL;
        actionStatus=m ? hal->awdlTransmit(m,channel) : kIOReturnNoMemory;
        if(actionStatus==kIOReturnSuccess) ++actionSubmitted; else ++actionFailed;
        // Native asks for PSF periods 110/440ms; honor the actual request.
        nextAction=now+uint64_t(syncParams.synchronization_frame_period)*1000;
    }
    grantedChannel=channel;
    // Native's default bitmap 0xffffffff selects all transmission slots.
    // Emit at most once per EAW and only after an actual firmware ROC grant.
    // Other bitmaps are rejected until their slot mapping is established.
    uint16_t eaw=state.counter(now)/state.presence;
    if(miTemplateLength && now>=miStart && (!miCounterValid || miCounter!=eaw)) {
        uint8_t frame[2304];
        size_t size=mtkawdl::buildActionWithTemplate(state,frame,sizeof(frame),now,miTemplate,miTemplateLength);
        mbuf_t m=size ? makeFrame(frame,size) : NULL;
        miStatus=m ? hal->awdlTransmit(m,channel) : kIOReturnNoMemory;
        if(miStatus==kIOReturnSuccess) ++miSubmitted; else ++miFailed;
        miCounter=eaw; miCounterValid=true;
    }
    unsigned pending=queueCount;
    while(pending-- && queueCount && hal->awdlCanTransmit(channel)) {
        mbuf_t m=queue[queueHead]; uint64_t time=queueTime[queueHead], expiry=queueDeadline[queueHead];
        const uint8_t *f=(const uint8_t *)mbuf_data(m);
        int peer=state.find(f+4);
        bool ready=(f[4]&1) && mtkawdl::multicastReady(state,now);
        if(peer>=0) {
            const mtkawdl::Peer &p=state.peers[peer];
            ready=p.channel(now)==channel;
        }
        // Block Ack responses are sent as built; only native MIs are rebuilt.
        bool blockAck=f[0]==0xd0 && f[24]==mtkawdl::blockAckCategory;
        bool unicastMI=f[0]==0xd0 && !blockAck;
        uint64_t &expired=blockAck ? addbaExpired : unicastMI ? umiExpired : dataTxExpired;
        uint64_t &deferred=blockAck ? addbaDeferred : unicastMI ? umiDeferred : dataTxDeferred;
        uint64_t &submitted=blockAck ? addbaSubmitted : unicastMI ? umiSubmitted : dataTxSubmitted;
        uint64_t &failed=blockAck ? addbaFailed : unicastMI ? umiFailed : dataTxFailed;
        queueHead=(queueHead+1)%64; --queueCount;
        if(now<expiry && !ready) {
            unsigned tail=(queueHead+queueCount)%64; queue[tail]=m; queueTime[tail]=time; queueDeadline[tail]=expiry; ++queueCount;
            continue;
        }
        if(now>=expiry) {
            ++expired;
            mbuf_freem(m);
        }
        else {
            if(unicastMI) {
                // Rebuild from the owned supplement at the actual RF overlap.
                // The schedule can grow while queued; do not reuse its old
                // synchronization TLV or timestamp on a later attempt.
                uint8_t refreshed[2304]; size_t length=mbuf_pkthdr_len(m);
                size_t size=0;
                if(length>=43 && length<=sizeof(refreshed) && f[40]==4) {
                    size_t prefix=43+mtkawdl::le16(f+41);
                    if(prefix<length) {
                        size=mtkawdl::buildActionWithTemplate(state,refreshed,sizeof(refreshed),now,f+prefix,length-prefix);
                        if(size) memcpy(refreshed+4,f+4,6);
                    }
                }
                mbuf_t replacement=size ? makeFrame(refreshed,size) : NULL;
                if(!replacement) {
                    if(!size) { ++umiFailed; mbuf_freem(m); continue; }
                    unsigned tail=(queueHead+queueCount)%64;
                    queue[tail]=m; queueTime[tail]=time; queueDeadline[tail]=expiry; ++queueCount;
                    ++umiDeferred; break;
                }
                mbuf_freem(m); m=replacement;
            }
            IOReturn txStatus=hal->awdlTryTransmit(&m,channel);
            if(unicastMI) umiStatus=txStatus; else if(!blockAck) dataTxStatus=txStatus;
            if(m) {
                // No firmware ownership on a transient failure. Keep the
                // original payload/sequence and its bounded queue lifetime.
                if(txStatus==kIOReturnNotFound) {
                    // One missing station must not block multicast/other peers.
                    unsigned tail=(queueHead+queueCount)%64;
                    queue[tail]=m; queueTime[tail]=time; queueDeadline[tail]=expiry; ++queueCount;
                    ++deferred;
                    continue;
                }
                queueHead=(queueHead+63)%64;
                queue[queueHead]=m; queueTime[queueHead]=time; queueDeadline[queueHead]=expiry; ++queueCount;
                ++deferred;
                // Busy/grant/ring pressure applies to the rest of this batch.
                break;
            }
            if(txStatus==kIOReturnSuccess) ++submitted; else ++failed;
        }
    }
    if(!hal->awdlCanTransmit(channel)) { requestedChannel=0; grantedChannel=0; }
    // Wake on the recorded radio end instead of overshooting it by a tick.
    uint64_t delay=awTimeline.open && awTimeline.radioEnd>now ? awTimeline.radioEnd-now : 2000;
    if(delay>2000) delay=2000;
    if(awTimeline.open && awTimeline.matchesSchedule(state)) {
        mtkawdl::Window next={};
        uint64_t prepareAt=awTimeline.active.end-awTimeline.switchLeadUS();
        if(prepareAt>now && mtkawdl::currentWindow(state,awTimeline.active.end,next) &&
           next.channel && hal->awdlChannelReachable(next.channel) &&
           !mtkawdl::grantCoversWindow(next,grant,next.start) && prepareAt-now<delay)
            delay=prepareAt-now;
    }
    timer->setTimeoutUS(static_cast<uint32_t>(delay));
}

bool AirportMtkAwdlInterface::receive(void *context,const uint8_t *f,size_t n,uint8_t channel,int8_t rssi)
{
    auto self=static_cast<AirportMtkAwdlInterface *>(context);
    if(!self || !self->enabled || self->stopping || n<24) return false;
    uint64_t now=awdlNow();
    if(f[0]==0xd0) {
        ++self->actionReceived;
        self->lastActionRSSI=rssi;
        if(mtkawdl::blockAckAction(f,n,self->state.mac)) {
            // The service loop may release the gate in firmware waits;
            // record the request and let it own the TX queue.
            if(f[25]==mtkawdl::addbaRequest && n>=24+9) {
                ++self->addbaRequests;
                int slot=-1; bool duplicate=false;
                for(unsigned i=0;i<4;++i) {
                    const auto &p=self->addbaPending[i];
                    if(p.valid && !memcmp(p.request+10,f+10,6) && p.request[26]==f[26]) duplicate=true;
                    if(!p.valid && slot<0) slot=i;
                }
                if(!duplicate && slot<0) ++self->addbaDropped;
                else if(!duplicate) {
                    memcpy(self->addbaPending[slot].request,f,24+9);
                    self->addbaPending[slot].valid=true;
                }
            } else if(f[25]==mtkawdl::delba) ++self->delbaReceived;
            return true;
        }
        if(!mtkawdl::validActionEnvelope(f,n) || !memcmp(f+10,self->state.mac,6)) {
            ++self->actionInvalid;
            if(n<=sizeof(self->rejectedAction)) {
                memcpy(self->rejectedAction,f,n); self->rejectedActionLength=n;
            }
            return true;
        }
        if(rssi<self->rssiThresholds.peer_rssi_threshold) { ++self->actionRSSIRejected; return true; }
        if(self->state.receiveAction(f,n,now)>=0) {
            self->dataPeers.observe(f+10,now);
            ++self->actionHostAccepted;
            self->setProperty("MTKAWDLLastPeer",const_cast<uint8_t *>(f+10),6);
        } else {
            ++self->actionHostRejected;
            memcpy(self->rejectedAction,f,n); self->rejectedActionLength=n;
        }
        mtkawdl::Advertisement nativeAdvertisement={};
        if(mtkawdl::parseAction(f,n,nativeAdvertisement)) {
            auto &sample=self->nativePeerSamples[self->nativePeerSampleNext++%16];
            sample.advertisement=nativeAdvertisement;
            memcpy(sample.peer,f+10,6); sample.received=now; sample.valid=true;
        }
        // Native discovery must not depend on the host parser accepting a
        // sync TLV or election policy. Native validates the bounded body itself.
        // Native AWDL action input receives the vendor action body and a
        // 72-byte host metadata header. bpfTapInput owns/returns the buffer.
        IO80211BufferPool *pool=self->getMgmtFramePool();
        IO80211Buffer *buffer=pool ? pool->getBuffer() : NULL;
        if(!buffer) { ++self->actionNoBuffer; return true; }
        void *bytes=buffer->getBytesNoCopy();
        if(bytes && buffer->getCapacity()>=n-24) {
            memcpy(bytes,f+24,n-24); buffer->setLength(n-24);
            uint8_t metadata[72]={}; memcpy(metadata,f+10,6);
            apple80211_channel c={APPLE80211_VERSION,channel,
                uint32_t(APPLE80211_C_FLAG_20MHZ | (channel<=14 ? APPLE80211_C_FLAG_2GHZ : APPLE80211_C_FLAG_5GHZ))};
            memcpy(metadata+12,&c,sizeof(c)); int32_t signal=rssi;
            // Native RX metadata RSSI is +44, noise +40 and rate +48.
            memcpy(metadata+44,&signal,4);
            // actionFrameReport reads metadata +48 as BE FW AF RX usec.
            // The host implementation uses this same monotonic clock for
            // AW events and advertisers correlation; this is not RF TSF.
            mtkawdl::putBE32(metadata+48,static_cast<uint32_t>(now));
            ++self->actionNativeDelivered;
            self->bpfTapInput(buffer,12,metadata,sizeof(metadata));
        } else { ++self->actionNoBuffer; buffer->returnBuffer(); }
        return true;
    }
    ++self->dataRxFrames;
    if(!mtkawdl::validDataFrame(f,n)) { ++self->dataRxInvalid; return true; }
    int peer=self->dataPeers.find(f+10,now);
    if(peer<0) { ++self->dataRxUnknownPeer; return true; }
    if(memcmp(f+4,self->state.mac,6) && !(f[4]&1)) {
        ++self->dataRxWrongDestination; return true;
    }
    // Validate the whole aggregate before delivering any subframe. Each
    // subframe carries its own AWDL sequence; MPDU sequence is insufficient
    // after hardware deaggregation. Keep the peer replay window per MSDU.
    size_t at=mtkawdl::dataHeaderLength(f,n);
    while(at<n) {
        mtkawdl::DataView view={};
        size_t used=mtkawdl::dataView(f,n,at,view);
        if(!used) { ++self->dataRxInvalid; break; }
        at+=used;
        if(memcmp(view.destination,self->state.mac,6) && !(view.destination[0]&1)) {
            ++self->dataRxWrongDestination; continue;
        }
        if(!self->state.acceptData(self->dataPeers.entries[peer].replayState,view.sequence)) {
            ++self->dataRxReplay; continue;
        }
        uint8_t eth[1514];
        size_t size=mtkawdl::decodeDataView(eth,sizeof(eth),view);
        ++self->dataRxDecoded;
        self->inputNative(eth,size);
    }
    return true;
}

#define AWDL_GET(name,type,field) IOReturn AirportMtkAwdlInterface::get##name(type *d) { if(!d) return kIOReturnBadArgument; *d=field; return kIOReturnSuccess; }
#define AWDL_SET(name,type,field) IOReturn AirportMtkAwdlInterface::set##name(type *d) { if(!d || d->version>APPLE80211_VERSION) return kIOReturnBadArgument; field=*d; return kIOReturnSuccess; }
AWDL_GET(AWDL_SYNC_PARAMS,apple80211_awdl_sync_params,syncParams)
AWDL_GET(AWDL_ELECTION_ID,apple80211_awdl_election_id,electionId)
AWDL_SET(AWDL_ELECTION_ID,apple80211_awdl_election_id,electionId)
AWDL_GET(AWDL_PRESENCE_MODE,apple80211_awdl_presence_mode,presence)
AWDL_GET(AWDL_AF_TX_MODE,apple80211_awdl_af_tx_mode,afMode)
AWDL_GET(AWDL_ELECTION_RSSI_THRESHOLDS,apple80211_awdl_election_rssi_thresholds,rssiThresholds)
AWDL_GET(AWDL_MIN_RATE,apple80211_awdl_min_rate,minRate)
AWDL_GET(AWDL_EXTENSION_STATE_MACHINE_PARAMETERS,apple80211_awdl_extension_state_machine_parameter,extension)
IOReturn AirportMtkAwdlInterface::setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter *d) {
    if(!d || d->version>APPLE80211_VERSION || d->unk1>255 || d->unk2>255 || d->unk3>255 || d->unk4>255) return kIOReturnBadArgument;
    state.minExtension[0]=d->unk1; state.minExtension[1]=d->unk2;
    state.minExtension[2]=d->unk3; state.minExtension[3]=d->unk4;
    extension=*d; return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::setAWDL_MIN_RATE(apple80211_awdl_min_rate *d) {
    if(!d || d->version>APPLE80211_VERSION) return kIOReturnBadArgument;
    if(d->min_rate!=12) return kIOReturnUnsupported;
    minRate=*d; return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::setAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode *d) {
    if(!d || d->version>APPLE80211_VERSION) return kIOReturnBadArgument;
    setProperty("MTKAWDLRequestedAFMode",static_cast<uint64_t>(d->mode),32);
    // Native setAFTXMode maps firmware 0 to allow and 3 to suppress PSFs.
    // OOB Master Indications remain independently scheduled in either mode.
    if(d->mode!=0 && d->mode!=3) return kIOReturnUnsupported;
    afMode=*d;
    setProperty("MTKAWDLAFMode",static_cast<uint64_t>(d->mode),32);
    nextAction=0;
    if(enabled && !stopping && timer) timer->setTimeoutMS(1);
    return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::setAWDL_OOB_REQUEST(apple80211_awdl_oob_request *d)
{
    if(!d || d->version>APPLE80211_VERSION) return kIOReturnBadArgument;
    ++umiRequests;
    setProperty("MTKAWDLUMIRequests",umiRequests,64);
    if(stopping || !enabled) return kIOReturnNotReady;
    const uint8_t *raw=reinterpret_cast<const uint8_t *>(d);
    setProperty("MTKAWDLUMIRequestLength",static_cast<uint64_t>(d->data_len),32);
    setProperty("MTKAWDLUMIRequestOptions",static_cast<uint64_t>(mtkawdl::le16(raw+20)),32);
    // Tahoe sendUnicastMI supplies destination +4, BSSID +22, borrowed
    // supplemental TLVs +48 and length +40. Its ordinary option word is 6.
    // Native selects option 14 in a special recent/2.4-GHz peer path. Its
    // RF semantics are not established; leave that mode unsupported.
    if(mtkawdl::le16(raw+20)!=6 || d->unk4 || d->unk3 ||
       memcmp(raw+22,mtkawdl::bssid,6)) return kIOReturnUnsupported;
    if(!mtkawdl::unicast(raw+4) || !memcmp(raw+4,state.mac,6) || !d->data ||
       !mtkawdl::validTemplateTLVs(d->data,d->data_len)) return kIOReturnBadArgument;
    uint64_t now=awdlNow();
    if(dataPeers.find(raw+4,now)<0 || state.find(raw+4)<0) return kIOReturnNotFound;
    if(queueCount>=64) return kIOReturnNoResources;
    if(!state.radioScheduleReady()) return kIOReturnNotReady;
    uint8_t frame[2304];
    size_t size=mtkawdl::buildActionWithTemplate(state,frame,sizeof(frame),now,d->data,d->data_len);
    if(!size) return kIOReturnBadArgument;
    memcpy(frame+4,raw+4,6);
    mbuf_t m=makeFrame(frame,size);
    if(!m) return kIOReturnNoMemory;
    unsigned tail=(queueHead+queueCount)%64;
    queue[tail]=m; queueTime[tail]=now;
    queueDeadline[tail]=now+mtkawdl::dataQueueLifetimeUs(state,raw+4); ++queueCount; ++umiQueued;
    // makeFrame copied the borrowed payload before returning to native.
    if(timer) timer->setTimeoutMS(1);
    return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::setAWDL_OOB_AUTO_REQUEST(apple80211_awdl_oob_request *d)
{
    if(!d || d->version>APPLE80211_VERSION) return kIOReturnBadArgument;
    // Tahoe passes supplemental MI TLVs through a borrowed kernel pointer at
    // +48. Copy them synchronously, before this ioctl returns to its owner.
    setProperty("MTKAWDLRequestedMILength",static_cast<uint64_t>(d->data_len),32);
    setProperty("MTKAWDLRequestedMIPeriod",static_cast<uint64_t>(d->unk4),32);
    setProperty("MTKAWDLRequestedMIDelay",static_cast<uint64_t>(d->unk3),32);
    if(stopping) return kIOReturnNotReady;
    // +16 is a bitmap (native setMiBitmap), +12 is the randomized start
    // delay in microseconds. Limit support to the observed broadcast/all-slot
    // request, so a different bitmap is never interpreted as an interval.
    const uint8_t *raw=reinterpret_cast<const uint8_t *>(d);
    static const uint8_t broadcast[6]={255,255,255,255,255,255};
    if(memcmp(raw+4,broadcast,6)) return kIOReturnUnsupported;
    if(d->unk4!=0 && d->unk4!=0xffffffffU) return kIOReturnUnsupported;
    if(!d->unk4 || !d->data_len) {
        miTemplateLength=0; miCounterValid=false;
        setProperty("MTKAWDLMITemplateLength",static_cast<uint64_t>(0),32);
        return kIOReturnSuccess;
    }
    if(!d->data || d->data_len>sizeof(miTemplate) || d->unk3>1000000 ||
       !mtkawdl::validTemplateTLVs(d->data,d->data_len)) return kIOReturnBadArgument;
    bool same=miTemplateLength==d->data_len && !memcmp(miTemplate,d->data,d->data_len);
    memcpy(miTemplate,d->data,d->data_len); miTemplateLength=d->data_len;
    if(!same) { miStart=awdlNow()+d->unk3; miCounterValid=false; }
    setProperty("MTKAWDLMITemplateLength",static_cast<uint64_t>(miTemplateLength),32);
    setProperty("MTKAWDLMIBitmap",static_cast<uint64_t>(d->unk4),32);
    if(enabled && timer) timer->setTimeoutMS(1);
    return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::setAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds *d) {
    if(!d || d->version>APPLE80211_VERSION || d->peer_rssi_threshold<-128 || d->peer_rssi_threshold>0)
        return kIOReturnBadArgument;
    // Only the discovery threshold at +0x10 is used by this host engine.
    if(d->unk1 || d->unk2 || d->unk3) return kIOReturnUnsupported;
    rssiThresholds=*d; return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::setAWDL_AWDL_ADVERTISERS(awdl_peer_advert_add *input)
{
    if(!input || stopping) return input ? kIOReturnNotReady : kIOReturnBadArgument;
    uint8_t raw[44]; memcpy(raw,input,sizeof(raw));
    ++advertisersRequests;
    setProperty("MTKAWDLAdvertisersRequests",advertisersRequests,64);
    setProperty("MTKAWDLAdvertisersPayload",raw,sizeof(raw));
    uint16_t counter=mtkawdl::le16(raw), down=mtkawdl::le16(raw+2);
    uint32_t timestamp=mtkawdl::be32(raw+20);
    IOReturn status=kIOReturnNotReady;
    const NativePeerSample *matched=NULL;
    uint64_t now=awdlNow();
    if(!mtkawdl::unicast(raw+32) || !mtkawdl::unicast(raw+38)) status=kIOReturnBadArgument;
    else if(mtkawdl::le16(raw+12)!=16 || !mtkawdl::validPresence(raw[16]) ||
            raw[15] || raw[17] || raw[19] || mtkawdl::le16(raw+10)!=16) status=kIOReturnUnsupported;
    else {
        for(unsigned i=0;i<16;++i) {
            const auto &sample=nativePeerSamples[i];
            if(sample.valid && now>=sample.received && now-sample.received<=mtkawdl::peerLifetimeUs &&
               static_cast<uint32_t>(sample.received)==timestamp && !memcmp(sample.peer,raw+38,6) &&
               !memcmp(sample.advertisement.master,raw+32,6) && sample.advertisement.counter==counter &&
               sample.advertisement.down==down && sample.advertisement.presence==raw[16]) { matched=&sample; break; }
        }
        if(matched) {
            // The native record lacks a channel sequence and election counter.
            // Recover both from the exact RX frame that native selected.
            NativePeerSample sample=*matched;
            const auto &a=sample.advertisement;
            uint64_t elapsed=uint64_t(unsigned(a.presence)*a.period-a.down)*1024;
            if(sample.received>=elapsed) {
                int peer=state.find(sample.peer);
                if(peer>=0) {
                    uint64_t epoch=sample.received-elapsed;
                    uint16_t base=counter-counter%a.presence;
                    bool changed=!state.nativeParentSelected || memcmp(state.nativeParent,sample.peer,6) ||
                        memcmp(state.master,a.master,6) || state.presence!=state.ownPresence ||
                        memcmp(&state.sequence,&state.ownSequence,sizeof(state.sequence));
                    // Native can replay a cached PSF after newer RX arrived.
                    // Keep the newest observation of this same clock tree;
                    // replacing only epoch but retaining the newer seen time
                    // makes an old sample look fresh and reverses discipline.
                    auto &p=state.peers[peer];
                    if(p.seen<=sample.received || memcmp(p.advertisement.master,a.master,6)) {
                        p.advertisement=a; p.epoch=epoch; p.baseCounter=base;
                        p.seen=sample.received;
                    }
                    state.selectNativeParent(sample.peer,a.master); state.elect(now);
                    if(changed && enabled && !state.controlsHeld) {
                        hal->awdlAbort(); requestedChannel=grantedChannel=0;
                        // No announced AW remains. The aborted reservation's
                        // future edge must not hold acquisition of this parent.
                        state.holdClock(0); state.elect(awdlNow());
                        nextAction=0; miCounterValid=false; timer->setTimeoutMS(1);
                    }
                    status=kIOReturnSuccess;
                    setProperty("MTKAWDLAdvertisersPeer",const_cast<uint8_t *>(sample.peer),6);
                    setProperty("MTKAWDLAdvertisersRoot",const_cast<uint8_t *>(a.master),6);
                    setProperty("MTKAWDLAdvertisersMatchedRXAgeUS",now-sample.received,64);
                    setProperty("MTKAWDLAdvertisersStage",changed ? "applied" : "duplicate");
                }
            }
        }
    }
    if(status!=kIOReturnSuccess) setProperty("MTKAWDLAdvertisersStage",matched ? "peer-unavailable" : "unmatched-rx");
    setProperty("MTKAWDLAdvertisersStatus",static_cast<uint64_t>(status),32);
    return status;
}

IOReturn AirportMtkAwdlInterface::getAWDL_SYNC_STATE(apple80211_awdl_sync_state *d) {
    if(!d) return kIOReturnBadArgument;
    *d=syncState; memcpy(d->master,state.master,6); return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::setAWDL_SYNC_STATE(apple80211_awdl_sync_state *d) {
    if(!d || d->version>APPLE80211_VERSION) return kIOReturnBadArgument;
    if(!d->state) {
        IOReturn status=setSync(false);
        if(status==kIOReturnSuccess) syncState=*d;
        return status;
    }
    // Native's warmup path can send opmode without an advertiser record.
    // BCM forwards mode bit 0x20 to firmware to permit immediate-master
    // synchronization and AW counter adoption. Authorize one real RX clock
    // acquisition when entering that mode or selecting another parent.
    const uint8_t *requestedMaster=state.controlsHeld ? state.pendingControls.forced : state.forcedSync;
    bool same=state.requestedRole()==d->state && !memcmp(requestedMaster,d->master,6);
    if(!state.setSyncRole(d->state,d->master)) return kIOReturnBadArgument;
    bool warmup=(d->mode_flags&0x20) && memcmp(d->master,state.mac,6);
    bool enterWarmup=warmup && (!(syncState.mode_flags&0x20) ||
        memcmp(syncState.master,d->master,6));
    bool select=warmup && (enterWarmup || !state.requestedParentIs(d->master));
    if(select) state.selectNativeParent(d->master,nullptr,enterWarmup);
    bool local=!memcmp(d->master,state.mac,6);
    bool clear=local && (state.controlsHeld ? state.pendingControls.parentSelected : state.nativeParentSelected);
    if(clear) state.clearNativeParent();
    setProperty("MTKAWDLSyncModeFlags",static_cast<uint64_t>(d->mode_flags),32);
    setProperty("MTKAWDLSyncFlags",static_cast<uint64_t>(d->flags),32);
    // Native regularly refreshes opmode/flags. A duplicate must not cancel
    // a live or pending ROC, or close an otherwise valid AW.
    if(same && !select && !clear) { syncState=*d; return kIOReturnSuccess; }
    if(stopping) return kIOReturnNotReady;
    syncState=*d; state.elect(awdlNow());
    setProperty("MTKAWDLSyncRole",d->state,32);
    if(enabled && !state.controlsHeld) {
        hal->awdlAbort(); requestedChannel=grantedChannel=0;
        // No announced AW remains after abort. Its pending reservation must
        // not keep the explicitly authorized warmup clock held in the future.
        state.holdClock(0); state.elect(awdlNow());
        nextAction=0; timer->setTimeoutMS(1);
    }
    return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::setAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode *d) {
    if(!d || d->version>APPLE80211_VERSION) return kIOReturnBadArgument;
    if(!mtkawdl::validPresence(d->mode)) return kIOReturnUnsupported;
    state.setPresence(d->mode,awdlNow()); channelSequence.step_count=d->mode-1;
    presence=*d; return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::setAWDL_SYNC_PARAMS(apple80211_awdl_sync_params *d)
{
    if(!d || d->version>APPLE80211_VERSION) return kIOReturnBadArgument;
    if(d->availability_window_length!=16 || d->availability_window_period!=16 ||
       d->extension_length!=16 || d->non_election_master>1 ||
       (d->synchronization_frame_period!=110 && d->synchronization_frame_period!=440))
        return kIOReturnUnsupported;
    // A period update must not reset an already selected synchronization peer.
    if(state.requestedRole()!=2) state.setSyncRole(d->non_election_master ? 3 : 1,state.mac);
    state.psfPeriod=d->synchronization_frame_period;
    state.elect(awdlNow()); syncParams=*d; nextAction=0;
    setProperty("MTKAWDLPSFPeriodMS",static_cast<uint64_t>(state.psfPeriod),32);
    return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *d) { if(!d) return kIOReturnBadArgument; *d={APPLE80211_VERSION,0,uint32_t(enabled)}; return kIOReturnSuccess; }
IOReturn AirportMtkAwdlInterface::setAWDL_CONFIG(apple80211_awdl_config_request *d)
{
    // Tahoe uses this request to commit the peer-manager configuration. The
    // request is opaque in the public headers; accept the common versioned
    // prefix and let the individual AWDL setters validate their payloads.
    if(!d || stopping) return stopping ? kIOReturnNotReady : kIOReturnBadArgument;
    uint32_t version=0;
    memcpy(&version,d,sizeof(version));
    if(version>APPLE80211_VERSION) return kIOReturnBadArgument;
    setProperty("MTKAWDLConfigured",true);
    setProperty("MTKAWDLConfigStatus",static_cast<uint64_t>(kIOReturnSuccess),32);
    return kIOReturnSuccess;
}

IOReturn AirportMtkAwdlInterface::setAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *d) { if(!d || d->version>APPLE80211_VERSION || d->enabled>1) return kIOReturnBadArgument; return setSync(d->enabled); }
IOReturn AirportMtkAwdlInterface::getSSID(apple80211_ssid_data *d) { if(!d) return kIOReturnBadArgument; bzero(d,sizeof(*d)); d->version=APPLE80211_VERSION; return kIOReturnSuccess; }
IOReturn AirportMtkAwdlInterface::getBSSID(apple80211_bssid_data *d) { if(!d) return kIOReturnBadArgument; bzero(d,sizeof(*d)); d->version=APPLE80211_VERSION; memcpy(&d->bssid,mtkawdl::bssid,6); return kIOReturnSuccess; }
IOReturn AirportMtkAwdlInterface::getSTATE(apple80211_state_data *d) { if(!d) return kIOReturnBadArgument; *d={APPLE80211_VERSION,enabled ? APPLE80211_S_RUN : APPLE80211_S_INIT}; return kIOReturnSuccess; }
IOReturn AirportMtkAwdlInterface::getCHANNEL(apple80211_channel_data *d) { if(!d) return kIOReturnBadArgument; bzero(d,sizeof(*d)); d->version=APPLE80211_VERSION; d->channel={APPLE80211_VERSION,state.channel(awdlNow()),APPLE80211_C_FLAG_20MHZ}; return kIOReturnSuccess; }
IOReturn AirportMtkAwdlInterface::setAWDL_BSSID(apple80211_awdl_bssid *d) { return d && !memcmp(d->bssid,mtkawdl::bssid,6) ? kIOReturnSuccess : kIOReturnUnsupported; }
IOReturn AirportMtkAwdlInterface::getAWDL_ELECTION_METRIC(apple80211_awdl_election_metric *d) { if(!d) return kIOReturnBadArgument; *d={APPLE80211_VERSION,state.metric}; return kIOReturnSuccess; }
IOReturn AirportMtkAwdlInterface::setAWDL_ELECTION_METRIC(apple80211_awdl_election_metric *d) { if(!d) return kIOReturnBadArgument; state.metric=d->metric; return kIOReturnSuccess; }
IOReturn AirportMtkAwdlInterface::getPEER_CACHE_MAXIMUM_SIZE(apple80211_peer_cache_maximum_size *d) { if(!d) return kIOReturnBadArgument; *d={APPLE80211_VERSION,hal->awdlPeerCapacity()}; return kIOReturnSuccess; }
IOReturn AirportMtkAwdlInterface::getAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel *d) { if(!d) return kIOReturnBadArgument; *d={APPLE80211_VERSION,state.ownSequence.channels[0]}; return kIOReturnSuccess; }
IOReturn AirportMtkAwdlInterface::setAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel *d) {
    if(!d || d->version>APPLE80211_VERSION || d->master_channel>165 || !hal->awdlChannelAllowed(d->master_channel)) return kIOReturnBadArgument;
    // Native owns the effective sequence; stage social channel updates when
    // a granted AW is still using the previous schedule.
    state.setSocialChannel(0,d->master_channel,awdlNow());
    channelSequence.seqs[0].channel=d->master_channel;
    return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::getAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel *d) { if(!d) return kIOReturnBadArgument; *d={APPLE80211_VERSION,secondaryMasterChannel}; return kIOReturnSuccess; }
IOReturn AirportMtkAwdlInterface::setAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel *d) {
    if(!d || d->version>APPLE80211_VERSION) return kIOReturnBadArgument;
    if(d->secondary_master_channel && (d->secondary_master_channel>165 ||
       !hal->awdlChannelAllowed(d->secondary_master_channel))) return kIOReturnBadArgument;
    secondaryMasterChannel=d->secondary_master_channel;
    // Keep a second discovery channel until the complete native sequence arrives.
    state.setSocialChannel(1,secondaryMasterChannel,awdlNow());
    channelSequence.seqs[1].channel=secondaryMasterChannel;
    setProperty("MTKAWDLSecondaryMasterChannel",static_cast<uint64_t>(secondaryMasterChannel),32);
    return kIOReturnSuccess;
}
AWDL_GET(AWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,apple80211_awdl_sync_channel_sequence,channelSequence)
IOReturn AirportMtkAwdlInterface::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence *d)
{
    if(!d || d->version>APPLE80211_VERSION || !d->length || d->length>16 ||
       d->encoding>1 || !mtkawdl::validPresence(unsigned(d->step_count)+1) || d->duplicate_count)
        return kIOReturnUnsupported;
    mtkawdl::Sequence s={}; s.count=d->length; s.step=d->step_count;
    for(unsigned i=0;i<s.count;++i) { unsigned c=d->seqs[i].channel; if(c && (c>165 || !hal->awdlChannelAllowed(c))) return kIOReturnBadArgument; s.channels[i]=c; }
    state.setSequence(s,awdlNow());
    channelSequence=*d;
    setProperty("MTKAWDLNativeChannelSequence",state.ownSequence.channels,state.ownSequence.count);
    return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::setAWDL_SYNC_FRAME_TEMPLATE(apple80211_awdl_sync_frame_template *d)
{
    if(!d || d->version>APPLE80211_VERSION || !d->payload || !d->payload_len || d->payload_len>sizeof(syncTemplate)) return kIOReturnBadArgument;
    const uint8_t *p=(const uint8_t *)d->payload;
    if(!mtkawdl::validTemplateTLVs(p,d->payload_len)) return kIOReturnBadArgument;
    memcpy(syncTemplate,p,d->payload_len); templateLength=d->payload_len;
    setProperty("MTKAWDLNativeTemplateLength",static_cast<uint64_t>(templateLength),32);
    return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES(apple80211_awdl_cap *d)
{
    if(!d) return kIOReturnBadArgument;
    *d={APPLE80211_VERSION,0}; // secondary payload is not implemented
    return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::getAWDL_DEVICE_CAPABILITIES(apple80211_awdl_device_capabilities *d)
{
    if(!d) return kIOReturnBadArgument;
    *d={APPLE80211_VERSION,0,10,hal->awdlPeerCapacity(),mtkawdl::slots};
    return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::getAWDL_ELECTION_PARAMS(apple80211_awdl_election_params *d)
{
    if(!d) return kIOReturnBadArgument;
    bzero(d,sizeof(*d)); d->version=APPLE80211_VERSION;
    d->election_id=electionId.election_id; d->distance=state.distance;
    memcpy(d->master,state.master,6); d->master_metric=state.masterMetric;
    d->self_metric=state.metric; return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::setAWDL_ELECTION_PARAMS(apple80211_awdl_election_params *d)
{
    if(!d || d->version>APPLE80211_VERSION || d->distance>10 || !mtkawdl::unicast(d->master)) return kIOReturnBadArgument;
    // The host election engine supplies these values. RF synchronization is
    // still learned from a received master frame, never guessed from an ioctl.
    electionId.election_id=d->election_id; state.metric=d->self_metric;
    if(!memcmp(d->master,state.mac,6)) {
        if(d->distance) return kIOReturnBadArgument;
        state.setSyncRole(syncParams.non_election_master ? 3 : 1,state.mac);
    } else {
        // Select a received candidate carrying this elected master. Never
        // fabricate its AW clock when the host request arrives first.
        const uint8_t *candidate=d->master;
        for(unsigned i=0;i<mtkawdl::maxPeers;++i)
            if(state.peers[i].used && !memcmp(state.peers[i].advertisement.master,d->master,6)) {
                candidate=state.peers[i].mac; break;
            }
        state.setSyncRole(2,candidate);
    }
    state.elect(awdlNow()); return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::setPEER_CACHE_CONTROL(apple80211_peer_cache_control *d)
{
    if(!d || stopping || !enabled) return kIOReturnNotReady;
    // Tahoe supplies a 0x1d0-byte request. Only the independently verified
    // version/op/address prefix is needed for our raw, unencrypted peer path.
    auto p=reinterpret_cast<uint8_t *>(d);
    uint32_t version=mtkawdl::le32(p), operation=mtkawdl::le32(p+4);
    if(version>APPLE80211_VERSION || !mtkawdl::unicast(p+8)) return kIOReturnBadArgument;
    if(operation!=0 && operation!=2 && operation!=6) return kIOReturnUnsupported;
    uint8_t address[6]; memcpy(address,p+8,6);
    if(operation!=2 && !dataPeers.canObserve(address,awdlNow())) return kIOReturnNoResources;
    retain();
    int member=dataPeers.find(address,awdlNow());
    bool trafficOwns=member>=0 && dataPeers.entries[member].traffic;
    IOReturn status=operation==2 && trafficOwns ? kIOReturnSuccess : hal->awdlPeer(address,operation!=2);
    if(status!=kIOReturnSuccess || stopping || !enabled) {
        if(status==kIOReturnSuccess) status=kIOReturnNotReady;
        release(); return status;
    }
    uint8_t completion[0x1d0]={}; mtkawdl::put32(completion,APPLE80211_VERSION);
    if(!dataPeers.setCached(address,operation!=2,awdlNow())) {
        // Firmware waits release the gate; another request may have filled
        // the bounded cache before this one returned. Never acknowledge a
        // native cache add that the receive path cannot authorize.
        hal->awdlPeer(address,false); release(); return kIOReturnNoResources;
    }
    if(operation==2 && !trafficOwns) {
        // Firmware station removal invalidates discovery's local tracking too.
        for(unsigned i=0;i<3;++i)
            if(!memcmp(discoveryTxPeers[i],address,6)) bzero(discoveryTxPeers[i],6);
    }
    mtkawdl::put32(completion+4,operation==2 ? 3 : 1); memcpy(completion+8,address,6);
    awdlControllerMessage(owner,this,0x33,completion,sizeof(completion),true);
    release(); return kIOReturnSuccess;
}
IOReturn AirportMtkAwdlInterface::setAWDL_PEER_TRAFFIC_REGISTRATION(apple80211_awdl_peer_traffic_registration *d)
{
    if(!d || d->version>APPLE80211_VERSION || d->active>1 || d->name_len>sizeof(d->name) ||
       (d->active && !mtkawdl::unicast(d->addr))) return kIOReturnBadArgument;
    uint64_t now=awdlNow();
    if(d->active && !dataPeers.setTraffic(d->addr,true,now)) return kIOReturnNoResources;
    if(trafficActive && (!d->active || memcmp(trafficPeer,d->addr,6)))
        dataPeers.setTraffic(trafficPeer,false,now);
    memcpy(trafficPeer,d->addr,6); trafficActive=d->active;
    // Expedite queued traffic during an active Continuity session.
    if(enabled && trafficActive) timer->setTimeoutMS(1);
    return kIOReturnSuccess;
}
#endif
