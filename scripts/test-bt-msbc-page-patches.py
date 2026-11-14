#!/usr/bin/env python3
"""Exercise the actual opt-in bluetoothd page hook; optionally check a binary."""
import argparse
import os
import runpy
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "kext/BT/Patcher.cpp").read_text()
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--binary", type=Path, help="optional Tahoe x86_64 bluetoothd to check, without modifying it")
sanitizers = parser.add_mutually_exclusive_group()
sanitizers.add_argument("--no-sanitize", action="store_true", help="run without ASan/UBSan when unavailable on the host")
sanitizers.add_argument("--thread-sanitize", action="store_true", help="check concurrent patch/verification with ThreadSanitizer")
args = parser.parse_args()


def function(declaration):
    # Match a definition, not a forward declaration followed by another body.
    declaration = declaration.removesuffix("\n{")
    match = re.search(re.escape(declaration) + r"[^;{}]*\{", source)
    assert match, declaration
    start, opening = match.start(), match.end() - 1
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def array(name):
    declaration = re.search(r"(?:static )?const uint8_t " + name + r"\[\]\s*(?:=\s*)?\{.*?\};", source, re.S)[0]
    return declaration, bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", declaration))


names = ("wbsFind", "wbsFindMask", "wbsReplace", "wbsReplaceMask",
         "hfpFillFind", "hfpFillFindMask", "hfpFillReplace", "hfpFillReplaceMask",
         "extAdvFind", "extAdvReplace",
         "advAddressFind", "advAddressFindMask", "advAddressReplace", "advAddressReplaceMask",
         "advAddressRetryFind", "advAddressRetryFindMask", "advAddressRetryReplace", "advAddressRetryReplaceMask")
rpa_prefixes = ("rpaHelpers", "rpaLegacy", "rpaScan", "rpaDecode", "rpaDevice", "rpaConnect", "rpaUpdate")
names += tuple(prefix + kind + "25G332" for prefix in rpa_prefixes for kind in ("Find", "Replace"))
arrays = {name: array(name)[1] for name in names}
wbs, wbs_patch = arrays["wbsFind"], arrays["wbsReplace"]
fill, fill_patch = arrays["hfpFillFind"], arrays["hfpFillReplace"]
assert len(wbs) == len(wbs_patch) == 76
assert [(i, a, b) for i, (a, b) in enumerate(zip(wbs, wbs_patch)) if a != b] == [(17, 255, 0), (19, 206, 207)]
assert len(fill) == len(fill_patch) == 31
assert fill[:4] == bytes.fromhex("480fafd0") and fill_patch[:4] == bytes.fromhex("0f1f4000")
assert fill[4:] == fill_patch[4:]
# Historical exact 25G241 signatures (the canonical arrays are 25G332's):
# the masked sites must turn them into their historical replacements.
history = {
    "advAddress241": ("e8d430e8ff83b83403000003772a31ffe83e6bceff", "e8d430e8ff83b83403000003772a90909090909090"),
    "advAddressRetry241": (
        "448b043a443384080e0d00008b543a03339408110d00004409c20f8480000000"
        "803dcc2a9200007477488d3c084881c70e0d0000488d157b000000e8eeceffff",
        "448b043a443384080e0d00008b543a03339408110d0000f684080e0d00000175"
        "084409c2747a909090488d3c084881c70e0d0000488d157b000000e8eeceffff"),
    "hfpFill241": ("480fafd04c89f6e83b2ffdff4885c00f848300000049" "89c4488983d0000000",
                   "0f1f40004c89f6e83b2ffdff4885c00f848300000049" "89c4488983d0000000"),
}
history_code = "".join(
    f"const uint8_t {name}{kind}[] {{{','.join(str(b) for b in bytes.fromhex(data))}}};\n"
    for name, pair in history.items() for kind, data in zip(("Find", "Replace"), pair))
assert [(i, a, b) for i, (a, b) in enumerate(zip(bytes.fromhex(history["hfpFill241"][0]), fill)) if a != b] == \
    [(8, 0x3b, 0x33)]
assert "-btlfxmtkwbs" not in source and "-smbtmsbc" not in source
assert 'number("BluetoothdWbsPatched", (uint32_t)bluetoothdWbsPatched);' in source
assert 'number("BluetoothdHfpFillPatched", (uint32_t)bluetoothdHfpFillPatched);' in source
for prefix in rpa_prefixes:
    suffix = prefix[3:]
    assert f'number("BluetoothdRpa{suffix}Patched", (uint32_t)bluetoothdRpa{suffix}Patched);' in source

mock = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>
using mach_vm_address_t = uintptr_t;
using vnode_t = const char *;
using memory_object_t = void *;
using memory_object_offset_t = uint64_t;
using errno_t = int;
using vfs_context_t = void *;
typedef void IOLock;
typedef void IOSimpleLock;
using thread_call_param_t = void *;
using thread_call_t = void *;
constexpr int KERN_SUCCESS = 0;
using SInt32 = int32_t;
constexpr size_t PAGE_SIZE = 4096;
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#define FunctionCast(fn, original) reinterpret_cast<decltype(&fn)>(original)
#define LIKELY(value) (value)
#define UNLIKELY(value) (value)
#define SYSLOG(...) do {} while (0)
namespace KernelVersion { enum { Monterey = 21, Tahoe = 25 }; }
int kernel;
bool flag, noExtAdv, noVerifyFix, msbcReady, extAdvPatchEnabled, extAdvPatchRouted, msbcPatchEnabled;
bool noRpa, rpaPatchEnabled, rpaHelpersReady, leReadyMaskEnabled, noLeReadyMask, anyBuild;
bool anyBuildPatches;
enum BluetoothdBuild : int32_t { BuildUnread = 0, BuildOther = 1, Build25G241 = 2, Build25G332 = 3, Build25G342 = 4, BuildLater = 5 };
int32_t bluetoothdBuild;
char bluetoothdBuildName[16];
int32_t bluetoothdSitesAmbiguous, bluetoothdPagesUnpatched, msbcDeferredBeforeProbe;
uint8_t featureLatch[3];
bool msbcSenderNative;
const char *osBuild = "25G332";
int osversionReads;
int sysctlbyname(const char *name, void *out, size_t *size, void *, size_t) {
    assert(!strcmp(name, "kern.osversion") && *size >= strlen(osBuild) && *size < 16); ++osversionReads;
    memcpy(out, osBuild, strlen(osBuild)); *size = strlen(osBuild); return 0;
}
MsbcControllerScope msbcControllerScope;
int32_t bluetoothdPages, bluetoothdPatched, bluetoothdAddressPatched, bluetoothdAddressRetryPatched;
int32_t bluetoothdWbsPatched, bluetoothdHfpFillPatched;
int32_t bluetoothdRpaHelpersPatched, bluetoothdRpaLegacyPatched, bluetoothdRpaScanPatched;
int32_t bluetoothdRpaDecodePatched, bluetoothdRpaDevicePatched, bluetoothdRpaConnectPatched, bluetoothdRpaUpdatePatched;
uint32_t rustFailed;
uint32_t patchRangeCount;
int32_t patchRangeOverflow, verifyRestored, verifyRestoreFailed, verifyFailed, verifyOpState;
uint64_t verifyLastFailOffset;
uint32_t vnodeOpsOffset;
bool verifyFixEnabled, verifyFixRouted;
// thread_call_enter is safe from any thread.
std::atomic<int> publishes;
void publishLater() { ++publishes; }
thread_call_t publishCall;
thread_call_t thread_call_allocate(void (*)(thread_call_param_t, thread_call_param_t), thread_call_param_t) {
    static int call; return &call;
}
// Match Lilu's contract retained by 87caf95: a failed enable has already
// restored writing/lock state, and must not receive a second disable/unlock.
bool writingFails; int writing, writingRounds, writingDisables;
struct MachInfo {
    static int setKernelWriting(bool enable, IOSimpleLock *) {
        if (enable) {
            assert(!writing); ++writingRounds;
            if (writingFails) return 1;
            writing = 1; return KERN_SUCCESS;
        }
        assert(writing == 1); writing = 0; ++writingDisables; return KERN_SUCCESS;
    }
};
void *leLock;
int getKernelVersion() { return kernel; }
bool checkKernelArgument(const char *name) {
    if (!strcmp(name, "-mtkbtnomsbc")) return !flag;
    if (!strcmp(name, "-mtkbtnoverifyfix")) return noVerifyFix;
    if (!strcmp(name, "-mtkbtnorpa")) return noRpa;
    if (!strcmp(name, "-mtkbtnolereadymask")) return noLeReadyMask;
    if (!strcmp(name, "-mtkbtanybuild")) return anyBuild;
    assert(!strcmp(name, "-mtkbtnoextadv")); return noExtAdv;
}
uintptr_t lockAllocs;
void *IOLockAlloc() { return reinterpret_cast<void *>(++lockAllocs); }
extern IOLock *verifyCopyLock;
thread_local int lockDepth;
std::mutex pageMutex, raceMutex;
std::condition_variable raceCondition;
thread_local bool watchLockAttempt;
bool lockAttempted, operationFinished, operationPaused, releaseOperation;
void IOLockLock(IOLock *lock) {
    assert(lock && lock != verifyCopyLock);
    if (watchLockAttempt) {
        std::lock_guard<std::mutex> guard(raceMutex);
        lockAttempted = true; raceCondition.notify_all();
    }
    assert(!lockDepth); pageMutex.lock(); lockDepth = 1;
}
// verifyCopyLock: only ever tried under pageLock, released after the hash.
std::mutex copyMutex; int copyTries, copyBusy, copyHeld;
void IOLockUnlock(IOLock *lock) {
    if (lock && lock == verifyCopyLock) { assert(copyHeld-- == 1); copyMutex.unlock(); return; }
    assert(lockDepth-- == 1); pageMutex.unlock();
}
bool IOLockTryLock(IOLock *lock) {
    assert(lock && lock == verifyCopyLock && lockDepth == 1); ++copyTries;
    if (copyMutex.try_lock()) { ++copyHeld; return true; }
    ++copyBusy; return false;
}
void (*beforeHash)();
void pauseOperation() {
    std::unique_lock<std::mutex> guard(raceMutex);
    operationPaused = true; raceCondition.notify_all();
    raceCondition.wait(guard, [] { return releaseOperation; });
}
void *IOMalloc(size_t size) { return malloc(size); }
void IOFree(void *p, size_t) { free(p); }
void OSIncrementAtomic(volatile int32_t *value) { ++*value; }

void publishCounters() {}
void publishDeferred(thread_call_param_t, thread_call_param_t) {}
int pathError, validations, searches;
// The real Rust matcher, wrapped: count searches and pause after a write.
#include "mtk_bt.h"
struct image_params { uint64_t untouched = 0x123456789abcdef0ULL; };
int macResult, macCalls, vnodeRefs, fileReads;
bool fileReadFails, execRouteFails;
uint32_t vnodeVersion = 7;
void (*duringRead)();
std::vector<uint8_t> rpaFixture;
int originalMacExec(vfs_context_t, vnode_t, image_params *params) {
    ++macCalls; assert(params && params->untouched == 0x123456789abcdef0ULL); return macResult;
}
int vnode_ref(vnode_t) { ++vnodeRefs; return 0; }
uint32_t vnode_vid(vnode_t) { return vnodeVersion; }
namespace FileIO {
size_t readFileSize(vnode_t, vfs_context_t) { assert(lockDepth == 0); return rpaFixture.size(); }
}
#ifndef __APPLE__
using user_addr_t = uintptr_t;
#endif
struct uio { void *buffer = nullptr; size_t size = 0, remaining = 0; };
using uio_t = uio *;
constexpr int UIO_SYSSPACE = 2, UIO_READ = 0, IO_NODELOCKED = 8;
int uios;
uio_t uio_create(int count, off_t offset, int segment, int rw) {
    assert(count == 1 && offset == 0 && segment == UIO_SYSSPACE && rw == UIO_READ); ++uios; return new uio;
}
int uio_addiov(uio_t io, user_addr_t address, size_t size) {
    io->buffer = reinterpret_cast<void *>(address); io->size = io->remaining = size; return 0;
}
int VNOP_READ(vnode_t, uio_t io, int flags, vfs_context_t) {
    assert(lockDepth == 0 && flags == IO_NODELOCKED && io->size == rpaFixture.size()); ++fileReads;
    if (duringRead) duringRead();
    if (fileReadFails) return 5;
    memcpy(io->buffer, rpaFixture.data(), io->size); io->remaining = 0; return 0;
}
size_t uio_resid(uio_t io) { return io->remaining; }
void uio_free(uio_t io) { --uios; delete io; }
int pageFinds;
int32_t harnessPageFind(const uint8_t *page, size_t len, const mtk_bt_page_site *site) {
    assert(validations && len == PAGE_SIZE && lockDepth == 1); ++searches; ++pageFinds;
    return mtk_bt_page_find(page, len, site);
}
void (*afterReplacement)();
bool harnessPageApply(uint8_t *page, size_t len, size_t offset, const mtk_bt_page_site *site) {
    assert(lockDepth == 1);
    const bool ok = mtk_bt_page_apply(page, len, offset, site);
    if (ok && afterReplacement) afterReplacement();
    return ok;
}
#define mtk_bt_page_find harnessPageFind
#define mtk_bt_page_apply harnessPageApply
int vn_getpath(vnode_t path, char *out, int *length) {
    if (pathError) return pathError;
    assert(strlen(path) + 1 <= size_t(*length)); strcpy(out, path); return 0;
}
// isBluetoothd's cached-name pre-check: basename, or NULL when the name cache misses.
int nameMissing, namesHeld;
const char *vnode_getname(vnode_t path) {
    if (nameMissing) return nullptr;
    ++namesHeld; const char *slash = strrchr(path, '/'); return slash ? slash + 1 : path;
}
void vnode_putname(const char *) { assert(namesHeld > 0); --namesHeld; }
// Another cs_validate_page patcher wrapped by ours (BlueToolFixup's 15.4 chipset patch).
const uint8_t foreignFind[8] {0x81, 0xf9, 0x6f, 0x17, 0x00, 0x00, 0x77, 0x30};
const uint8_t foreignReplace[8] {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
void originalValidate(vnode_t, memory_object_t, memory_object_offset_t, const void *data, int *, int *, int *) {
    ++validations;
    if (!data) return;
    auto *bytes = static_cast<uint8_t *>(const_cast<void *>(data));
    auto *hit = std::search(bytes, bytes + PAGE_SIZE, foreignFind, foreignFind + sizeof(foreignFind));
    if (hit != bytes + PAGE_SIZE) memcpy(hit, foreignReplace, sizeof(foreignReplace));
}
const uint8_t *pristine; // the file as stored; the verify mock hashes against it
int verifies;
mach_vm_address_t orgCsValidatePage;
// _VNOP_VERIFY's code, decoded for offsetof(struct vnode, v_op), and _vnop_verify_desc.
struct FakeDesc { int vdesc_offset; } fakeDesc;
uint8_t fakeVerifyCode[192];
bool verifySymbolsMissing;
struct KernelPatcher {
    static constexpr int KernelID = 0;
    struct RouteRequest {
        const char *symbol;
        mach_vm_address_t *original;
        template<class Fn> RouteRequest(const char *s, Fn, mach_vm_address_t &o) : symbol(s), original(&o) {}
    };
    static IOSimpleLock *kernelWriteLock;
    bool routeFails = false;
    bool routeMultipleLong(int id, RouteRequest *routes, size_t count) {
        assert(id == KernelID && count == 1);
        if (!strcmp(routes[0].symbol, "_mac_vnode_check_exec")) {
            if (execRouteFails || routeFails) return false;
            *routes[0].original = reinterpret_cast<mach_vm_address_t>(originalMacExec); return true;
        }
        assert(!strcmp(routes[0].symbol, "_cs_validate_page"));
        if (routeFails) return false;
        *routes[0].original = reinterpret_cast<mach_vm_address_t>(originalValidate); return true;
    }
    mach_vm_address_t solveSymbol(int id, const char *symbol) {
        assert(id == KernelID);
        if (verifySymbolsMissing) return 0;
        if (!strcmp(symbol, "_vnop_verify_desc")) return reinterpret_cast<mach_vm_address_t>(&fakeDesc);
        assert(!strcmp(symbol, "_VNOP_VERIFY")); return reinterpret_cast<mach_vm_address_t>(fakeVerifyCode);
    }
    void clearError() {}
    static bool findAndReplace(void *data, size_t size, const void *find, size_t findSize,
                               const void *replace, size_t replaceSize) {
        assert(validations && findSize == replaceSize && size == PAGE_SIZE && lockDepth == verifyFixRouted); ++searches;
        auto *bytes = static_cast<uint8_t *>(data);
        const auto *needle = static_cast<const uint8_t *>(find);
        auto *hit = std::search(bytes, bytes + size, needle, needle + findSize);
        if (hit == bytes + size) return false;
        memcpy(hit, replace, replaceSize);
        if (afterReplacement) afterReplacement();
        return true;
    }
};
IOSimpleLock *KernelPatcher::kernelWriteLock;
int kextIOBluetoothFamily, kextIOUSBHostFamily;
void processKext(void *, KernelPatcher &, size_t, mach_vm_address_t, size_t) {}
struct Lilu {
    int pageRegistrations = 0;
    KernelPatcher patcher;
    void onKextLoadForce(int *, int, decltype(&processKext)) {}
    void onPatcherLoadForce(void (*callback)(void *, KernelPatcher &)) {
        ++pageRegistrations; callback(nullptr, patcher);
    }
} lilu;
namespace Patcher { void init(uint32_t); void observeMsbcController(bool); }
'''

# After VnopVerifyArgs/VnodeOp: the APFS operations vector of the bluetoothd vnode.
vop_mock = r'''
constexpr int kVerifyIndex = 37;
VnodeOp fakeOps[64];
void *fakeFields[3];
int fakeMount, fakeData, fakeContext;
bool badFields;
void *const *vnodeOpsFields(vnode_t) { assert(vnodeOpsOffset); return fakeFields; }
void *vnode_mount(vnode_t) { return &fakeMount; }
void *vnode_fsnode(vnode_t) { return badFields ? nullptr : &fakeData; }
size_t verifyBlockSize = 16384;
int apfsVerify(void *arg) {
    auto *ap = static_cast<VnopVerifyArgs *>(arg); ++verifies;
    assert(ap->a_desc == &fakeDesc && ap->a_context == &fakeContext && ap->a_verifyblksize == &verifyBlockSize);
    if (beforeHash) beforeHash();
    return ap->a_buf && memcmp(ap->a_buf, pristine + ap->a_foffset, ap->a_bufsize) ? 94 : 0;
}
int defaultOp(void *) { return 45; }
// VNOP_VERIFY as decmpfs inlines it: vp->v_op[vnop_verify_desc.vdesc_offset](&args).
int verifyCall(vnode_t vp, off_t offset, uint8_t *buf, size_t size, uint32_t flags = 0) {
    VnopVerifyArgs args {&fakeDesc, vp, offset, buf, size, &verifyBlockSize, nullptr, flags, &fakeContext, nullptr};
    return fakeOps[fakeDesc.vdesc_offset](&args);
}
// 25G332: 49 8b 8f d0 00 00 00 (movq 0xd0(%r15),%rcx), 48 63 15 rel32 (movslq _vnop_verify_desc(%rip),%rdx).
void writeVerifyCode(size_t at, bool disp8 = false, int64_t skew = 0) {
    memset(fakeVerifyCode, 0xcc, sizeof(fakeVerifyCode));
    uint8_t *p = fakeVerifyCode + at;
    const uint8_t load32[] {0x49, 0x8b, 0x8f, 0xd0, 0x00, 0x00, 0x00}, load8[] {0x48, 0x8b, 0x4f, 0x68};
    if (disp8) { memcpy(p, load8, sizeof(load8)); p += sizeof(load8); }
    else { memcpy(p, load32, sizeof(load32)); p += sizeof(load32); }
    const uint8_t movslq[] {0x48, 0x63, 0x15}; memcpy(p, movslq, sizeof(movslq));
    const int32_t rel = int32_t(reinterpret_cast<intptr_t>(&fakeDesc) + skew - reinterpret_cast<intptr_t>(p + 7));
    memcpy(p + 3, &rel, sizeof(rel));
}
'''

tests = r'''
using Page = std::array<uint8_t, PAGE_SIZE>;
void apply(Page &page, vnode_t path = "/usr/sbin/bluetoothd") {
    csValidatePage(path, nullptr, 0, page.data(), nullptr, nullptr, nullptr);
}
Page originals() {
    Page page {};
    memcpy(page.data() + 10, wbsFind, sizeof(wbsFind));
    memcpy(page.data() + 110, hfpFillFind, sizeof(hfpFillFind));
    memcpy(page.data() + 210, extAdvFind, sizeof(extAdvFind));
    memcpy(page.data() + 310, advAddressFind, sizeof(advAddressFind));
    memcpy(page.data() + 410, advAddressRetryFind, sizeof(advAddressRetryFind));
    return page;
}
void reset(int version, bool enabled, bool disableAdv, bool disableVerify = false) {
    kernel = version; flag = enabled; noExtAdv = disableAdv; noVerifyFix = disableVerify;
    verifyFixEnabled = verifyFixRouted = false; patchRangeCount = 0; bluetoothdVnode = nullptr;
    patchRangeOverflow = verifyRestored = verifyRestoreFailed = verifies = 0;
    free(pageSnapshot); pageSnapshot = nullptr; pageLock = nullptr;
    vnopVerifyDesc = 0; vnodeOpsOffset = 0; verifyOpState = verifyFailed = 0; verifyLastFailOffset = 0;
    orgVerifyOp = nullptr; publishCall = nullptr; publishes = 0; verifySymbolsMissing = false;
    writingFails = false; writingRounds = writingDisables = 0; badFields = false;
    fakeDesc.vdesc_offset = kVerifyIndex;
    for (auto &op : fakeOps) op = nullptr;
    fakeOps[0] = defaultOp; fakeOps[kVerifyIndex] = apfsVerify;
    fakeFields[0] = fakeOps; fakeFields[1] = &fakeMount; fakeFields[2] = &fakeData;
    writeVerifyCode(0x70);
    msbcReady = extAdvPatchEnabled = extAdvPatchRouted = msbcPatchEnabled = false;
    noRpa = rpaPatchEnabled = rpaHelpersReady = false; bluetoothdBuild = BuildUnread; bluetoothdBuildName[0] = 0;
    free(rpaPlan); rpaPlan = nullptr; bluetoothdImageVnode = nullptr; bluetoothdImageVid = 0;
    rpaExecRouted = rpaValidationStarted = false; rpaPlanState = RpaPlanUnread; rpaPlanRejected = 0;
    bluetoothdCommonAbi = 0; bluetoothdForeignPagesRolledBack = 0;
    macResult = macCalls = vnodeRefs = fileReads = 0; fileReadFails = execRouteFails = false;
    duringRead = nullptr; vnodeVersion = 7;
    anyBuild = anyBuildPatches = false; bluetoothdSitesAmbiguous = bluetoothdPagesUnpatched = 0;
    memset(featureLatch, 0, sizeof(featureLatch)); msbcSenderNative = false; msbcDeferredBeforeProbe = 0;
    free(verifyCopy); verifyCopy = nullptr; verifyCopyLock = nullptr;
    copyTries = copyBusy = 0; pageFinds = 0;
    noLeReadyMask = leReadyMaskEnabled = false;
    bluetoothdRpaHelpersPatched = bluetoothdRpaLegacyPatched = bluetoothdRpaScanPatched = 0;
    bluetoothdRpaDecodePatched = bluetoothdRpaDevicePatched = bluetoothdRpaConnectPatched = bluetoothdRpaUpdatePatched = 0;
    msbcControllerScope = {};
    bluetoothdPages = bluetoothdPatched = bluetoothdAddressPatched = bluetoothdAddressRetryPatched = 0;
    bluetoothdWbsPatched = bluetoothdHfpFillPatched = 0;
    pathError = validations = searches = 0;
    orgCsValidatePage = reinterpret_cast<mach_vm_address_t>(originalValidate);
    lilu.pageRegistrations = 0; lilu.patcher.routeFails = false;
}
// A native page suffices to exercise verifier failure and unsupported scope.
Page helperPage() { return {}; }
bool prepareRpa(vnode_t path = "/usr/sbin/bluetoothd", bool isolate = true) {
    image_params params;
    if (!rpaExecRouted) return false;
    assert(macVnodeCheckExec(&fakeContext, path, &params) == macResult);
    assert(params.untouched == 0x123456789abcdef0ULL && uios == 0);
    if (isolate && rpaPlanState == RpaPlanReady) {
        // Isolate the RPA ledger tests; advertising/mSBC are tested separately.
        extAdvPatchEnabled = msbcPatchEnabled = false;
    }
    return rpaPlanState == RpaPlanReady;
}
Page plannedPage(size_t edit) {
    Page page {};
    const size_t offset = rpaPlan->edits[edit].file_offset & ~(PAGE_SIZE-1);
    assert(offset + PAGE_SIZE <= rpaFixture.size());
    memcpy(page.data(), rpaFixture.data() + offset, PAGE_SIZE);
    return page;
}
void recursiveReadValidation() {
    Page page {}; apply(page);
}
void rpaTests() {
    const char *btd = "/usr/sbin/bluetoothd";
    for (const char *build : {"25G241", "25G332", "25G342", "25H100"}) {
        osBuild = build; reset(25, true, false); Patcher::init(0);
        const bool enabled = strcmp(build, "25G241") != 0;
        assert(prepareRpa() == enabled);
        assert(rpaPlanState == (enabled ? RpaPlanReady : RpaPlanDisabled));
        assert(fileReads == 1 && vnodeRefs == 1 && bluetoothdCommonAbi == 3);
    }
    osBuild = "25G332";
    for (int version : {20,21,24,25,26}) for (bool disableAdv : {false,true})
    for (bool disableRpa : {false,true}) for (bool disableVerify : {false,true}) {
        reset(version, true, disableAdv, disableVerify); noRpa = disableRpa; Patcher::init(0);
        const bool enabled = version == 25 && !disableAdv && !disableRpa && !disableVerify;
        assert(prepareRpa() == enabled);
        assert(!rpaHelpersReady); // Readiness is the plan, not a prefaulted helper.
        if (enabled) assert(rpaPlan->count == MTK_BT_RPA_EDITS && verifyFixRouted && patchRangeCount);
    }
    // MAC rejection, wrong vnode, missing route/read/verify, and early/recursive
    // executable validation cannot accidentally activate a partial plan.
    reset(25,true,false); Patcher::init(0); macResult = 13;
    assert(!prepareRpa() && rpaPlanState == RpaPlanUnread && !fileReads);
    reset(25,true,false); Patcher::init(0);
    assert(!prepareRpa("/tmp/bluetoothd") && rpaPlanState == RpaPlanUnread && !fileReads);
    reset(25,true,false); execRouteFails = true; Patcher::init(0);
    assert(!rpaExecRouted && !prepareRpa() && !fileReads);
    reset(25,true,false); fileReadFails = true; Patcher::init(0);
    assert(!prepareRpa() && rpaPlanState == RpaPlanReadFailed && !patchRangeCount);
    reset(25,true,false); verifySymbolsMissing = true; Patcher::init(0);
    assert(!prepareRpa() && rpaPlanState == RpaPlanNoVerify && !patchRangeCount);
    reset(25,true,false); Patcher::init(0); Page untouched {}; apply(untouched);
    assert(!prepareRpa() && rpaPlanState == RpaPlanLate && !fileReads);
    reset(25,true,false); duringRead = recursiveReadValidation; Patcher::init(0);
    assert(!prepareRpa() && rpaPlanState == RpaPlanLate && !patchRangeCount);
    reset(25,true,false); Patcher::init(0); patchRangeCount = kPatchRanges;
    assert(!prepareRpa() && rpaPlanState == RpaPlanNoRoom);

    // All destinations are bound to universal-file offsets. Children may run
    // before their helper page faults; no callback pointer is retained.
    reset(25,true,false); Patcher::init(0); assert(prepareRpa());
    const uint32_t reserved = patchRangeCount;
    auto file = rpaFixture;
    file.resize((file.size()+16383)&~size_t(16383));
    auto cache = file, expected = file;
    pristine = file.data();
    std::vector<uint64_t> pages;
    for (size_t i = 1; i < MTK_BT_RPA_EDITS; ++i) {
        const uint64_t off = rpaPlan->edits[i].file_offset & ~(uint64_t(PAGE_SIZE)-1);
        if (std::find(pages.begin(),pages.end(),off) == pages.end()) pages.push_back(off);
    }
    const uint64_t helper = rpaPlan->edits[0].file_offset & ~(uint64_t(PAGE_SIZE)-1);
    for (const auto &edit : rpaPlan->edits)
        memcpy(expected.data()+edit.file_offset,edit.replacement,edit.len);
    for (uint64_t off : pages) {
        csValidatePage(btd,nullptr,off,cache.data()+off,nullptr,nullptr,nullptr);
        assert(!rpaHelpersReady);
    }
    csValidatePage(btd,nullptr,helper,cache.data()+helper,nullptr,nullptr,nullptr);
    assert(rpaHelpersReady && cache == expected && patchRangeCount == reserved);
    assert(bluetoothdRpaHelpersPatched == 1 && bluetoothdRpaLegacyPatched == 1 && bluetoothdRpaScanPatched == 1 &&
           bluetoothdRpaDecodePatched == 1 && bluetoothdRpaDevicePatched == 1 && bluetoothdRpaConnectPatched == 1 && bluetoothdRpaUpdatePatched == 1);
    assert(prepareRpa() && fileReads == 1 && vnodeRefs == 1); // immutable across re-exec
    pages.push_back(helper);
    for (uint64_t off : pages) {
        csValidatePage(btd,nullptr,off,cache.data()+off,nullptr,nullptr,nullptr);
        Page fresh {}; memcpy(fresh.data(),file.data()+off,PAGE_SIZE);
        csValidatePage(btd,nullptr,off,fresh.data(),nullptr,nullptr,nullptr);
        assert(!memcmp(fresh.data(),expected.data()+off,PAGE_SIZE));
    }
    assert(patchRangeCount == reserved && !patchRangeOverflow);
    const uint64_t block = helper & ~uint64_t(16383);
    assert(verifyCall(btd,block,cache.data()+block,16384) == 0 && verifyRestored && !verifyRestoreFailed);
    // After activation, unrelated ledger exhaustion cannot skip the helper.
    for (uint32_t i = reserved; i < kPatchRanges; ++i) patchRanges[i] = {};
    patchRangeCount = kPatchRanges;
    Page fresh = plannedPage(0); csValidatePage(btd,nullptr,helper,fresh.data(),nullptr,nullptr,nullptr);
    assert(!memcmp(fresh.data(),expected.data()+helper,PAGE_SIZE));
    // A changed destination rejects executable validation, with no partial
    // write to any other destination on the same page.
    reset(25,true,false); Patcher::init(0); assert(prepareRpa());
    Page bad = plannedPage(1); const auto &edit = rpaPlan->edits[1];
    bad[edit.file_offset % PAGE_SIZE] ^= 1; const Page before = bad;
    int validated = 1, tainted = 0, nx = 0;
    csValidatePage(btd,nullptr,edit.file_offset & ~(uint64_t(PAGE_SIZE)-1),bad.data(),&validated,&tainted,&nx);
    assert(bad == before && !validated && tainted && nx && rpaPlanRejected == 1);
    static char different[] = "/usr/sbin/bluetoothd";
    Page other = plannedPage(0), original = other;
    csValidatePage(different,nullptr,rpaPlan->edits[0].file_offset & ~(uint64_t(PAGE_SIZE)-1),other.data(),nullptr,nullptr,nullptr);
    assert(other == original && !rpaHelpersReady);
    // Downstream BlueToolFixup writes on a different vnode must be rolled
    // back, not left resident outside the referenced image's restore ledger.
    Page foreign {}; memcpy(foreign.data()+800,foreignFind,sizeof(foreignFind));
    const Page native = foreign; const uint32_t ranges = patchRangeCount;
    std::vector<uint8_t> nativeBlock(16384,0);
    memcpy(nativeBlock.data(),native.data(),PAGE_SIZE); pristine = nativeBlock.data();
    for (int i = 0; i < 2; ++i)
        csValidatePage(different,nullptr,0,foreign.data(),nullptr,nullptr,nullptr);
    assert(foreign == native && patchRangeCount == ranges && bluetoothdForeignPagesRolledBack == 2);
    memcpy(nativeBlock.data(),foreign.data(),PAGE_SIZE);
    assert(verifyCall(different,0,nativeBlock.data(),nativeBlock.size()) == 0);
    // Common features on an unlisted layout are validated even with RPA off.
    osBuild = "25H100";
    for (bool disableAdv : {false,true}) {
        reset(25,true,disableAdv); noRpa = true; Patcher::init(0);
        assert(!prepareRpa(btd,false) && rpaPlanState == RpaPlanDisabled && bluetoothdCommonAbi == 3);
        msbcReady = true; Patcher::observeMsbcController(true);
        Page page = originals(), expected = page;
        memcpy(expected.data()+10,wbsReplace,sizeof(wbsReplace));
        memcpy(expected.data()+110,hfpFillReplace,sizeof(hfpFillReplace));
        if (!disableAdv) {
            memcpy(expected.data()+210,extAdvReplace,sizeof(extAdvReplace));
            memcpy(expected.data()+310,advAddressReplace,sizeof(advAddressReplace));
            memcpy(expected.data()+410,advAddressRetryReplace,sizeof(advAddressRetryReplace));
        }
        apply(page); assert(page == expected && bluetoothdWbsPatched == 1 && bluetoothdHfpFillPatched == 1);
        assert(bluetoothdPatched == !disableAdv && !rpaHelpersReady);
    }
    // A missing RPA signature does not veto a valid common ABI.
    const auto goodFixture = rpaFixture;
    rpaFixture[0x4000+0x2100] ^= 1;
    reset(25,true,false); Patcher::init(0);
    assert(!prepareRpa(btd,false) && rpaPlanState == RpaPlanInvalid && bluetoothdCommonAbi == 3);
    Page page = originals(); apply(page); assert(bluetoothdPatched == 1 && bluetoothdAddressPatched == 1);
    rpaFixture = goodFixture;
    // A missing advertising signature cannot veto HFP-only validation.
    rpaFixture[0x4000+0x5300] ^= 1;
    reset(25,true,true); Patcher::init(0);
    assert(!prepareRpa(btd,false) && rpaPlanState == RpaPlanDisabled && bluetoothdCommonAbi == MTK_BT_COMMON_MSBC);
    msbcReady = true; Patcher::observeMsbcController(true); page = originals(); apply(page);
    assert(bluetoothdWbsPatched == 1 && bluetoothdHfpFillPatched == 1 && !bluetoothdPatched);
    rpaFixture = goodFixture;
    // The opposite direction is independent too: failed HFP ABI leaves valid
    // advertising enabled, including with the explicit RPA opt-out.
    rpaFixture[0x4000+0x5500] ^= 1;
    reset(25,true,false); noRpa = true; Patcher::init(0);
    assert(!prepareRpa(btd,false) && bluetoothdCommonAbi == MTK_BT_COMMON_ADVERTISING);
    msbcReady = true; Patcher::observeMsbcController(true); page = originals(); apply(page);
    assert(bluetoothdPatched == 1 && bluetoothdAddressPatched == 1 && !bluetoothdWbsPatched && !bluetoothdHfpFillPatched);
    rpaFixture = goodFixture; osBuild = "25G332";
    puts("PASS: dynamic pre-exec RPA, independent advertising/HFP validation, vnode rollback, lazy helper and reserved restoration");
}
// Build scopes: masked sites on 25G241, unlisted builds and -mtkbtanybuild,
// ambiguous matches, and the per-vnode feature decision.
void buildTests() {
    osBuild = "25G241";
    reset(25, true, false); Patcher::init(0);
    Page page {};
    memcpy(page.data() + 310, advAddress241Find, sizeof(advAddress241Find));
    memcpy(page.data() + 410, advAddressRetry241Find, sizeof(advAddressRetry241Find));
    apply(page);
    assert(!memcmp(page.data() + 310, advAddress241Replace, sizeof(advAddress241Replace)));
    assert(!memcmp(page.data() + 410, advAddressRetry241Replace, sizeof(advAddressRetry241Replace)));
    assert(bluetoothdAddressPatched == 1 && bluetoothdAddressRetryPatched == 1 && bluetoothdBuild == Build25G241);
    // Unlisted build: only ScopeAnyBuild sites, unless -mtkbtanybuild; never RPA.
    for (bool any : {false, true}) {
        osBuild = "25G240";
        reset(25, true, false); anyBuild = any; Patcher::init(0); msbcReady = true;
        Patcher::observeMsbcController(true);
        assert(anyBuildPatches == any);
        assert(!bluetoothdAddressRetryPatched);  // known-build scope, like advAddress
        Page p = originals(), expected = p;
        if (any) {
            memcpy(expected.data() + 210, extAdvReplace, sizeof(extAdvReplace));
            memcpy(expected.data() + 10, wbsReplace, sizeof(wbsReplace));
            memcpy(expected.data() + 110, hfpFillReplace, sizeof(hfpFillReplace));
            memcpy(expected.data() + 310, advAddressReplace, sizeof(advAddressReplace));
            memcpy(expected.data() + 410, advAddressRetryReplace, sizeof(advAddressRetryReplace));
        }
        apply(p);
        assert(p == expected && bluetoothdBuild == BuildOther && !strcmp(bluetoothdBuildName, "25G240"));
        assert(bluetoothdPatched == any && bluetoothdAddressRetryPatched == any);
        // Retry without advAddress stalls the set start: its guard calls a
        // helper that returns 0 without sending HCI or calling the callback
        // when the gate is false (verified on 25G332). ScopeKnownBuild, like advAddress.
        assert(bluetoothdAddressPatched == any && bluetoothdWbsPatched == any && bluetoothdHfpFillPatched == any);
        Page helpers = helperPage(); const Page original = helpers; apply(helpers);
        assert(helpers == original && !rpaHelpersReady && !bluetoothdRpaHelpersPatched);
    }
    for (const char *build : {"25G342", "25H100"}) {
        osBuild = build; reset(25,true,false); Patcher::init(0); msbcReady = true;
        if (!strcmp(build,"25H100")) assert(prepareRpa("/usr/sbin/bluetoothd",false));
        Patcher::observeMsbcController(true);
        Page p = originals(), expected = p;
        memcpy(expected.data()+10,wbsReplace,sizeof(wbsReplace));
        memcpy(expected.data()+110,hfpFillReplace,sizeof(hfpFillReplace));
        memcpy(expected.data()+210,extAdvReplace,sizeof(extAdvReplace));
        memcpy(expected.data()+310,advAddressReplace,sizeof(advAddressReplace));
        memcpy(expected.data()+410,advAddressRetryReplace,sizeof(advAddressRetryReplace));
        apply(p); assert(p == expected && bluetoothdAddressPatched == 1 && bluetoothdAddressRetryPatched == 1);
        assert(!anyBuildPatches && !rpaHelpersReady); // The helper itself stays lazy.
    }
    osBuild = "25H100"; reset(25,true,false); Patcher::init(0);
    Page pending = originals(); apply(pending); assert(pending == originals()); // no partial advertising before preflight
    osBuild = "25G332";
    // Two matches in one page: neither is patched, and the feature is off for this vnode.
    reset(25, true, false); Patcher::init(0);
    page = {};
    memcpy(page.data() + 100, advAddressRetryFind, sizeof(advAddressRetryFind));
    memcpy(page.data() + 1000, advAddressRetryFind, sizeof(advAddressRetryFind));
    Page before = page; apply(page);
    assert(page == before && bluetoothdSitesAmbiguous == 1 && !bluetoothdAddressRetryPatched);
    assert(featureLatch[FeatureAdv] == LatchOff);
    page = originals(); apply(page); assert(page == originals() && !bluetoothdPatched);
    // A new vnode (bluetoothd recycled) decides again.
    static char recycled[] = "/usr/sbin/bluetoothd";
    csValidatePage(recycled, nullptr, 0, page.data(), nullptr, nullptr, nullptr);
    assert(bluetoothdPatched == 1 && featureLatch[FeatureAdv] == LatchOn && bluetoothdVnode == recycled);
    // Unknown USB scope is deferred, not permanently Off for this vnode.
    reset(25, true, true); Patcher::init(0);
    page = {}; memcpy(page.data() + 10, wbsFind, sizeof(wbsFind)); before = page; apply(page);
    assert(page == before && featureLatch[FeatureMsbc] == LatchUndecided && msbcDeferredBeforeProbe);
    msbcReady = true; Patcher::observeMsbcController(true);
    Page fill {}; memcpy(fill.data() + 110, hfpFillFind, sizeof(hfpFillFind)); apply(fill);
    assert(!memcmp(fill.data() + 110, hfpFillReplace, sizeof(hfpFillReplace)));
    assert(bluetoothdHfpFillPatched == 1 && featureLatch[FeatureMsbc] == LatchOn);
    // Decided on: a later controller veto cannot split already patched pages.
    Patcher::observeMsbcController(false);
    apply(page);
    assert(bluetoothdWbsPatched == 1 && !memcmp(page.data() + 10, wbsReplace, sizeof(wbsReplace)));
    puts("PASS: historical 25G241 bytes, 25G241+ masked sites including 25G342, below-minimum override, ambiguous and per-vnode decisions");
}
void msbcOrderingTests() {
    for (bool senderFirst : {false, true}) for (bool bridgeReadyAtPage : {false, true}) {
        reset(25, true, true); Patcher::init(0); msbcReady = bridgeReadyAtPage;
        Page gate {}, sender {};
        memcpy(gate.data() + 10, wbsFind, sizeof(wbsFind));
        memcpy(sender.data() + 110, hfpFillFind, sizeof(hfpFillFind));
        const Page nativeGate = gate, nativeSender = sender;
        apply(senderFirst ? sender : gate);
        assert(gate == nativeGate && sender == nativeSender && msbcDeferredBeforeProbe);
        assert(featureLatch[FeatureMsbc] == LatchUndecided && !bluetoothdWbsPatched && !bluetoothdHfpFillPatched);
        assert(msbcSenderNative == senderFirst);
        Patcher::observeMsbcController(true); msbcReady = true;
        if (senderFirst) {
            // The sender's early native code can be resident. A later WBS
            // page must remain native until the sender is revalidated.
            apply(gate);
            assert(gate == nativeGate && featureLatch[FeatureMsbc] == LatchUndecided);
        }
        apply(sender);
        assert(!memcmp(sender.data() + 110, hfpFillReplace, sizeof(hfpFillReplace)));
        assert(!msbcSenderNative && featureLatch[FeatureMsbc] == LatchOn);
        assert(gate == nativeGate); // no retroactive writes to saved page pointers
        apply(gate);
        assert(!memcmp(gate.data() + 10, wbsReplace, sizeof(wbsReplace)));
        assert(bluetoothdWbsPatched == 1 && bluetoothdHfpFillPatched == 1);
    }
    // Preserve the normal post-probe gate-first/cold-sender ordering: its
    // later code-page fault sees the same On latch, including after a veto.
    reset(25, true, true); Patcher::init(0); msbcReady = true; Patcher::observeMsbcController(true);
    Page gate {}, sender {};
    memcpy(gate.data() + 10, wbsFind, sizeof(wbsFind));
    memcpy(sender.data() + 110, hfpFillFind, sizeof(hfpFillFind));
    apply(gate); assert(bluetoothdWbsPatched == 1 && featureLatch[FeatureMsbc] == LatchOn);
    Patcher::observeMsbcController(false);
    apply(sender); assert(bluetoothdHfpFillPatched == 1 && !msbcSenderNative);
    // Unknown -> unsupported/mixed scope is a real Off decision; a later
    // supported probe cannot undo the USB scope veto.
    for (bool mixed : {false, true}) {
        reset(25, true, true); Patcher::init(0); msbcReady = true;
        Page early = originals(); const Page native = early; apply(early);
        assert(early == native && featureLatch[FeatureMsbc] == LatchUndecided && msbcSenderNative);
        if (mixed) Patcher::observeMsbcController(true);
        Patcher::observeMsbcController(false);
        apply(early);
        assert(early == native && featureLatch[FeatureMsbc] == LatchOff);
        Patcher::observeMsbcController(true); apply(early);
        assert(early == native && !bluetoothdWbsPatched && !bluetoothdHfpFillPatched);
    }
    // A new vnode resets the early native-sender marker. Do not carry an
    // unrelated file's boot-order suppression into its gate-first ordering.
    reset(25, true, true); Patcher::init(0); msbcReady = true;
    sender = {}; memcpy(sender.data() + 110, hfpFillFind, sizeof(hfpFillFind)); apply(sender);
    assert(msbcSenderNative && featureLatch[FeatureMsbc] == LatchUndecided);
    Patcher::observeMsbcController(true);
    static char otherVnode[] = "/usr/sbin/bluetoothd";
    gate = {}; memcpy(gate.data() + 10, wbsFind, sizeof(wbsFind)); ::apply(gate, otherVnode);
    assert(!msbcSenderNative && bluetoothdWbsPatched == 1 && featureLatch[FeatureMsbc] == LatchOn);
    reset(25, true, true, true); Patcher::init(0);
    assert(publishCall && !verifyFixEnabled); // USB diagnostics also work with the verifier disabled.
    puts("PASS: both early page/probe orders defer unknown scope, native cached sender blocks WBS until safe revalidation, normal ordering and unsupported/mixed/vnode rules");
}
// One 16 KB verify block: page 1 holds our and a foreign patch site, page 0 none.
void verifyTests() {
    const char *btd = "/usr/sbin/bluetoothd", *other = "/usr/sbin/BlueTool";
    std::vector<uint8_t> file(PAGE_SIZE * 4, 0x5a);
    memcpy(file.data() + PAGE_SIZE + 210, extAdvFind, sizeof(extAdvFind));
    memcpy(file.data() + PAGE_SIZE + 3000, foreignFind, sizeof(foreignFind));
    pristine = file.data();
    auto verify = [&](vnode_t vp, off_t offset, uint8_t *buf, size_t size, uint32_t flags = 0) {
        return verifyCall(vp, offset, buf, size, flags);
    };

    reset(25, true, false); Patcher::init(0);
    // Installed at bluetoothd's first validated page, not at patcher load.
    assert(!verifyFixRouted && !verifyOpState && extAdvPatchRouted && fakeOps[kVerifyIndex] == apfsVerify);
    std::vector<uint8_t> cache = file; // the file cache: page 1 resident and patched
    csValidatePage(btd, nullptr, PAGE_SIZE, cache.data() + PAGE_SIZE, nullptr, nullptr, nullptr);
    assert(verifyFixRouted && verifyOpState == VerifyOpInstalled && fakeOps[kVerifyIndex] == verifyOp);
    assert(bluetoothdPatched == 1 && patchRangeCount == 2 && bluetoothdVnode == btd && !lockDepth);
    assert(!memcmp(cache.data() + PAGE_SIZE + 3000, foreignReplace, sizeof(foreignReplace)));
    const std::vector<uint8_t> patched = cache;
    // Same page validated again after eviction: no duplicate ranges.
    std::vector<uint8_t> again(file.begin() + PAGE_SIZE, file.begin() + 2 * PAGE_SIZE);
    csValidatePage(btd, nullptr, PAGE_SIZE, again.data(), nullptr, nullptr, nullptr);
    assert(patchRangeCount == 2 && !memcmp(again.data(), patched.data() + PAGE_SIZE, PAGE_SIZE));

    // Page-in of a neighbour hashes the block with the resident patched page.
    assert(verify(btd, 0, cache.data(), cache.size()) == 0 && verifyRestored == 1 && cache == patched);
    assert(verify(btd, PAGE_SIZE, cache.data() + PAGE_SIZE, PAGE_SIZE) == 0 && verifyRestored == 2);
    // Restored copies use the preallocated buffer; a busy one falls back to the heap.
    assert(copyTries == 2 && !copyBusy && !copyHeld);
    copyMutex.lock();
    assert(verify(btd, 0, cache.data(), cache.size()) == 0 && verifyRestored == 3 && copyBusy == 1);
    copyMutex.unlock();
    // verifyOp hands APFS the restored copy through the caller's arguments,
    // then gives the caller its own buffer back.
    {
        VnopVerifyArgs args {&fakeDesc, btd, 0, cache.data(), cache.size(), &verifyBlockSize, nullptr, 0, &fakeContext, nullptr};
        assert(fakeOps[fakeDesc.vdesc_offset](&args) == 0 && args.a_buf == cache.data() && verifyRestored == 4);
    }
    verifyRestored = 2;
    // Untouched page, other vnode, non-default flags, digest-only call: pass through unchanged.
    assert(verify(btd, 0, cache.data(), PAGE_SIZE) == 0 && verifyRestored == 2);
    assert(verify(other, 0, cache.data(), cache.size()) == 94);
    assert(verify(btd, 0, cache.data(), cache.size(), 8) == 94);
    assert(verify(btd, 0, nullptr, 0) == 0 && verifyRestored == 2);
    // Range not wholly in the buffer: no restore.
    assert(verify(btd, PAGE_SIZE + 216, cache.data() + PAGE_SIZE + 216, 100) == 94 && verifyRestored == 2);
    // Restored only while the block holds exactly the patched bytes.
    std::vector<uint8_t> altered = cache; altered[PAGE_SIZE + 3001] ^= 1;
    assert(verify(btd, 0, altered.data(), altered.size()) == 94 && verifyRestored == 2 && verifyRestoreFailed == 1);
    // Freshly read block: nothing patched, nothing restored.
    std::vector<uint8_t> fresh = file;
    assert(verify(btd, 0, fresh.data(), fresh.size()) == 0 && verifyRestored == 2 && fresh == file);

    // Range table full: later (foreign) ranges are counted, not stored, and
    // ours stop kPageRangeHeadroom early: never patched without originals.
    reset(25, true, false); Patcher::init(0);
    for (int round = 0; round < 140; ++round) {
        std::vector<uint8_t> page(PAGE_SIZE, 0);
        memcpy(page.data() + (round % 40) * 100, foreignFind, sizeof(foreignFind));
        if (round >= 100) memcpy(page.data() + 4000 - sizeof(extAdvFind), extAdvFind, sizeof(extAdvFind));
        csValidatePage(btd, nullptr, uint64_t(round) * PAGE_SIZE, page.data(), nullptr, nullptr, nullptr);
    }
    // Rounds 100..106 patch (2 ranges each) while 16 slots remain: 114; the
    // foreign ranges fill the table at round 120, the last 19 overflow.
    assert(patchRangeCount == kPatchRanges && patchRangeOverflow == 19);
    assert(bluetoothdPatched == 7 && bluetoothdPagesUnpatched == 33);

    // Long differences split into ranges of at most kPatchRangeMax bytes.
    reset(25, true, true); Patcher::init(0);
    Page before {}, after {}; for (size_t i = 0; i < 200; ++i) after[100 + i] = 0xcc;
    recordPageDiff(0, before.data(), after.data());
    assert(patchRangeCount == 3 && patchRanges[0].length == 96 && patchRanges[2].length == 8);
    assert(patchRanges[0].fileOffset == 100 && patchRanges[2].fileOffset == 292);

    // Opt-out and route failure fail closed: no page patch without the restore
    // (a wrapped foreign patcher still runs; it is not ours to refuse).
    reset(25, true, false, true); Patcher::init(0);
    assert(!verifyFixRouted && extAdvPatchRouted && !pageSnapshot && !verifyCopy);
    cache = file; csValidatePage(btd, nullptr, PAGE_SIZE, cache.data() + PAGE_SIZE, nullptr, nullptr, nullptr);
    assert(!bluetoothdPatched && patchRangeCount == 0 && !bluetoothdVnode && bluetoothdPagesUnpatched == 1);
    assert(!memcmp(cache.data() + PAGE_SIZE + 210, extAdvFind, sizeof(extAdvFind)) && !pageFinds);
    assert(!verifyOpState && fakeOps[kVerifyIndex] == apfsVerify && !writingRounds);
    reset(25, true, false); verifySymbolsMissing = true; Patcher::init(0);
    assert(!verifyFixRouted && extAdvPatchRouted);
    cache = file; csValidatePage(btd, nullptr, PAGE_SIZE, cache.data() + PAGE_SIZE, nullptr, nullptr, nullptr);
    assert(patchRangeCount == 0 && verify(btd, 0, cache.data(), cache.size()) == 94);
    assert(verifyOpState == VerifyOpNoSymbols && fakeOps[kVerifyIndex] == apfsVerify && !writingRounds);
    puts("PASS: vnop_verify restores recorded patch ranges (ours and a wrapped foreign patcher) only in bluetoothd blocks");
}
// The verifier entry of the bluetoothd vnode's operations vector.
void verifyOpTests() {
    const char *btd = "/usr/sbin/bluetoothd", *other = "/usr/sbin/BlueTool";
    const auto code = reinterpret_cast<uint64_t>(fakeVerifyCode), desc = reinterpret_cast<uint64_t>(&fakeDesc);
    auto decode = [&](size_t size = sizeof(fakeVerifyCode)) { return decodeVnodeOpsOffset(fakeVerifyCode, size, code, desc); };
    writeVerifyCode(0x70); assert(decode() == 0xd0);
    writeVerifyCode(0x10, true); assert(decode() == 0x68);
    writeVerifyCode(0x70, false, 8); assert(!decode());                // movslq of another symbol
    writeVerifyCode(0x70); fakeVerifyCode[0x72] = 0x8c; assert(!decode()); // SIB form
    writeVerifyCode(0x70); fakeVerifyCode[0x71] = 0x89; assert(!decode()); // a store, not a load
    writeVerifyCode(0x70); fakeVerifyCode[0x73] = 0xd4; assert(!decode()); // not pointer aligned
    writeVerifyCode(0x70); assert(!decode(0x70 + 13));                  // past the scanned bytes
    if (realKernel) {
        const uint32_t offset = decodeVnodeOpsOffset(realVerifyCode, sizeof(realVerifyCode), realVerifyAddress, realDescAddress);
        assert(offset);
        printf("PASS: this host's kernel: _VNOP_VERIFY loads v_op from vnode+%#x\n", offset);
    }

    reset(25, true, false); Patcher::init(0);
    assert(vnodeOpsOffset == 0xd0 && vnopVerifyDesc && !verifyOpState && fakeOps[kVerifyIndex] == apfsVerify);
    std::vector<uint8_t> file(PAGE_SIZE * 4, 0x5a); pristine = file.data();
    Page page {}; apply(page, other);
    assert(!verifyOpState && !writingRounds);
    page = originals(); apply(page);
    assert(verifyOpState == VerifyOpInstalled && verifyFixRouted && orgVerifyOp == apfsVerify);
    assert(fakeOps[kVerifyIndex] == verifyOp && fakeOps[0] == defaultOp && writingRounds == 1 && !writing && publishes);
    page = originals(); apply(page); assert(writingRounds == 1);
    // Other vnodes, flags and digest-only calls reach APFS with the caller's arguments.
    std::vector<uint8_t> buf = file; int before = verifies;
    assert(verifyCall(other, 0, buf.data(), buf.size()) == 0 && verifies == before + 1);
    buf[5] ^= 1;
    assert(verifyCall(other, 0, buf.data(), buf.size()) == 94 && verifyCall(btd, 0, buf.data(), buf.size(), 8) == 94);
    assert(verifyCall(btd, 0, nullptr, 0) == 0 && !verifyFailed);
    // A bluetoothd block failing without recorded patch bytes is counted and published.
    const int published = publishes;
    assert(verifyCall(btd, PAGE_SIZE, buf.data() + PAGE_SIZE, PAGE_SIZE) == 0 && !verifyFailed);
    assert(verifyCall(btd, 0, buf.data(), buf.size()) == 94 && verifyFailed == 1 && !verifyLastFailOffset);
    assert(publishes == published + 1 && !verifyRestored && !verifyRestoreFailed);

    // Every failed step leaves APFS's verifier, and RPA, alone.
    for (int step = VerifyOpNoSymbols; step <= VerifyOpNoWrite + 1; ++step) {
        reset(25, true, false);
        if (step == VerifyOpNoSymbols) verifySymbolsMissing = true;
        if (step == VerifyOpNoWrite + 1) writeVerifyCode(0x70, false, 8); // undecodable: also NoSymbols
        if (step == VerifyOpBadVnode) badFields = true;
        if (step == VerifyOpNoEntry) fakeOps[kVerifyIndex] = nullptr;
        if (step == VerifyOpNoWrite) writingFails = true;
        Patcher::init(0);
        if (step == VerifyOpBadIndex) fakeDesc.vdesc_offset = 0;
        Page helpers = helperPage(); const Page unpatched = helpers; apply(helpers);
        assert(verifyOpState == (step > VerifyOpNoWrite ? int(VerifyOpNoSymbols) : step) && !verifyFixRouted);
        assert(helpers == unpatched && !rpaHelpersReady && !patchRangeCount && !writing);
        assert(fakeOps[kVerifyIndex] == (step == VerifyOpNoEntry ? nullptr : apfsVerify));
        assert(writingRounds == (step == VerifyOpNoWrite) && !writingDisables);
        page = originals(); apply(page); assert(!bluetoothdPatched && !patchRangeCount && page == originals());
    }
    puts("PASS: verifyOp replaces vnop_verify in the bluetoothd vnode's operations vector once, at its first page");
}
// Pause real patching after bytes change but before their originals are recorded.
// Use the live fault's universal-file offset: __DATA_CONST follows the helper
// page inside the same 16 KB APFS verification block, after a 16 KB fat header.
void verifyRaceTests() {
    const char *btd = "/usr/sbin/bluetoothd";
    const size_t blockOffset = 0x4000 + 0xaa0000;
    const size_t pageOffset = blockOffset + 0x2000;
    std::vector<uint8_t> file(blockOffset + PAGE_SIZE * 4, 0x5a);
    memcpy(file.data() + blockOffset + 0x2b88, extAdvFind, sizeof(extAdvFind));
    pristine = file.data();
    for (bool knownVnode : {false, true}) {
        reset(25, true, false); Patcher::init(0);
        if (knownVnode) { Page untouched {}; apply(untouched); }
        auto cache = file;
        lockAttempted = operationFinished = operationPaused = releaseOperation = false;
        afterReplacement = pauseOperation;
        std::thread patcher([&] {
            csValidatePage(btd, nullptr, pageOffset, cache.data() + pageOffset, nullptr, nullptr, nullptr);
        });
        {
            std::unique_lock<std::mutex> guard(raceMutex);
            raceCondition.wait(guard, [] { return operationPaused; });
        }
        int result = -1;
        std::thread verifier([&] {
            watchLockAttempt = true;
            result = verifyCall(btd, blockOffset, cache.data() + blockOffset, 16384);
            std::lock_guard<std::mutex> guard(raceMutex);
            operationFinished = true; raceCondition.notify_all();
        });
        {
            std::unique_lock<std::mutex> guard(raceMutex);
            raceCondition.wait(guard, [] { return lockAttempted || operationFinished; });
            assert(lockAttempted && !operationFinished);
            releaseOperation = true; raceCondition.notify_all();
        }
        patcher.join(); verifier.join(); afterReplacement = nullptr;
        assert(result == 0 && verifyRestored == 1 && !verifyRestoreFailed);
        assert(!memcmp(cache.data() + blockOffset + 0x2b88,
                       extAdvReplace, sizeof(extAdvReplace)));
    }

    // Verification of an untouched block must also exclude a new patch until
    // the original verifier finishes hashing the shared buffer.
    reset(25, true, false); Patcher::init(0);
    Page untouched {}; apply(untouched);
    auto cache = file;
    lockAttempted = operationFinished = operationPaused = releaseOperation = false;
    beforeHash = pauseOperation;
    int result = -1;
    std::thread verifier([&] {
        result = verifyCall(btd, blockOffset, cache.data() + blockOffset, 16384);
    });
    {
        std::unique_lock<std::mutex> guard(raceMutex);
        raceCondition.wait(guard, [] { return operationPaused; });
    }
    std::thread patcher([&] {
        watchLockAttempt = true;
        csValidatePage(btd, nullptr, pageOffset, cache.data() + pageOffset, nullptr, nullptr, nullptr);
        std::lock_guard<std::mutex> guard(raceMutex);
        operationFinished = true; raceCondition.notify_all();
    });
    {
        std::unique_lock<std::mutex> guard(raceMutex);
        raceCondition.wait(guard, [] { return lockAttempted || operationFinished; });
        assert(lockAttempted && !operationFinished);
        releaseOperation = true; raceCondition.notify_all();
    }
    verifier.join(); patcher.join(); beforeHash = nullptr;
    assert(result == 0 && !verifyRestored && !verifyRestoreFailed);
    puts("PASS: concurrent patch/verify waits for recorded originals, including first vnode, fat offsets and untouched blocks");
}
int main() {
    const char *fixture = std::getenv("MTK_RPA_FIXTURE"); assert(fixture);
    std::ifstream stream(fixture, std::ios::binary); assert(stream);
    rpaFixture.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    assert(!rpaFixture.empty());
    for (int version : {21, 25}) for (bool disable : {false, true}) {
        reset(version, false, true, true); noLeReadyMask = disable; Patcher::init(0);
        assert(leReadyMaskEnabled == (version == 25 && !disable));
    }
    for (int version : {20, 21, 24, 25, 26}) for (bool enabled : {false, true})
    for (bool disableAdv : {false, true}) for (bool ready : {false, true})
    for (unsigned scope : {0u, 1u, 2u, 3u}) {
        reset(version, enabled, disableAdv); Patcher::init(0); msbcReady = ready;
        if (scope & MsbcControllerScope::Supported) Patcher::observeMsbcController(true);
        if (scope & MsbcControllerScope::Unsupported) Patcher::observeMsbcController(false);
        const bool adv = version >= 21 && !disableAdv;
        const bool wideband = version == 25 && enabled && ready && scope == MsbcControllerScope::Supported;
        assert(lilu.pageRegistrations == int(version >= 21));
        assert(!verifyFixRouted && !verifyOpState);
        assert(extAdvPatchEnabled == adv && msbcPatchEnabled == (version == 25 && enabled));
        Page page = originals(), expected = page;
        if (wideband) {
            memcpy(expected.data() + 10, wbsReplace, sizeof(wbsReplace));
            memcpy(expected.data() + 110, hfpFillReplace, sizeof(hfpFillReplace));
        }
        if (adv) {
            memcpy(expected.data() + 210, extAdvReplace, sizeof(extAdvReplace));
            memcpy(expected.data() + 310, advAddressReplace, sizeof(advAddressReplace));
            memcpy(expected.data() + 410, advAddressRetryReplace, sizeof(advAddressRetryReplace));
        }
        apply(page); assert(page == expected && validations == 1 && bluetoothdPages == 1);
        assert(verifyFixRouted == (version >= 21) && (fakeOps[kVerifyIndex] == verifyOp) == (version >= 21));
        assert(bluetoothdWbsPatched == wideband && bluetoothdHfpFillPatched == wideband);
        assert(bluetoothdPatched == adv && bluetoothdAddressPatched == adv && bluetoothdAddressRetryPatched == adv);
        // A re-fault of an already patched page is restored to the recorded
        // originals before cs_validate_page and patched again.
        apply(page); assert(page == expected && bluetoothdWbsPatched == 2 * wideband && bluetoothdHfpFillPatched == 2 * wideband);
        assert(bluetoothdPatched == 2 * adv && bluetoothdAddressPatched == 2 * adv && bluetoothdAddressRetryPatched == 2 * adv);
        for (const char *path : {"/usr/sbin/BlueTool", "/tmp/bluetoothd", "/usr/sbin/bluetoothd.backup"}) {
            page = originals(); const int previous = searches; apply(page, path);
            assert(page == originals() && searches == previous);
        }
        page = originals(); pathError = 1; apply(page); assert(page == originals()); pathError = 0;
        const int previous = searches;
        csValidatePage("/usr/sbin/bluetoothd", nullptr, 0, nullptr, nullptr, nullptr, nullptr);
        assert(searches == previous);
    }
    reset(25, true, true); Patcher::init(0); msbcReady = true;
    Patcher::observeMsbcController(true);
    // The masked HFP fill site still patches 25G241 (other call displacement)
    // exactly as its old exact signature did.
    osBuild = "25G241";
    Page fill241 {}; memcpy(fill241.data() + 110, hfpFill241Find, sizeof(hfpFill241Find));
    apply(fill241); assert(!memcmp(fill241.data() + 110, hfpFill241Replace, sizeof(hfpFill241Replace)));
    assert(bluetoothdHfpFillPatched == 1); apply(fill241); assert(bluetoothdHfpFillPatched == 2);
    osBuild = "25G332";
    for (bool fill : {false, true}) {
        const uint8_t *find = fill ? hfpFillFind : wbsFind;
        const uint8_t *replace = fill ? hfpFillReplace : wbsReplace;
        const size_t length = fill ? sizeof(hfpFillFind) : sizeof(wbsFind);
        for (size_t offset : {size_t(0), PAGE_SIZE - length}) {
            Page page {}; memcpy(page.data() + offset, find, length); apply(page);
            assert(!memcmp(page.data() + offset, replace, length));
        }
        Page wrong {}; memcpy(wrong.data(), find, length); wrong[length - 1] ^= 1;
        Page page = wrong; apply(page); assert(page == wrong);
        std::array<uint8_t, PAGE_SIZE * 2> crossing {};
        memcpy(crossing.data() + PAGE_SIZE - 10, find, length);
        auto unchanged = crossing;
        csValidatePage("/usr/sbin/bluetoothd", nullptr, 0, crossing.data(), nullptr, nullptr, nullptr);
        csValidatePage("/usr/sbin/bluetoothd", nullptr, PAGE_SIZE, crossing.data()+PAGE_SIZE, nullptr, nullptr, nullptr);
        assert(crossing == unchanged);
    }
    reset(25, true, true); lilu.patcher.routeFails = true; Patcher::init(0);
    assert(!extAdvPatchRouted);
    // A later supported probe must never undo an unsupported controller's veto.
    reset(25, true, true); Patcher::init(0); msbcReady = true;
    Patcher::observeMsbcController(false); Patcher::observeMsbcController(true);
    Page blocked = originals(); apply(blocked);
    assert(blocked == originals() && !bluetoothdWbsPatched && !bluetoothdHfpFillPatched);
    buildTests();
    msbcOrderingTests();
    verifyTests();
    verifyOpTests();
    rpaTests();
    verifyRaceTests();
    if (const char *path = std::getenv("MTK_RPA_REAL_BINARY")) {
        std::ifstream input(path,std::ios::binary); assert(input);
        rpaFixture.assign(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
        osBuild = "25G342"; reset(25,true,false); noRpa = true; Patcher::init(0);
        assert(!prepareRpa("/usr/sbin/bluetoothd",false) && rpaPlanState == RpaPlanDisabled && bluetoothdCommonAbi == 3);
        puts("PASS: supplied binary common advertising/HFP ABI validates independently with RPA disabled");
    }
    puts("PASS: actual page hook gates unknown/unsupported/mixed controllers; supported IDs retain WBS; verification and advertising checks pass");
}
'''

declarations = "\n".join(array(name)[0] for name in names if "25G332" not in name)
types = source[source.index("// struct vnop_verify_args"):source.index("/*\n * offsetof(struct vnode, v_op)")]


def kernel_verify_code():
    """192 bytes of this host's _VNOP_VERIFY, as processPatcher decodes them."""
    kernel = Path("/System/Library/Kernels/kernel")
    if platform.system() != "Darwin" or not kernel.exists():
        return None
    symbols = {}
    for line in subprocess.run(["nm", "-arch", "x86_64", str(kernel)], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in ("_VNOP_VERIFY", "_vnop_verify_desc"):
            symbols[parts[2]] = int(parts[0], 16)
    if len(symbols) != 2:
        return None
    data = kernel.read_bytes()
    base = 0
    if int.from_bytes(data[:4], "big") == 0xcafebabe:  # fat: pick the x86_64 slice
        for i in range(int.from_bytes(data[4:8], "big")):
            cputype, _, offset = (int.from_bytes(data[8 + 20 * i + 4 * j:12 + 20 * i + 4 * j], "big") for j in range(3))
            if cputype == 0x01000007:
                base = offset
    assert int.from_bytes(data[base:base + 4], "little") == 0xfeedfacf
    ncmds, at = int.from_bytes(data[base + 16:base + 20], "little"), base + 32
    address = symbols["_VNOP_VERIFY"]
    for _ in range(ncmds):
        cmd, size = (int.from_bytes(data[at + 4 * j:at + 4 * j + 4], "little") for j in range(2))
        if cmd == 0x19:
            vmaddr, vmsize, fileoff = (int.from_bytes(data[at + 24 + 8 * j:at + 32 + 8 * j], "little") for j in range(3))
            if vmaddr <= address < vmaddr + vmsize:
                start = base + fileoff + address - vmaddr
                return data[start:start + 192], address, symbols["_vnop_verify_desc"]
        at += size
    return None


real = kernel_verify_code()
if real:
    real_code = ("const uint8_t realVerifyCode[] {" + ",".join(str(b) for b in real[0]) + "};\n" +
                 f"const uint64_t realVerifyAddress = {real[1]:#x}ULL, realDescAddress = {real[2]:#x}ULL;\n" +
                 "constexpr bool realKernel = true;\n")
else:
    real_code = "const uint8_t realVerifyCode[1] {};\nconst uint64_t realVerifyAddress = 0, realDescAddress = 0;\n" \
                "constexpr bool realKernel = false;\n"
state_start = source.index("constexpr size_t kPatchRangeMax")
state = source[state_start:source.index("\n", source.index("uint8_t *verifyCopy;"))]
site_table = source[source.index("enum PatchFeature : uint8_t"):source.index("bool siteScopeAllows(")]
rpa_state = source[source.index("mach_vm_address_t orgMacVnodeCheckExec;"):source.index("// -mtkbtanybuild:")]
forward = "bool patchBluetoothdPage(void *data);\nSInt32 readBluetoothdBuild();\nint32_t applyRpaPage(vnode_t, uint64_t, void *, int *, int *, int *);\n"
wrappers = forward + "\n\n".join(function(name) for name in (
    "bool isMsbcReady()", "void recordPatchRange(", "void recordPageDiff(", "void restorePatchedPageLocked(", "static __attribute__((noinline)) bool isBluetoothdPath(", "bool isBluetoothd(",
    "uint32_t decodeVnodeOpsOffset(", "void noteVerifyFailure(", "template <typename Verify>\nerrno_t verifyBluetoothdBlock(",
    "int verifyOp(void *arg)", "void installVerifyOp(", "void csValidatePage(", "SInt32 readBluetoothdBuild()",
    "bool siteScopeAllows(", "bool commonFeatureAllowed(", "bool featureWanted(", "bool featureOn(", "bool patchBluetoothdPage(void *data)\n{",
    "int readRpaImage(", "uint32_t validateCommonImage(", "int macVnodeCheckExec(", "int32_t applyRpaPage(", "void processPatcher(",
    "void Patcher::observeMsbcController(", "void Patcher::init("))
compiler = shutil.which("clang++")
assert compiler, "clang++ is required"
# The real masked page matcher (mtk_bt::page_patch) through its C ABI.
rust_host = ROOT / "build/rust-bt-host"
subprocess.run(["cargo", "build", "-q", "-p", "mtk-ffi", "--features", "bt", "--target-dir", str(rust_host)],
               cwd=ROOT / "rust", check=True)
with tempfile.TemporaryDirectory(prefix="mtk-msbc-pages-") as directory:
    directory = Path(directory)
    cpp, binary = directory / "pages.cpp", directory / "pages"
    scope_include = '#include "' + str(ROOT / "kext/BT/msbc/MsbcControllerScope.hpp") + '"\n'
    cpp.write_text(scope_include + mock + rpa_state + declarations + history_code + state + "\n" + types + vop_mock +
                   real_code + "\n" + site_table + wrappers + tests)
    sanitizer = [] if args.no_sanitize else ["-fsanitize=" + ("thread" if args.thread_sanitize else "address,undefined"),
                                            "-fno-sanitize-recover=all"]
    subprocess.run([compiler, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "kext/BT"), "-I", str(ROOT / "kext/include"),
                    *sanitizer, str(cpp), str(rust_host / "debug/libmtk_ffi.a"), "-o", str(binary)], check=True)
    fixture = directory / "rpa-fixture.bin"
    factory = runpy.run_path(str(ROOT / "scripts/bt-rpa-fixture.py"))
    fixture.write_bytes(factory["image"](fat=0x4000)[0])
    environment = {**os.environ, "MTK_RPA_FIXTURE": str(fixture)}
    if args.binary:
        environment["MTK_RPA_REAL_BINARY"] = str(args.binary.resolve())
    subprocess.run([str(binary)], check=True, env=environment)

    if platform.machine() == "x86_64":
        # Replace only controller lookup with the argument; execute the real gate instructions.
        assert wbs[:15] == wbs_patch[:15] == bytes.fromhex("554889e5e831fd01008b8834030000")
        prefix = bytes.fromhex("554889e589f9") + b"\x90" * 9
        assembly = ".text\n" + "\n".join(
            f".globl _{name}\n_{name}:\n.byte " + ",".join(str(x) for x in prefix + code[15:])
            for name, code in (("before", wbs), ("after", wbs_patch))) + "\n"
        if platform.system() != "Darwin":
            assembly = assembly.replace("_before", "before").replace("_after", "after")
        (directory / "gate.S").write_text(assembly)
        (directory / "gate.cpp").write_text(r'''
#include <cassert>
#include <cstdint>
extern "C" uint8_t before(uint32_t), after(uint32_t);
void check(uint32_t chip) {
    const bool expected = (chip >= 1 && chip < 10 && ((0x1f3 >> chip) & 1)) || (chip >= 10 && chip < 6000);
    assert(before(chip) == expected && after(chip) == (chip == 0 || expected));
}
int main() {
    for (uint32_t chip = 0; chip < 65536; ++chip) check(chip);
    const uint32_t edges[] = {0x7fffffff, 0x80000000, 0xfffff060, 0xfffffffe, 0xffffffff};
    for (auto chip : edges) check(chip);
}
''')
        native = directory / "gate"
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(directory / "gate.cpp"),
                        str(directory / "gate.S"), "-o", str(native)], check=True)
        subprocess.run([str(native)], check=True)
        print("PASS: 65,541 native WBS gate cases; only chipset 0 gains wideband")
    else:
        print("SKIP: native WBS instruction test requires x86_64")

if args.binary:
    # Masked WBS/HFP fill (and every other site) uniqueness and page checks.
    subprocess.run([sys.executable, str(ROOT / "scripts/test-bt-page-patches.py"),
                    "--binary", str(args.binary)], check=True)

# Also exercise the production EP0/interrupt wrappers and their Rust ABI.
subprocess.run([sys.executable, str(ROOT / "scripts/test-bt-le-ready.py"),
                *(["--no-sanitize"] if args.no_sanitize else [])], check=True)
