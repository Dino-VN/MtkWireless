#define USB_MSBC_BRIDGE_TEST 1
#include "UsbMsbcTestSDK.hpp"
#include "../MsbcControllerScope.hpp"
#include "../UsbMsbcScope.hpp"
#include <cstdio>
#include <thread>

static bool accepted(uint32_t vendor, uint32_t product) {
    IORegistryEntry device;
    device.setNumber("idVendor", vendor);
    device.setNumber("idProduct", product);
    return msbcCachedMtkDevice(&device);
}

int main() {
    MsbcControllerScope unknown;
    assert(!unknown.allowsWideband());
    for (const auto &id : {std::pair<uint32_t, uint32_t>{0x13d3, 0x3610}, {0x0489, 0xe0cd}}) {
        MsbcControllerScope supported;
        supported.observe(accepted(id.first, id.second));
        supported.observe(accepted(id.first, id.second));
        assert(supported.allowsWideband());
    }
    assert(!accepted(0x0489, 0xe0d8));
    assert(!accepted(0x0489, 0x3610));
    assert(!accepted(0x13d3, 0xe0cd));
    MsbcControllerScope unsupported;
    unsupported.observe(accepted(0x0489, 0xe0d8));
    assert(!unsupported.allowsWideband());
    for (bool first : {false, true}) {
        MsbcControllerScope mixed;
        mixed.observe(first); mixed.observe(!first);
        assert(!mixed.allowsWideband() && mixed.observed() == 3);
    }
    MsbcControllerScope concurrent;
    std::thread good([&] { for (unsigned i = 0; i < 10000; ++i) concurrent.observe(true); });
    std::thread bad([&] { for (unsigned i = 0; i < 10000; ++i) concurrent.observe(false); });
    good.join(); bad.join();
    assert(!concurrent.allowsWideband() && concurrent.observed() == 3);
    puts("PASS: production USB-ID scope rejects E0D8; unknown/mixed/concurrent probes fail closed and supported IDs retain wideband");
}
