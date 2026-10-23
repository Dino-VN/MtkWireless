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
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>

#include "mtk_bt.h"

#define DRV_NAME "mtkbt"

namespace {

mach_vm_address_t orgFindQueuedRequest, orgDeviceRequest, orgPipeIo, orgPipeIoSync, orgCopyPipe;

uint32_t rustFailed;
bool randomAddressHack;
bool leShimEnabled;
IOLock *leLock;
volatile SInt32 readClockRewritten, aclPbRewritten;
volatile SInt32 leCommands, leCompletions, leErrors;

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

/* Remember which interrupt IN pipe carries HCI events of a loaded MediaTek
 * controller. Pipe addresses are lookup keys only. */
IOUSBHostPipe *copyPipe(IOUSBHostInterface *interface, uint8_t address)
{
    IOUSBHostPipe *pipe = FunctionCast(copyPipe, orgCopyPipe)(interface, address);
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
    leShimEnabled = leLock && io && request && ioSync && pipes && !checkKernelArgument("-mtkbtnolefix");
    SYSLOG(DRV_NAME, "MediaTek LE feature-mask USB shim %s", leShimEnabled ? "enabled" : "unavailable");
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
        lilu.onKextLoadForce(&kextIOUSBHostFamily, 1, processKext);
    }
    publishCounters();
}
