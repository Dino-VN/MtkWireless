//
//  MtkWirelessBT.cpp
//  MtkWireless
//
//  Lilu plugin. Phase 0: runs the Rust core self-test at plugin start and
//  publishes it on IOResources. The firmware loader (IOUSBHostDevice) and the
//  bluetoothd HCI routes are added in phase 1.
//

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/plugin_start.hpp>

#include "mtk_rust.h"

#define DRV_NAME "mtkbt"

static const char *bootargOff[] { "-mtkbtoff" };
static const char *bootargDebug[] { "-mtkbtdbg" };
static const char *bootargBeta[] { "-mtkbtbeta" };

static void rustLog(const char *msg)
{
    SYSLOG(DRV_NAME, "%s", msg);
}

static void publishRustState(uint32_t failed)
{
    IOService *res = IOService::getResourceService();
    OSDictionary *d = OSDictionary::withCapacity(2);
    if (!res || !d) {
        OSSafeReleaseNULL(d);
        return;
    }
    OSString *version = OSString::withCString(mtk_rust_version());
    OSNumber *mask = OSNumber::withNumber(failed, 32);
    if (version) { d->setObject("RustCore", version); version->release(); }
    if (mask) { d->setObject("RustSelfTestFailed", mask); mask->release(); }
    res->setProperty("MtkWirelessBT", d);
    d->release();
}

static void pluginStart()
{
    mtk_rust_set_log(rustLog);
    const uint32_t failed = mtk_rust_selftest();
    SYSLOG(DRV_NAME, "Rust core %s, self-test failed mask 0x%x", mtk_rust_version(), failed);
    publishRustState(failed);
}

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::MountainLion,
    KernelVersion::Tahoe,
    pluginStart
};
