#!/usr/bin/env python3
"""Host tests of production WMT polling, HCI read lifetimes, and loader release API."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--no-sanitize", action="store_true")
args = parser.parse_args()
source = (ROOT / "kext/BT/USBDeviceController.cpp").read_text()
loader = (ROOT / "kext/BT/BtLoader.cpp").read_text()
loader_header = (ROOT / "kext/BT/BtLoader.hpp").read_text()


def function(text, declaration):
    start = text.index(declaration)
    opening = text.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


wmt = function(source, "IOReturn USBDeviceController::readWMTEvent(")
assert "IODelay(" not in wmt and "IOSleep(1)" in wmt
assert "void release();" not in loader_header
assert "void releaseUsbSession();" in loader_header
assert "releaseUsbSession();" in function(loader, "bool MtkWirelessBTLoader::start(")
stop = function(loader, "void MtkWirelessBTLoader::stop(")
assert "releaseUsbSession();" in stop and "Patcher::forgetController(" in stop
assert "super::release();" in stop  # watchdog reference handling remains qualified

mock = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using std::min;
using IOReturn = int32_t;
using SInt32 = int32_t;
using UInt32 = uint32_t;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnBadArgument = -1;
constexpr IOReturn kIOReturnNoMemory = -2, kIOReturnTimeout = -3, kIOReturnAborted = -4;
constexpr int kIODirectionIn = 1, kMillisecondScale = 1;
constexpr int kRequestDirectionIn = 1, kRequestTypeVendor = 2, kRequestRecipientDevice = 0;
uint8_t makeDeviceRequestbmRequestType(int direction, int type, int recipient) {
    return uint8_t((direction << 7) | (type << 5) | recipient);
}
namespace StandardUSB {
struct DeviceRequest { uint8_t bmRequestType, bRequest; uint16_t wValue, wIndex, wLength; };
}
struct IOService {};
uint64_t now;
unsigned sleeps, sleptMs;
void clock_get_uptime(uint64_t *value) { *value = now; }
void clock_interval_to_deadline(uint32_t ms, int scale, uint64_t *deadline) {
    assert(scale == kMillisecondScale); *deadline = now + ms;
}
void IOSleep(uint32_t ms) { ++sleeps; sleptMs += ms; now += ms; }
bool contextAllocationFails, mdAllocationFails, prepareFails;
int contexts, descriptors;
void *IOMalloc(size_t size) {
    if (contextAllocationFails) return nullptr;
    ++contexts; return malloc(size);
}
void IOFree(void *p, size_t) { assert(contexts-- > 0); free(p); }
SInt32 OSDecrementAtomic(volatile SInt32 *p) { return (*p)--; }
struct IOBufferMemoryDescriptor {
    std::vector<uint8_t> bytes;
    int prepared = 0;
    explicit IOBufferMemoryDescriptor(size_t capacity) : bytes(capacity) { ++descriptors; }
    static IOBufferMemoryDescriptor *withCapacity(size_t capacity, int direction) {
        assert(direction == kIODirectionIn);
        return mdAllocationFails ? nullptr : new IOBufferMemoryDescriptor(capacity);
    }
    IOReturn prepare(int direction) {
        assert(direction == kIODirectionIn && !prepared);
        if (prepareFails) return kIOReturnAborted;
        ++prepared; return 0;
    }
    void complete(int direction) { assert(direction == kIODirectionIn && prepared-- == 1); }
    void release() { assert(!prepared && descriptors-- > 0); delete this; }
    size_t readBytes(size_t offset, void *out, size_t length) {
        assert(prepared && offset + length <= bytes.size());
        memcpy(out, bytes.data() + offset, length); return length;
    }
};
struct IOUSBHostCompletion {
    void *owner;
    void (*action)(void *, void *, IOReturn, uint32_t);
    void *parameter;
};
namespace IOUSBHostIOSource { enum { kAbortSynchronous = 1 }; }
struct Device {
    unsigned requests = 0, emptyPolls = 0;
    uint32_t requestMs = 0;
    IOReturn response = 0;
    std::vector<uint8_t> event;
    IOReturn deviceRequest(IOService *, StandardUSB::DeviceRequest &request, void *out,
                           uint32_t &transferred, uint32_t timeout) {
        assert(request.bmRequestType == 0xc0 && request.bRequest == 1 && request.wValue == 48);
        assert(request.wIndex == 0 && request.wLength == 64 && timeout == 100);
        ++requests; now += requestMs; transferred = 0;
        if (requests <= emptyPolls) return 0;
        if (response) return response;
        transferred = uint32_t(event.size());
        if (!event.empty()) memcpy(out, event.data(), min(event.size(), size_t(request.wLength)));
        return 0;
    }
};
struct Pipe {
    enum Mode { Inline, Reject, AbortInline, AbortLate } mode = Inline;
    IOUSBHostCompletion pending {};
    IOBufferMemoryDescriptor *buffer = nullptr;
    std::vector<uint8_t> event {0x0e, 4, 1, 3, 0x0c, 0};
    unsigned submissions = 0, aborts = 0;
    void finish(IOReturn status) {
        assert(pending.action && buffer && buffer->prepared);
        auto completion = pending; pending = {};
        if (!status) memcpy(buffer->bytes.data(), event.data(), event.size());
        buffer = nullptr;
        completion.action(completion.owner, completion.parameter, status, status ? 0 : uint32_t(event.size()));
    }
    IOReturn io(IOBufferMemoryDescriptor *md, uint32_t capacity, IOUSBHostCompletion *completion, uint32_t timeout) {
        ++submissions; assert(capacity == 260 && !timeout && md->prepared);
        if (mode == Reject) return kIOReturnAborted;
        buffer = md; pending = *completion;
        if (mode == Inline) finish(0);
        return 0;
    }
    IOReturn abort(int source, IOReturn status) {
        ++aborts; assert(source == IOUSBHostIOSource::kAbortSynchronous && status == kIOReturnTimeout);
        if (mode == AbortInline) { finish(status); return 0; }
        return kIOReturnAborted;
    }
};
struct USBDeviceController {
    Device *m_pDevice;
    IOService *m_pClient = nullptr;
    Pipe *m_pInterruptReadPipe;
    IOReturn readWMTEvent(void *, uint32_t, uint32_t *, uint32_t);
    IOReturn readHCIEvent(void *, uint32_t, uint32_t *, uint32_t);
};
'''
state = source[source.index("struct EventRead {"):source.index("void eventReadRelease(")]
wrappers = "\n".join(function(source, name) for name in (
    "void eventReadRelease(", "void eventReadComplete(", "IOReturn USBDeviceController::readHCIEvent("))

tests = r'''
void resetClock() { now = sleeps = sleptMs = 0; }
int main() {
    Device device; Pipe pipe; USBDeviceController controller {&device, nullptr, &pipe};
    std::array<uint8_t, 16> out; out.fill(0xa5); uint32_t received = 99;
    resetClock(); device.emptyPolls = 3; device.event = {0xe4, 1, 2, 3};
    assert(controller.readWMTEvent(out.data(), out.size(), &received, 10000) == 0);
    assert(device.requests == 4 && sleeps == 3 && sleptMs == 3 && received == 4);
    assert(out[0] == 0xe4 && out[3] == 3 && out[4] == 0xa5);
    device = {}; resetClock(); out.fill(0xa5); received = 99;
    assert(controller.readWMTEvent(out.data(), out.size(), &received, 10000) == kIOReturnTimeout);
    assert(sleeps == 10000 && sleptMs == 10000 && device.requests == 10001 && received == 99);
    assert(out.front() == 0xa5 && out.back() == 0xa5);
    // A blocking USB timeout still follows the same overall deadline. The
    // yielding poll does not add a fresh 10-second budget after each request.
    device = {}; device.requestMs = 100; device.response = kIOReturnTimeout; resetClock();
    assert(controller.readWMTEvent(out.data(), out.size(), &received, 1000) == kIOReturnTimeout);
    assert(now >= 1000 && now <= 1100 && sleeps && sleptMs == sleeps);
    device = {}; device.response = kIOReturnAborted; resetClock();
    assert(controller.readWMTEvent(out.data(), out.size(), &received, 1000) == kIOReturnAborted);
    assert(device.requests == 1 && !sleeps);
    assert(controller.readWMTEvent(nullptr, 16, &received, 1000) == kIOReturnBadArgument);
    assert(controller.readWMTEvent(out.data(), 0, &received, 1000) == kIOReturnBadArgument);
    device = {}; device.event.assign(64, 0x5a); received = 0;
    assert(controller.readWMTEvent(out.data(), 7, &received, 1000) == 0 && received == 7 && out[7] == 0xa5);
    puts("PASS: production WMT polling yields on empty reads, retains deadline/error/framing behavior and shutdown timeout");

    // Preserve the prior heap HCI read fix: inline/rejected submissions and
    // timeout/failed-abort callbacks never touch a dead stack or freed buffer.
    for (Pipe::Mode mode : {Pipe::Inline, Pipe::Reject, Pipe::AbortInline, Pipe::AbortLate}) {
        pipe = {}; pipe.mode = mode; resetClock(); out.fill(0xa5); received = 99;
        auto *reader = new USBDeviceController {&device, nullptr, &pipe};
        const IOReturn status = reader->readHCIEvent(out.data(), out.size(), &received, 10);
        delete reader; // a late callback's owner is no longer live
        if (mode == Pipe::Inline) {
            assert(status == 0 && received == pipe.event.size() && !memcmp(out.data(), pipe.event.data(), received));
        } else {
            assert(status == (mode == Pipe::Reject ? kIOReturnAborted : kIOReturnTimeout));
            assert(received == 99 && out.front() == 0xa5);
        }
        assert(pipe.submissions == 1 && pipe.aborts == unsigned(mode == Pipe::AbortInline || mode == Pipe::AbortLate));
        if (mode == Pipe::AbortLate) {
            assert(contexts == 1 && descriptors == 1 && pipe.buffer->prepared);
            pipe.finish(0); // event arrives after timeout and destruction
            assert(out.front() == 0xa5 && received == 99);
        }
        assert(!contexts && !descriptors);
    }
    pipe = {}; resetClock();
    mdAllocationFails = true;
    assert(controller.readHCIEvent(out.data(), out.size(), &received, 10) == kIOReturnNoMemory);
    mdAllocationFails = false; contextAllocationFails = true;
    assert(controller.readHCIEvent(out.data(), out.size(), &received, 10) == kIOReturnNoMemory);
    contextAllocationFails = false; prepareFails = true;
    assert(controller.readHCIEvent(out.data(), out.size(), &received, 10) == kIOReturnAborted);
    prepareFails = false;
    assert(!contexts && !descriptors && !pipe.submissions);
    puts("PASS: preserved heap HCI event context/refcounts for inline, rejected, aborted, late callbacks and allocation failures");
}
'''

# Compile the real loader header, omitting only platform include directives.
# Both inherited OSObject overloads must be callable on a non-const loader:
# the old private release() helper hid them. Also syntax-check its unchanged
# USB cleanup body under the new name, without running any watchdog/shutdown.
loader_mock = r'''
#include <cstddef>
#include <cstdint>
using SInt32 = int32_t;
using IOReturn = int32_t;
using IOOptionBits = unsigned;
using thread_call_param_t = void *;
using thread_call_t = void *;
#define OSDeclareDefaultStructors(name)
class OSObject {
public:
    virtual void release() const {}
    virtual void release(int) const {}
};
class IOService : public OSObject {
public:
    virtual IOService *probe(IOService *, SInt32 *) { return nullptr; }
    virtual bool start(IOService *) { return false; }
    virtual void stop(IOService *) {}
    virtual void free() {}
    virtual IOReturn setPowerState(unsigned long, IOService *) { return 0; }
    virtual void systemWillShutdown(IOOptionBits) {}
};
class IOUSBHostDevice : public IOService {
public:
    bool isOpen(const IOService *) const { return false; }
    void close(IOService *) {}
};
class USBDeviceController : public OSObject {};
#define OSSafeReleaseNULL(object) do { if (object) { (object)->release(); (object) = nullptr; } } while (0)
'''
loader_api = loader_mock + re.sub(r"(?m)^#include .*\n", "", loader_header)
loader_api += "\n" + function(loader, "void MtkWirelessBTLoader::releaseUsbSession(")
loader_api += "\nvoid inheritedRelease(MtkWirelessBTLoader *loader) { loader->release(); loader->release(1); }\n"

with tempfile.TemporaryDirectory(prefix="mtk-bt-usb-") as directory:
    work = Path(directory)
    (work / "usb.cpp").write_text(mock + state + wmt + wrappers + tests)
    (work / "loader.cpp").write_text(loader_api)
    flags = ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror"]
    sanitizer = [] if args.no_sanitize else ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    subprocess.run(["clang++", *flags, *sanitizer, str(work / "usb.cpp"), "-o", str(work / "usb")], check=True)
    subprocess.run([str(work / "usb")], check=True)
    subprocess.run(["clang++", *flags, "-fsyntax-only", str(work / "loader.cpp")], check=True)
    print("PASS: production loader header exposes inherited release overloads; USB cleanup uses releaseUsbSession; watchdog references unchanged")
