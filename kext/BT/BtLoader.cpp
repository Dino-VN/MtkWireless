//
//  BtLoader.cpp
//  MtkWireless
//
//  Based on SimpleMtkBtFw (zxystd, laobamac). The protocol now lives in
//  rust/mtk-bt; this file only provides USB access to it.
//

#include "BtLoader.hpp"

#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>
#include <libkern/version.h>

#include "BtFirmware.hpp"
#include "Log.h"
#include "USBDeviceController.hpp"
#include "mtk_bt.h"

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
    IOReturn ret = usb(ctx)->sendHCICommand(cmd, cmdLen, HCI_INIT_TIMEOUT);
    if (ret != kIOReturnSuccess) {
        BTLog("WMT command failed: %s\n", usb(ctx)->stringFromReturn(ret));
        return ret;
    }
    ret = usb(ctx)->readWMTEvent(event, eventCap, eventLen, HCI_INIT_TIMEOUT);
    if (ret != kIOReturnSuccess)
        BTLog("WMT event read failed: %s\n", usb(ctx)->stringFromReturn(ret));
    return ret;
}

void sleepMs(void *, uint32_t ms)
{
    IOSleep(ms);
}

const void *fwLoad(void *, const char *name, const uint8_t **data, size_t *len)
{
    OSData *fw = mtkBtCopyFirmware(name);
    if (!fw)
        return nullptr;
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

} // namespace

IOService *MtkWirelessBTLoader::probe(IOService *provider, SInt32 *score)
{
    if (!super::probe(provider, score))
        return nullptr;
    auto *device = OSDynamicCast(IOUSBHostDevice, provider);
    if (!device)
        return nullptr;
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
    const mtk_bt_transport transport = {
        .ctx = m_usb,
        .reg_read = regRead,
        .uhw_read = uhwRead,
        .uhw_write = uhwWrite,
        .wmt = wmt,
        .sleep_ms = sleepMs,
        .fw_load = fwLoad,
        .fw_release = fwRelease,
    };
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

void MtkWirelessBTLoader::stop(IOService *provider)
{
    release();
    PMstop();
    super::stop(provider);
}
