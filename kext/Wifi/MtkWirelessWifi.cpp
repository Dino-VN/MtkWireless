//
//  MtkWirelessWifi.cpp
//  MtkWireless
//

#include "MtkWirelessWifi.hpp"

#include <IOKit/IOLib.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

#include "mtk_wlan.h"
#include <AWDL/AwdlLayout.hpp>

#define super IOService
OSDefineMetaClassAndStructors(MtkWirelessWifi, IOService)

static void rustLog(const char *msg)
{
    IOLog("MtkWirelessWifi: %s\n", msg);
}

bool MtkWirelessWifi::start(IOService *provider)
{
    if (!super::start(provider))
        return false;

    mtk_rust_set_log(rustLog);
    const uint32_t failed = mtk_rust_selftest();
    IOLog("MtkWirelessWifi: Rust core %s, self-test failed mask 0x%x\n", mtk_rust_version(), failed);

    // The AWDL structs are shared between C++ and Rust; a drift between the
    // two declarations would corrupt state, so report it loudly.
    const long awdlLayout = mtkawdl::layoutMismatch();
    if (awdlLayout >= 0)
        IOLog("MtkWirelessWifi: AWDL struct layout mismatch at entry %ld, AWDL is unsafe\n", awdlLayout);

    setProperty("RustCore", mtk_rust_version());
    setProperty("RustSelfTestFailed", failed, 32);
    setProperty("AwdlLayoutOk", awdlLayout < 0);
    registerService();
    return true;
}
