#!/usr/bin/env python3
"""Run production LE USB wrappers with the host Rust ABI under ASan/UBSan."""
import argparse
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--no-sanitize', action='store_true')
args = parser.parse_args()
source = (root / 'kext/BT/Patcher.cpp').read_text()

def function(declaration):
    start = source.index(declaration)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

mock = r'''
#include "mtk_bt.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
using IOReturn = int32_t;
using SInt32 = int32_t;
using UInt32 = uint32_t;
using mach_vm_address_t = uintptr_t;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr int kIODirectionInOut = 3;
#define FunctionCast(fn, original) reinterpret_cast<decltype(&fn)>(original)
#define SYSLOG(...) do {} while (0)
namespace StandardUSB {
struct DeviceRequest { uint8_t bmRequestType, bRequest; uint16_t wValue, wIndex, wLength; };
}
struct IOService {};
struct IOUSBHostDevice { bool loaded = true; uint64_t getRegistryEntryID() { return 42; } };
struct IOMemoryDescriptor {
    std::vector<uint8_t> bytes;
    int refs = 1, prepared = 0;
    bool prepareFails = false, writeFails = false;
    size_t getLength() { return bytes.size(); }
    IOReturn prepare(int) { if (prepareFails) return -1; ++prepared; return 0; }
    void complete(int) { assert(prepared-- > 0); }
    size_t readBytes(size_t off, void *out, size_t len) {
        assert(prepared && off + len <= bytes.size()); memcpy(out, bytes.data() + off, len); return len;
    }
    size_t writeBytes(size_t off, const void *in, size_t len) {
        assert(prepared && off + len <= bytes.size());
        if (writeFails) return 0;
        memcpy(bytes.data() + off, in, len); return len;
    }
    void retain() { ++refs; }
    void release() { assert(--refs >= 1); }
};
struct IOUSBHostPipe {};
struct IOUSBHostCompletion {
    void *owner;
    void (*action)(void *, void *, IOReturn, uint32_t);
    void *parameter;
};
bool leShimEnabled = true, leReadyMaskEnabled = true, randomAddressHack = false;
int lockDepth, publications, allocations;
void *leLock = reinterpret_cast<void *>(1);
int32_t leCommands, leCompletions, leErrors;
void IOLockLock(void *) { assert(!lockDepth++); }
void IOLockUnlock(void *) { assert(lockDepth-- == 1); }
void OSIncrementAtomic(volatile SInt32 *v) { ++*v; }
SInt32 OSDecrementAtomic(volatile SInt32 *v) { return (*v)--; }
bool OSCompareAndSwap(UInt32 old, UInt32 value, volatile UInt32 *p) {
    if (*p != old) return false; *p = value; return true;
}
bool allocationFails;
void *IOMalloc(size_t size) { if (allocationFails) return nullptr; ++allocations; return malloc(size); }
void IOFree(void *p, size_t) { --allocations; free(p); }
void publishCounters() { assert(!lockDepth); ++publications; }
bool isLoadedMediaTek(IOUSBHostDevice *d) { return d && d->loaded; }
void fixReadClock(StandardUSB::DeviceRequest &, void *, IOMemoryDescriptor *) {}
void randomAddress(void *, IOService *, StandardUSB::DeviceRequest &, void *, IOMemoryDescriptor *, unsigned) {}
void fixAclFlush(IOUSBHostPipe *, IOMemoryDescriptor *, uint32_t) {}
mach_vm_address_t orgDeviceRequest, orgPipeIo, orgPipeIoSync;
IOUSBHostCompletion pending {};
int mode; // 0 sync, 1 callback before return, 2 deferred, 3 failed submission
int calls;
std::vector<uint8_t> sent;
IOReturn originalRequest(void *, IOService *, StandardUSB::DeviceRequest &request, void *data,
    IOMemoryDescriptor *descriptor, unsigned &length, IOUSBHostCompletion *completion, unsigned) {
    ++calls; assert(length == request.wLength);
    auto *b = data ? static_cast<uint8_t *>(data) : descriptor->bytes.data();
    sent.assign(b, b + request.wLength);
    if (mode == 3) return -7;
    if (completion && completion->action) {
        if (mode == 1) completion->action(completion->owner, completion->parameter, 0, length);
        else pending = *completion;
    }
    return 0;
}
IOReturn originalPipe(void *, IOMemoryDescriptor *, uint32_t, IOUSBHostCompletion *completion, uint32_t) {
    pending = *completion; return 0;
}
IOReturn originalPipeSync(void *, IOMemoryDescriptor *buffer, uint32_t, uint32_t &transferred, uint32_t) {
    transferred = buffer->bytes.size(); return 0;
}
int callbacks; IOReturn callbackStatus; uint32_t callbackLength;
void client(void *, void *p, IOReturn status, uint32_t length) {
    assert(p == reinterpret_cast<void *>(17)); ++callbacks; callbackStatus = status; callbackLength = length;
}
void finish(IOReturn status, uint32_t length) {
    auto c = pending; pending = {}; assert(c.action); c.action(c.owner, c.parameter, status, length);
}
mtk_bt_le_ready_diagnostics snapshot() {
    mtk_bt_le_ready_diagnostics d {}; mtk_bt_le_ready_snapshot(&d); return d;
}
static_assert(sizeof(mtk_bt_le_ready_diagnostics) == 360);
static_assert(offsetof(mtk_bt_le_ready_diagnostics, commands) == 40);
static_assert(offsetof(mtk_bt_le_ready_diagnostics, meta) == 232);
'''

state = source[source.index('struct LeCompletion {'):source.index('LeCompletion *makeLeCompletion(')]
wrappers = '\n'.join(function(name) for name in (
    'bool readRequestBytes(', 'uint32_t leReadyCommand(', 'void leReadyUsbStatus(',
    'uint64_t leControllerForPipe(', 'void leCancel(', 'uint64_t leArm(', 'void leReceive(',
    'LeCompletion *makeLeCompletion(', 'void releaseLeCompletion(', 'void leUsbCompletion(',
    'void finishLeSubmission(', 'IOReturn deviceRequest(', 'IOReturn pipeIo(', 'IOReturn pipeIoSync('))
tests = r'''
int main() {
    orgDeviceRequest = reinterpret_cast<uintptr_t>(originalRequest);
    orgPipeIo = reinterpret_cast<uintptr_t>(originalPipe);
    orgPipeIoSync = reinterpret_cast<uintptr_t>(originalPipeSync);
    IOUSBHostDevice device;
    StandardUSB::DeviceRequest request {0x20, 0, 0, 0, 11};
    IOUSBHostCompletion completion {nullptr, client, reinterpret_cast<void *>(17)};
    const std::vector<uint8_t> mask {1, 0x20, 8, 0x1f, 0, 0, 0, 0, 0, 0, 0};
    for (bool raw : {false, true}) for (int m : {0, 1, 2, 3}) {
        mode = m; IOMemoryDescriptor buffer; buffer.bytes = mask;
        unsigned length = 11; int oldCallbacks = callbacks, oldCalls = calls;
        auto before = snapshot();
        auto *async = m ? &completion : nullptr;
        auto status = deviceRequest(&device, nullptr, request, raw ? buffer.bytes.data() : nullptr,
                                    raw ? nullptr : &buffer, length, async, 1000);
        assert(status == (m == 3 ? -7 : 0));
        assert(calls == oldCalls + 1 && sent[3] == 0x5f && sent[4] == 8);
        if (m == 2) { assert(allocations == 1); finish(-9, 0); }
        assert(callbacks == oldCallbacks + (m == 1 || m == 2));
        auto d = snapshot(); assert(d.commands[0].count == before.commands[0].count + 1);
        assert(d.mask_rewrites == before.mask_rewrites + 1);
        assert(d.commands[0].usb_errors == before.commands[0].usb_errors + (m == 2 || m == 3));
        assert(!allocations && !buffer.prepared && buffer.refs == 1);
    }
    mode = 0;
    for (int scenario = 0; scenario < 6; ++scenario) {
        IOMemoryDescriptor b; b.bytes = mask; unsigned length = 11;
        auto before = snapshot();
        device.loaded = scenario != 0; leShimEnabled = scenario != 1; leReadyMaskEnabled = scenario != 2;
        request.bmRequestType = scenario == 3 ? 0x40 : 0x20;
        b.writeFails = scenario == 4; b.prepareFails = scenario == 5;
        assert(!deviceRequest(&device, nullptr, request, nullptr, &b, length, nullptr, 1000));
        assert(sent == mask && !b.prepared);
        if (scenario == 4) assert(snapshot().mask_write_errors == before.mask_write_errors + 1);
    }
    device.loaded = leShimEnabled = leReadyMaskEnabled = true; request.bmRequestType = 0x20;
    IOMemoryDescriptor zero; zero.bytes = mask; zero.bytes[3] = 0; unsigned length = 11;
    deviceRequest(&device, nullptr, request, nullptr, &zero, length, nullptr, 1000);
    assert(sent == zero.bytes && sent[3] == 0);
    // Real 0xFD3C rewrite remains separate and preserves command credits.
    IOMemoryDescriptor feature; feature.bytes = {0x3c, 0xfd, 8, 0xfd, 0xfd, 1, 0xff, 9, 0, 0, 0};
    deviceRequest(&device, nullptr, request, nullptr, &feature, length, nullptr, 1000);
    IOUSBHostPipe pipe; mtk_bt_le_bind_event_pipe(&pipe, 42, true);
    IOMemoryDescriptor event;
    event.bytes = {0x0f, 4, 1, 3, 0x3c, 0xfd};
    assert(!pipeIo(&pipe, &event, event.bytes.size(), &completion, 1000));
    finish(0, event.bytes.size());
    assert((event.bytes == std::vector<uint8_t>{0x0e, 4, 3, 0x3c, 0xfd, 0}));
    assert(leCompletions == 1 && !allocations && event.refs == 1);
    // PHY and DLE are observed without changing any hardware bytes or length.
    event.bytes = {0x3e, 6, 12, 0, 1, 2, 2, 2, 0x3e, 11, 7, 1, 2, 251, 0, 0x48, 8, 251, 0, 0x48, 8};
    auto wire = event.bytes;
    pipeIo(&pipe, &event, event.bytes.size(), &completion, 1000); finish(0, event.bytes.size());
    assert(event.bytes == wire && callbackLength == wire.size() && callbackStatus == 0);
    assert(snapshot().meta[1].count == 1 && snapshot().meta[0].count == 1);
    uint32_t transferred = 0;
    pipeIoSync(&pipe, &event, wire.size(), transferred, 1000);
    assert(event.bytes == wire && transferred == wire.size() && snapshot().meta[1].count == 2);
    // Other pipes and failed/oversized reads do not feed the observer.
    IOUSBHostPipe foreign;
    pipeIo(&foreign, &event, wire.size(), &completion, 1000); finish(0, wire.size());
    pipeIo(&pipe, &event, wire.size(), &completion, 1000); finish(-1, wire.size());
    pipeIo(&pipe, &event, wire.size(), &completion, 1000); finish(0, wire.size() + 1);
    assert(snapshot().meta[1].count == 2 && !allocations && event.refs == 1);
    allocationFails = true;
    pipeIo(&pipe, &event, wire.size(), &completion, 1000); finish(0, wire.size());
    assert(snapshot().meta[1].count == 2);
    puts("PASS: production LE EP0/interrupt routes, real Rust ABI, scope, mask writes, async lifetime/errors, FD3C coexistence");
}
'''
host = root / 'build/rust-bt-host'
subprocess.run(['cargo', 'build', '-q', '-p', 'mtk-ffi', '--features', 'bt',
                '--target-dir', str(host)], cwd=root / 'rust', check=True)
with tempfile.TemporaryDirectory(prefix='mtk-bt-le-ready-') as directory:
    work = Path(directory)
    (work / 'test.cpp').write_text(mock + state + wrappers + tests)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    *([] if args.no_sanitize else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']),
                    '-I', str(root / 'kext/BT'), '-I', str(root / 'kext/include'),
                    str(work / 'test.cpp'), str(host / 'debug/libmtk_ffi.a'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
