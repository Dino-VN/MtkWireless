//
//  MtkWirelessBT.cpp
//  MtkWireless
//
//  Lilu plugin entry. The firmware loader (BtLoader.cpp) matches the USB
//  device on its own; the plugin start installs the bluetoothd routes.
//

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/plugin_start.hpp>

#include "Patcher.hpp"
#include "mtk_rust.h"

#define DRV_NAME "mtkbt"

static const char *bootargOff[] { "-mtkbtoff" };
static const char *bootargDebug[] { "-mtkbtdbg" };
static const char *bootargBeta[] { "-mtkbtbeta" };

static void pluginStart()
{
    const uint32_t failed = mtk_rust_selftest();
    SYSLOG(DRV_NAME, "Rust core %s, self-test failed mask 0x%x", mtk_rust_version(), failed);
    Patcher::init(failed);
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
