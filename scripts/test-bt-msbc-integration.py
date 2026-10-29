#!/usr/bin/env python3
"""Execute Dino's actual mSBC route/gate wrappers against a small USB mock."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--no-sanitize", action="store_true", help="run without ASan/UBSan when unavailable on the host")
args = parser.parse_args()
source = (ROOT / "kext/BT/Patcher.cpp").read_text()


def function(declaration):
    start = source.index(declaration)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


wrappers = "\n\n".join(function(name) for name in (
    "bool isMsbcReady()", "IOReturn isoIo(", "IOReturn selectAlternateSetting(",
    "void interfaceClose(", "IOReturn getPipeProperties(", "IOUSBHostPipe *copyPipe(", "void installMsbcBridge("))
assert "getDeviceDescriptor(" not in source
assert source.count('"__ZN18IOUSBHostInterface8copyPipeEh"') == 1
assert "installMsbcBridge(patcher, index, address, size, pipes);" in function("void processKext(")
assert function("IOReturn deviceRequest(").count("msbc") == 0
assert "ScoSetup" not in source and "HciScoMetadata" not in source

mock = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
using mach_vm_address_t = uintptr_t;
using IOReturn = int;
using IOOptionBits = unsigned;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnAborted = -2;
constexpr bool kOSBooleanTrue = true;
#define FunctionCast(fn, original) reinterpret_cast<decltype(&fn)>(original)
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)
#define SYSLOG(...) do {} while (0)
#define arrsize(value) (sizeof(value) / sizeof((value)[0]))
namespace KernelVersion { enum { Tahoe = 25 }; }
int kernel = 25;
bool msbcFlag;
int getKernelVersion() { return kernel; }
bool checkKernelArgument(const char *name) {
    assert(!strcmp(name, "-mtkbtnomsbc")); return !msbcFlag;
}
namespace StandardUSB {
struct EndpointDescriptor { uint8_t bmAttributes, bEndpointAddress; };
struct InterfaceDescriptor { uint8_t bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol; };
}
struct IOService {
    virtual ~IOService() = default;
    IOService *provider = nullptr;
    bool resourceReady = false;
    bool publicationFails = false;
    virtual IOService *getProvider() { return provider; }
    virtual uint64_t getRegistryEntryID() { return 123; }
    bool setProperty(const char *key, bool value) {
        assert(!strcmp(key, "MtkBtMsbcBridgeReady"));
        if (publicationFails) return false;
        resourceReady = value; return true;
    }
    static IOService *getResourceService();
};
IOService resource;
bool resourceAvailable = true;
IOService *IOService::getResourceService() { return resourceAvailable ? &resource : nullptr; }
struct IOUSBHostDevice : IOService { bool loaded = true; };
struct IOUSBHostPipe {
    StandardUSB::EndpointDescriptor descriptor {1, 3};
    unsigned getters = 0;
    const StandardUSB::EndpointDescriptor *getEndpointDescriptor() { ++getters; return &descriptor; }
};
struct IOUSBHostInterface : IOService {
    uint16_t vendor = 0x13d3, product = 0x3610;
    uint8_t number = 1;
    StandardUSB::InterfaceDescriptor descriptor {0xe0, 1, 1};
    IOUSBHostPipe *pipe = nullptr;
    unsigned getters = 0;
    uint8_t alternate = 0;
    const StandardUSB::InterfaceDescriptor *getInterfaceDescriptor() { ++getters; return &descriptor; }
};
bool msbcCachedScoInterface(IOUSBHostInterface *interface) {
    return interface && interface->vendor == 0x13d3 && interface->product == 0x3610 && interface->number == 1;
}
struct MtkMsbcStream { enum { kWirePacket = 63 }; };
struct IOMemoryDescriptor {};
struct IOUSBHostIsochronousFrame {};
struct IOUSBHostIsochronousCompletion {};
std::vector<int> events;
unsigned copyCalls, isoCalls, alternateCalls, closeCalls, bindCalls, lockCalls;
unsigned propertyCalls;
uint8_t lastAlternate;
IOReturn propertyResult;
uint64_t properties[] = {0, 3, 1, 63, 4};
void (*duringAlternate)(IOUSBHostInterface *);
bool lastBindingLoaded;
IOReturn alternateResult;
IOUSBHostPipe *originalCopy(IOUSBHostInterface *interface, uint8_t) { ++copyCalls; return interface->pipe; }
IOReturn originalIso(IOUSBHostPipe *, IOMemoryDescriptor *, IOUSBHostIsochronousFrame *, uint32_t,
                     uint64_t, IOUSBHostIsochronousCompletion *) { ++isoCalls; events.push_back(40); return 42; }
IOReturn originalAlternate(IOUSBHostInterface *interface, uint8_t alternate) {
    ++alternateCalls; lastAlternate = alternate; events.push_back(20);
    if (!alternateResult) interface->alternate = alternate;
    if (duringAlternate) duringAlternate(interface);
    return alternateResult;
}
IOReturn originalProperties(IOService *, uint8_t pipeRef, uint64_t *direction, uint64_t *number,
    uint64_t *type, uint64_t *maxPacket, uint64_t *interval) {
    assert(pipeRef == 1 || pipeRef == 2); ++propertyCalls;
    uint64_t *outputs[] = {direction, number, type, maxPacket, interval};
    for (unsigned i = 0; i < 5; ++i) if (outputs[i]) *outputs[i] = properties[i];
    return propertyResult;
}
void originalClose(IOUSBHostInterface *, IOService *, IOOptionBits) { ++closeCalls; events.push_back(21); }
bool leShimEnabled;
void *leLock;
void IOLockLock(void *) { ++lockCalls; }
void IOLockUnlock(void *) {}
bool isLoadedMediaTek(IOUSBHostDevice *device) { return device && device->loaded; }
void mtk_bt_le_bind_event_pipe(IOUSBHostPipe *, uint64_t id, bool loaded) {
    assert(id == 123); ++bindCalls; lastBindingLoaded = loaded;
}
struct UsbMsbcBridge {
    using IsoIo = decltype(&originalIso);
    static bool allocationFails;
    static unsigned allocations, releases;
    IOUSBHostInterface *interface = nullptr;
    IOUSBHostPipe *pipe = nullptr;
    uint64_t generation = 1;
    bool changing = false;
    unsigned observations = 0, activations = 0, publications = 0;
    static UsbMsbcBridge *create() {
        ++allocations; return allocationFails ? nullptr : new UsbMsbcBridge;
    }
    void release() { ++releases; delete this; }
    bool observePipe(IOUSBHostInterface *candidate, IOUSBHostPipe *candidatePipe) {
        ++observations;
        if (!candidatePipe || !msbcCachedScoInterface(candidate) || !mapsPhysicalInterface(candidate)) return false;
        pipe = candidatePipe; ++activations; return true;
    }
    uint64_t beginInterfaceChange() { events.push_back(10); changing = true; return ++generation; }
    bool bindPhysicalInterface(IOUSBHostInterface *candidate, uint64_t ticket) {
        if (!msbcCachedScoInterface(candidate) || candidate->alternate != 6 || ticket != generation || interface) return false;
        interface = candidate; changing = false; events.push_back(31); return true;
    }
    bool mapsPhysicalInterface(IOUSBHostInterface *candidate) { return ownsInterface(candidate) && !changing; }
    void setMsbcActive(bool active) { assert(active); ++activations; }
    bool handlesPipe(IOUSBHostPipe *candidate) { return candidate == pipe; }
    bool ownsInterface(IOUSBHostInterface *candidate) { return candidate == interface; }
    void stop() { events.push_back(10); }
    void detach(uint64_t ticket = 0) {
        if (ticket && ticket != generation) return;
        events.push_back(30); interface = nullptr; pipe = nullptr; changing = false;
    }
    void publishStatus() { ++publications; }
    IOReturn io(IOUSBHostPipe *, IOMemoryDescriptor *, IOUSBHostIsochronousFrame *, uint32_t count,
                uint64_t firstFrame, IOUSBHostIsochronousCompletion *, IsoIo original) {
        assert(count == 15 && firstFrame == 900 && original == originalIso);
        events.push_back(41); return 43;
    }
};
bool UsbMsbcBridge::allocationFails;
unsigned UsbMsbcBridge::allocations, UsbMsbcBridge::releases;
mach_vm_address_t orgCopyPipe, orgIsoIo, orgSelectAlternateSetting, orgInterfaceClose, orgGetPipeProperties;
UsbMsbcBridge *msbcBridge;
bool msbcReady;
struct KernelPatcher {
    enum class Error { NoError, Failed };
    struct RouteRequest {
        const char *symbol;
        void *replacement;
        mach_vm_address_t *original;
        mach_vm_address_t from = 0;
        RouteRequest(const char *s, void *r, mach_vm_address_t &o) : symbol(s), replacement(r), original(&o) {}
    };
    unsigned solves = 0, routeCalls = 0;
    int failSolve = -1, missingOriginal = -1;
    bool routingFails = false;
    Error error = Error::NoError;
    mach_vm_address_t solveSymbol(size_t, const char *, mach_vm_address_t, size_t) {
        return int(solves++) == failSolve ? 0 : 1;
    }
    Error getError() { return error; }
    void clearError() { error = Error::NoError; }
    bool routeMultiple(size_t, RouteRequest *routes, size_t count, mach_vm_address_t, size_t) {
        ++routeCalls; assert(solves == 4 && count == 4);
        const mach_vm_address_t originals[] = {reinterpret_cast<mach_vm_address_t>(originalIso),
            reinterpret_cast<mach_vm_address_t>(originalAlternate), reinterpret_cast<mach_vm_address_t>(originalClose),
            reinterpret_cast<mach_vm_address_t>(originalProperties)};
        for (size_t i = 0; i < count; ++i) {
            assert(routes[i].from && routes[i].replacement);
            if (int(i) != missingOriginal) *routes[i].original = originals[i];
        }
        if (routingFails) error = Error::Failed;
        return !routingFails;
    }
};
'''

tests = r'''
void reset() {
    if (msbcBridge) msbcBridge->release();
    msbcBridge = nullptr; msbcReady = false; resource.resourceReady = false;
    resource.publicationFails = false; resourceAvailable = true;
    orgCopyPipe = reinterpret_cast<mach_vm_address_t>(originalCopy);
    orgIsoIo = orgSelectAlternateSetting = orgInterfaceClose = orgGetPipeProperties = 0;
    msbcFlag = true; kernel = 25; leShimEnabled = false;
    UsbMsbcBridge::allocationFails = false;
    UsbMsbcBridge::allocations = UsbMsbcBridge::releases = 0;
    copyCalls = isoCalls = alternateCalls = closeCalls = bindCalls = lockCalls = 0;
    propertyCalls = 0; propertyResult = 0;
    duringAlternate = nullptr;
    uint64_t defaults[] = {0, 3, 1, 63, 4};
    memcpy(properties, defaults, sizeof(properties));
    alternateResult = 0; events.clear();
}
int main() {
    // Real initializer: opt-in, Tahoe, and the existing copyPipe route/original are required.
    for (int version : {24, 25, 26}) for (bool flag : {false, true})
    for (bool copied : {false, true}) {
        reset(); kernel = version; msbcFlag = flag;
        KernelPatcher patcher;
        installMsbcBridge(patcher, 0, 0, 0, copied);
        const bool expected = version == 25 && flag && copied;
        assert(isMsbcReady() == expected && resource.resourceReady == expected);
        assert(patcher.solves == (expected ? 4U : 0U));
        assert(!expected || (msbcBridge && msbcBridge->publications == 1));
    }
    reset(); orgCopyPipe = 0;
    { KernelPatcher p; installMsbcBridge(p, 0, 0, 0, true); assert(!isMsbcReady() && p.solves == 0); }
    for (int index = 0; index < 4; ++index) {
        reset(); KernelPatcher p; p.failSolve = index;
        installMsbcBridge(p, 0, 0, 0, true);
        assert(!isMsbcReady() && !resource.resourceReady && !p.routeCalls && !UsbMsbcBridge::allocations);
        reset(); KernelPatcher q; q.missingOriginal = index;
        installMsbcBridge(q, 0, 0, 0, true);
        assert(!isMsbcReady() && !msbcBridge && !resource.resourceReady && UsbMsbcBridge::releases == 1);
    }
    reset(); UsbMsbcBridge::allocationFails = true;
    { KernelPatcher p; installMsbcBridge(p, 0, 0, 0, true); assert(!isMsbcReady() && !p.routeCalls); }
    reset();
    { KernelPatcher p; p.routingFails = true; installMsbcBridge(p, 0, 0, 0, true);
      assert(!isMsbcReady() && !msbcBridge && !resource.resourceReady && UsbMsbcBridge::releases == 1); }
    reset(); resourceAvailable = false;
    { KernelPatcher p; installMsbcBridge(p, 0, 0, 0, true);
      assert(!isMsbcReady() && !msbcBridge && UsbMsbcBridge::releases == 1); }
    reset(); resource.publicationFails = true;
    { KernelPatcher p; installMsbcBridge(p, 0, 0, 0, true);
      assert(p.routeCalls == 1 && !isMsbcReady() && !resource.resourceReady && !msbcBridge &&
             UsbMsbcBridge::releases == 1); }
    // Existing LE processing and new SCO observation remain independent in all combinations.
    for (bool msbc : {false, true}) for (bool le : {false, true}) {
        reset(); msbcFlag = msbc; leShimEnabled = le;
        KernelPatcher p; installMsbcBridge(p, 0, 0, 0, true);
        IOUSBHostDevice device;
        IOUSBHostPipe sco, interrupt;
        interrupt.descriptor = {3, 0x81};
        IOUSBHostInterface interface; interface.provider = &device; interface.pipe = &sco;
        if (msbc) assert(selectAlternateSetting(&interface, 4) == 0 && lastAlternate == 6);
        assert(copyPipe(&interface, 3) == &sco && copyCalls == 1);
        assert(!msbc || msbcBridge->activations == 1);
        interface.pipe = &interrupt;
        assert(copyPipe(&interface, 0x81) == &interrupt && copyCalls == 2);
        assert(bindCalls == unsigned(le));
        assert(!le || lastBindingLoaded);
        assert(!msbc || msbcBridge->observations == 1);
    }
    reset();
    { KernelPatcher p; installMsbcBridge(p, 0, 0, 0, true); }
    IOUSBHostPipe sco, foreign;
    IOUSBHostInterface owned, f1;
    f1.vendor = 0x1d57; f1.product = 0xfa60; f1.pipe = &foreign; owned.pipe = &sco;
    // Even an unexpected F1 endpoint address cannot add descriptor getters or locks.
    assert(copyPipe(&f1, 3) == &foreign && !msbcBridge->activations);
    assert(!foreign.getters && !f1.getters && !lockCalls);
    assert(copyPipe(&owned, 3) == &sco && !msbcBridge->activations);
    alternateResult = 7;
    assert(selectAlternateSetting(&owned, 4) == 7 && lastAlternate == 6);
    assert(!msbcBridge->ownsInterface(&owned));
    alternateResult = 0;
    duringAlternate = [](IOUSBHostInterface *candidate) { candidate->alternate = 5; };
    assert(selectAlternateSetting(&owned, 4) == kIOReturnAborted && !msbcBridge->ownsInterface(&owned));
    duringAlternate = [](IOUSBHostInterface *) { msbcBridge->beginInterfaceChange(); };
    assert(selectAlternateSetting(&owned, 4) == kIOReturnAborted && !msbcBridge->ownsInterface(&owned));
    duringAlternate = nullptr;
    assert(selectAlternateSetting(&owned, 2) == 0 && lastAlternate == 2);
    assert(!msbcBridge->ownsInterface(&owned));
    assert(selectAlternateSetting(&owned, 4) == 0 && lastAlternate == 6);
    assert(msbcBridge->mapsPhysicalInterface(&owned));
    assert(copyPipe(&owned, 3) == &sco && msbcBridge->activations == 1);
    assert(isoIo(&foreign, nullptr, nullptr, 15, 900, nullptr) == 42);
    assert(isoIo(&sco, nullptr, nullptr, 15, 900, nullptr) == 43 && isoCalls == 1);
    IOService ownedClient, foreignClient;
    ownedClient.provider = &owned; foreignClient.provider = &f1;
    uint64_t outputs[5];
    assert(getPipeProperties(&ownedClient, 1, outputs, outputs+1, outputs+2, outputs+3, outputs+4) == 0);
    assert(outputs[0] == 0 && outputs[1] == 3 && outputs[2] == 1 && outputs[3] == 33 && outputs[4] == 4);
    properties[0] = 1;
    assert(getPipeProperties(&ownedClient, 2, outputs, outputs+1, outputs+2, outputs+3, outputs+4) == 0 && outputs[3] == 33);
    assert(getPipeProperties(&foreignClient, 1, outputs, outputs+1, outputs+2, outputs+3, outputs+4) == 0);
    assert(outputs[3] == 63 && !foreign.getters && !f1.getters && !lockCalls);
    for (unsigned mismatch = 0; mismatch < 5; ++mismatch) {
        const uint64_t original = properties[mismatch]; properties[mismatch] = 99;
        assert(getPipeProperties(&ownedClient, 1, outputs, outputs+1, outputs+2, outputs+3, outputs+4) == 0);
        assert(outputs[3] == properties[3]); properties[mismatch] = original;
    }
    for (unsigned missing = 0; missing < 5; ++missing) {
        uint64_t *pointers[] = {outputs, outputs+1, outputs+2, outputs+3, outputs+4}; pointers[missing] = nullptr;
        assert(getPipeProperties(&ownedClient, 1, pointers[0], pointers[1], pointers[2], pointers[3], pointers[4]) == 0);
        if (missing != 3) assert(outputs[3] == 63);
    }
    propertyResult = 9;
    assert(getPipeProperties(&ownedClient, 1, outputs, outputs+1, outputs+2, outputs+3, outputs+4) == 9 && outputs[3] == 63);
    propertyResult = 0;
    events.clear();
    assert(selectAlternateSetting(&f1, 0) == 0 && events == std::vector<int>({20}));
    events.clear(); alternateResult = 7;
    assert(selectAlternateSetting(&owned, 0) == 7 && events == std::vector<int>({10, 20}));
    assert(msbcBridge->ownsInterface(&owned));
    assert(!msbcBridge->mapsPhysicalInterface(&owned));
    events.clear(); alternateResult = 0;
    assert(selectAlternateSetting(&owned, 0) == 0 && events == std::vector<int>({10, 20, 30}));
    assert(selectAlternateSetting(&owned, 4) == 0);
    owned.pipe = &sco; copyPipe(&owned, 3); events.clear();
    interfaceClose(&f1, nullptr, 0); assert(events == std::vector<int>({21}));
    events.clear(); interfaceClose(&owned, nullptr, 0);
    assert(events == std::vector<int>({10, 21, 30}) && !msbcBridge->ownsInterface(&owned));
    // A partial routing failure leaves installed lifecycle wrappers inert and pass-through.
    reset();
    { KernelPatcher p; p.routingFails = true; installMsbcBridge(p, 0, 0, 0, true); }
    assert(isoIo(&foreign, nullptr, nullptr, 15, 900, nullptr) == 42);
    events.clear(); selectAlternateSetting(&owned, 0); interfaceClose(&owned, nullptr, 0);
    assert(events == std::vector<int>({20, 21}));
    reset();
    puts("PASS: actual Dino mSBC flags/readiness/failures, independent LE/SCO paths, F1 bypass and ordered lifecycle wrappers");
}
'''

with tempfile.TemporaryDirectory(prefix="dino-msbc-test-") as directory:
    cpp = Path(directory) / "integration.cpp"
    cpp.write_text(mock + wrappers + tests)
    binary = Path(directory) / "integration"
    sanitizer = [] if args.no_sanitize else ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    subprocess.run(["xcrun", "clang++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    *sanitizer, str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
