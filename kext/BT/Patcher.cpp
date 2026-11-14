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
#include <Headers/kern_file.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <sys/syslimits.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <sys/uio.h>
#include <sys/errno.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>

#include "mtk_bt.h"
#include "msbc/MsbcControllerScope.hpp"
#include "msbc/UsbMsbcBridge.hpp"
#include "msbc/UsbMsbcScope.hpp"

#define DRV_NAME "mtkbt"

// Opaque: the pre-exec route never reads a kernel image_params layout.
struct image_params;

namespace {

mach_vm_address_t orgFindQueuedRequest, orgDeviceRequest, orgPipeIo, orgPipeIoSync, orgCopyPipe;
mach_vm_address_t orgIsoIo, orgSelectAlternateSetting, orgInterfaceClose, orgGetPipeProperties;
UsbMsbcBridge *msbcBridge;
bool msbcReady;
MsbcControllerScope msbcControllerScope;

bool isMsbcReady()
{
    return __atomic_load_n(&msbcReady, __ATOMIC_ACQUIRE);
}

uint32_t rustFailed;
bool randomAddressHack;
bool leShimEnabled;
bool leReadyMaskEnabled;
IOLock *leLock;
volatile SInt32 readClockRewritten, aclPbRewritten, aclPipesBound;
volatile SInt32 leCommands, leCompletions, leErrors;
bool extAdvPatchEnabled, extAdvPatchRouted, msbcPatchEnabled;
bool rpaPatchEnabled, rpaHelpersReady;
mach_vm_address_t orgMacVnodeCheckExec;
bool rpaExecRouted;
enum RpaPlanState : SInt32 {
    RpaPlanUnread = 0, RpaPlanPreparing = 1, RpaPlanReady = 2, RpaPlanDisabled = 3,
    RpaPlanReadFailed = 4, RpaPlanInvalid = 5, RpaPlanLate = 6, RpaPlanNoVerify = 7,
    RpaPlanNoRoom = 8, RpaPlanNoVnode = 9
};
volatile SInt32 rpaPlanState, rpaPlanRejected, bluetoothdForeignPagesRolledBack;
volatile uint32_t bluetoothdCommonAbi;
mtk_bt_rpa_plan *rpaPlan;
vnode_t bluetoothdImageVnode; // Referenced for this boot; never reuse a plan for another vnode.
uint32_t bluetoothdImageVid;
bool rpaValidationStarted;
static_assert(sizeof(mtk_bt_rpa_edit) == 608 && offsetof(mtk_bt_rpa_plan, edits) == 72 &&
              sizeof(mtk_bt_rpa_plan) == 4328, "RPA Rust/C layout");
// -mtkbtanybuild: ScopeMinimumBuild sites also below the minimum/other ABI families.
bool anyBuildPatches;
// kern.osversion, written by launchd after the kext starts (BluetoothdBuild).
// Pattern scopes key on it; read at bluetoothd's first page, then cached.
enum BluetoothdBuild : SInt32 {
    BuildUnread = 0, BuildOther = 1, Build25G241 = 2, Build25G332 = 3, Build25G342 = 4, BuildLater = 5
};
volatile SInt32 bluetoothdBuild;
char bluetoothdBuildName[16];
volatile SInt32 bluetoothdSitesAmbiguous;
// mSBC matching pages seen before USB scope was known (decision deferred).
volatile SInt32 msbcDeferredBeforeProbe;
// bluetoothd pages left native: no verifier restore, or no patch-range room.
volatile SInt32 bluetoothdPagesUnpatched;
// Per-feature decision for the current bluetoothd vnode (FeatureLatch), under pageLock.
uint8_t featureLatch[3];
// A validated native sender page may stay resident. Do not enable WBS until
// that page is safely revalidated/patched; never retain its raw page pointer.
bool msbcSenderNative;
volatile SInt32 bluetoothdPages, bluetoothdPatched, bluetoothdAddressPatched, bluetoothdAddressRetryPatched;
volatile SInt32 bluetoothdWbsPatched, bluetoothdHfpFillPatched;
volatile SInt32 bluetoothdRpaHelpersPatched, bluetoothdRpaLegacyPatched, bluetoothdRpaScanPatched;
volatile SInt32 bluetoothdRpaDecodePatched, bluetoothdRpaDevicePatched;
volatile SInt32 bluetoothdRpaConnectPatched, bluetoothdRpaUpdatePatched;
uint32_t patchRangeCount; // bluetoothd patch ranges, stored with release after each entry is filled
volatile SInt32 patchRangeOverflow, verifyRestored, verifyRestoreFailed;
// bluetoothd blocks whose hash failed with no recorded patch in them.
volatile SInt32 verifyFailed;
volatile uint64_t verifyLastFailOffset;
// verifyOp in the vnode operations vector: 0 no bluetoothd page yet, 1 in,
// otherwise the VerifyOpState step that failed.
volatile SInt32 verifyOpState;
uint32_t vnodeOpsOffset;
bool verifyFixEnabled, verifyFixRouted;

void publishCounters()
{
    IOService *res = IOService::getResourceService();
    OSDictionary *d = OSDictionary::withCapacity(160);
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
    number("AclPipesBound", (uint32_t)aclPipesBound);
    d->setObject("LeFeaturesShimEnabled", leShimEnabled ? kOSBooleanTrue : kOSBooleanFalse);
    number("LeFeaturesCommands", (uint32_t)leCommands);
    number("LeFeaturesCompletions", (uint32_t)leCompletions);
    number("LeFeaturesErrors", (uint32_t)leErrors);
    d->setObject("LeReadyDiagnosticsRouted", leShimEnabled ? kOSBooleanTrue : kOSBooleanFalse);
    d->setObject("LeReadyMaskEnabled", leReadyMaskEnabled && leShimEnabled ? kOSBooleanTrue : kOSBooleanFalse);
    mtk_bt_le_ready_diagnostics ready {};
    if (leLock) {
        IOLockLock(leLock);
        mtk_bt_le_ready_snapshot(&ready);
        IOLockUnlock(leLock);
    }
    auto number64 = [d](const char *key, uint64_t value) {
        if (OSNumber *n = OSNumber::withNumber(value, 64)) {
            d->setObject(key, n);
            n->release();
        }
    };
    number64("LeReadyControllerID", ready.controller_id);
    number64("LeEventMaskRequested", ready.mask_before);
    number64("LeEventMaskSent", ready.mask_after);
    number("LeEventMaskRewritten", ready.mask_rewrites);
    number("LeEventMaskWriteErrors", ready.mask_write_errors);
    number("LeEventMaskDleEnabled", (ready.mask_after & 0x40) != 0);
    number("LeEventMaskPhyEnabled", (ready.mask_after & 0x800) != 0);
    number("LeReadyMalformedEvents", ready.malformed);
    number("LeReadySerial", ready.serial);
    const char *commandNames[] {"LeSetEventMask", "LeSetDataLength", "LeSetPhy", "LeReadPhy"};
    const char *eventNames[] {"LeDataLengthChange", "LePhyUpdateComplete", "LeChannelSelection", "LeRemoteFeatures"};
    char key[80];
    for (size_t i = 0; i < 4; ++i) {
        const auto &c = ready.commands[i];
        auto commandNumber = [&](const char *suffix, uint32_t value) {
            snprintf(key, sizeof(key), "%s%s", commandNames[i], suffix);
            number(key, value);
        };
        commandNumber("Commands", c.count);
        commandNumber("UsbErrors", c.usb_errors);
        commandNumber("LastUsbStatus", c.last_usb_status);
        commandNumber("StatusEvents", c.status_events);
        commandNumber("CompleteEvents", c.complete_events);
        commandNumber("LastStatus", c.last_status);
        commandNumber("LastResponseKind", c.last_kind);
        commandNumber("LastHandle", c.last_handle);
        commandNumber("LastArg0", c.arg0);
        commandNumber("LastArg1", c.arg1);
        commandNumber("Serial", c.serial);
        commandNumber("ResponseSerial", c.response_serial);
        const auto &e = ready.meta[i];
        auto eventNumber = [&](const char *suffix, uint32_t value) {
            snprintf(key, sizeof(key), "%s%s", eventNames[i], suffix);
            number(key, value);
        };
        eventNumber("Events", e.count);
        eventNumber("LastHandle", e.last_handle);
        eventNumber("LastStatus", e.last_status);
        eventNumber("LastValue0", e.value0);
        eventNumber("LastValue1", e.value1);
        eventNumber("LastValue2", e.value2);
        eventNumber("LastValue3", e.value3);
        eventNumber("Serial", e.serial);
    }
    d->setObject("BluetoothdExtAdvRouted", extAdvPatchEnabled && extAdvPatchRouted ? kOSBooleanTrue : kOSBooleanFalse);
    number("BluetoothdPages", (uint32_t)bluetoothdPages);
    number("BluetoothdExtAdvPatched", (uint32_t)bluetoothdPatched);
    number("BluetoothdAdvAddressPatched", (uint32_t)bluetoothdAddressPatched);
    number("BluetoothdAdvAddressRetryPatched", (uint32_t)bluetoothdAddressRetryPatched);
    const SInt32 build = __atomic_load_n(&bluetoothdBuild, __ATOMIC_ACQUIRE);
    if (build != BuildUnread) {
        if (OSString *v = OSString::withCString(bluetoothdBuildName)) {
            d->setObject("BluetoothdBuild", v);
            v->release();
        }
    }
    d->setObject("BluetoothdBuildKnown", build == Build25G241 || build == Build25G332 || build == Build25G342 ? kOSBooleanTrue : kOSBooleanFalse);
    d->setObject("BluetoothdBuildSupported", build >= Build25G241 ? kOSBooleanTrue : kOSBooleanFalse);
    d->setObject("BluetoothdAnyBuild", anyBuildPatches ? kOSBooleanTrue : kOSBooleanFalse);
    // 2 bits per feature (adv, mSBC, RPA): 0 undecided, 1 on, 2 off.
    number("BluetoothdPatchFeatures", (uint32_t)(featureLatch[0] | featureLatch[1] << 2 | featureLatch[2] << 4));
    number("BluetoothdSitesAmbiguous", (uint32_t)bluetoothdSitesAmbiguous);
    number("BluetoothdMsbcDeferredBeforeProbe", (uint32_t)msbcDeferredBeforeProbe);
    number("BluetoothdPagesUnpatched", (uint32_t)bluetoothdPagesUnpatched);
    const SInt32 rpaState = __atomic_load_n(&rpaPlanState, __ATOMIC_ACQUIRE);
    d->setObject("BluetoothdRpaEnabled",
                 rpaPatchEnabled && verifyFixRouted && rpaState == RpaPlanReady ? kOSBooleanTrue : kOSBooleanFalse);
    d->setObject("BluetoothdRpaExecRouted", rpaExecRouted ? kOSBooleanTrue : kOSBooleanFalse);
    number("BluetoothdRpaPlanState", (uint32_t)rpaState);
    number("BluetoothdRpaPlanRejected", (uint32_t)rpaPlanRejected);
    number("BluetoothdCommonAbiVerified", __atomic_load_n(&bluetoothdCommonAbi, __ATOMIC_ACQUIRE));
    number("BluetoothdForeignPagesRolledBack", (uint32_t)bluetoothdForeignPagesRolledBack);
    if (rpaState == RpaPlanReady && rpaPlan) {
        number("BluetoothdRpaPlanEdits", rpaPlan->count);
        number64("BluetoothdRpaHelperAddress", rpaPlan->helper_address);
        number64("BluetoothdRpaSliceOffset", rpaPlan->slice_offset);
        if (OSData *uuid = OSData::withBytes(rpaPlan->uuid, sizeof(rpaPlan->uuid))) {
            d->setObject("BluetoothdRpaImageUUID", uuid);
            uuid->release();
        }
    }
    d->setObject("BluetoothdRpaHelpersReady",
        __atomic_load_n(&rpaHelpersReady, __ATOMIC_ACQUIRE) ? kOSBooleanTrue : kOSBooleanFalse);
    number("BluetoothdRpaHelpersPatched", (uint32_t)bluetoothdRpaHelpersPatched);
    number("BluetoothdRpaLegacyPatched", (uint32_t)bluetoothdRpaLegacyPatched);
    number("BluetoothdRpaScanPatched", (uint32_t)bluetoothdRpaScanPatched);
    number("BluetoothdRpaDecodePatched", (uint32_t)bluetoothdRpaDecodePatched);
    number("BluetoothdRpaDevicePatched", (uint32_t)bluetoothdRpaDevicePatched);
    number("BluetoothdRpaConnectPatched", (uint32_t)bluetoothdRpaConnectPatched);
    number("BluetoothdRpaUpdatePatched", (uint32_t)bluetoothdRpaUpdatePatched);
    number("BluetoothdWbsPatched", (uint32_t)bluetoothdWbsPatched);
    number("BluetoothdHfpFillPatched", (uint32_t)bluetoothdHfpFillPatched);
    number("MsbcScopeFix", 2);
    number("MsbcControllerScope", msbcControllerScope.observed());
    d->setObject("MsbcWidebandAllowed",
        msbcPatchEnabled && isMsbcReady() && msbcControllerScope.allowsWideband() ? kOSBooleanTrue : kOSBooleanFalse);
    d->setObject("BluetoothdVerifyFixRouted", verifyFixRouted ? kOSBooleanTrue : kOSBooleanFalse);
    number("BluetoothdPatchRanges", __atomic_load_n(&patchRangeCount, __ATOMIC_ACQUIRE));
    number("BluetoothdPatchRangeOverflow", (uint32_t)patchRangeOverflow);
    number("BluetoothdVerifyRestored", (uint32_t)verifyRestored);
    number("BluetoothdVerifyRestoreFailed", (uint32_t)verifyRestoreFailed);
    number("BluetoothdVerifyHookState", (uint32_t)__atomic_load_n(&verifyOpState, __ATOMIC_ACQUIRE));
    number("BluetoothdVnodeOpsOffset", vnodeOpsOffset);
    number("BluetoothdVerifyFailed", (uint32_t)verifyFailed);
    number("BluetoothdVerifyLastFailOffset", (uint32_t)__atomic_load_n(&verifyLastFailOffset, __ATOMIC_RELAXED));
    res->setProperty("MtkWirelessBT", d);
    d->release();
}

thread_call_t publishCall;
volatile UInt32 publishQueued;
constexpr uint32_t kCounterPublishDelayMs = 1000;

// USB completions/EP0 and vnode hooks only queue a snapshot. Coalesce bursts
// (including malformed events) to at most one registry allocation pass per
// second, without extending the deadline on every new transfer.
void publishLater()
{
    if (publishCall && OSCompareAndSwap(0, 1, &publishQueued)) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(kCounterPublishDelayMs, kMillisecondScale, &deadline);
        thread_call_enter_delayed(publishCall, deadline);
    }
}

void publishDeferred(thread_call_param_t, thread_call_param_t)
{
    // Updates during this snapshot can queue the next one; none are stranded
    // behind a flag that is cleared only after publication finishes.
    __atomic_store_n(&publishQueued, 0, __ATOMIC_RELEASE);
    publishCounters();
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

/* Only HCI commands (class, host to device) to the loaded MediaTek
 * controller; other devices' 6-byte EP0 requests are left untouched. */
void fixReadClock(IOUSBHostDevice *device, StandardUSB::DeviceRequest &request, void *data,
                  IOMemoryDescriptor *descriptor)
{
    if (request.wLength != 6 || (request.bmRequestType & 0xE0) != 0x20 || !isLoadedMediaTek(device))
        return;
    if (data) {
        if (mtk_bt_fix_read_clock(request.bmRequestType, static_cast<uint8_t *>(data), 6)) {
            OSIncrementAtomic(&readClockRewritten);
            publishLater();
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
        publishLater();
    }
    descriptor->complete(kIODirectionInOut);
}

#pragma mark - Non-flushable ACL

/* HCI ACL bulk OUT pipes of loaded MediaTek controllers. Address keys and
 * numeric owner ids only: retaining a pipe/interface here would keep a dead
 * USB session alive across re-enumeration. leLock protects the entire record
 * (not independently published address/owner fields). */
constexpr size_t kAclPipeSlots = 8;
struct AclPipeBinding {
    uintptr_t pipe;
    uint64_t interfaceId;
    uint64_t controllerId;
};
AclPipeBinding aclPipes[kAclPipeSlots];
size_t aclPipeNext;

bool isMediaTekAclPipe(IOUSBHostPipe *pipe)
{
    if (!pipe || !leLock)
        return false;
    const uintptr_t key = reinterpret_cast<uintptr_t>(pipe);
    bool bound = false;
    IOLockLock(leLock);
    for (const auto &slot : aclPipes) {
        if (slot.pipe == key) {
            bound = true;
            break;
        }
    }
    IOLockUnlock(leLock);
    return bound;
}

// Clear every returned address before any descriptor or class filter. Rust
// keeps provisional command state for a repeated live owner, but no mapping
// is visible until the endpoint is classified and bound again.
void preparePipeMappings(IOUSBHostPipe *pipe, uint64_t interfaceId)
{
    if (!pipe || !leLock)
        return;
    const uintptr_t key = reinterpret_cast<uintptr_t>(pipe);
    IOLockLock(leLock);
    for (auto &slot : aclPipes)
        if (slot.pipe == key)
            slot = {};
    mtk_bt_le_prepare_event_pipe(pipe, interfaceId);
    IOLockUnlock(leLock);
}

void bindAclPipe(IOUSBHostPipe *pipe, uint64_t interfaceId, uint64_t controllerId, bool mediatek)
{
    if (!pipe || !leLock)
        return;
    const uintptr_t key = reinterpret_cast<uintptr_t>(pipe);
    IOLockLock(leLock);
    for (auto &slot : aclPipes)
        if (slot.pipe == key)
            slot = {};
    if (mediatek && interfaceId && controllerId) {
        OSIncrementAtomic(&aclPipesBound);
        AclPipeBinding *free = nullptr;
        for (auto &slot : aclPipes)
            if (!slot.pipe && !free)
                free = &slot;
        // A bounded table may evict a key, never retain obsolete USB objects.
        if (!free)
            free = &aclPipes[aclPipeNext++ % kAclPipeSlots];
        *free = {key, interfaceId, controllerId};
    }
    IOLockUnlock(leLock);
}

void forgetInterfaceMappings(uint64_t interfaceId)
{
    if (!interfaceId || !leLock)
        return;
    IOLockLock(leLock);
    for (auto &slot : aclPipes)
        if (slot.interfaceId == interfaceId)
            slot = {};
    mtk_bt_le_unbind_interface(interfaceId);
    IOLockUnlock(leLock);
}

void forgetControllerMappings(uint64_t controllerId)
{
    if (!controllerId || !leLock)
        return;
    IOLockLock(leLock);
    for (auto &slot : aclPipes)
        if (slot.controllerId == controllerId)
            slot = {};
    mtk_bt_le_unbind_controller(controllerId);
    IOLockUnlock(leLock);
}

void fixAclFlush(IOUSBHostPipe *pipe, IOMemoryDescriptor *buffer, uint32_t length)
{
    if (!buffer || !isMediaTekAclPipe(pipe))
        return;
    const StandardUSB::EndpointDescriptor *ep = pipe->getEndpointDescriptor();
    if (!ep || !mtk_bt_acl_candidate(ep->bmAttributes, ep->bEndpointAddress, length) ||
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
            publishLater();
    }
    buffer->complete(kIODirectionInOut);
}

#pragma mark - LE feature-mask compatibility

/* Loaded MediaTek only. Rust validates HCI framing; C++ moves the eight
 * mask bytes. An explicit zero mask remains zero. No added USB transaction. */
uint32_t leReadyCommand(IOUSBHostDevice *device, StandardUSB::DeviceRequest &request, void *data,
                        IOMemoryDescriptor *descriptor)
{
    // Pure request filter first: it rejects almost every EP0 request without
    // the two registry property lookups.
    if (!leShimEnabled ||
        !mtk_bt_le_ready_candidate_request(request.bmRequestType, request.bRequest, request.wValue,
                                           request.wIndex, request.wLength) ||
        !isLoadedMediaTek(device))
        return 0;
    uint8_t command[16];
    if (request.wLength > sizeof(command) || !readRequestBytes(data, descriptor, command, request.wLength))
        return 0;
    IOLockLock(leLock);
    uint32_t observed = mtk_bt_le_ready_command(device->getRegistryEntryID(), command, request.wLength,
                                               leReadyMaskEnabled);
    IOLockUnlock(leLock);
    if (observed & 0x100) {
        bool ok = false;
        if (data) {
            memcpy(static_cast<uint8_t *>(data) + 3, command + 3, 8);
            ok = true;
        } else if (descriptor && descriptor->prepare(kIODirectionInOut) == kIOReturnSuccess) {
            uint8_t original[8];
            if (descriptor->readBytes(3, original, sizeof(original)) == sizeof(original)) {
                ok = descriptor->writeBytes(3, command + 3, 8) == 8;
                if (!ok)
                    descriptor->writeBytes(3, original, sizeof(original));
            }
            descriptor->complete(kIODirectionInOut);
        }
        IOLockLock(leLock);
        mtk_bt_le_ready_mask_written(ok);
        IOLockUnlock(leLock);
    }
    if (observed)
        publishLater();
    return observed & 0xff;
}

void leReadyUsbStatus(uint32_t command, IOReturn status)
{
    if (!command)
        return;
    IOLockLock(leLock);
    mtk_bt_le_ready_usb_status(command, static_cast<uint32_t>(status));
    IOLockUnlock(leLock);
    publishLater();
}

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
        publishLater();
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
    bool observed = false;
    if (buffer->readBytes(0, events, length) == length) {
        IOLockLock(leLock);
        // Observe original hardware events, before the separate 0xFD3C shim.
        observed = mtk_bt_le_ready_events(id, events, length);
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
    }
    if (rewritten || observed)
        publishLater();
}

/* Wraps a USB completion. Two references: the callback and the submitter,
 * so a callback that runs before the submission returns is covered. */
struct LeCompletion {
    IOUSBHostCompletion original;
    IOMemoryDescriptor *buffer; /* event pipe reads; NULL for EP0 commands */
    uint32_t capacity;
    uint64_t registryId;
    uint64_t token;
    uint32_t readyCommand;
    volatile SInt32 references;
    volatile UInt32 claimed;
};

LeCompletion *makeLeCompletion(IOUSBHostCompletion *completion, uint64_t id, uint64_t token,
                               IOMemoryDescriptor *buffer = nullptr, uint32_t capacity = 0,
                               uint32_t readyCommand = 0)
{
    auto *c = static_cast<LeCompletion *>(IOMalloc(sizeof(LeCompletion)));
    if (!c)
        return nullptr;
    *c = { *completion, buffer, capacity, id, token, readyCommand, 2, 0 };
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
    } else {
        leReadyUsbStatus(c->readyCommand, status);
        if (c->token && status != kIOReturnSuccess) {
            leCancel(c->registryId, c->token);
            OSIncrementAtomic(&leErrors);
        }
    }
    const IOUSBHostCompletion original = c->original;
    original.action(original.owner, original.parameter, status, transferred);
    releaseLeCompletion(c);
}

void finishLeSubmission(LeCompletion *c, IOReturn status)
{
    // USB promises a callback only for a successful enqueue.
    if (status != kIOReturnSuccess && OSCompareAndSwap(0, 1, &c->claimed)) {
        leReadyUsbStatus(c->readyCommand, status);
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
    auto *device = static_cast<IOUSBHostDevice *>(that);
    fixReadClock(device, request, data, descriptor);
    if (randomAddressHack)
        randomAddress(that, provider, request, data, descriptor, timeout);

    const uint32_t readyCommand = leReadyCommand(device, request, data, descriptor);
    const uint64_t token = (!completion || completion->action) ? leArm(device, request, data, descriptor) : 0;
    LeCompletion *context = nullptr;
    IOUSBHostCompletion replacement {};
    if ((token || readyCommand) && completion && completion->action) {
        context = makeLeCompletion(completion, device->getRegistryEntryID(), token, nullptr, 0, readyCommand);
        if (context) {
            replacement = { nullptr, leUsbCompletion, context };
        } else {
            if (token)
                leCancel(device->getRegistryEntryID(), token);
            OSIncrementAtomic(&leErrors);
        }
    }
    IOReturn status = FunctionCast(deviceRequest, orgDeviceRequest)(that, provider, request, data, descriptor, length,
                                                                    context ? &replacement : completion, timeout);
    if (context) {
        finishLeSubmission(context, status);
    } else {
        leReadyUsbStatus(readyCommand, status);
        if (token && status != kIOReturnSuccess) {
            leCancel(device->getRegistryEntryID(), token);
            OSIncrementAtomic(&leErrors);
        }
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
    // IOUSBHostInterface has one open session. Only its actual owner closes
    // it; a no-op close by another client must not invalidate live mappings.
    // Its real close synchronously aborts session IO before we detach mSBC.
    const bool closing = interface && client && interface->isOpen(client);
    const bool scoped = closing && isMsbcReady() && msbcBridge &&
        (msbcBridge->ownsInterface(interface) || msbcCachedScoInterface(interface));
    const uint64_t generation = scoped ? msbcBridge->beginInterfaceChange() : 0;
    if (closing)
        forgetInterfaceMappings(interface->getRegistryEntryID());
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

/* Remember which interrupt IN pipe carries HCI events and which bulk OUT pipe
 * carries ACL data of a loaded MediaTek controller, and let the mSBC bridge
 * see the SCO pipes (0x03/0x83). Pipe addresses are lookup keys only: every
 * pipe handed out first unbinds stale mappings of its address. */
IOUSBHostPipe *copyPipe(IOUSBHostInterface *interface, uint8_t address)
{
    IOUSBHostPipe *pipe = FunctionCast(copyPipe, orgCopyPipe)(interface, address);
    if (!pipe)
        return pipe;
    const uint64_t interfaceId = interface ? interface->getRegistryEntryID() : 0;
    preparePipeMappings(pipe, interfaceId);
    if ((address == 0x03 || address == 0x83) && isMsbcReady() && msbcBridge &&
        msbcBridge->observePipe(interface, pipe))
        msbcBridge->publishStatus();
    const auto *ep = pipe->getEndpointDescriptor();
    const auto *desc = interface ? interface->getInterfaceDescriptor() : nullptr;
    const bool hci = ep && desc && desc->bInterfaceClass == 0xE0 && desc->bInterfaceSubClass == 1 &&
                     desc->bInterfaceProtocol == 1;
    const bool eventPipe = hci && (ep->bmAttributes & 3) == 3 && (ep->bEndpointAddress & 0x80);
    const bool aclPipe = hci && (ep->bmAttributes & 3) == 2 && !(ep->bEndpointAddress & 0x80);
    IOUSBHostDevice *device = nullptr;
    if (eventPipe || aclPipe)
        for (IOService *p = interface->getProvider(); p && !device; p = p->getProvider())
            device = OSDynamicCast(IOUSBHostDevice, p);
    const bool mediatek = isLoadedMediaTek(device);
    const uint64_t controllerId = device ? device->getRegistryEntryID() : 0;
    bindAclPipe(pipe, interfaceId, controllerId, aclPipe && mediatek);
    if (leShimEnabled) {
        // A same-interface endpoint that no longer carries HCI events is also
        // unbound. No unrelated transfer schedules a diagnostic publication.
        IOLockLock(leLock);
        mtk_bt_le_bind_event_pipe(pipe, interfaceId, controllerId, eventPipe && mediatek);
        IOLockUnlock(leLock);
    }
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
        !copyPipeRouted || !orgCopyPipe || !orgInterfaceClose)
        return;
    KernelPatcher::RouteRequest routes[] {
        {"__ZN13IOUSBHostPipe2ioEP18IOMemoryDescriptorP25IOUSBHostIsochronousFramejyP30IOUSBHostIsochronousCompletion",
         reinterpret_cast<void *>(isoIo), orgIsoIo},
        {"__ZN18IOUSBHostInterface22selectAlternateSettingEh",
         reinterpret_cast<void *>(selectAlternateSetting), orgSelectAlternateSetting},
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
    // HCI mapping teardown is needed even with -mtkbtnomsbc. Route close once;
    // the mSBC installer requires this original, rather than routing it again.
    const bool close = route(patcher, index, address, size, "__ZN18IOUSBHostInterface5closeEP9IOServicej",
                             reinterpret_cast<void *>(interfaceClose), orgInterfaceClose);
    installMsbcBridge(patcher, index, address, size, pipes && close);
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
 * advertising sets on as well. x86_64 bluetoothd of macOS 26.7.1 (25G241)
 * and 26.7.2 (25G332, 0x10033abec): the whole 77-byte function, exact, unique
 * in the binary (its BT-spec-sets twin differs only in sete/setae at +54) and
 * inside one page, so a per-page search finds it. It has no displacements;
 * it stops matching only if Apple changes the chip ranges. Boot-arg
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
 * 25G332 x86_64 at 0x100511239 (25G241: 0x100510e99), inside one page. Both
 * call displacements are wildcards (they are all that differs between the
 * two builds). The NOPed call's target cannot be checked from this page, so
 * ScopeMinimumBuild preserves the offline-verified legacy setter/gate layouts;
 * later layouts must also pass the full-image pre-exec validator.
 */
const uint8_t advAddressFind[] {
    0xe8, 0x34, 0x32, 0xe8, 0xff, 0x83, 0xb8, 0x34, 0x03, 0x00, 0x00, 0x03, 0x77, 0x2a,
    0x31, 0xff, 0xe8, 0x1e, 0x6c, 0xce, 0xff};
const uint8_t advAddressFindMask[] {
    0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00};
const uint8_t advAddressReplace[] {
    0xe8, 0x34, 0x32, 0xe8, 0xff, 0x83, 0xb8, 0x34, 0x03, 0x00, 0x00, 0x03, 0x77, 0x2a,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
const uint8_t advAddressReplaceMask[] {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static_assert(sizeof(advAddressFind) == sizeof(advAddressReplace) && sizeof(advAddressFind) == sizeof(advAddressFindMask) &&
                  sizeof(advAddressFind) == sizeof(advAddressReplaceMask),
              "address patch size");

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
 * 25G332 x86_64 0x1001fa68d (25G241: 0x1001fa20d), 64 bytes inside one page.
 * Wildcards: the gate's RIP-relative offset (+34, overwritten by the guard)
 * and the trailing call displacement (+60, kept). The guard's short branches
 * re-encode the native +26/+39 targets, so those stay exact. ScopeMinimumBuild
 * (with advAddress): natively, a false gate jumps straight to the
 * continuation; the patched guard instead calls a helper that checks the same
 * gate and returns 0 without sending HCI 0xcb or calling the callback, so
 * with advAddress unpatched (gate false) the set start waits for a completion
 * that never comes (verified against 25G332: gate 0x100b1cd00, helper
 * 0x1001f75bb, continuation 0x1001fa743).
 */
const uint8_t advAddressRetryFind[] {
    0x44, 0x8b, 0x04, 0x3a, 0x44, 0x33, 0x84, 0x08, 0x0e, 0x0d, 0x00, 0x00, 0x8b, 0x54, 0x3a, 0x03,
    0x33, 0x94, 0x08, 0x11, 0x0d, 0x00, 0x00, 0x44, 0x09, 0xc2, 0x0f, 0x84, 0x80, 0x00, 0x00, 0x00,
    0x80, 0x3d, 0x4c, 0x26, 0x92, 0x00, 0x00, 0x74, 0x77, 0x48, 0x8d, 0x3c, 0x08, 0x48, 0x81, 0xc7,
    0x0e, 0x0d, 0x00, 0x00, 0x48, 0x8d, 0x15, 0x7b, 0x00, 0x00, 0x00, 0xe8, 0xee, 0xce, 0xff, 0xff};
const uint8_t advAddressRetryFindMask[] {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00};
const uint8_t advAddressRetryReplace[] {
    0x44, 0x8b, 0x04, 0x3a, 0x44, 0x33, 0x84, 0x08, 0x0e, 0x0d, 0x00, 0x00, 0x8b, 0x54, 0x3a, 0x03,
    0x33, 0x94, 0x08, 0x11, 0x0d, 0x00, 0x00, 0xf6, 0x84, 0x08, 0x0e, 0x0d, 0x00, 0x00, 0x01, 0x75,
    0x08, 0x44, 0x09, 0xc2, 0x74, 0x7a, 0x90, 0x90, 0x90, 0x48, 0x8d, 0x3c, 0x08, 0x48, 0x81, 0xc7,
    0x0e, 0x0d, 0x00, 0x00, 0x48, 0x8d, 0x15, 0x7b, 0x00, 0x00, 0x00, 0xe8, 0xee, 0xce, 0xff, 0xff};
// Only the guard (+23..+40) is written.
const uint8_t advAddressRetryReplaceMask[] {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static_assert(sizeof(advAddressRetryFind) == sizeof(advAddressRetryReplace) &&
                  sizeof(advAddressRetryFind) == sizeof(advAddressRetryFindMask) &&
                  sizeof(advAddressRetryFind) == sizeof(advAddressRetryReplaceMask),
              "address retry patch size");

/* Historical 1.1.0 Instant Hotspot / outgoing BLE oracle (25G332).
 * The arrays below are test-only; runtime 1.1.1 resolves native signatures,
 * VM mappings and every helper/child relocation in mtk_bt::rpa before exec.
 * This historical description explains the behavior the new payload preserves.
 * The host AH resolver replaces a peer RPA with its identity in scan reports,
 * but chip 0 has no initialized BCM/ACI/SRS privacy backend. Its accept list
 * therefore contains a public identity that cannot match the on-air RPA.
 * Keep the verified legacy ADV_IND RPA separately, update LeDevice's last
 * advertised address under its existing mutex, and use that address only at
 * the two initiating/accept-list call sites. UUID, identity, IRK and security
 * logic remain native; all HCI commands wait for real controller completions.
 *
 * Five leaf helpers occupy 261 bytes of the zero padding at the end of the
 * executable __TEXT segment, after __eh_frame (anchor: its last 16 bytes plus
 * the padding). They must stay off the header/load-command pages: exec checks
 * those against the code signature, so helpers at 0x1000026c8 (right after
 * the load commands) made every re-exec of bluetoothd fail (OS_REASON_EXEC,
 * 2026-10-31). Helpers (unslid VA): legacy 0x100aa2c00, retain 0x100aa2c0d,
 * device 0x100aa2c57, address 0x100aa2cca, paired 0x100aa2ce8.
 * Assembly source: tests/bt-page-patches/rpa-25G332.S.
 *
 * The internal direct-address type 0x81 exists only on resolved undirected
 * legacy reports, whose original field is zero. It is cleared before cache
 * merge/listeners. Clear exactly seven descriptor bytes between reports;
 * the following flag is live. Extended/directed reports keep their meaning.
 * The device helper checks the exact native caller before reading its frame;
 * it keeps the bool ABI and does not change the public paired-test function.
 *
 * Disabled by -mtkbtnorpa or -mtkbtnoextadv, and requires the verify-block
 * route. Other builds keep their advertising patches; no RPA patch for 25G241.
 * The helper page is not touched at launch, so child sites cannot wait for
 * it: a page validated before it would stay native forever (1.0.10 patched
 * only 2 of 7 sites). Children are patched whenever their page is validated
 * on 25G332 (kern.osversion, ScopeRpaBuild); their first call faults the
 * helper page in, and validation installs the helpers before that call
 * executes. The feature is decided once per vnode (featureOn).
 */
// Historical byte oracles for offline regression only. Runtime uses the
// full-image signature/relocation resolver in rust/mtk-bt/src/rpa.rs, not
// these build-specific displacements or the old helper anchor.
#ifdef MTK_BT_RPA_LEGACY_TEST_ORACLES
const uint8_t rpaHelpersFind25G332[] {
    0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t rpaHelpersReplace25G332[] {
    0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x31, 0xc0, 0x89, 0x47, 0x33, 0x89, 0x47, 0x36,
    0xe9, 0x17, 0x08, 0x78, 0xff, 0x48, 0x8d, 0x15, 0xdd, 0x97, 0x76, 0xff, 0x48, 0x39, 0x55, 0x08,
    0x75, 0x2d, 0x80, 0x3b, 0x00, 0x75, 0x28, 0x48, 0x8b, 0x15, 0x8a, 0x7f, 0x09, 0x00, 0x48, 0x85,
    0xd2, 0x74, 0x1c, 0x83, 0xba, 0x34, 0x03, 0x00, 0x00, 0x00, 0x75, 0x13, 0x41, 0x8b, 0x04, 0x24,
    0x41, 0x8b, 0x4c, 0x24, 0x03, 0x89, 0x43, 0x33, 0x89, 0x4b, 0x36, 0xc6, 0x43, 0x33, 0x81, 0x8b,
    0x45, 0xb8, 0x8b, 0x4d, 0xbb, 0x41, 0x89, 0x4c, 0x24, 0x03, 0x41, 0x89, 0x04, 0x24, 0xc3, 0x48,
    0x8d, 0x0d, 0x1e, 0x8c, 0xc9, 0xff, 0x48, 0x39, 0x4d, 0x08, 0x75, 0x5c, 0x48, 0x8b, 0x4d, 0x00,
    0x48, 0x8b, 0x89, 0x80, 0xfb, 0xff, 0xff, 0x48, 0x8b, 0x49, 0x08, 0x80, 0xb9, 0xd6, 0x00, 0x00,
    0x00, 0x81, 0x75, 0x44, 0x48, 0x8b, 0x91, 0xd0, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x81, 0xd0, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x36, 0x48, 0x8b, 0x0d, 0x16, 0x7f,
    0x09, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x21, 0x83, 0xb9, 0x34, 0x03, 0x00, 0x00, 0x00, 0x75, 0x18,
    0x48, 0x83, 0x78, 0x18, 0x00, 0x75, 0x08, 0x48, 0x8b, 0x48, 0x10, 0x48, 0x89, 0x48, 0x18, 0x48,
    0x0f, 0xba, 0xf2, 0x37, 0x48, 0x89, 0x50, 0x10, 0x48, 0x85, 0xc0, 0x74, 0x04, 0x44, 0x88, 0x70,
    0x61, 0xc3, 0x31, 0xd2, 0x48, 0x8b, 0x05, 0xdd, 0x7e, 0x09, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x0b,
    0x83, 0xb8, 0x34, 0x03, 0x00, 0x00, 0x00, 0x75, 0x02, 0xff, 0xc2, 0xe9, 0x08, 0x3c, 0xc6, 0xff,
    0x48, 0x8b, 0x05, 0xc1, 0x7e, 0x09, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x0c, 0x83, 0xb8, 0x34, 0x03,
    0x00, 0x00, 0x00, 0x75, 0x03, 0x31, 0xc0, 0xc3, 0xe9, 0x51, 0x14, 0xc7, 0xff, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static_assert(sizeof(rpaHelpersFind25G332) == sizeof(rpaHelpersReplace25G332), "RPA patch size");

const uint8_t rpaLegacyFind25G332[] {
    0xc6, 0x45, 0xac, 0x00, 0xc7, 0x45, 0xaf, 0x00, 0x01, 0x00, 0x00, 0x48, 0x8d, 0x7d, 0x80, 0xe8,
    0x33, 0x70, 0x01, 0x00, 0x48, 0x8b, 0x7d, 0x90, 0xe8, 0xcb, 0x6e, 0xee, 0xff};
const uint8_t rpaLegacyReplace25G332[] {
    0xc6, 0x45, 0xac, 0x00, 0xc7, 0x45, 0xaf, 0x00, 0x01, 0x00, 0x00, 0x48, 0x8d, 0x7d, 0x80, 0xe8,
    0x0f, 0x68, 0x89, 0x00, 0x48, 0x8b, 0x7d, 0x90, 0xe8, 0xcb, 0x6e, 0xee, 0xff};
static_assert(sizeof(rpaLegacyFind25G332) == sizeof(rpaLegacyReplace25G332), "RPA patch size");

const uint8_t rpaScanFind25G332[] {
    0xe8, 0xa3, 0x41, 0x00, 0x00, 0x85, 0xc0, 0x75, 0x15, 0x80, 0x7d, 0xd7, 0x00, 0x74, 0x0f, 0x8b,
    0x45, 0xb8, 0x8b, 0x4d, 0xbb, 0x41, 0x89, 0x4c, 0x24, 0x03, 0x41, 0x89, 0x04, 0x24};
const uint8_t rpaScanReplace25G332[] {
    0xe8, 0xa3, 0x41, 0x00, 0x00, 0x85, 0xc0, 0x75, 0x15, 0x80, 0x7d, 0xd7, 0x00, 0x74, 0x0f, 0xe8,
    0x36, 0xf7, 0x87, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
static_assert(sizeof(rpaScanFind25G332) == sizeof(rpaScanReplace25G332), "RPA patch size");

const uint8_t rpaDecodeFind25G332[] {
    0x49, 0x8b, 0xb6, 0x68, 0x17, 0x00, 0x00, 0x0f, 0xb6, 0x06, 0x48, 0x83, 0xf8, 0x01, 0x77, 0x48,
    0x0f, 0xb6, 0x7e, 0x01, 0x48, 0xc1, 0xe7, 0x28, 0x44, 0x0f, 0xb6, 0x46, 0x02, 0x49, 0xc1, 0xe0,
    0x20, 0x44, 0x0f, 0xb6, 0x4e, 0x03, 0x41, 0xc1, 0xe1, 0x18, 0x44, 0x0f, 0xb6, 0x56, 0x04, 0x41,
    0xc1, 0xe2, 0x10, 0x44, 0x0f, 0xb6, 0x5e, 0x05, 0x41, 0xc1, 0xe3, 0x08, 0x0f, 0xb6, 0x76, 0x06,
    0x48, 0xc1, 0xe0, 0x30, 0x48, 0x09, 0xf8, 0x4c, 0x09, 0xc0, 0x4c, 0x09, 0xc8, 0x4c, 0x09, 0xd0,
    0x4c, 0x09, 0xd8, 0x48, 0x09, 0xf0, 0xeb, 0x02, 0x31, 0xc0, 0x48, 0x8b, 0x75, 0x88, 0x48, 0x89,
    0x86, 0xd0, 0x00, 0x00, 0x00, 0x08, 0xca, 0x0f, 0x84, 0x88, 0x00, 0x00, 0x00, 0x48, 0x83, 0x3d,
    0xaf, 0x04, 0x3f, 0x00, 0xff};
const uint8_t rpaDecodeReplace25G332[] {
    0x49, 0x8b, 0xb6, 0x68, 0x17, 0x00, 0x00, 0x0f, 0xb6, 0x06, 0x3c, 0x01, 0x76, 0x1d, 0x3c, 0x81,
    0x75, 0x25, 0x41, 0x80, 0xbe, 0x56, 0x17, 0x00, 0x00, 0x00, 0x75, 0x1b, 0x41, 0x80, 0x3e, 0x00,
    0x75, 0x15, 0x8a, 0x46, 0x01, 0x24, 0xc0, 0x3c, 0x40, 0x75, 0x0c, 0x48, 0x8b, 0x06, 0x48, 0x0f,
    0xc8, 0x48, 0xc1, 0xe8, 0x08, 0xeb, 0x23, 0x31, 0xc0, 0xeb, 0x1f, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x48, 0x8b, 0x75, 0x88, 0x48, 0x89,
    0x86, 0xd0, 0x00, 0x00, 0x00, 0x08, 0xca, 0x0f, 0x84, 0x88, 0x00, 0x00, 0x00, 0x48, 0x83, 0x3d,
    0xaf, 0x04, 0x3f, 0x00, 0xff};
static_assert(sizeof(rpaDecodeFind25G332) == sizeof(rpaDecodeReplace25G332), "RPA patch size");

const uint8_t rpaDeviceFind25G332[] {
    0xe8, 0xe7, 0x90, 0xfe, 0xff, 0x48, 0x85, 0xc0, 0x74, 0x04, 0x44, 0x88, 0x70, 0x61, 0x48, 0x8d,
    0x7d, 0xd0, 0xe8, 0xf7, 0x55, 0xba, 0xff, 0x48, 0x89, 0xdf, 0xff, 0x15, 0x7e, 0xd1, 0x38, 0x00};
const uint8_t rpaDeviceReplace25G332[] {
    0xe8, 0xe7, 0x90, 0xfe, 0xff, 0xe8, 0x4b, 0xbc, 0x38, 0x00, 0x90, 0x90, 0x90, 0x90, 0x48, 0x8d,
    0x7d, 0xd0, 0xe8, 0xf7, 0x55, 0xba, 0xff, 0x48, 0x89, 0xdf, 0xff, 0x15, 0x7e, 0xd1, 0x38, 0x00};
static_assert(sizeof(rpaDeviceFind25G332) == sizeof(rpaDeviceReplace25G332), "RPA patch size");

const uint8_t rpaConnectFind25G332[] {
    0x4c, 0x89, 0xff, 0x48, 0x89, 0xde, 0xe8, 0x88, 0x00, 0x00, 0x00, 0x84, 0xc0, 0x74, 0x15, 0x4c,
    0x89, 0xf7, 0x4c, 0x89, 0xfe, 0x48, 0x89, 0xda, 0xb9, 0x02, 0x00, 0x00, 0x00, 0xe8, 0x0b, 0x01,
    0x00, 0x00, 0xeb, 0x25, 0x4c, 0x89, 0xff, 0x48, 0x89, 0xde, 0x31, 0xd2, 0xe8, 0xfc, 0x27, 0xff,
    0xff};
const uint8_t rpaConnectReplace25G332[] {
    0x4c, 0x89, 0xff, 0x48, 0x89, 0xde, 0xe8, 0x1a, 0xec, 0x38, 0x00, 0x84, 0xc0, 0x74, 0x15, 0x4c,
    0x89, 0xf7, 0x4c, 0x89, 0xfe, 0x48, 0x89, 0xda, 0xb9, 0x02, 0x00, 0x00, 0x00, 0xe8, 0x0b, 0x01,
    0x00, 0x00, 0xeb, 0x25, 0x4c, 0x89, 0xff, 0x48, 0x89, 0xde, 0x31, 0xd2, 0xe8, 0xd6, 0xeb, 0x38,
    0x00};
static_assert(sizeof(rpaConnectFind25G332) == sizeof(rpaConnectReplace25G332), "RPA patch size");

const uint8_t rpaUpdateFind25G332[] {
    0x48, 0x8b, 0x3d, 0x2f, 0xbd, 0x44, 0x00, 0x48, 0x8b, 0xb5, 0x78, 0xfd, 0xff, 0xff, 0xe8, 0x69,
    0x2f, 0x04, 0x00, 0x84, 0xc0, 0x0f, 0x84, 0x86, 0x00, 0x00, 0x00, 0x48, 0x83, 0x3d, 0xa3, 0xc7,
    0x44, 0x00, 0xff, 0x0f, 0x85, 0x42, 0x07, 0x00, 0x00, 0x48, 0x8b, 0x35, 0x06, 0xbd, 0x44, 0x00,
    0x4c, 0x89, 0xef, 0x48, 0x8b, 0x95, 0x78, 0xfd, 0xff, 0xff, 0xb9, 0x02, 0x00, 0x00, 0x00, 0xe8,
    0xd2, 0x2f, 0x04, 0x00, 0x48, 0x8b, 0xb5, 0x60, 0xfd, 0xff, 0xff, 0x48, 0x8d, 0xbd, 0x58, 0xfd,
    0xff, 0xff, 0xe8, 0x6b, 0x28, 0xba, 0xff, 0x48, 0x8b, 0x45, 0xa0, 0x48, 0x8b, 0x75, 0xa8, 0x48,
    0x89, 0x85, 0x58, 0xfd, 0xff, 0xff, 0x48, 0x89, 0xb5, 0x60, 0xfd, 0xff, 0xff, 0x48, 0x8b, 0x45,
    0xb0, 0x48, 0x89, 0x85, 0x68, 0xfd, 0xff, 0xff, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0xe1, 0x00, 0x00,
    0x00, 0x48, 0x8d, 0x85, 0x60, 0xfd, 0xff, 0xff, 0x48, 0x89, 0x46, 0x10, 0x48, 0x8d, 0x45, 0xa8,
    0x48, 0x89, 0x45, 0xa0, 0x0f, 0x57, 0xc0, 0x0f, 0x11, 0x00, 0x31, 0xf6, 0xe9, 0xcf, 0x00, 0x00,
    0x00, 0x48, 0x83, 0x3d, 0x1d, 0xc7, 0x44, 0x00, 0xff, 0x0f, 0x85, 0xc6, 0x06, 0x00, 0x00, 0x48,
    0x8b, 0x3d, 0x80, 0xbc, 0x44, 0x00, 0x48, 0x8b, 0xb5, 0x78, 0xfd, 0xff, 0xff, 0x31, 0xd2, 0xe8,
    0x52, 0x56, 0x03, 0x00};
const uint8_t rpaUpdateReplace25G332[] {
    0x48, 0x8b, 0x3d, 0x2f, 0xbd, 0x44, 0x00, 0x48, 0x8b, 0xb5, 0x78, 0xfd, 0xff, 0xff, 0xe8, 0xfb,
    0x1a, 0x3d, 0x00, 0x84, 0xc0, 0x0f, 0x84, 0x86, 0x00, 0x00, 0x00, 0x48, 0x83, 0x3d, 0xa3, 0xc7,
    0x44, 0x00, 0xff, 0x0f, 0x85, 0x42, 0x07, 0x00, 0x00, 0x48, 0x8b, 0x35, 0x06, 0xbd, 0x44, 0x00,
    0x4c, 0x89, 0xef, 0x48, 0x8b, 0x95, 0x78, 0xfd, 0xff, 0xff, 0xb9, 0x02, 0x00, 0x00, 0x00, 0xe8,
    0xd2, 0x2f, 0x04, 0x00, 0x48, 0x8b, 0xb5, 0x60, 0xfd, 0xff, 0xff, 0x48, 0x8d, 0xbd, 0x58, 0xfd,
    0xff, 0xff, 0xe8, 0x6b, 0x28, 0xba, 0xff, 0x48, 0x8b, 0x45, 0xa0, 0x48, 0x8b, 0x75, 0xa8, 0x48,
    0x89, 0x85, 0x58, 0xfd, 0xff, 0xff, 0x48, 0x89, 0xb5, 0x60, 0xfd, 0xff, 0xff, 0x48, 0x8b, 0x45,
    0xb0, 0x48, 0x89, 0x85, 0x68, 0xfd, 0xff, 0xff, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0xe1, 0x00, 0x00,
    0x00, 0x48, 0x8d, 0x85, 0x60, 0xfd, 0xff, 0xff, 0x48, 0x89, 0x46, 0x10, 0x48, 0x8d, 0x45, 0xa8,
    0x48, 0x89, 0x45, 0xa0, 0x0f, 0x57, 0xc0, 0x0f, 0x11, 0x00, 0x31, 0xf6, 0xe9, 0xcf, 0x00, 0x00,
    0x00, 0x48, 0x83, 0x3d, 0x1d, 0xc7, 0x44, 0x00, 0xff, 0x0f, 0x85, 0xc6, 0x06, 0x00, 0x00, 0x48,
    0x8b, 0x3d, 0x80, 0xbc, 0x44, 0x00, 0x48, 0x8b, 0xb5, 0x78, 0xfd, 0xff, 0xff, 0x31, 0xd2, 0xe8,
    0x2c, 0x1a, 0x3d, 0x00};
static_assert(sizeof(rpaUpdateFind25G332) == sizeof(rpaUpdateReplace25G332), "RPA patch size");
#endif // MTK_BT_RPA_LEGACY_TEST_ORACLES

/* HFP wideband gate: bluetoothd enables mSBC only for known chip IDs; a
 * THIRD_PARTY_DONGLE (chip 0) is outside the range. Accept chip 0, only while
 * the USB mSBC bridge is ready (-mtkbtnomsbc off). Same bytes in 25G241 and
 * 25G332 (0x100374738); the controller-lookup call displacement (+5) is a
 * wildcard. Only +17 and +19 are written.
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
const uint8_t wbsFindMask[] {
    0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
const uint8_t wbsReplaceMask[] {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static_assert(sizeof(wbsFind) == sizeof(wbsReplace) && sizeof(wbsFind) == sizeof(wbsFindMask) &&
                  sizeof(wbsFind) == sizeof(wbsReplaceMask),
              "wideband patch size");

/* HFP sender: read one complete PCM chunk per timer tick (drop the imul), so
 * no oversized burst or partial tail overflows the outstanding-byte counter.
 * 25G332 at 0x100337505; 25G241 differs only in the PCM-read call
 * displacement (+8). That and the local branch displacement (+17) are
 * wildcards; only the imul (+0..+3) is written.
 */
const uint8_t hfpFillFind[] {
    0x48, 0x0f, 0xaf, 0xd0, 0x4c, 0x89, 0xf6, 0xe8, 0x33, 0x2f, 0xfd, 0xff, 0x48, 0x85, 0xc0, 0x0f,
    0x84, 0x83, 0x00, 0x00, 0x00, 0x49, 0x89, 0xc4, 0x48, 0x89, 0x83, 0xd0, 0x00, 0x00, 0x00};
const uint8_t hfpFillFindMask[] {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
const uint8_t hfpFillReplace[] {
    0x0f, 0x1f, 0x40, 0x00, 0x4c, 0x89, 0xf6, 0xe8, 0x33, 0x2f, 0xfd, 0xff, 0x48, 0x85, 0xc0, 0x0f,
    0x84, 0x83, 0x00, 0x00, 0x00, 0x49, 0x89, 0xc4, 0x48, 0x89, 0x83, 0xd0, 0x00, 0x00, 0x00};
const uint8_t hfpFillReplaceMask[] {
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static_assert(sizeof(hfpFillFind) == sizeof(hfpFillReplace) && sizeof(hfpFillFind) == sizeof(hfpFillFindMask) &&
                  sizeof(hfpFillFind) == sizeof(hfpFillReplaceMask),
              "HFP fill patch size");

mach_vm_address_t orgCsValidatePage;

/*
 * Page patches live in the shared file cache, and the sealed system volume
 * hashes compressed files per verify block (16 KB for bluetoothd, 4 pages).
 * decmpfs_pagein_compressed fills only the absent pages of a block and then
 * hashes the whole block in place, resident pages included (xnu decmpfs.c,
 * "valid_pages"). Once a patched page stays resident while a neighbour is
 * evicted, every later fault on that neighbour fails with EBADMSG and
 * bluetoothd dies with SIGBUS (2026-10-27/29: BlueToolFixup's 15.4 chipset
 * patch at 0x1e3737, fault at 0x1e2327, crash ~20 s after every launch).
 *
 * Record what each validated bluetoothd page looked like before ANY
 * cs_validate_page patcher touched it (ours wraps BlueToolFixup's route when
 * it is installed later, as with Lilu plugin order today), and hand the
 * file system's verifier a copy of the block with those bytes restored. A
 * range is restored only while the block still holds exactly the patched
 * bytes. Both hooks hold pageLock: verification must not hash a page between
 * its modification and recording the originals, including the first page.
 * -mtkbtnoverifyfix disables it.
 *
 * The verifier is APFS's vnop_verify, reached through vp->v_op. 25G332's
 * kernel inlines VNOP_VERIFY into decmpfs_pagein_compressed
 * (`movq 0xd0(%rdx),%rax; movslq _vnop_verify_desc(%rip),%rcx; callq
 * *(%rax,%rcx,8)`), so the _VNOP_VERIFY route of 1.0.4-1.0.8 never saw a
 * page-in. 1.0.8 hardware, 2026-11-03 17:31 after sleep: bluetoothd aborted
 * itself and every relaunch died at dyld start ("Data hash mismatch for
 * 16384 bytes at offset 11157504", valid_pages 3, invalid_pages 1: the RPA
 * helper page resident, __DATA_CONST's __got absent); the hash APFS got is
 * that of the block with exactly our helper patch applied. launchd then
 * throttled bluetoothd and Bluetooth stayed dead until reboot. So the
 * verifier entry of the bluetoothd vnode's operations vector (APFS's, shared
 * by its regular files) is replaced instead, at bluetoothd's first validated
 * page, which works whether or not a caller inlines VNOP_VERIFY.
 */

// struct vnop_verify_args (xnu bsd/sys/vnode_if.h; not in MacKernelSDK).
struct VnopVerifyArgs {
    void *a_desc;
    vnode_t a_vp;
    off_t a_foffset;
    uint8_t *a_buf;
    size_t a_bufsize;
    size_t *a_verifyblksize;
    void **a_verifyctxp;
    uint32_t a_flags;
    vfs_context_t a_context;
    void *a_verifykind;
};
static_assert(offsetof(VnopVerifyArgs, a_flags) == 0x38 && offsetof(VnopVerifyArgs, a_verifykind) == 0x48 &&
                  sizeof(VnopVerifyArgs) == 0x50,
              "struct vnop_verify_args");
using VnodeOp = int (*)(void *);
VnodeOp orgVerifyOp;
// struct vnodeop_desc, whose first field is int vdesc_offset. The offsets are
// assigned by vfs_op_init in bsd_init, so read it only once bluetoothd runs.
mach_vm_address_t vnopVerifyDesc;

enum VerifyOpState : SInt32 {
    VerifyOpInstalled = 1,
    VerifyOpNoSymbols = 2, // _vnop_verify_desc / _VNOP_VERIFY missing or v_op load not decoded
    VerifyOpBadIndex = 3,  // vdesc_offset out of range
    VerifyOpBadVnode = 4,  // v_op is not followed by v_mount, v_data
    VerifyOpNoEntry = 5,   // empty vector slot
    VerifyOpNoWrite = 6,   // kernel write protection could not be lifted
};

/*
 * offsetof(struct vnode, v_op) from _VNOP_VERIFY, which loads vp->v_op right
 * before vdesc_offset (25G332: `49 8b 8f d0 00 00 00` movq 0xd0(%r15),%rcx;
 * `48 63 15 <rel32>` movslq _vnop_verify_desc(%rip),%rdx). The movslq must
 * address desc; 0 when the pattern is not there.
 */
uint32_t decodeVnodeOpsOffset(const uint8_t *code, size_t size, uint64_t codeAddress, uint64_t desc)
{
    for (size_t i = 4; i + 7 <= size; i++) {
        // REX.W 63 /r, mod 00 rm 101: movslq disp32(%rip), %reg64
        if ((code[i] & 0xfb) != 0x48 || code[i + 1] != 0x63 || (code[i + 2] & 0xc7) != 0x05)
            continue;
        int32_t rel;
        memcpy(&rel, code + i + 3, sizeof(rel));
        if (codeAddress + i + 7 + (uint64_t)(int64_t)rel != desc)
            continue;
        // REX.W 8b /r, mod 10 (disp32) or 01 (disp8), no SIB: movq disp(%base), %reg64
        int64_t disp = -1;
        if (i >= 7 && (code[i - 7] & 0xf8) == 0x48 && code[i - 6] == 0x8b && (code[i - 5] & 0xc7) != 0x84 &&
            (code[i - 5] & 0xc0) == 0x80) {
            int32_t d32;
            memcpy(&d32, code + i - 4, sizeof(d32));
            disp = d32;
        } else if ((code[i - 4] & 0xf8) == 0x48 && code[i - 3] == 0x8b && (code[i - 2] & 0xc7) != 0x44 &&
                   (code[i - 2] & 0xc0) == 0x40) {
            disp = (int8_t)code[i - 1];
        }
        return disp > 0 && disp < 0x200 && !(disp & 7) ? (uint32_t)disp : 0;
    }
    return 0;
}

// v_op, v_mount and v_data, adjacent in struct vnode.
void *const *vnodeOpsFields(vnode_t vp)
{
    return reinterpret_cast<void *const *>(reinterpret_cast<uintptr_t>(vp) + vnodeOpsOffset);
}
constexpr size_t kPatchRangeMax = 96;
// Ours need ~16 on 25G332, BlueToolFixup ~5; re-validated pages reuse theirs.
constexpr uint32_t kPatchRanges = 128;
// A page is patched only with this much room left, so our bytes are never
// in a page without their recorded originals.
constexpr uint32_t kPageRangeHeadroom = 16;
struct PatchRange {
    uint64_t fileOffset;
    uint32_t length;
    uint8_t original[kPatchRangeMax];
    uint8_t patched[kPatchRangeMax];
};
PatchRange patchRanges[kPatchRanges];
IOLock *pageLock;
uint8_t *pageSnapshot;
vnode_t bluetoothdVnode;
// Restored copies for the verifier, preallocated: no allocation on page-in.
constexpr size_t kVerifyCopySize = 64 * 1024;
IOLock *verifyCopyLock;
uint8_t *verifyCopy;

// Caller holds pageLock.
void recordPatchRange(uint64_t fileOffset, const uint8_t *original, const uint8_t *patched, uint32_t length)
{
    uint32_t count = __atomic_load_n(&patchRangeCount, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < count; i++) {
        const PatchRange &r = patchRanges[i];
        if (fileOffset >= r.fileOffset && fileOffset - r.fileOffset <= r.length &&
            length <= r.length - (uint32_t)(fileOffset - r.fileOffset)) {
            const size_t within = (size_t)(fileOffset - r.fileOffset);
            if (memcmp(r.original + within, original, length) == 0 && memcmp(r.patched + within, patched, length) == 0)
                return; // A pre-reserved RPA range also covers its smaller page diff.
        }
    }
    if (count == kPatchRanges) {
        OSIncrementAtomic(&patchRangeOverflow);
        return;
    }
    PatchRange &r = patchRanges[count];
    r.fileOffset = fileOffset;
    r.length = length;
    memcpy(r.original, original, length);
    memcpy(r.patched, patched, length);
    __atomic_store_n(&patchRangeCount, count + 1, __ATOMIC_RELEASE);
}

// Caller holds pageLock. Differences closer than 8 bytes share a range.
void recordPageDiff(uint64_t pageOffset, const uint8_t *before, const uint8_t *after)
{
    size_t i = 0;
    while (i < PAGE_SIZE) {
        if (before[i] == after[i]) {
            i++;
            continue;
        }
        size_t start = i, end = i + 1;
        for (size_t j = end; j < PAGE_SIZE && j < end + 8 && j - start < kPatchRangeMax; j++)
            if (before[j] != after[j])
                end = j + 1;
        recordPatchRange(pageOffset + start, before + start, after + start, (uint32_t)(end - start));
        i = end;
    }
}

/*
 * A patched bluetoothd page lives on in the shared file cache, so when the
 * page is evicted and faulted in again cs_validate_page receives a buffer
 * that already carries our patch. AMFI hashes that copy, marks the page
 * invalid and kills bluetoothd at the page's first instruction
 * (2026-11-05: every crash at bluetoothd+0x33a39e, the extended-advertising
 * page patched 172 times). Put the recorded originals back for the duration
 * of the check; patchBluetoothdPage re-applies them immediately after.
 * Caller holds pageLock.
 */
void restorePatchedPageLocked(memory_object_offset_t pageOffset, const void *data)
{
    if (!data)
        return;
    uint8_t *page = const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(data));
    const uint64_t base = (uint64_t)pageOffset;
    const uint32_t count = __atomic_load_n(&patchRangeCount, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < count; i++) {
        const PatchRange &r = patchRanges[i];
        if (r.fileOffset < base || r.fileOffset - base + r.length > PAGE_SIZE)
            continue;
        uint8_t *dst = page + (r.fileOffset - base);
        if (memcmp(dst, r.patched, r.length) == 0)
            memcpy(dst, r.original, r.length);
    }
}

static __attribute__((noinline)) bool isBluetoothdPath(vnode_t vp)
{
    static constexpr char bluetoothd[] = "/usr/sbin/bluetoothd";
    char path[PATH_MAX];
    int pathLength = PATH_MAX;
    return vn_getpath(vp, path, &pathLength) == 0 && UNLIKELY(strcmp(path, bluetoothd) == 0);
}

// Runs for every code page validated system-wide. The cached name check
// rejects other files without building a full path (and a PATH_MAX buffer).
bool isBluetoothd(vnode_t vp)
{
    const char *name = vnode_getname(vp);
    if (name) {
        bool maybe = strcmp(name, "bluetoothd") == 0;
        vnode_putname(name);
        if (LIKELY(!maybe))
            return false;
    }
    return isBluetoothdPath(vp);
}

void noteVerifyFailure(off_t offset)
{
    OSIncrementAtomic(&verifyFailed);
    __atomic_store_n(&verifyLastFailOffset, (uint64_t)offset, __ATOMIC_RELAXED);
}

/*
 * verify(buf) runs the file system's verifier. When buf holds recorded
 * bluetoothd patch ranges, it runs on a copy with their originals restored.
 */
template <typename Verify>
errno_t verifyBluetoothdBlock(vnode_t vp, off_t offset, uint8_t *buf, size_t size, uint32_t flags, Verify &&verify)
{
    if (LIKELY(!buf) || !size || !pageLock || flags != 0 || offset < 0)
        return verify(buf);
    if ((__atomic_load_n(&rpaPlanState, __ATOMIC_ACQUIRE) == RpaPlanReady ||
         __atomic_load_n(&bluetoothdCommonAbi, __ATOMIC_ACQUIRE) != 0) &&
        (vp != bluetoothdImageVnode || vnode_vid(vp) != bluetoothdImageVid))
        return verify(buf);
    // csValidatePage publishes the vnode under pageLock before it changes a
    // byte. Until then, any bluetoothd vnode waits for the lock.
    vnode_t known = __atomic_load_n(&bluetoothdVnode, __ATOMIC_ACQUIRE);
    if (LIKELY(known ? vp != known : !isBluetoothd(vp)))
        return verify(buf);
    IOLockLock(pageLock);
    const uint64_t start = (uint64_t)offset;
    const uint32_t count = __atomic_load_n(&patchRangeCount, __ATOMIC_ACQUIRE);
    auto patchedInBlock = [&](const PatchRange &r) {
        return r.fileOffset >= start && r.fileOffset - start <= size && r.length <= size - (r.fileOffset - start) &&
               memcmp(buf + (r.fileOffset - start), r.patched, r.length) == 0;
    };
    bool restore = false;
    for (uint32_t i = 0; i < count && !restore; i++)
        restore = patchedInBlock(patchRanges[i]);
    if (!restore) {
        // The shared buffer must remain untouched until the hash finishes.
        errno_t err = verify(buf);
        IOLockUnlock(pageLock);
        if (err) {
            noteVerifyFailure(offset);
            publishLater();
        }
        return err;
    }
    // The preallocated copy when free (try only: pageLock is held), else the heap.
    const bool pooled = verifyCopy && size <= kVerifyCopySize && IOLockTryLock(verifyCopyLock);
    auto copy = pooled ? verifyCopy : static_cast<uint8_t *>(IOMalloc(size));
    if (!copy) {
        OSIncrementAtomic(&verifyRestoreFailed);
        errno_t err = verify(buf);
        IOLockUnlock(pageLock);
        publishLater();
        return err;
    }
    memcpy(copy, buf, size);
    for (uint32_t i = 0; i < count; i++) {
        const PatchRange &r = patchRanges[i];
        if (patchedInBlock(r))
            memcpy(copy + (r.fileOffset - start), r.original, r.length);
    }
    // The private copy no longer depends on the mutable file-cache pages.
    IOLockUnlock(pageLock);
    errno_t err = verify(copy);
    if (pooled)
        IOLockUnlock(verifyCopyLock);
    else
        IOFree(copy, size);
    OSIncrementAtomic(err ? &verifyRestoreFailed : &verifyRestored);
    publishLater();
    return err;
}

// Our entry in the vnode operations vector, in place of vnop_verify.
int verifyOp(void *arg)
{
    auto *ap = static_cast<VnopVerifyArgs *>(arg);
    const VnodeOp org = __atomic_load_n(&orgVerifyOp, __ATOMIC_ACQUIRE);
    return verifyBluetoothdBlock(ap->a_vp, ap->a_foffset, ap->a_buf, ap->a_bufsize, ap->a_flags,
                                 [ap, org](uint8_t *buf) {
        if (buf == ap->a_buf)
            return org(ap);
        // Swap only the buffer: no copy of an argument struct whose tail
        // another kernel may extend.
        uint8_t *const shared = ap->a_buf;
        ap->a_buf = buf;
        const int err = org(ap);
        ap->a_buf = shared;
        return err;
    });
}

// Caller holds pageLock. Once, at bluetoothd's first validated page: by then
// bsd_init has numbered the vnode operations and the vnode is APFS's.
void installVerifyOp(vnode_t vp)
{
    SInt32 state = VerifyOpNoSymbols;
    if (vnopVerifyDesc && vnodeOpsOffset) {
        const int index = *reinterpret_cast<const volatile int *>(vnopVerifyDesc);
        void *const *fields = vnodeOpsFields(vp);
        if (index <= 0 || index >= 1024) {
            state = VerifyOpBadIndex;
        } else if (!fields[0] || fields[1] != static_cast<void *>(vnode_mount(vp)) || fields[2] != vnode_fsnode(vp)) {
            state = VerifyOpBadVnode;
        } else {
            VnodeOp *slot = static_cast<VnodeOp *>(fields[0]) + index;
            const VnodeOp current = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
            if (!current) {
                state = VerifyOpNoEntry;
            } else {
                __atomic_store_n(&orgVerifyOp, current, __ATOMIC_RELEASE);
                // vfs_fsadd allocates the vectors on the heap; lift write
                // protection anyway in case a kernel keeps them read-only.
                const bool writable = MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS;
                // A failed enable already undid itself; disabling again would
                // unlock kernelWriteLock twice.
                if (writable) {
                    __atomic_store_n(slot, &verifyOp, __ATOMIC_RELEASE);
                    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
                }
                state = writable ? VerifyOpInstalled : VerifyOpNoWrite;
            }
        }
    }
    verifyFixRouted = state == VerifyOpInstalled;
    __atomic_store_n(&verifyOpState, state, __ATOMIC_RELEASE);
    SYSLOG(DRV_NAME, "bluetoothd verify-block fix state %d (v_op at 0x%x)", (int)state, vnodeOpsOffset);
}

bool patchBluetoothdPage(void *data);
SInt32 readBluetoothdBuild();

// The exec loader already owns the vnode lock (its following vn_rdwr uses
// IO_NODELOCKED). Use the same contract and verify an exact read; do not
// reacquire that lock or leak the temporary UIO on any error path.
int readRpaImage(void *buffer, size_t size, vnode_t vp, vfs_context_t context)
{
    uio_t io = uio_create(1, 0, UIO_SYSSPACE, UIO_READ);
    if (!io)
        return ENOMEM;
    int result = uio_addiov(io, reinterpret_cast<user_addr_t>(buffer), size);
    if (!result)
        result = VNOP_READ(vp, io, IO_NODELOCKED, context);
    if (!result && uio_resid(io) != 0)
        result = EIO;
    uio_free(io);
    return result;
}

// Resolve before exec, never from a VM page-validation callback. The original
// MAC decision is preserved and image_params remains opaque. A complete owned
// image is read from the same referenced vnode, without pageLock held.
uint32_t validateCommonImage(const uint8_t *image, size_t size)
{
    const mtk_bt_page_site sites[] {
        {extAdvFind, nullptr, extAdvReplace, nullptr, sizeof(extAdvFind)},
        {advAddressFind, advAddressFindMask, advAddressReplace, advAddressReplaceMask, sizeof(advAddressFind)},
        {advAddressRetryFind, advAddressRetryFindMask, advAddressRetryReplace, advAddressRetryReplaceMask, sizeof(advAddressRetryFind)},
        {wbsFind, wbsFindMask, wbsReplace, wbsReplaceMask, sizeof(wbsFind)},
        {hfpFillFind, hfpFillFindMask, hfpFillReplace, hfpFillReplaceMask, sizeof(hfpFillFind)}
    };
    return mtk_bt_common_validate(image, size, sites);
}

int macVnodeCheckExec(vfs_context_t context, vnode_t vp, struct image_params *imageParams)
{
    const int result = FunctionCast(macVnodeCheckExec, orgMacVnodeCheckExec)(context, vp, imageParams);
    if (result || (!extAdvPatchEnabled && !msbcPatchEnabled) || !rpaExecRouted || !pageLock || !rpaPlan || !isBluetoothd(vp))
        return result;
    if (readBluetoothdBuild() == BuildUnread)
        return result;
    IOLockLock(pageLock);
    if (rpaPlanState != RpaPlanUnread) {
        IOLockUnlock(pageLock);
        return result;
    }
    SInt32 state = RpaPlanPreparing;
    if (!mtk_bt_tahoe_build_since(reinterpret_cast<const uint8_t *>(bluetoothdBuildName), strlen(bluetoothdBuildName), 241) && !anyBuildPatches)
        state = RpaPlanDisabled;
    else if (rpaValidationStarted)
        state = RpaPlanLate;
    else if (vnode_ref(vp) != 0)
        state = RpaPlanNoVnode;
    if (state == RpaPlanPreparing) {
        bluetoothdImageVnode = vp;
        bluetoothdImageVid = vnode_vid(vp);
    }
    __atomic_store_n(&rpaPlanState, state, __ATOMIC_RELEASE);
    IOLockUnlock(pageLock);
    if (state != RpaPlanPreparing) {
        publishLater();
        return result;
    }
    const size_t size = FileIO::readFileSize(vp, context);
    auto *image = size && size <= MTK_BT_RPA_MAX_IMAGE ? static_cast<uint8_t *>(IOMalloc(size)) : nullptr;
    uint32_t common = 0;
    const bool wantRpa = rpaPatchEnabled &&
        mtk_bt_tahoe_build_since(reinterpret_cast<const uint8_t *>(bluetoothdBuildName), strlen(bluetoothdBuildName), 332);
    if (image) {
        memset(image, 0, size);
        if (readRpaImage(image, size, vp, context) == 0 && FileIO::readFileSize(vp, context) == size) {
            // A prior wrapper may be visible through the file cache. Restore
            // only recorded exact patched bytes, in this owned copy.
            IOLockLock(pageLock);
            for (uint32_t i = 0; i < patchRangeCount; ++i) {
                const PatchRange &range = patchRanges[i];
                if (range.fileOffset <= size && range.length <= size - range.fileOffset &&
                    memcmp(image + range.fileOffset, range.patched, range.length) == 0)
                    memcpy(image + range.fileOffset, range.original, range.length);
            }
            IOLockUnlock(pageLock);
            common = validateCommonImage(image, size);
            if (wantRpa)
                state = (common & MTK_BT_COMMON_ADVERTISING) && mtk_bt_rpa_resolve(image, size, rpaPlan) == 0 ? RpaPlanReady : RpaPlanInvalid;
            else
                state = RpaPlanDisabled; // Common ABI verification is still retained.
        } else
            state = RpaPlanReadFailed;
        IOFree(image, size);
    } else
        state = RpaPlanReadFailed;
    IOLockLock(pageLock);
    // Never activate after *any* observed native executable validation. This
    // also catches unexpected validation recursively triggered by acquisition.
    if (rpaValidationStarted) {
        state = RpaPlanLate;
        common = 0;
    }
    if ((common || state == RpaPlanReady) && vnode_vid(vp) != bluetoothdImageVid) {
        state = RpaPlanNoVnode;
        common = 0;
    }
    if ((common || state == RpaPlanReady) && !verifyOpState)
        installVerifyOp(vp);
    if ((common || state == RpaPlanReady) && !verifyFixRouted) {
        state = RpaPlanNoVerify;
        common = 0;
    }
    if (common)
        __atomic_store_n(&bluetoothdVnode, vp, __ATOMIC_RELEASE);
    uint32_t needed = 0;
    if (state == RpaPlanReady) {
        for (const auto &edit : rpaPlan->edits)
            for (uint32_t at = 0; at < edit.len; at += kPatchRangeMax) {
                const uint32_t n = edit.len - at < kPatchRangeMax ? edit.len - at : kPatchRangeMax;
                needed += memcmp(edit.original + at, edit.replacement + at, n) != 0;
            }
        if (patchRangeCount > kPatchRanges || needed > kPatchRanges - patchRangeCount)
            state = RpaPlanNoRoom;
    }
    if (state == RpaPlanReady) {
        __atomic_store_n(&bluetoothdVnode, vp, __ATOMIC_RELEASE);
        // Reserve every changed range BEFORE a single helper/child can run.
        // Later per-page headroom exhaustion must never skip part of this plan.
        for (const auto &edit : rpaPlan->edits)
            for (uint32_t at = 0; at < edit.len; at += kPatchRangeMax) {
                const uint32_t n = edit.len - at < kPatchRangeMax ? edit.len - at : kPatchRangeMax;
                if (memcmp(edit.original + at, edit.replacement + at, n) != 0)
                    recordPatchRange(edit.file_offset + at, edit.original + at, edit.replacement + at, n);
            }
    }
    __atomic_store_n(&bluetoothdCommonAbi, common, __ATOMIC_RELEASE);
    __atomic_store_n(&rpaPlanState, state, __ATOMIC_RELEASE);
    IOLockUnlock(pageLock);
    SYSLOG(DRV_NAME, "bluetoothd ABI bits 0x%x, dynamic RPA plan state %d", common, (int)state);
    publishLater();
    return result;
}

// Called only for a ready, matching vnode. The Rust applier checks *all*
// destinations on this page before writing. A mismatch rejects executable
// validation rather than silently executing a half-patched helper group.
int32_t applyRpaPage(vnode_t vp, uint64_t pageOffset, void *data, int *validated, int *tainted, int *nx)
{
    if (__atomic_load_n(&rpaPlanState, __ATOMIC_ACQUIRE) != RpaPlanReady || vp != bluetoothdImageVnode || vnode_vid(vp) != bluetoothdImageVid)
        return 0;
    const int32_t edits = verifyFixRouted ? mtk_bt_rpa_apply_page(rpaPlan, pageOffset, static_cast<uint8_t *>(data), PAGE_SIZE) : -1;
    if (edits < 0) {
        if (validated) *validated = 0;
        if (tainted) *tainted = 1;
        if (nx) *nx = 1;
        OSIncrementAtomic(&rpaPlanRejected);
        SYSLOG(DRV_NAME, "bluetoothd RPA destination rejected at 0x%llx", pageOffset);
        publishLater();
        return edits;
    }
    volatile SInt32 *const counters[] {&bluetoothdRpaHelpersPatched, &bluetoothdRpaLegacyPatched,
        &bluetoothdRpaScanPatched, &bluetoothdRpaDecodePatched, &bluetoothdRpaDevicePatched,
        &bluetoothdRpaConnectPatched, &bluetoothdRpaUpdatePatched};
    for (size_t i = 0; i < MTK_BT_RPA_EDITS; ++i)
        if (edits & (1 << i))
            OSIncrementAtomic(counters[i]);
    if (edits & 1)
        __atomic_store_n(&rpaHelpersReady, true, __ATOMIC_RELEASE);
    return edits;
}

// Lilu's user patcher is off on Big Sur and later (kern_start.cpp), so
// onProcLoad never fires there. Patch bluetoothd's pages as the kernel
// validates them instead, as BlueToolFixup and RestrictEvents do.
void csValidatePage(vnode_t vp, memory_object_t pager, memory_object_offset_t pageOffset, const void *data,
                    int *validated, int *tainted, int *nx)
{
    auto org = FunctionCast(csValidatePage, orgCsValidatePage);
    if (!data || !isBluetoothd(vp))
        return org(vp, pager, pageOffset, data, validated, tainted, nx);
    // The referenced plan/restore ledger belongs to one vnode for this boot.
    // Another executable image stays native; never reuse its predecessor's
    // offsets or overwrite its original bytes with a stale restore range.
    if ((__atomic_load_n(&rpaPlanState, __ATOMIC_ACQUIRE) == RpaPlanReady ||
         __atomic_load_n(&bluetoothdCommonAbi, __ATOMIC_ACQUIRE) != 0) &&
        (vp != bluetoothdImageVnode || vnode_vid(vp) != bluetoothdImageVid)) {
        // org also contains downstream patchers (e.g. BlueToolFixup). Calling
        // it alone would leave B dirty without B's restoration ledger. Undo
        // those writes before return so the excluded vnode really stays native.
        IOLockLock(pageLock);
        memcpy(pageSnapshot, data, PAGE_SIZE);
        org(vp, pager, pageOffset, data, validated, tainted, nx);
        if (memcmp(pageSnapshot, data, PAGE_SIZE) != 0) {
            memcpy(const_cast<void *>(data), pageSnapshot, PAGE_SIZE);
            OSIncrementAtomic(&bluetoothdForeignPagesRolledBack);
        }
        IOLockUnlock(pageLock);
        OSIncrementAtomic(&bluetoothdPagesUnpatched);
        publishLater();
        return;
    }
    // A pre-exec plan must precede executable validation; remember early or
    // recursive validation permanently rather than trying to activate late.
    if (pageLock) {
        IOLockLock(pageLock);
        if (rpaPlanState != RpaPlanReady)
            rpaValidationStarted = true;
        IOLockUnlock(pageLock);
    }
    OSIncrementAtomic(&bluetoothdPages);
    if (verifyFixEnabled && pageLock && pageSnapshot && UNLIKELY(!__atomic_load_n(&verifyOpState, __ATOMIC_ACQUIRE))) {
        IOLockLock(pageLock);
        if (!verifyOpState)
            installVerifyOp(vp);
        IOLockUnlock(pageLock);
        publishLater();
    }
    // Fail closed: without the verifier restore, a patched page that stays
    // resident makes every later page-in of its 16 KB block fail (bluetoothd
    // SIGBUS, Bluetooth dead until reboot). Leave all pages native.
    if (!verifyFixRouted) {
        if (pageLock) {
            IOLockLock(pageLock);
            restorePatchedPageLocked(pageOffset, data);
            IOLockUnlock(pageLock);
        }
        org(vp, pager, pageOffset, data, validated, tainted, nx);
        applyRpaPage(vp, pageOffset, const_cast<void *>(data), validated, tainted, nx);
        OSIncrementAtomic(&bluetoothdPagesUnpatched);
        return;
    }
    IOLockLock(pageLock);
    if (__atomic_load_n(&bluetoothdVnode, __ATOMIC_RELAXED) != vp) {
        memset(featureLatch, 0, sizeof(featureLatch));
        msbcSenderNative = false;
    }
    __atomic_store_n(&bluetoothdVnode, vp, __ATOMIC_RELEASE);
    featureLatch[2] = rpaPlanState == RpaPlanReady && vp == bluetoothdImageVnode ? 1 : 2;
    // The page may already hold our patch from an earlier fault; hand the
    // signature check the original bytes so the page never validates dirty.
    restorePatchedPageLocked(pageOffset, data);
    memcpy(pageSnapshot, data, PAGE_SIZE);
    org(vp, pager, pageOffset, data, validated, tainted, nx);
    const int32_t rpaEdits = applyRpaPage(vp, pageOffset, const_cast<void *>(data), validated, tainted, nx);
    bool patched = rpaEdits > 0;
    if (rpaEdits >= 0 && __atomic_load_n(&patchRangeCount, __ATOMIC_RELAXED) + kPageRangeHeadroom <= kPatchRanges)
        patched = patchBluetoothdPage(const_cast<void *>(data)) || patched;
    else
        OSIncrementAtomic(&bluetoothdPagesUnpatched);
    if (memcmp(pageSnapshot, data, PAGE_SIZE) != 0)
        recordPageDiff(pageOffset, pageSnapshot, static_cast<const uint8_t *>(data));
    IOLockUnlock(pageLock);
    if (patched)
        publishLater();
}

// Empty until launchd sets it, so only call this once bluetoothd runs. An
// empty value is not cached; every build-scoped site waits for it.
SInt32 readBluetoothdBuild()
{
    SInt32 state = __atomic_load_n(&bluetoothdBuild, __ATOMIC_ACQUIRE);
    if (state != BuildUnread)
        return state;
    char build[sizeof(bluetoothdBuildName)] {};
    size_t size = sizeof(build) - 1;
    if (sysctlbyname("kern.osversion", build, &size, nullptr, 0) != 0 || build[0] == 0)
        return BuildUnread;
    state = strcmp(build, "25G332") == 0 ? Build25G332 : strcmp(build, "25G241") == 0 ? Build25G241 :
            strcmp(build, "25G342") == 0 ? Build25G342 :
            mtk_bt_tahoe_build_since(reinterpret_cast<const uint8_t *>(build), strlen(build), 241) ? BuildLater : BuildOther;
    memcpy(bluetoothdBuildName, build, sizeof(build));
    __atomic_store_n(&bluetoothdBuild, state, __ATOMIC_RELEASE);
    return state;
}

/*
 * Common patches only, searched in this order. Scopes use kern.osversion:
 *  - ScopeAnyBuild: exact build-independent extAdv function signature;
 *    unlisted layouts additionally require complete pre-exec validation.
 *  - ScopeMinimumBuild: Darwin 25 from 25G241; preserve the offline-verified
 *    legacy layouts, require full-image setter/gate/ABI checks on later ones.
 *    -mtkbtanybuild remains an explicit below-minimum debug override.
 * A site must match exactly once in the page (ambiguous: left native).
 * RPA is a complete vnode-bound relocation plan, never an independent site.
 */
enum PatchFeature : uint8_t { FeatureAdv, FeatureMsbc, FeatureRpa, FeatureCount };
static_assert(FeatureCount == sizeof(featureLatch), "one latch per feature");
enum SiteScope : uint8_t { ScopeAnyBuild, ScopeMinimumBuild };
enum FeatureLatch : uint8_t { LatchUndecided = 0, LatchOn = 1, LatchOff = 2 };

struct BluetoothdSite {
    const char *name;
    PatchFeature feature;
    SiteScope scope;
    mtk_bt_page_site site;
    volatile SInt32 *counter;
};

#define BLUETOOTHD_SITE(name, feature, scope, find, findMask, replace, replaceMask, counter) \
    {name, feature, scope, {find, findMask, replace, replaceMask, sizeof(find)}, &counter}
// RPA is deliberately absent: its entire relocation plan is prepared before
// exec, not independently guessed/applied as each child page faults in.
const BluetoothdSite bluetoothdSites[] {
    // Sender before gate, including when both happen to match one page.
    BLUETOOTHD_SITE("HFP complete PCM chunks", FeatureMsbc, ScopeMinimumBuild, hfpFillFind, hfpFillFindMask,
                    hfpFillReplace, hfpFillReplaceMask, bluetoothdHfpFillPatched),
    BLUETOOTHD_SITE("HFP wideband gate", FeatureMsbc, ScopeMinimumBuild, wbsFind, wbsFindMask, wbsReplace,
                    wbsReplaceMask, bluetoothdWbsPatched),
    BLUETOOTHD_SITE("extended-advertising check", FeatureAdv, ScopeAnyBuild, extAdvFind, nullptr, extAdvReplace,
                    nullptr, bluetoothdPatched),
    BLUETOOTHD_SITE("per-set advertising address gate", FeatureAdv, ScopeMinimumBuild, advAddressFind,
                    advAddressFindMask, advAddressReplace, advAddressReplaceMask, bluetoothdAddressPatched),
    BLUETOOTHD_SITE("random address before enable", FeatureAdv, ScopeMinimumBuild, advAddressRetryFind,
                    advAddressRetryFindMask, advAddressRetryReplace, advAddressRetryReplaceMask,
                    bluetoothdAddressRetryPatched),
};

bool siteScopeAllows(SiteScope scope, SInt32 build)
{
    switch (scope) {
    case ScopeAnyBuild:
        return true;
    case ScopeMinimumBuild:
        return build >= Build25G241 || (build == BuildOther && anyBuildPatches);
    }
    return false;
}

bool commonFeatureAllowed(uint32_t mask)
{
    if (__atomic_load_n(&bluetoothdCommonAbi, __ATOMIC_ACQUIRE) & mask)
        return true;
    // Preserve already offline-verified legacy page-only behavior when an
    // older kernel cannot route pre-exec. Unlisted layouts require their own
    // feature's full-image ABI validation, never another feature's activation.
    const SInt32 build = __atomic_load_n(&bluetoothdBuild, __ATOMIC_ACQUIRE);
    return build == Build25G241 || build == Build25G332 || build == Build25G342 ||
           (build == BuildOther && anyBuildPatches);
}

bool featureWanted(PatchFeature feature)
{
    switch (feature) {
    case FeatureAdv:
        return extAdvPatchEnabled && commonFeatureAllowed(MTK_BT_COMMON_ADVERTISING);
    case FeatureMsbc:
        return msbcPatchEnabled && isMsbcReady() && msbcControllerScope.allowsWideband() &&
               commonFeatureAllowed(MTK_BT_COMMON_MSBC);
    case FeatureRpa:
        return rpaPatchEnabled;
    case FeatureCount:
        break;
    }
    return false;
}

/*
 * Caller holds pageLock. Once decided, a feature stays latched for this
 * vnode: its patched pages can remain resident across bluetoothd relaunches.
 * Unknown USB scope is not an Off decision. Early native pages are retried
 * only on validation, never through a saved raw page pointer; the WBS gate
 * additionally waits for the sender fix in patchBluetoothdPage.
 */
bool featureOn(PatchFeature feature)
{
    if (featureLatch[feature] == LatchUndecided) {
        if (feature == FeatureMsbc && msbcPatchEnabled && !msbcControllerScope.observed()) {
            __atomic_store_n(&msbcDeferredBeforeProbe, 1, __ATOMIC_RELAXED);
            publishLater();
            return false;
        }
        featureLatch[feature] = featureWanted(feature) ? LatchOn : LatchOff;
    }
    return featureLatch[feature] == LatchOn;
}

// Caller holds pageLock; data is one validated bluetoothd page.
bool patchBluetoothdPage(void *data)
{
    auto *page = static_cast<uint8_t *>(data);
    const SInt32 build = readBluetoothdBuild();
    bool patched = false;
    for (const BluetoothdSite &s : bluetoothdSites) {
        if (!siteScopeAllows(s.scope, build))
            continue;
        const int32_t at = mtk_bt_page_find(page, PAGE_SIZE, &s.site);
        if (at == MTK_BT_PAGE_NO_MATCH)
            continue;
        if (at < 0) {
            // Not specific enough for this build: never decide on it.
            if (s.counter == &bluetoothdHfpFillPatched)
                msbcSenderNative = true;
            if (featureLatch[s.feature] == LatchUndecided)
                featureLatch[s.feature] = LatchOff;
            OSIncrementAtomic(&bluetoothdSitesAmbiguous);
            SYSLOG(DRV_NAME, "bluetoothd %s: ambiguous (%d), left native", s.name, (int)at);
            continue;
        }
        if (s.counter == &bluetoothdHfpFillPatched)
            msbcSenderNative = true;
        if (s.counter == &bluetoothdWbsPatched && msbcSenderNative)
            continue; // HFP's early native page may still be resident.
        if (!featureOn(s.feature) || !mtk_bt_page_apply(page, PAGE_SIZE, (size_t)at, &s.site))
            continue;
        if (s.counter == &bluetoothdHfpFillPatched)
            msbcSenderNative = false;
        OSIncrementAtomic(s.counter);
        SYSLOG(DRV_NAME, "patched bluetoothd %s (page offset 0x%x)", s.name, (unsigned)at);
        patched = true;
    }
    return patched;
}

void processPatcher(void *, KernelPatcher &patcher)
{
    // csValidatePage installs verifyOp at bluetoothd's first page and records
    // patch ranges only once it is in.
    if (verifyFixEnabled && pageLock && pageSnapshot) {
        vnopVerifyDesc = patcher.solveSymbol(KernelPatcher::KernelID, "_vnop_verify_desc");
        const mach_vm_address_t kpi = patcher.solveSymbol(KernelPatcher::KernelID, "_VNOP_VERIFY");
        if (!vnopVerifyDesc || !kpi)
            patcher.clearError();
        else
            vnodeOpsOffset = decodeVnodeOpsOffset(reinterpret_cast<const uint8_t *>(kpi), 192, kpi, vnopVerifyDesc);
        SYSLOG(DRV_NAME, "bluetoothd verify-block fix: v_op at 0x%x", vnodeOpsOffset);
    }
    KernelPatcher::RouteRequest route {"_cs_validate_page", csValidatePage, orgCsValidatePage};
    extAdvPatchRouted = patcher.routeMultipleLong(KernelPatcher::KernelID, &route, 1);
    if (!extAdvPatchRouted)
        patcher.clearError();
    SYSLOG(DRV_NAME, "bluetoothd page patches %s", extAdvPatchRouted ? "routed" : "failed to route");
    if ((extAdvPatchEnabled || msbcPatchEnabled) && rpaPlan && pageLock) {
        KernelPatcher::RouteRequest execRoute {"_mac_vnode_check_exec", macVnodeCheckExec, orgMacVnodeCheckExec};
        rpaExecRouted = patcher.routeMultipleLong(KernelPatcher::KernelID, &execRoute, 1);
        if (!rpaExecRouted)
            patcher.clearError();
        SYSLOG(DRV_NAME, "bluetoothd dynamic RPA pre-exec %s", rpaExecRouted ? "routed" : "unavailable");
    }
    publishCounters();
}

} // namespace

void Patcher::observeMsbcController(bool supported)
{
    msbcControllerScope.observe(supported);
    publishLater();
}

void Patcher::forgetController(uint64_t registryId)
{
    forgetControllerMappings(registryId);
}

void Patcher::init(uint32_t rustSelfTestFailed)
{
    rustFailed = rustSelfTestFailed;
    // USB diagnostics still need a publisher with the APFS verifier disabled.
    publishCall = thread_call_allocate(publishDeferred, nullptr);
    if (getKernelVersion() < KernelVersion::Monterey) {
        lilu.onKextLoadForce(&kextIOBluetoothFamily, 1, processKext);
    } else {
        leLock = IOLockAlloc();
        leReadyMaskEnabled = getKernelVersion() == KernelVersion::Tahoe &&
                             !checkKernelArgument("-mtkbtnolereadymask");
        extAdvPatchEnabled = !checkKernelArgument("-mtkbtnoextadv");
        // RPA is resolved as one complete relocatable plan before exec. Its
        // minimum build and native ABI guards are checked there, not here
        // (kern.osversion is still empty at kext initialization).
        rpaPatchEnabled = extAdvPatchEnabled && getKernelVersion() == KernelVersion::Tahoe &&
                          !checkKernelArgument("-mtkbtnorpa");
        msbcPatchEnabled = getKernelVersion() == KernelVersion::Tahoe && !checkKernelArgument("-mtkbtnomsbc");
        anyBuildPatches = checkKernelArgument("-mtkbtanybuild");
        // Page patches need the verifier restore (fail closed without it).
        verifyFixEnabled = !checkKernelArgument("-mtkbtnoverifyfix");
        if (verifyFixEnabled) {
            pageLock = IOLockAlloc();
            pageSnapshot = static_cast<uint8_t *>(IOMalloc(PAGE_SIZE));
            if ((extAdvPatchEnabled || msbcPatchEnabled) && pageLock && pageSnapshot) {
                rpaPlan = static_cast<mtk_bt_rpa_plan *>(IOMalloc(sizeof(*rpaPlan)));
                if (rpaPlan)
                    memset(rpaPlan, 0, sizeof(*rpaPlan));
            }
            verifyCopyLock = IOLockAlloc();
            verifyCopy = verifyCopyLock ? static_cast<uint8_t *>(IOMalloc(kVerifyCopySize)) : nullptr;
        }
        lilu.onKextLoadForce(&kextIOUSBHostFamily, 1, processKext);
        if (extAdvPatchEnabled || msbcPatchEnabled || verifyFixEnabled)
            lilu.onPatcherLoadForce(processPatcher);
    }
    publishCounters();
}
