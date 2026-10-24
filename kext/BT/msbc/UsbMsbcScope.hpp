#pragma once

#ifndef USB_MSBC_BRIDGE_TEST
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSNumber.h>
#endif

// Registry identity is available after enumeration and never sends USB requests.
static inline bool msbcCachedNumber(IORegistryEntry *entry, const char *key, uint32_t expected) {
    auto *number = entry ? OSDynamicCast(OSNumber, entry->getProperty(key)) : nullptr;
    return number && number->unsigned32BitValue() == expected;
}

static inline bool msbcCachedMtkDevice(IORegistryEntry *entry) {
    return msbcCachedNumber(entry, "idVendor", 0x13d3) &&
        msbcCachedNumber(entry, "idProduct", 0x3610);
}

static inline bool msbcCachedScoInterface(IORegistryEntry *entry) {
    return msbcCachedMtkDevice(entry) && msbcCachedNumber(entry, "bInterfaceNumber", 1);
}
