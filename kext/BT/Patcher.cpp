//
//  Patcher.cpp
//  MtkWireless
//
//  Based on SimpleMtkBtPatcher (zxystd, laobamac). Packet decisions are made
//  by the Rust core (rust/mtk-bt/src/{hci,le_features}.rs); this file moves
//  bytes in and out of USB memory descriptors and keeps the completions.
//

#include "Patcher.hpp"

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLocks.h>
#include <sys/syslimits.h>
#include <sys/vnode.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>

#include "mtk_bt.h"
#include "msbc/UsbMsbcBridge.hpp"
#include "msbc/UsbMsbcScope.hpp"

#define DRV_NAME "mtkbt"

namespace {

mach_vm_address_t orgFindQueuedRequest, orgDeviceRequest, orgPipeIo, orgPipeIoSync, orgCopyPipe;
mach_vm_address_t orgIsoIo, orgSelectAlternateSetting, orgInterfaceClose, orgGetPipeProperties;
UsbMsbcBridge *msbcBridge;
bool msbcReady;

bool isMsbcReady()
{
    return __atomic_load_n(&msbcReady, __ATOMIC_ACQUIRE);
}

uint32_t rustFailed;
bool randomAddressHack;
bool leShimEnabled;
IOLock *leLock;
volatile SInt32 readClockRewritten, aclPbRewritten;
volatile SInt32 leCommands, leCompletions, leErrors;
bool extAdvPatchEnabled, extAdvPatchRouted, msbcPatchEnabled;
volatile SInt32 bluetoothdPages, bluetoothdPatched, bluetoothdAddressPatched, bluetoothdAddressRetryPatched;
volatile SInt32 bluetoothdWbsPatched, bluetoothdHfpFillPatched;

void publishCounters()
{
    IOService *res = IOService::getResourceService();
    OSDictionary *d = OSDictionary::withCapacity(8);
    if (!res || !d) {
        OSSafeReleaseNULL(d);
        return;
    }
    auto number = [d](const char *key, uint32_t value) {
        if (OSNumber *n = OSNumber::withNumber(value, 32)) {
            d->setObject(key, n);
            n->release();
        }
    };
    if (OSString *v = OSString::withCString(mtk_rust_version())) {
        d->setObject("RustCore", v);
        v->release();
    }
    number("RustSelfTestFailed", rustFailed);
    number("ReadClockRewritten", (uint32_t)readClockRewritten);
    number("AclPbRewritten", (uint32_t)aclPbRewritten);
    d->setObject("LeFeaturesShimEnabled", leShimEnabled ? kOSBooleanTrue : kOSBooleanFalse);
    number("LeFeaturesCommands", (uint32_t)leCommands);
    number("LeFeaturesCompletions", (uint32_t)leCompletions);
    number("LeFeaturesErrors", (uint32_t)leErrors);
    d->setObject("BluetoothdExtAdvRouted", extAdvPatchEnabled && extAdvPatchRouted ? kOSBooleanTrue : kOSBooleanFalse);
    number("BluetoothdPages", (uint32_t)bluetoothdPages);
    number("BluetoothdExtAdvPatched", (uint32_t)bluetoothdPatched);
    number("BluetoothdAdvAddressPatched", (uint32_t)bluetoothdAddressPatched);
    number("BluetoothdAdvAddressRetryPatched", (uint32_t)bluetoothdAddressRetryPatched);
    number("BluetoothdWbsPatched", (uint32_t)bluetoothdWbsPatched);
    number("BluetoothdHfpFillPatched", (uint32_t)bluetoothdHfpFillPatched);
    res->setProperty("MtkWirelessBT", d);
    d->release();
}

/* MtkBtLog and FirmwareLoaded are published together by MtkWirelessBTLoader
 * after a successful MT7921/MT7922 setup. Leave other USB devices alone. */
bool isLoadedMediaTek(IOUSBHostDevice *device)
{
    return device && device->getProperty("MtkBtLog") && device->getProperty("FirmwareLoaded") == kOSBooleanTrue;
}

/* Copy the first `len` bytes of an EP0 request into `out`. */
bool readRequestBytes(void *data, IOMemoryDescriptor *descriptor, uint8_t *out, size_t len)
{
    if (data) {
        memcpy(out, data, len);
        return true;
    }
    if (!descriptor || descriptor->getLength() < len || descriptor->prepare(kIODirectionInOut) != kIOReturnSuccess)
        return false;
    bool ok = descriptor->readBytes(0, out, len) == len;
    descriptor->complete(kIODirectionInOut);
    return ok;
}

#pragma mark - HCI Read Clock

void fixReadClock(StandardUSB::DeviceRequest &request, void *data, IOMemoryDescriptor *descriptor)
{
    if (request.wLength != 6)
        return;
    if (data) {
        if (mtk_bt_fix_read_clock(request.bmRequestType, static_cast<uint8_t *>(data), 6)) {
            OSIncrementAtomic(&readClockRewritten);
            publishCounters();
        }
        return;
    }
    if (!descriptor || descriptor->prepare(kIODirectionInOut) != kIOReturnSuccess)
        return;
    uint8_t cmd[6];
    if (descriptor->readBytes(0, cmd, sizeof(cmd)) == sizeof(cmd) &&
        mtk_bt_fix_read_clock(request.bmRequestType, cmd, sizeof(cmd)) &&
        descriptor->writeBytes(5, &cmd[5], 1) == 1) {
        OSIncrementAtomic(&readClockRewritten);
        publishCounters();
    }
    descriptor->complete(kIODirectionInOut);
}

#pragma mark - Non-flushable ACL

void fixAclFlush(IOUSBHostPipe *pipe, IOMemoryDescriptor *buffer, uint32_t length)
{
    const StandardUSB::EndpointDescriptor *ep = pipe->getEndpointDescriptor();
    if (!buffer || !ep || !mtk_bt_acl_candidate(ep->bmAttributes, ep->bEndpointAddress, length) ||
        buffer->prepare(kIODirectionInOut) != kIOReturnSuccess)
        return;
    uint8_t hdr[4], byte1;
    if (buffer->readBytes(0, hdr, sizeof(hdr)) == sizeof(hdr) && mtk_bt_acl_non_flushable(hdr, length, &byte1) &&
        buffer->writeBytes(1, &byte1, 1) == 1) {
        SInt32 n = OSIncrementAtomic(&aclPbRewritten);
        if (n < 3)
            SYSLOG(DRV_NAME, "ACL PB flushable->non-flushable handle 0x%03x len %u",
                   (hdr[0] | (hdr[1] << 8)) & 0x0fff, length - 4);
        if ((n & 0x3ff) == 0)
            publishCounters();
    }
    buffer->complete(kIODirectionInOut);
}

#pragma mark - LE feature-mask compatibility

uint64_t leControllerForPipe(IOUSBHostPipe *pipe)
{
    if (!leShimEnabled)
        return 0;
    IOLockLock(leLock);
    uint64_t id = mtk_bt_le_controller_for_pipe(pipe);
    IOLockUnlock(leLock);
    return id;
}

void leCancel(uint64_t id, uint64_t token)
{
    IOLockLock(leLock);
    mtk_bt_le_cancel(id, token);
    IOLockUnlock(leLock);
}

uint64_t leArm(IOUSBHostDevice *device, StandardUSB::DeviceRequest &request, void *data,
               IOMemoryDescriptor *descriptor)
{
    if (!leShimEnabled ||
        !mtk_bt_le_candidate_request(request.bmRequestType, request.bRequest, request.wValue, request.wIndex,
                                     request.wLength) ||
        !isLoadedMediaTek(device))
        return 0;
    uint8_t command[16];
    if (request.wLength > sizeof(command) || !readRequestBytes(data, descriptor, command, request.wLength))
        return 0;
    IOLockLock(leLock);
    uint64_t token = mtk_bt_le_command(device->getRegistryEntryID(), command, request.wLength);
    IOLockUnlock(leLock);
    if (token) {
        OSIncrementAtomic(&leCommands);
        publishCounters();
    }
    return token;
}

void leReceive(uint64_t id, IOMemoryDescriptor *buffer, uint32_t length)
{
    uint8_t events[1024];
    if (!id || !buffer || !length || length > sizeof(events) || length > buffer->getLength() ||
        buffer->prepare(kIODirectionInOut) != kIOReturnSuccess)
        return;
    mtk_bt_le_received r { MTK_BT_LE_NONE, 0, {} };
    if (buffer->readBytes(0, events, length) == length) {
        IOLockLock(leLock);
        r = mtk_bt_le_receive(id, events, length);
        IOLockUnlock(leLock);
    }
    bool rewritten = false;
    if (r.reply == MTK_BT_LE_REWRITTEN && r.offset + 6 <= length) {
        rewritten = buffer->writeBytes(r.offset, events + r.offset, 6) == 6;
        if (!rewritten) {
            buffer->writeBytes(r.offset, r.original, sizeof(r.original));
            OSIncrementAtomic(&leErrors);
        }
    }
    buffer->complete(kIODirectionInOut);
    if (rewritten) {
        OSIncrementAtomic(&leCompletions);
        SYSLOG(DRV_NAME, "MediaTek 0xFD3C feature-mask status -> Command Complete, credits preserved");
        publishCounters();
    }
}

/* Wraps a USB completion. Two references: the callback and the submitter,
 * so a callback that runs before the submission returns is covered. */
struct LeCompletion {
    IOUSBHostCompletion original;
    IOMemoryDescriptor *buffer; /* event pipe reads; NULL for EP0 commands */
    uint32_t capacity;
    uint64_t registryId;
    uint64_t token;
    volatile SInt32 references;
    volatile UInt32 claimed;
};

LeCompletion *makeLeCompletion(IOUSBHostCompletion *completion, uint64_t id, uint64_t token,
                               IOMemoryDescriptor *buffer = nullptr, uint32_t capacity = 0)
{
    auto *c = static_cast<LeCompletion *>(IOMalloc(sizeof(LeCompletion)));
    if (!c)
        return nullptr;
    *c = { *completion, buffer, capacity, id, token, 2, 0 };
    if (buffer)
        buffer->retain();
    return c;
}

void releaseLeCompletion(LeCompletion *c)
{
    if (OSDecrementAtomic(&c->references) == 1) {
        if (c->buffer)
            c->buffer->release();
        IOFree(c, sizeof(*c));
    }
}

void leUsbCompletion(void *, void *parameter, IOReturn status, uint32_t transferred)
{
    auto *c = static_cast<LeCompletion *>(parameter);
    if (!OSCompareAndSwap(0, 1, &c->claimed))
        return;
    if (c->buffer) {
        if (status == kIOReturnSuccess && transferred <= c->capacity)
            leReceive(c->registryId, c->buffer, transferred);
    } else if (status != kIOReturnSuccess) {
        leCancel(c->registryId, c->token);
        OSIncrementAtomic(&leErrors);
    }
    const IOUSBHostCompletion original = c->original;
    original.action(original.owner, original.parameter, status, transferred);
    releaseLeCompletion(c);
}

void finishLeSubmission(LeCompletion *c, IOReturn status)
{
    // USB promises a callback only for a successful enqueue.
    if (status != kIOReturnSuccess && OSCompareAndSwap(0, 1, &c->claimed)) {
        if (c->token)
            leCancel(c->registryId, c->token);
        OSIncrementAtomic(&leErrors);
        releaseLeCompletion(c); // unused callback reference
    }
    releaseLeCompletion(c); // submitter reference
}

#pragma mark - Routes

/* Big Sur and older: IOBluetoothFamily does not match the LE Start
 * Encryption (0x2019) response; retry the lookup with any handle. */
IOReturn findQueuedRequest(void *that, unsigned short opcode, void *addr, unsigned short handle, bool arg3,
                           void **request)
{
    IOReturn ret = FunctionCast(findQueuedRequest, orgFindQueuedRequest)(that, opcode, addr, handle, arg3, request);
    if (ret != 0 && opcode == 0x2019)
        ret = FunctionCast(findQueuedRequest, orgFindQueuedRequest)(that, opcode, addr, 0xffff, arg3, request);
    return ret;
}

/* Opt-in (-mtkbtrandaddr): LE Set Random Address before the first LE Set
 * Scan Parameters after reset (originally written for Intel controllers). */
bool randomAddressSent;
const uint8_t randomAddressHci[9] = {0x05, 0x20, 0x06, 0x94, 0x50, 0x64, 0xD0, 0x78, 0x6B};

void randomAddress(void *that, IOService *provider, StandardUSB::DeviceRequest &request, void *data,
                   IOMemoryDescriptor *descriptor, unsigned int timeout);

IOReturn deviceRequest(void *that, IOService *provider, StandardUSB::DeviceRequest &request, void *data,
                       IOMemoryDescriptor *descriptor, unsigned int &length, IOUSBHostCompletion *completion,
                       unsigned int timeout)
{
    fixReadClock(request, data, descriptor);
    if (randomAddressHack)
        randomAddress(that, provider, request, data, descriptor, timeout);

    auto *device = static_cast<IOUSBHostDevice *>(that);
    const uint64_t token = (!completion || completion->action) ? leArm(device, request, data, descriptor) : 0;
    LeCompletion *context = nullptr;
    IOUSBHostCompletion replacement {};
    if (token && completion) {
        context = makeLeCompletion(completion, device->getRegistryEntryID(), token);
        if (context) {
            replacement = { nullptr, leUsbCompletion, context };
        } else {
            leCancel(device->getRegistryEntryID(), token);
            OSIncrementAtomic(&leErrors);
        }
    }
    IOReturn status = FunctionCast(deviceRequest, orgDeviceRequest)(that, provider, request, data, descriptor, length,
                                                                    context ? &replacement : completion, timeout);
    if (context) {
        finishLeSubmission(context, status);
    } else if (token && status != kIOReturnSuccess) {
        leCancel(device->getRegistryEntryID(), token);
        OSIncrementAtomic(&leErrors);
    }
    return status;
}

void randomAddress(void *that, IOService *provider, StandardUSB::DeviceRequest &request, void *data,
                   IOMemoryDescriptor *descriptor, unsigned int timeout)
{
    if ((request.bmRequestType & 0xE0) != 0x20 || request.wLength < 3) // HCI command
        return;
    uint8_t opcode[3];
    if (!readRequestBytes(data, descriptor, opcode, sizeof(opcode)))
        return;
    uint16_t op = (uint16_t)(opcode[0] | (opcode[1] << 8));
    if (op == 0x0c03) { // HCI Reset
        randomAddressSent = false;
        return;
    }
    if (op != 0x200B || randomAddressSent) // LE Set Scan Parameters
        return;
    randomAddressSent = true;
    StandardUSB::DeviceRequest set = {
        .bmRequestType = makeDeviceRequestbmRequestType(kRequestDirectionOut, kRequestTypeClass,
                                                        kRequestRecipientInterface),
        .bRequest = 0xE0,
        .wValue = 0,
        .wIndex = 0,
        .wLength = sizeof(randomAddressHci),
    };
    IOBufferMemoryDescriptor *buf =
        IOBufferMemoryDescriptor::withBytes(randomAddressHci, sizeof(randomAddressHci), kIODirectionOut);
    if (!buf)
        return;
    unsigned int len = sizeof(randomAddressHci);
    buf->prepare(kIODirectionOut);
    IOReturn ret = FunctionCast(deviceRequest, orgDeviceRequest)(that, provider, set, nullptr, buf, len, nullptr,
                                                                 timeout);
    buf->complete(kIODirectionOut);
    buf->release();
    SYSLOG(DRV_NAME, "sent LE Set Random Address before scan parameters: %d", ret);
}

IOReturn pipeIo(void *that, IOMemoryDescriptor *buffer, uint32_t length, IOUSBHostCompletion *completion,
                uint32_t timeoutMs)
{
    auto *pipe = static_cast<IOUSBHostPipe *>(that);
    fixAclFlush(pipe, buffer, length);

    const uint64_t id = leControllerForPipe(pipe);
    if (!id || !buffer || !completion || !completion->action || !length || length > 1024)
        return FunctionCast(pipeIo, orgPipeIo)(that, buffer, length, completion, timeoutMs);
    LeCompletion *context = makeLeCompletion(completion, id, 0, buffer, length);
    if (!context) {
        OSIncrementAtomic(&leErrors);
        return FunctionCast(pipeIo, orgPipeIo)(that, buffer, length, completion, timeoutMs);
    }
    IOUSBHostCompletion replacement { nullptr, leUsbCompletion, context };
    IOReturn status = FunctionCast(pipeIo, orgPipeIo)(that, buffer, length, &replacement, timeoutMs);
    finishLeSubmission(context, status);
    return status;
}

IOReturn pipeIoSync(void *that, IOMemoryDescriptor *buffer, uint32_t length, uint32_t &transferred,
                    uint32_t timeoutMs)
{
    const uint64_t id = leControllerForPipe(static_cast<IOUSBHostPipe *>(that));
    IOReturn status = FunctionCast(pipeIoSync, orgPipeIoSync)(that, buffer, length, transferred, timeoutMs);
    if (status == kIOReturnSuccess && transferred <= length)
        leReceive(id, buffer, transferred);
    return status;
}

IOReturn isoIo(IOUSBHostPipe *pipe, IOMemoryDescriptor *data, IOUSBHostIsochronousFrame *frames,
               uint32_t count, uint64_t firstFrame, IOUSBHostIsochronousCompletion *completion)
{
    auto original = reinterpret_cast<UsbMsbcBridge::IsoIo>(orgIsoIo);
    if (isMsbcReady() && msbcBridge && msbcBridge->handlesPipe(pipe))
        return msbcBridge->io(pipe, data, frames, count, firstFrame, completion, original);
    return original(pipe, data, frames, count, firstFrame, completion);
}

IOReturn selectAlternateSetting(IOUSBHostInterface *interface, uint8_t alternate)
{
    const bool cached = isMsbcReady() && msbcBridge && msbcCachedScoInterface(interface);
    const bool scoped = cached || (isMsbcReady() && msbcBridge && msbcBridge->ownsInterface(interface));
    const bool mapped = cached && alternate == 4;
    const uint64_t generation = scoped ? msbcBridge->beginInterfaceChange() : 0;
    IOReturn result = FunctionCast(selectAlternateSetting, orgSelectAlternateSetting)(interface, mapped ? 6 : alternate);
    if (scoped && result == kIOReturnSuccess) {
        msbcBridge->detach(generation);
        if (mapped && !msbcBridge->bindPhysicalInterface(interface, generation))
            return kIOReturnAborted;
    }
    return result;
}

void interfaceClose(IOUSBHostInterface *interface, IOService *client, IOOptionBits options)
{
    const bool scoped = isMsbcReady() && msbcBridge &&
        (msbcBridge->ownsInterface(interface) || msbcCachedScoInterface(interface));
    const uint64_t generation = scoped ? msbcBridge->beginInterfaceChange() : 0;
    FunctionCast(interfaceClose, orgInterfaceClose)(interface, client, options);
    if (scoped)
        msbcBridge->detach(generation);
}

IOReturn getPipeProperties(IOService *client, uint8_t pipeRef, uint64_t *direction,
                          uint64_t *number, uint64_t *type, uint64_t *maxPacket, uint64_t *interval)
{
    IOReturn result = FunctionCast(getPipeProperties, orgGetPipeProperties)(client, pipeRef, direction,
        number, type, maxPacket, interval);
    if (result != kIOReturnSuccess || !isMsbcReady() || !msbcBridge || !client)
        return result;
    auto *interface = OSDynamicCast(IOUSBHostInterface, client->getProvider());
    // Keep native PCM at 33 bytes; only the MTK wire pipe uses 63-byte mSBC packets.
    if (msbcCachedScoInterface(interface) && msbcBridge->mapsPhysicalInterface(interface) &&
        direction && number && type && maxPacket && interval && *direction <= 1 &&
        *number == 3 && *type == 1 && *maxPacket == MtkMsbcStream::kWirePacket && *interval == 4)
        *maxPacket = 33;
    return result;
}

/* Remember which interrupt IN pipe carries HCI events of a loaded MediaTek
 * controller, and let the mSBC bridge see the SCO pipes (0x03/0x83). Pipe
 * addresses are lookup keys only. */
IOUSBHostPipe *copyPipe(IOUSBHostInterface *interface, uint8_t address)
{
    IOUSBHostPipe *pipe = FunctionCast(copyPipe, orgCopyPipe)(interface, address);
    if ((address == 0x03 || address == 0x83) && isMsbcReady() && msbcBridge &&
        msbcBridge->observePipe(interface, pipe))
        msbcBridge->publishStatus();
    if (!leShimEnabled || !pipe)
        return pipe;
    const auto *ep = pipe->getEndpointDescriptor();
    const auto *desc = interface->getInterfaceDescriptor();
    if (!ep || (ep->bmAttributes & 3) != 3 || !(ep->bEndpointAddress & 0x80) || !desc ||
        desc->bInterfaceClass != 0xE0 || desc->bInterfaceSubClass != 1 || desc->bInterfaceProtocol != 1)
        return pipe;
    IOUSBHostDevice *device = nullptr;
    for (IOService *p = interface->getProvider(); p && !device; p = p->getProvider())
        device = OSDynamicCast(IOUSBHostDevice, p);
    const uint64_t id = device ? device->getRegistryEntryID() : 0;
    IOLockLock(leLock);
    mtk_bt_le_bind_event_pipe(pipe, id, isLoadedMediaTek(device));
    IOLockUnlock(leLock);
    return pipe;
}

const char *pathIOBluetoothFamily[] {
    "/System/Library/Extensions/IOBluetoothFamily.kext/Contents/MacOS/IOBluetoothFamily"};
const char *pathIOUSBHostFamily[] {
    "/System/Library/Extensions/IOUSBHostFamily.kext/Contents/MacOS/IOUSBHostFamily"};

KernelPatcher::KextInfo kextIOBluetoothFamily {"com.apple.iokit.IOBluetoothFamily", pathIOBluetoothFamily, 1,
                                               {true, true}, {}, KernelPatcher::KextInfo::Unloaded};
KernelPatcher::KextInfo kextIOUSBHostFamily {"com.apple.iokit.IOUSBHostFamily", pathIOUSBHostFamily, 1,
                                             {true, true}, {}, KernelPatcher::KextInfo::Unloaded};

bool route(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size, const char *symbol,
           void *replacement, mach_vm_address_t &original)
{
    KernelPatcher::RouteRequest request {symbol, replacement, original};
    patcher.routeMultiple(index, &request, 1, address, size);
    if (patcher.getError() != KernelPatcher::Error::NoError) {
        SYSLOG(DRV_NAME, "failed to route %s (%d)", symbol, patcher.getError());
        patcher.clearError();
        return false;
    }
    SYSLOG(DRV_NAME, "routed %s", symbol);
    return true;
}

void installMsbcBridge(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size,
                       bool copyPipeRouted)
{
    if (getKernelVersion() != KernelVersion::Tahoe || checkKernelArgument("-mtkbtnomsbc") ||
        !copyPipeRouted || !orgCopyPipe)
        return;
    KernelPatcher::RouteRequest routes[] {
        {"__ZN13IOUSBHostPipe2ioEP18IOMemoryDescriptorP25IOUSBHostIsochronousFramejyP30IOUSBHostIsochronousCompletion",
         reinterpret_cast<void *>(isoIo), orgIsoIo},
        {"__ZN18IOUSBHostInterface22selectAlternateSettingEh",
         reinterpret_cast<void *>(selectAlternateSetting), orgSelectAlternateSetting},
        {"__ZN18IOUSBHostInterface5closeEP9IOServicej", reinterpret_cast<void *>(interfaceClose), orgInterfaceClose},
        {"__ZN31AppleUSBHostInterfaceUserClient17GetPipePropertiesEhPyS0_S0_S0_S0_",
         reinterpret_cast<void *>(getPipeProperties), orgGetPipeProperties}
    };
    // Resolve every ABI before routing; partial routes stay inert without readiness.
    for (auto &request : routes) {
        request.from = patcher.solveSymbol(index, request.symbol, address, size);
        if (!request.from || patcher.getError() != KernelPatcher::Error::NoError) {
            SYSLOG(DRV_NAME, "mSBC bridge symbol unavailable: %s", request.symbol);
            patcher.clearError();
            return;
        }
    }
    auto *bridge = UsbMsbcBridge::create();
    if (!bridge) {
        SYSLOG(DRV_NAME, "mSBC bridge allocation failed");
        return;
    }
    msbcBridge = bridge;
    const bool routed = patcher.routeMultiple(index, routes, arrsize(routes), address, size);
    if (!routed || patcher.getError() != KernelPatcher::Error::NoError ||
        !orgIsoIo || !orgSelectAlternateSetting || !orgInterfaceClose || !orgGetPipeProperties) {
        SYSLOG(DRV_NAME, "mSBC bridge routes incomplete; native WBS remains disabled");
        patcher.clearError();
        msbcBridge = nullptr;
        bridge->release();
        return;
    }
    auto *resource = IOService::getResourceService();
    if (!resource) {
        msbcBridge = nullptr;
        bridge->release();
        return;
    }
    if (!resource->setProperty("MtkBtMsbcBridgeReady", kOSBooleanTrue)) {
        msbcBridge = nullptr;
        bridge->release();
        return;
    }
    __atomic_store_n(&msbcReady, true, __ATOMIC_RELEASE);
    bridge->publishStatus();
    SYSLOG(DRV_NAME, "opt-in MT7922 USB mSBC bridge ready");
}

void processKext(void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size)
{
    if (index == kextIOBluetoothFamily.loadIndex) {
        route(patcher, index, address, size,
              "__ZN25IOBluetoothHostController17FindQueuedRequestEtP22BluetoothDeviceAddresstbPP21IOBluetoothHCIRequest",
              reinterpret_cast<void *>(findQueuedRequest), orgFindQueuedRequest);
        return;
    }
    if (index != kextIOUSBHostFamily.loadIndex)
        return;
    randomAddressHack = checkKernelArgument("-mtkbtrandaddr");
    const bool io = route(patcher, index, address, size,
                          "__ZN13IOUSBHostPipe2ioEP18IOMemoryDescriptorjP19IOUSBHostCompletionj",
                          reinterpret_cast<void *>(pipeIo), orgPipeIo);
    const bool request = route(
        patcher, index, address, size,
        "__ZN15IOUSBHostDevice13deviceRequestEP9IOServiceRN11StandardUSB13DeviceRequestEPvP18IOMemoryDescriptorRjP19IOUSBHostCompletionj",
        reinterpret_cast<void *>(deviceRequest), orgDeviceRequest);
    const bool ioSync = route(patcher, index, address, size, "__ZN13IOUSBHostPipe2ioEP18IOMemoryDescriptorjRjj",
                              reinterpret_cast<void *>(pipeIoSync), orgPipeIoSync);
    const bool pipes = route(patcher, index, address, size, "__ZN18IOUSBHostInterface8copyPipeEh",
                             reinterpret_cast<void *>(copyPipe), orgCopyPipe);
    installMsbcBridge(patcher, index, address, size, pipes);
    leShimEnabled = leLock && io && request && ioSync && pipes && !checkKernelArgument("-mtkbtnolefix");
    SYSLOG(DRV_NAME, "MediaTek LE feature-mask USB shim %s", leShimEnabled ? "enabled" : "unavailable");
    publishCounters();
}

/*
 * bluetoothd enables BLE multiple advertising only when its chip table says
 * the controller has extended or BT-spec advertising sets. That table keys on
 * an Apple chip ID ([this+0x334]: 19-25, 2001-3999, 5000-5999, 6000); a
 * THIRD_PARTY_DONGLE gets neither, LE_InitMultipleAdvertising returns 0x98,
 * LeBroadcaster never starts and nothing (AirDrop, Nearby) is advertised.
 * The AE/DisableExtAdvertising preference can only turn it off.
 *
 * MediaTek reports LE Extended Advertising (LE feature bit 12) and takes the
 * standard 0x2036-0x2039 commands, as BlueZ uses on Linux. Make the
 * extended-advertising check return true; bluetoothd then forces BT-spec
 * advertising sets on as well. x86_64 bluetoothd of macOS 26.7.1 (25G241):
 * the whole 77-byte function, unique in the binary (its BT-spec-sets twin
 * differs only in sete/setae) and inside one page (offset 0x6ec), so a
 * per-page search finds it; other versions do not match. Boot-arg
 * -mtkbtnoextadv disables it.
 */
const uint8_t extAdvFind[] {
    0x55, 0x48, 0x89, 0xe5, 0x8b, 0x8f, 0x34, 0x03, 0x00, 0x00, 0x81, 0xf9, 0xd0, 0x07, 0x00, 0x00,
    0x72, 0x14, 0x81, 0xf9, 0x9f, 0x0f, 0x00, 0x00, 0x77, 0x21, 0xb0, 0x01, 0x81, 0xf9, 0xd0, 0x07,
    0x00, 0x00, 0x74, 0x0c, 0xeb, 0x13, 0x8d, 0x51, 0xed, 0xb0, 0x01, 0x83, 0xfa, 0x07, 0x72, 0x09,
    0x81, 0xf9, 0x70, 0x17, 0x00, 0x00, 0x0f, 0x94, 0xc0, 0x5d, 0xc3, 0x8d, 0x91, 0x78, 0xec, 0xff,
    0xff, 0xb0, 0x01, 0x81, 0xfa, 0xe8, 0x03, 0x00, 0x00, 0x73, 0xe5, 0xeb, 0xec};
// push rbp; mov rbp, rsp; mov al, 1; pop rbp; ret; the rest is unreachable.
const uint8_t extAdvReplace[] {
    0x55, 0x48, 0x89, 0xe5, 0xb0, 0x01, 0x5d, 0xc3, 0x00, 0x00, 0x81, 0xf9, 0xd0, 0x07, 0x00, 0x00,
    0x72, 0x14, 0x81, 0xf9, 0x9f, 0x0f, 0x00, 0x00, 0x77, 0x21, 0xb0, 0x01, 0x81, 0xf9, 0xd0, 0x07,
    0x00, 0x00, 0x74, 0x0c, 0xeb, 0x13, 0x8d, 0x51, 0xed, 0xb0, 0x01, 0x83, 0xfa, 0x07, 0x72, 0x09,
    0x81, 0xf9, 0x70, 0x17, 0x00, 0x00, 0x0f, 0x94, 0xc0, 0x5d, 0xc3, 0x8d, 0x91, 0x78, 0xec, 0xff,
    0xff, 0xb0, 0x01, 0x81, 0xfa, 0xe8, 0x03, 0x00, 0x00, 0x73, 0xe5, 0xeb, 0xec};
static_assert(sizeof(extAdvFind) == sizeof(extAdvReplace), "patch size");

/* initCoreStack also disables per-set address updates for chip IDs 0..3.
 * The gate at 0x100b1cd00 starts true, but this call sets it false for a
 * THIRD_PARTY_DONGLE. LE_UpdateMultiAdvertisingSetAddress then returns
 * without issuing any command, and the before-enable callback skips it.
 * Extended advertising needs a random address on EACH set (0x2035), even
 * if the global scanning address (0x2005) is already set. Preserve the
 * default gate by removing only that disabling call, keeping the remaining
 * chip-specific setup intact. macOS still owns the address and ordering.
 * Exact 25G241 x86_64 sequence at 0x100510e99, inside one page.
 */
const uint8_t advAddressFind[] {
    0xe8, 0xd4, 0x30, 0xe8, 0xff, 0x83, 0xb8, 0x34, 0x03, 0x00, 0x00, 0x03, 0x77, 0x2a,
    0x31, 0xff, 0xe8, 0x3e, 0x6b, 0xce, 0xff};
const uint8_t advAddressReplace[] {
    0xe8, 0xd4, 0x30, 0xe8, 0xff, 0x83, 0xb8, 0x34, 0x03, 0x00, 0x00, 0x03, 0x77, 0x2a,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
static_assert(sizeof(advAddressFind) == sizeof(advAddressReplace), "address patch size");

/* 0.1.8 HCI capture: macOS sends 0x2035 for handles 0/2 before
 * 0x2036 creates them. MediaTek returns Unknown Advertising Identifier (42),
 * but LE_UpdateMultiAdvertisingSetAddress already cached the address. The
 * unchanged-address fast path then skips 0x2035 after creation, and enable
 * fails Invalid HCI Command Parameters (12). Handle 3, initialized in order,
 * enables successfully. Always program a RANDOM address at this before-enable
 * point, after parameters/data exist, even when the host cache matches.
 * Public addresses keep the old changed-address test. The gate check replaced
 * here is redundant with advAddress above (default true, sole false setter
 * removed); these patches are enabled/disabled together. No HCI reply is
 * synthesized; the existing callback waits for the real controller result.
 * 25G241 x86_64 0x1001fa20d..0x1001fa24d, 64 bytes inside one page.
 */
const uint8_t advAddressRetryFind[] {
    0x44, 0x8b, 0x04, 0x3a, 0x44, 0x33, 0x84, 0x08, 0x0e, 0x0d, 0x00, 0x00, 0x8b, 0x54, 0x3a, 0x03,
    0x33, 0x94, 0x08, 0x11, 0x0d, 0x00, 0x00, 0x44, 0x09, 0xc2, 0x0f, 0x84, 0x80, 0x00, 0x00, 0x00,
    0x80, 0x3d, 0xcc, 0x2a, 0x92, 0x00, 0x00, 0x74, 0x77, 0x48, 0x8d, 0x3c, 0x08, 0x48, 0x81, 0xc7,
    0x0e, 0x0d, 0x00, 0x00, 0x48, 0x8d, 0x15, 0x7b, 0x00, 0x00, 0x00, 0xe8, 0xee, 0xce, 0xff, 0xff};
const uint8_t advAddressRetryReplace[] {
    0x44, 0x8b, 0x04, 0x3a, 0x44, 0x33, 0x84, 0x08, 0x0e, 0x0d, 0x00, 0x00, 0x8b, 0x54, 0x3a, 0x03,
    0x33, 0x94, 0x08, 0x11, 0x0d, 0x00, 0x00, 0xf6, 0x84, 0x08, 0x0e, 0x0d, 0x00, 0x00, 0x01, 0x75,
    0x08, 0x44, 0x09, 0xc2, 0x74, 0x7a, 0x90, 0x90, 0x90, 0x48, 0x8d, 0x3c, 0x08, 0x48, 0x81, 0xc7,
    0x0e, 0x0d, 0x00, 0x00, 0x48, 0x8d, 0x15, 0x7b, 0x00, 0x00, 0x00, 0xe8, 0xee, 0xce, 0xff, 0xff};
static_assert(sizeof(advAddressRetryFind) == sizeof(advAddressRetryReplace), "address retry patch size");

/* HFP wideband gate: bluetoothd enables mSBC only for known chip IDs; a
 * THIRD_PARTY_DONGLE (chip 0) is outside the range. Accept chip 0, only while
 * the USB mSBC bridge is ready (-mtkbtnomsbc off). 25G241 x86_64, exact signature.
 */
const uint8_t wbsFind[] {
    0x55, 0x48, 0x89, 0xe5, 0xe8, 0x31, 0xfd, 0x01, 0x00, 0x8b, 0x88, 0x34, 0x03, 0x00, 0x00, 0x8d,
    0x41, 0xff, 0x3d, 0xce, 0x07, 0x00, 0x00, 0x77, 0x10, 0xb0, 0x01, 0x83, 0xf9, 0x0a, 0x73, 0x28,
    0xb8, 0xf3, 0x01, 0x00, 0x00, 0xd3, 0xe8, 0xeb, 0x1f, 0x8d, 0x91, 0x30, 0xf8, 0xff, 0xff, 0xb0,
    0x01, 0x81, 0xfa, 0xd0, 0x07, 0x00, 0x00, 0x72, 0x0f, 0x81, 0xc1, 0x60, 0xf0, 0xff, 0xff, 0x81,
    0xf9, 0xd0, 0x07, 0x00, 0x00, 0x0f, 0x92, 0xc0, 0x24, 0x01, 0x5d, 0xc3};
const uint8_t wbsReplace[] {
    0x55, 0x48, 0x89, 0xe5, 0xe8, 0x31, 0xfd, 0x01, 0x00, 0x8b, 0x88, 0x34, 0x03, 0x00, 0x00, 0x8d,
    0x41, 0x00, 0x3d, 0xcf, 0x07, 0x00, 0x00, 0x77, 0x10, 0xb0, 0x01, 0x83, 0xf9, 0x0a, 0x73, 0x28,
    0xb8, 0xf3, 0x01, 0x00, 0x00, 0xd3, 0xe8, 0xeb, 0x1f, 0x8d, 0x91, 0x30, 0xf8, 0xff, 0xff, 0xb0,
    0x01, 0x81, 0xfa, 0xd0, 0x07, 0x00, 0x00, 0x72, 0x0f, 0x81, 0xc1, 0x60, 0xf0, 0xff, 0xff, 0x81,
    0xf9, 0xd0, 0x07, 0x00, 0x00, 0x0f, 0x92, 0xc0, 0x24, 0x01, 0x5d, 0xc3};
static_assert(sizeof(wbsFind) == sizeof(wbsReplace), "wideband patch size");

/* HFP sender: read one complete PCM chunk per timer tick (drop the imul), so
 * no oversized burst or partial tail overflows the outstanding-byte counter.
 */
const uint8_t hfpFillFind[] {
    0x48, 0x0f, 0xaf, 0xd0, 0x4c, 0x89, 0xf6, 0xe8, 0x3b, 0x2f, 0xfd, 0xff, 0x48, 0x85, 0xc0, 0x0f,
    0x84, 0x83, 0x00, 0x00, 0x00, 0x49, 0x89, 0xc4, 0x48, 0x89, 0x83, 0xd0, 0x00, 0x00, 0x00};
const uint8_t hfpFillReplace[] {
    0x0f, 0x1f, 0x40, 0x00, 0x4c, 0x89, 0xf6, 0xe8, 0x3b, 0x2f, 0xfd, 0xff, 0x48, 0x85, 0xc0, 0x0f,
    0x84, 0x83, 0x00, 0x00, 0x00, 0x49, 0x89, 0xc4, 0x48, 0x89, 0x83, 0xd0, 0x00, 0x00, 0x00};
static_assert(sizeof(hfpFillFind) == sizeof(hfpFillReplace), "HFP fill patch size");

mach_vm_address_t orgCsValidatePage;

// Lilu's user patcher is off on Big Sur and later (kern_start.cpp), so
// onProcLoad never fires there. Patch bluetoothd's pages as the kernel
// validates them instead, as BlueToolFixup and RestrictEvents do.
void csValidatePage(vnode_t vp, memory_object_t pager, memory_object_offset_t pageOffset, const void *data,
                    int *validated, int *tainted, int *nx)
{
    FunctionCast(csValidatePage, orgCsValidatePage)(vp, pager, pageOffset, data, validated, tainted, nx);
    static constexpr char bluetoothd[] = "/usr/sbin/bluetoothd";
    char path[PATH_MAX];
    int pathLength = PATH_MAX;
    if (!data || vn_getpath(vp, path, &pathLength) != 0 || LIKELY(strcmp(path, bluetoothd) != 0))
        return;
    // Page-in path: count only; the next publishCounters() reports it.
    OSIncrementAtomic(&bluetoothdPages);
    if (msbcPatchEnabled && isMsbcReady()) {
        if (KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE, wbsFind, sizeof(wbsFind),
                                          wbsReplace, sizeof(wbsReplace))) {
            OSIncrementAtomic(&bluetoothdWbsPatched);
            SYSLOG(DRV_NAME, "enabled MediaTek HFP wideband with USB mSBC bridge ready");
        }
        if (KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE, hfpFillFind, sizeof(hfpFillFind),
                                          hfpFillReplace, sizeof(hfpFillReplace))) {
            OSIncrementAtomic(&bluetoothdHfpFillPatched);
            SYSLOG(DRV_NAME, "limited MediaTek HFP sender to complete PCM chunks");
        }
    }
    if (extAdvPatchEnabled) {
        if (KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE, extAdvFind, sizeof(extAdvFind),
                                          extAdvReplace, sizeof(extAdvReplace))) {
            OSIncrementAtomic(&bluetoothdPatched);
            SYSLOG(DRV_NAME, "patched bluetoothd extended-advertising check");
        }
        if (KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE, advAddressFind,
                                          sizeof(advAddressFind), advAddressReplace, sizeof(advAddressReplace))) {
            OSIncrementAtomic(&bluetoothdAddressPatched);
            SYSLOG(DRV_NAME, "patched bluetoothd per-set advertising address gate");
        }
        if (KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE, advAddressRetryFind,
                                          sizeof(advAddressRetryFind), advAddressRetryReplace,
                                          sizeof(advAddressRetryReplace))) {
            OSIncrementAtomic(&bluetoothdAddressRetryPatched);
            SYSLOG(DRV_NAME, "patched bluetoothd random address initialization before enable");
        }
    }
}

void processPatcher(void *, KernelPatcher &patcher)
{
    KernelPatcher::RouteRequest route {"_cs_validate_page", csValidatePage, orgCsValidatePage};
    extAdvPatchRouted = patcher.routeMultipleLong(KernelPatcher::KernelID, &route, 1);
    if (!extAdvPatchRouted)
        patcher.clearError();
    SYSLOG(DRV_NAME, "bluetoothd page patches %s", extAdvPatchRouted ? "routed" : "failed to route");
    publishCounters();
}

} // namespace

void Patcher::init(uint32_t rustSelfTestFailed)
{
    rustFailed = rustSelfTestFailed;
    if (getKernelVersion() < KernelVersion::Monterey) {
        lilu.onKextLoadForce(&kextIOBluetoothFamily, 1, processKext);
    } else {
        leLock = IOLockAlloc();
        extAdvPatchEnabled = !checkKernelArgument("-mtkbtnoextadv");
        msbcPatchEnabled = getKernelVersion() == KernelVersion::Tahoe && !checkKernelArgument("-mtkbtnomsbc");
        lilu.onKextLoadForce(&kextIOUSBHostFamily, 1, processKext);
        if (extAdvPatchEnabled || msbcPatchEnabled)
            lilu.onPatcherLoadForce(processPatcher);
    }
    publishCounters();
}
