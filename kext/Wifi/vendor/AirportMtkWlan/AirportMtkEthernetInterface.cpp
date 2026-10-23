//
//  AirportMtkEthernetInterface.cpp
//  AirportMtkWlan-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#include "AirportMtkEthernetInterface.hpp"

#include <sys/_if_ether.h>
#include <sys/sockio.h>
#include <net80211/ieee80211_var.h>
#include "AirportMtkAwdlInterface.hpp"
#ifdef AIRPORT_WCL
#include <IOKit/IOCommandGate.h>
extern IOCommandGate *_fCommandGate;
#endif
#ifdef MTK_NATIVE_AWDL
// Native Skywalk prepareBSDInterface borrows the compat bridge's ifnet.
// The matching finalizer disables it and clears that borrowed native handle.
extern void awdlFinalizeBSDInterface(IOSkywalkNetworkInterface*,ifnet_t,unsigned int)
    __asm__("__ZN25IOSkywalkNetworkInterface20finalizeBSDInterfaceEP7__ifnetj");
#endif

#define super IOEthernetInterface
OSDefineMetaClassAndStructors(AirportMtkEthernetInterface, IOEthernetInterface);

const char *AirportMtkEthernetInterface::getNamePrefix() const
{
#ifdef MTK_NATIVE_AWDL
    // The native AWDL object owns the awdl0 BSD name. Keep the controller's
    // compatibility interface on a distinct name so it cannot collide with
    // native ifnet allocation during PeerManager startup.
    if (interface && interface->getInterfaceRole()==6) return "awdlc";
#endif
    return "en";
}

bool AirportMtkEthernetInterface::
initWithSkywalkInterfaceAndProvider(IONetworkController *controller, IO80211SkywalkInterface *interface)
{
    this->interface=NULL;
    this->isAttach=false;
    this->retainsInterface=false;
    bool ret = super::init(controller);
    if (ret) {
        this->interface = interface;
#ifdef MTK_NATIVE_AWDL
        if(OSDynamicCast(AirportMtkAwdlInterface,interface)) {
            interface->retain();
            retainsInterface=true;
        }
#endif
    }
    return ret;
}

void AirportMtkEthernetInterface::free()
{
    // Data-link teardown can outlive the controller's interface pointer.
    // Keep the native owner alive until this bridge itself is released.
    if(retainsInterface) OSSafeReleaseNULL(interface);
    super::free();
}

SInt32 AirportMtkEthernetInterface::performCommand(IONetworkController *controller,
    unsigned long command,void *arg0,void *arg1)
{
#ifdef MTK_NATIVE_AWDL
    if(auto awdl=OSDynamicCast(AirportMtkAwdlInterface,interface)) {
        if(!getIfnet()) return ENXIO;
        // The AWDL peer manager and socket Apple80211 ioctls belong to the
        // virtual interface. Ethernet's default handler would instead enable
        // the infrastructure controller and reject those private commands.
        SInt32 status=awdl->processBSDCommand(getIfnet(),(unsigned int)command,arg1);
        if(status==0 && command==SIOCSIFFLAGS) {
            // Skywalk's flags handler changes multicast/promiscuous filters;
            // native Skywalk enable requests normally arrive separately. The
            // compat bridge must forward that administrative transition too.
            bool up=(ifnet_flags(getIfnet()) & IFF_UP)!=0;
            status=awdl->setInterfaceEnable(up);
            if(status==0 && up) startOutputThread();
            else { stopOutputThread(); flushOutputQueue(); }
        }
        return status;
    }
#endif
    return super::performCommand(controller,command,arg0,arg1);
}

#ifdef AIRPORT_WCL
IOReturn AirportMtkEthernetInterface::prepareBSDGated(OSObject *, void *arg0, void *, void *, void *)
{
    auto self = static_cast<AirportMtkEthernetInterface *>(arg0);
    auto controller = OSDynamicCast(IO80211Controller, self->getController());
    auto queue = controller ? controller->getWorkQueue() : nullptr;
    if (!queue || !queue->inGate() || queue->onThread()) {
        return kIOReturnNotPermitted;
    }
    return self->interface->prepareBSDInterface(self->getIfnet(), 0);
}
#endif

IOReturn AirportMtkEthernetInterface::
attachToDataLinkLayer( IOOptionBits options, void *parameter )
{
    XYLog("%s\n", __FUNCTION__);
    char infName[IFNAMSIZ];
#ifdef AIRPORT_WCL
    if (!setInterfaceSubType(3)) {
        return kIOReturnNotReady;
    }
#endif
    IOReturn ret = super::attachToDataLinkLayer(options, parameter);
    if (ret == kIOReturnSuccess && interface) {
#ifdef MTK_NATIVE_AWDL
        if (auto awdl=OSDynamicCast(AirportMtkAwdlInterface,interface))
            awdl->setBSDInterface(getIfnet());
#endif
        UInt8 builtIn = 0;
        IOEthernetAddress addr;
        interface->setProperty("built-in", OSData::withBytes(&builtIn, sizeof(builtIn)));
        snprintf(infName, sizeof(infName), "%s%u", ifnet_name(getIfnet()), ifnet_unit(getIfnet()));
        // The native AWDL peer manager resolves awdl0 itself. Do not publish
        // the compatibility bridge as awdl0, or native ifnet allocation fails.
#ifdef MTK_NATIVE_AWDL
        if(auto awdl=OSDynamicCast(AirportMtkAwdlInterface,interface))
            awdl->setBSDName("awdl");
#endif
        interface->setProperty("IOInterfaceName", OSString::withCString(infName));
        interface->setProperty(kIOInterfaceUnit, OSNumber::withNumber(ifnet_unit(getIfnet()), 8));
        interface->setProperty(kIOInterfaceNamePrefix, OSString::withCString(ifnet_name(getIfnet())));
        IOReturn addressStatus;
#ifdef MTK_NATIVE_AWDL
        if(auto awdl=OSDynamicCast(AirportMtkAwdlInterface,interface))
            addressStatus=awdl->getHardwareAddress(reinterpret_cast<ether_addr *>(&addr));
        else
#endif
            addressStatus=OSDynamicCast(IOEthernetController, getController())->getHardwareAddress(&addr);
        if(addressStatus == kIOReturnSuccess)
            setProperty(kIOMACAddress,  (void *) &addr,
                        kIOEthernetAddressSize);
#ifdef AIRPORT_WCL
        ret = _fCommandGate ? _fCommandGate->runAction(prepareBSDGated, this) : kIOReturnNotReady;
        if (ret != kIOReturnSuccess) {
#ifdef MTK_NATIVE_AWDL
            if(auto awdl=OSDynamicCast(AirportMtkAwdlInterface,interface)) awdl->setBSDInterface(NULL);
#endif
            super::detachFromDataLinkLayer(options, parameter);
            isAttach = false;
            return ret;
        }
        isAttach = true;
        interface->registerService();
#else
        interface->registerService();
        interface->prepareBSDInterface(getIfnet(), 0);
#endif
//        ret = bpf_attach(getIfnet(), DLT_RAW, 0x48, &AirportMtkEthernetInterface::bpfOutputPacket, &AirportMtkEthernetInterface::bpfTap);
    }
#ifdef AIRPORT_WCL
    isAttach = ret == kIOReturnSuccess;
#else
    isAttach = true;
#endif
    return ret;
}

void AirportMtkEthernetInterface::
detachFromDataLinkLayer(IOOptionBits options, void *parameter)
{
#ifdef MTK_NATIVE_AWDL
    if(auto awdl=OSDynamicCast(AirportMtkAwdlInterface,interface)) awdl->setBSDInterface(NULL);
    if(isAttach && OSDynamicCast(AirportMtkAwdlInterface,interface))
        awdlFinalizeBSDInterface(interface,getIfnet(),0);
#endif
    super::detachFromDataLinkLayer(options, parameter);
    isAttach = false;
}

/**
 Add another hack to fake that the provider is IOSkywalkNetworkInterface, to avoid skywalkfamily instance cast panic.
 */
IOService *AirportMtkEthernetInterface::
getProvider() const
{
    return isAttach ? this->interface : super::getProvider();
}

errno_t AirportMtkEthernetInterface::
bpfOutputPacket(ifnet_t interface, u_int32_t data_link_type, mbuf_t packet)
{
    XYLog("%s data_link_type: %d\n", __FUNCTION__, data_link_type);
    AirportMtkEthernetInterface *networkInterface = (AirportMtkEthernetInterface *)ifnet_softc(interface);
    return networkInterface->enqueueOutputPacket(packet);
}

errno_t AirportMtkEthernetInterface::
bpfTap(ifnet_t interface, u_int32_t data_link_type, bpf_tap_mode direction)
{
    XYLog("%s data_link_type: %d direction: %d\n", __FUNCTION__, data_link_type, direction);
    return 0;
}

bool AirportMtkEthernetInterface::
setLinkState(IO80211LinkState state)
{
    if(!getIfnet()) return false;
    if (state == kIO80211NetworkLinkUp) {
        ifnet_set_flags(getIfnet(), ifnet_flags(getIfnet()) | (IFF_UP | IFF_RUNNING), (IFF_UP | IFF_RUNNING));
    } else {
        ifnet_set_flags(getIfnet(), 0, IFF_UP | IFF_RUNNING);
    }
    return true;
}

#ifdef AIRPORT_WCL
// Skywalk's compat input path expects one contiguous mbuf. A-MSDU subframes
// from mbuf_split() can arrive as a chain, so copy those into a single buffer.
static mbuf_t normalizeInputPacket(mbuf_t packet, UInt32 length)
{
    const size_t packetLength = length ? length : mbuf_pkthdr_len(packet);
    if (!(mbuf_flags(packet) & MBUF_PKTHDR) || packetLength < ETHER_HDR_LEN) {
        mbuf_freem(packet);
        return NULL;
    }
    if (!mbuf_next(packet) && mbuf_len(packet) >= packetLength)
        return packet;

    unsigned int maxChunks = 1;
    mbuf_t linear = NULL;
    if (mbuf_allocpacket(MBUF_DONTWAIT, packetLength + ETHER_ALIGN, &maxChunks, &linear)) {
        mbuf_freem(packet);
        return NULL;
    }
    UInt8 *data = static_cast<UInt8 *>(mbuf_data(linear)) + ETHER_ALIGN;
    if (mbuf_copydata(packet, 0, packetLength, data) ||
        mbuf_copy_pkthdr(linear, packet) ||
        mbuf_setdata(linear, data, packetLength)) {
        mbuf_freem(linear);
        mbuf_freem(packet);
        return NULL;
    }
    mbuf_pkthdr_setlen(linear, packetLength);
    mbuf_pkthdr_setheader(linear, NULL);
    mbuf_freem(packet);
    return linear;
}
#endif

UInt32 AirportMtkEthernetInterface::
inputPacket(mbuf_t packet, UInt32 length, IOOptionBits options, void *param)
{
#ifdef AIRPORT_WCL
    if (!packet)
        return 0;
    if (!getIfnet()) {
        mbuf_freem(packet);
        return 0;
    }
    const bool directInput = param == packet;
    packet = normalizeInputPacket(packet, length);
    if (!packet) {
        IONetworkData *data = getParameter(kIONetworkStatsKey);
        IONetworkStats *stats = data ? (IONetworkStats *)data->getBuffer() : NULL;
        if (stats)
            stats->inputErrors++;
        return 0;
    }
    if (directInput)
        param = packet;
#endif
    return IOEthernetInterface::inputPacket(packet, length, options, param);
}
