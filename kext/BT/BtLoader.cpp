//
//  BtLoader.cpp
//  MtkWireless
//
//  Based on SimpleMtkBtFw (zxystd, laobamac). The protocol now lives in
//  rust/mtk-bt; this file only provides USB access to it.
//

#include "BtLoader.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <libkern/OSByteOrder.h>
#include <libkern/version.h>

#include <Headers/kern_util.hpp>

#include "EmbeddedFirmware.hpp"
#include "Log.h"
#include "Patcher.hpp"
#include "USBDeviceController.hpp"
#include "mtk_bt.h"
#include "msbc/UsbMsbcScope.hpp"

#define super IOService
OSDefineMetaClassAndStructors(MtkWirelessBTLoader, IOService)

static IOPMPowerState powerStates[2] = {
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0},
};

namespace {

USBDeviceController *usb(void *ctx)
{
    return static_cast<USBDeviceController *>(ctx);
}

int32_t regRead(void *ctx, uint32_t reg, uint32_t *value)
{
    uint32_t le = 0;
    IOReturn ret = usb(ctx)->controlRequest(
        makeDeviceRequestbmRequestType(kRequestDirectionIn, kRequestTypeVendor, kRequestRecipientDevice),
        0x63, reg >> 16, reg & 0xffff, &le, sizeof(le), HCI_INIT_TIMEOUT);
    *value = OSSwapLittleToHostInt32(le);
    if (ret == kIOReturnSuccess)
        BTLog("reg 0x%08x = 0x%08x\n", reg, *value);
    else
        BTLog("reg 0x%08x read failed: %s\n", reg, usb(ctx)->stringFromReturn(ret));
    return ret;
}

int32_t uhwRead(void *ctx, uint32_t reg, uint32_t *value)
{
    uint32_t le = 0;
    IOReturn ret = usb(ctx)->controlRequest(0xDE, 0x01, reg >> 16, reg & 0xffff, &le, sizeof(le),
                                           HCI_INIT_TIMEOUT);
    *value = OSSwapLittleToHostInt32(le);
    if (ret != kIOReturnSuccess)
        BTLog("uhw 0x%08x read failed: %s\n", reg, usb(ctx)->stringFromReturn(ret));
    return ret;
}

int32_t uhwWrite(void *ctx, uint32_t reg, uint32_t value)
{
    uint32_t le = OSSwapHostToLittleInt32(value);
    IOReturn ret = usb(ctx)->controlRequest(0x5E, 0x02, reg >> 16, reg & 0xffff, &le, sizeof(le),
                                           HCI_INIT_TIMEOUT);
    if (ret != kIOReturnSuccess)
        BTLog("uhw 0x%08x write failed: %s\n", reg, usb(ctx)->stringFromReturn(ret));
    return ret;
}

int32_t wmt(void *ctx, const uint8_t *cmd, uint32_t cmdLen, uint8_t *event, uint32_t eventCap,
            uint32_t *eventLen)
{
    const uint32_t timeout = usb(ctx)->commandTimeout(HCI_INIT_TIMEOUT);
    IOReturn ret = usb(ctx)->sendHCICommand(cmd, cmdLen, timeout);
    if (ret != kIOReturnSuccess) {
        BTLog("WMT command failed: %s\n", usb(ctx)->stringFromReturn(ret));
        return ret;
    }
    ret = usb(ctx)->readWMTEvent(event, eventCap, eventLen, timeout);
    if (ret != kIOReturnSuccess)
        BTLog("WMT event read failed: %s\n", usb(ctx)->stringFromReturn(ret));
    return ret;
}

int32_t hci(void *ctx, const uint8_t *cmd, uint32_t cmdLen, uint8_t *event, uint32_t eventCap,
            uint32_t *eventLen)
{
    if (cmdLen) {
        IOReturn ret = usb(ctx)->sendHCICommand(cmd, cmdLen, HCI_PROBE_TIMEOUT);
        if (ret != kIOReturnSuccess) {
            BTLog("HCI command failed: %s\n", usb(ctx)->stringFromReturn(ret));
            return ret;
        }
    }
    IOReturn ret = usb(ctx)->readHCIEvent(event, eventCap, eventLen, HCI_PROBE_TIMEOUT);
    if (ret != kIOReturnSuccess)
        BTLog("HCI event read failed: %s\n", usb(ctx)->stringFromReturn(ret));
    return ret;
}

void sleepMs(void *, uint32_t ms)
{
    IOSleep(ms);
}

const void *fwLoad(void *, const char *name, const uint8_t **data, size_t *len)
{
    OSData *fw = mtkCopyEmbeddedFirmware(name);
    if (!fw) {
        BTLog("firmware %s missing or corrupt\n", name);
        return nullptr;
    }
    *data = static_cast<const uint8_t *>(fw->getBytesNoCopy());
    *len = fw->getLength();
    return fw;
}

void fwRelease(void *, const void *handle)
{
    const_cast<OSData *>(static_cast<const OSData *>(handle))->release();
}

void rustLog(const char *msg)
{
    BTLog("%s\n", msg);
}

mtk_bt_transport makeTransport(USBDeviceController *controller)
{
    return {
        .ctx = controller,
        .reg_read = regRead,
        .uhw_read = uhwRead,
        .uhw_write = uhwWrite,
        .wmt = wmt,
        .hci = hci,
        .sleep_ms = sleepMs,
        .fw_load = fwLoad,
        .fw_release = fwRelease,
    };
}

/* ms per WMT command/event at shutdown; a wedged controller must not hold
 * up power-off for HCI_INIT_TIMEOUT. */
constexpr uint32_t kShutdownCommandTimeout = 1000;

/* Open watchdog (see armOpenWatchdog). */
constexpr uint64_t kWatchdogMinUptimeSec = 180;
constexpr uint32_t kWatchdogDelayMs = 20000;
constexpr uint64_t kWatchdogWindowSec = 600;
constexpr uint32_t kWatchdogMaxResets = 2;
/* Static: reset() destroys this loader and a new one starts. */
uint64_t gWatchdogWindowStart = 0;
uint32_t gWatchdogResets = 0;

uint64_t uptimeSeconds()
{
    uint64_t now = 0, ns = 0;
    clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now, &ns);
    return ns / NSEC_PER_SEC;
}

} // namespace

IOService *MtkWirelessBTLoader::probe(IOService *provider, SInt32 *score)
{
    if (!super::probe(provider, score))
        return nullptr;
    auto *device = OSDynamicCast(IOUSBHostDevice, provider);
    if (!device)
        return nullptr;
    // Use the exact predicate used by USB pipe binding, before bluetoothd
    // can negotiate wideband on this controller. No USB request is sent.
    Patcher::observeMsbcController(msbcCachedMtkDevice(device));
    BTLog("probe %s vendor=0x%04x product=0x%04x\n", device->getName(),
          USBToHost16(device->getDeviceDescriptor()->idVendor),
          USBToHost16(device->getDeviceDescriptor()->idProduct));
    return this;
}

bool MtkWirelessBTLoader::start(IOService *provider)
{
    m_device = OSDynamicCast(IOUSBHostDevice, provider);
    if (!m_device || !super::start(provider))
        return false;
    PMinit();
    registerPowerDriver(this, powerStates, 2);
    provider->joinPMtree(this);
    makeUsable();

    mtk_rust_set_log(rustLog);
    BTLog("start, Rust core %s\n", mtk_rust_version());

    char firmware[64] {};
    bool ok = bringUp(firmware, sizeof(firmware));
    publish(ok, ok ? firmware : nullptr);
    release();
    if (!ok) {
        PMstop();
        super::stop(provider);
        return false;
    }
    armOpenWatchdog();
    return true;
}

bool MtkWirelessBTLoader::bringUp(char *firmware, size_t len)
{
    if (!m_device->open(this)) {
        BTLog("can not open device\n");
        return false;
    }
    m_usb = new USBDeviceController;
    if (!m_usb || !m_usb->init(this, m_device) || !m_usb->initConfiguration() || !m_usb->findInterface() ||
        !m_usb->findPipes()) {
        BTLog("USB setup failed\n");
        return false;
    }
    const mtk_bt_transport transport = makeTransport(m_usb);
    mtk_bt_setup_info info {};
    int32_t ret = mtk_bt_setup(&transport, &info);
    if (ret != 0) {
        BTLog("bring-up failed (%d)\n", ret);
        return false;
    }
    strlcpy(firmware, info.firmware, len);
    BTLog("MediaTek 0x%04x ready, firmware %s\n", info.dev_id, firmware);
    return true;
}

void MtkWirelessBTLoader::publish(bool loaded, const char *firmware)
{
    // MtkBtLog + FirmwareLoaded also tell the routes (Patcher.cpp) that this
    // is a MediaTek controller we set up.
    m_device->setProperty("MtkBtLog", mtkbt_log_buf());
    m_device->setProperty("FirmwareLoaded", loaded);
    if (firmware)
        setProperty("fw_name", firmware);
    if (version_major >= 21) {
        // Registry name only. Leave "USB Product Name" alone: bluetoothd skips
        // devices named "Bluetooth USB Host Controller" (Apple's internal
        // module, assumed Broadcom); with the NVRAM variables
        // bluetoothInternalControllerInfo / bluetoothExternalDongleFailed the
        // device is used as THIRD_PARTY_DONGLE instead.
        m_device->setName("Bluetooth USB Host Controller");
    }
}

void MtkWirelessBTLoader::release()
{
    OSSafeReleaseNULL(m_usb);
    if (m_device && m_device->isOpen(this))
        m_device->close(this);
}

IOReturn MtkWirelessBTLoader::setPowerState(unsigned long, IOService *)
{
    return IOPMAckImplied;
}

// The controller keeps USB power across shutdown and restart (at boot it is
// usually "already patched"), and with it every ACL/LE link: paired devices
// stayed connected to a powered-off Mac until the next boot's bring-up reset
// the controller. bluetoothd is gone by now and never turned it off. Do what
// Linux btmtk_usb_shutdown does on every close: WMT FUNC_CTRL off.
// -mtkbtnoshutdownoff disables it.
void MtkWirelessBTLoader::systemWillShutdown(IOOptionBits specifier)
{
    if ((specifier == kIOMessageSystemWillPowerOff || specifier == kIOMessageSystemWillRestart) &&
        !checkKernelArgument("-mtkbtnoshutdownoff"))
        powerOffController(specifier);
    super::systemWillShutdown(specifier);
}

void MtkWirelessBTLoader::powerOffController(IOOptionBits specifier)
{
    const char *what = specifier == kIOMessageSystemWillRestart ? "restart" : "power off";
    if (!m_device || isInactive() || m_device->isInactive())
        return;
    if (!m_device->open(this)) {
        BTLog("%s: device busy, Bluetooth left on\n", what);
        return;
    }
    auto *controller = new USBDeviceController;
    if (controller && controller->init(this, m_device) && controller->findInterface()) {
        controller->setCommandTimeout(kShutdownCommandTimeout);
        const mtk_bt_transport transport = makeTransport(controller);
        int32_t ret = mtk_bt_enable(&transport, false);
        BTLog("%s: FUNC_CTRL off %s (%d)\n", what, ret == 0 ? "done" : "failed", ret);
    } else {
        BTLog("%s: HCI interface unavailable, Bluetooth left on\n", what);
    }
    OSSafeReleaseNULL(controller);
    if (m_device->isOpen(this))
        m_device->close(this);
}

// Mac Pro sleep cuts USB power, so every wake re-enumerates the controller
// and the bring-up downloads the patch again (~22 s). 2026-11-12 18:42:
// bluetoothd's full-wake handler and its device notification then both ran
// DeviceFoundCB for the new device at the same instant (two BTDevices, two
// device user clients, BTDeviceOpen never ran, no HCI interface user
// client): every HCI command failed with OI status 1205, BLE advertising and
// scanning died and AirDrop saw nobody until bluetoothd was restarted.
// After a bring-up outside boot, if nobody has opened the HCI interface
// within 20 s, re-enumerate once more: the patch survives a port reset, so
// the next bring-up takes well under a second and bluetoothd sees one clean
// arrival. At most 2 resets per 10 minutes; -mtkbtnoopenwatchdog off.
void MtkWirelessBTLoader::armOpenWatchdog()
{
    if (checkKernelArgument("-mtkbtnoopenwatchdog") || uptimeSeconds() < kWatchdogMinUptimeSec)
        return;
    if (!m_openWatchdog)
        m_openWatchdog = thread_call_allocate(openWatchdogFired, this);
    if (!m_openWatchdog)
        return;
    uint64_t deadline = 0;
    clock_interval_to_deadline(kWatchdogDelayMs, kMillisecondScale, &deadline);
    // The pending call owns a reference (release() here is the USB close).
    super::retain();
    if (thread_call_enter_delayed(m_openWatchdog, deadline))
        super::release(); // was already pending and keeps its own reference
}

bool MtkWirelessBTLoader::hciInterfaceOpen(IOUSBHostDevice *device)
{
    bool open = false;
    OSIterator *iterator = device->getChildIterator(gIOServicePlane);
    if (!iterator)
        return true; // can not tell: do nothing
    while (OSObject *candidate = iterator->getNextObject()) {
        auto *interface = OSDynamicCast(IOUSBHostInterface, candidate);
        if (!interface)
            continue;
        const StandardUSB::InterfaceDescriptor *desc = interface->getInterfaceDescriptor();
        if (desc && desc->bInterfaceNumber == 0 && interface->isOpen()) {
            open = true;
            break;
        }
    }
    OSSafeReleaseNULL(iterator);
    return open;
}

void MtkWirelessBTLoader::openWatchdogFired(thread_call_param_t param, thread_call_param_t)
{
    auto *self = static_cast<MtkWirelessBTLoader *>(param);
    // Retained parent: stop()/detach may run concurrently.
    IORegistryEntry *parent = self->copyParentEntry(gIOServicePlane);
    auto *device = OSDynamicCast(IOUSBHostDevice, parent);
    if (device && !self->isInactive() && !device->isInactive() && !hciInterfaceOpen(device)) {
        const uint64_t now = uptimeSeconds();
        if (now - gWatchdogWindowStart > kWatchdogWindowSec) {
            gWatchdogWindowStart = now;
            gWatchdogResets = 0;
        }
        if (gWatchdogResets < kWatchdogMaxResets) {
            ++gWatchdogResets;
            BTLog("HCI interface not opened %u ms after bring-up, re-enumerating (%u/%u)\n",
                  kWatchdogDelayMs, gWatchdogResets, kWatchdogMaxResets);
            IOReturn ret = device->reset();
            if (ret != kIOReturnSuccess)
                BTLog("re-enumerate failed: %s\n", device->stringFromReturn(ret));
        } else {
            BTLog("HCI interface still not opened, reset limit reached\n");
        }
    }
    OSSafeReleaseNULL(parent);
    self->OSObject::release();
}

void MtkWirelessBTLoader::stop(IOService *provider)
{
    // No cancel_wait: the callback may be inside reset(), which terminates
    // this object. A running callback holds its own reference; the call is
    // freed in free().
    if (m_openWatchdog && thread_call_cancel(m_openWatchdog))
        super::release();
    release();
    PMstop();
    super::stop(provider);
}

void MtkWirelessBTLoader::free()
{
    if (m_openWatchdog) {
        thread_call_free(m_openWatchdog);
        m_openWatchdog = nullptr;
    }
    super::free();
}
