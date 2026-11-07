#!/usr/bin/env python3
"""Exercise the actual opt-in bluetoothd page hook; optionally check a binary."""
import argparse
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
    start = source.index(declaration)
    opening = source.index("{", start)
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
enum BluetoothdBuild : int32_t { BuildUnread = 0, BuildOther = 1, Build25G241 = 2, Build25G332 = 3 };
int32_t bluetoothdBuild;
char bluetoothdBuildName[16];
int32_t bluetoothdSitesAmbiguous, bluetoothdPagesUnpatched;
uint8_t featureLatch[3];
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
// Kernel write protection lifted around the vector store, always restored.
bool writingFails; int writing, writingRounds;
struct MachInfo {
    static int setKernelWriting(bool enable, IOSimpleLock *) {
        if (enable) { assert(!writing); writing = 1; ++writingRounds; return writingFails ? 1 : KERN_SUCCESS; }
        assert(writing == 1); writing = 0; return KERN_SUCCESS;
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
int pathError, validations, searches;
// The real Rust matcher, wrapped: count searches and pause after a write.
#include "mtk_bt.h"
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
    writingFails = false; writingRounds = 0; badFields = false;
    fakeDesc.vdesc_offset = kVerifyIndex;
    for (auto &op : fakeOps) op = nullptr;
    fakeOps[0] = defaultOp; fakeOps[kVerifyIndex] = apfsVerify;
    fakeFields[0] = fakeOps; fakeFields[1] = &fakeMount; fakeFields[2] = &fakeData;
    writeVerifyCode(0x70);
    msbcReady = extAdvPatchEnabled = extAdvPatchRouted = msbcPatchEnabled = false;
    noRpa = rpaPatchEnabled = rpaHelpersReady = false; bluetoothdBuild = BuildUnread; bluetoothdBuildName[0] = 0;
    anyBuild = anyBuildPatches = false; bluetoothdSitesAmbiguous = bluetoothdPagesUnpatched = 0;
    memset(featureLatch, 0, sizeof(featureLatch)); free(verifyCopy); verifyCopy = nullptr; verifyCopyLock = nullptr;
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
struct RpaSite {
    const uint8_t *find, *replacement;
    size_t length, pageOffset;
    int32_t *counter;
};
const RpaSite rpaSites[] {
    {rpaHelpersFind25G332, rpaHelpersReplace25G332, sizeof(rpaHelpersFind25G332), 0xb88, &bluetoothdRpaHelpersPatched},
    {rpaLegacyFind25G332, rpaLegacyReplace25G332, sizeof(rpaLegacyFind25G332), 0x3dd, &bluetoothdRpaLegacyPatched},
    {rpaScanFind25G332, rpaScanReplace25G332, sizeof(rpaScanFind25G332), 0x4c3, &bluetoothdRpaScanPatched},
    {rpaDecodeFind25G332, rpaDecodeReplace25G332, sizeof(rpaDecodeFind25G332), 0x834, &bluetoothdRpaDecodePatched},
    {rpaDeviceFind25G332, rpaDeviceReplace25G332, sizeof(rpaDeviceFind25G332), 0x002, &bluetoothdRpaDevicePatched},
    {rpaConnectFind25G332, rpaConnectReplace25G332, sizeof(rpaConnectFind25G332), 0x0c3, &bluetoothdRpaConnectPatched},
    {rpaUpdateFind25G332, rpaUpdateReplace25G332, sizeof(rpaUpdateFind25G332), 0x1da, &bluetoothdRpaUpdatePatched},
};
Page helperPage() {
    Page page {};
    memcpy(page.data() + 0xb88, rpaHelpersFind25G332, sizeof(rpaHelpersFind25G332));
    return page;
}
void rpaTests() {
    // Other macOS builds (25G241 shares child signatures) keep RPA off.
    // kern.osversion is empty when the kext starts (launchd sets it later):
    // init must not decide, and an empty value is retried, not cached.
    osBuild = "";
    reset(25, true, false, false); noRpa = false; osversionReads = 0; Patcher::init(0);
    assert(rpaPatchEnabled && !osversionReads);
    { Page helpers = helperPage(), original = helpers; apply(helpers);
      assert(!rpaHelpersReady && helpers == original && bluetoothdBuild == BuildUnread && osversionReads == 1); }
    osBuild = "25G332";
    { Page helpers = helperPage(); apply(helpers); assert(rpaHelpersReady && bluetoothdBuild == Build25G332);
      const int reads = osversionReads; apply(helpers); assert(osversionReads == reads); }
    osBuild = "25G241";
    reset(25, true, false, false); noRpa = false; Patcher::init(0);
    assert(rpaPatchEnabled);
    { Page helpers = helperPage(), original = helpers; apply(helpers);
      assert(!rpaHelpersReady && !bluetoothdRpaHelpersPatched && helpers == original && bluetoothdBuild == Build25G241); }
    for (size_t i = 1; i < std::size(rpaSites); ++i) {
        const auto &site = rpaSites[i];
        Page page {}; memcpy(page.data() + site.pageOffset, site.find, site.length);
        const Page before = page; apply(page); assert(page == before && !*site.counter);
    }
    osBuild = "25G332";
    for (int version : {20, 21, 24, 25, 26}) for (bool disableAdv : {false, true})
    for (bool disableRpa : {false, true}) for (bool disableVerify : {false, true}) {
        reset(version, true, disableAdv, disableVerify); noRpa = disableRpa; Patcher::init(0);
        const bool enabled = version == 25 && !disableAdv && !disableRpa && !disableVerify;
        assert(rpaPatchEnabled == (version == 25 && !disableAdv && !disableRpa));
        Page helpers = helperPage(), original = helpers;
        apply(helpers);
        assert(rpaHelpersReady == enabled && bluetoothdRpaHelpersPatched == enabled);
        if (enabled) memcpy(original.data() + 0xb88, rpaHelpersReplace25G332, sizeof(rpaHelpersReplace25G332));
        assert(helpers == original);
        for (size_t i = 1; i < std::size(rpaSites); ++i) {
            const auto &site = rpaSites[i];
            Page page {}; memcpy(page.data() + site.pageOffset, site.find, site.length);
            Page expected = page;
            if (enabled) memcpy(expected.data() + site.pageOffset, site.replacement, site.length);
            apply(page);
            assert(page == expected && *site.counter == enabled);
            apply(page); assert(page == expected && *site.counter == enabled);
        }
    }
    // The helper page is faulted lazily (end of __TEXT): a child page
    // validated before it must still be patched; its call faults the helpers in.
    reset(25, true, false); Patcher::init(0);
    for (size_t i = 1; i < std::size(rpaSites); ++i) {
        const auto &site = rpaSites[i];
        Page page {}; memcpy(page.data() + site.pageOffset, site.find, site.length);
        Page expected = page; memcpy(expected.data() + site.pageOffset, site.replacement, site.length);
        apply(page);
        assert(page == expected && *site.counter == 1 && !rpaHelpersReady);
    }
    Page helper = helperPage(); apply(helper); assert(rpaHelpersReady);
    for (const auto &site : rpaSites) {
        for (size_t offset : {size_t(0), PAGE_SIZE - site.length}) {
            Page page {}; memcpy(page.data() + offset, site.find, site.length);
            const int previous = *site.counter; apply(page);
            assert(!memcmp(page.data() + offset, site.replacement, site.length) && *site.counter == previous + 1);
            apply(page); assert(*site.counter == previous + 1);
        }
        Page wrong {}; memcpy(wrong.data(), site.find, site.length); wrong[site.length - 1] ^= 1;
        Page page = wrong; const int previous = *site.counter; apply(page);
        assert(page == wrong && *site.counter == previous);
        for (const char *path : {"/usr/sbin/BlueTool", "/tmp/bluetoothd", "/usr/sbin/bluetoothd.backup"}) {
            page = {}; memcpy(page.data(), site.find, site.length);
            const Page before = page; apply(page, path); assert(page == before && *site.counter == previous);
        }
        std::array<uint8_t, PAGE_SIZE * 2> crossing {};
        memcpy(crossing.data() + PAGE_SIZE - 10, site.find, site.length);
        const auto before = crossing;
        csValidatePage("/usr/sbin/bluetoothd", nullptr, 0, crossing.data(), nullptr, nullptr, nullptr);
        csValidatePage("/usr/sbin/bluetoothd", nullptr, PAGE_SIZE, crossing.data() + PAGE_SIZE, nullptr, nullptr, nullptr);
        assert(crossing == before && *site.counter == previous);
    }
    // A changed helper anchor (other bytes at the end of __TEXT) is left alone.
    reset(25, true, false); Patcher::init(0);
    helper = helperPage(); helper[0xb88 + 4] ^= 1;
    const Page unsupported = helper; apply(helper);
    assert(helper == unsupported && !rpaHelpersReady);
    reset(25, true, false); verifySymbolsMissing = true; Patcher::init(0);
    helper = helperPage(); const Page noVerify = helper; apply(helper);
    assert(helper == noVerify && !rpaHelpersReady && !bluetoothdRpaHelpersPatched);

    // One verify block containing every RPA patch, existing advertising
    // patches and a wrapped foreign patch. Helpers span >96 changed bytes.
    reset(25, true, false); Patcher::init(0);
    const char *btd = "/usr/sbin/bluetoothd";
    std::vector<uint8_t> file(PAGE_SIZE * 4, 0x5a);
    const size_t offsets[] {0xb88, PAGE_SIZE + 0x3dd, PAGE_SIZE + 0x4c3,
                           2 * PAGE_SIZE + 0x834, PAGE_SIZE + 0x802,
                           2 * PAGE_SIZE + 0xc3, 2 * PAGE_SIZE + 0x1da};
    for (size_t i = 0; i < std::size(rpaSites); ++i)
        memcpy(file.data() + offsets[i], rpaSites[i].find, rpaSites[i].length);
    memcpy(file.data() + 3 * PAGE_SIZE + 210, extAdvFind, sizeof(extAdvFind));
    memcpy(file.data() + 3 * PAGE_SIZE + 310, advAddressFind, sizeof(advAddressFind));
    memcpy(file.data() + 3 * PAGE_SIZE + 410, advAddressRetryFind, sizeof(advAddressRetryFind));
    memcpy(file.data() + 3 * PAGE_SIZE + 3000, foreignFind, sizeof(foreignFind));
    pristine = file.data();
    auto cache = file, expected = file;
    for (size_t i = 0; i < std::size(rpaSites); ++i)
        memcpy(expected.data() + offsets[i], rpaSites[i].replacement, rpaSites[i].length);
    memcpy(expected.data() + 3 * PAGE_SIZE + 210, extAdvReplace, sizeof(extAdvReplace));
    memcpy(expected.data() + 3 * PAGE_SIZE + 310, advAddressReplace, sizeof(advAddressReplace));
    memcpy(expected.data() + 3 * PAGE_SIZE + 410, advAddressRetryReplace, sizeof(advAddressRetryReplace));
    memcpy(expected.data() + 3 * PAGE_SIZE + 3000, foreignReplace, sizeof(foreignReplace));
    for (size_t page = 0; page < 4; ++page)
        csValidatePage(btd, nullptr, page * PAGE_SIZE, cache.data() + page * PAGE_SIZE, nullptr, nullptr, nullptr);
    assert(cache == expected && patchRangeCount < kPatchRanges && !patchRangeOverflow);
    for (const auto &site : rpaSites) assert(*site.counter == 1);
    const uint32_t ranges = patchRangeCount;
    assert(fakeOps[kVerifyIndex] == verifyOp);
    assert(verifyCall(btd, 0, cache.data(), cache.size()) == 0);
    assert(cache == expected && verifyRestored == 1 && !verifyRestoreFailed);
    // Revalidation is idempotent, and a refault must reuse the same ranges.
    for (size_t page = 0; page < 4; ++page) {
        csValidatePage(btd, nullptr, page * PAGE_SIZE, cache.data() + page * PAGE_SIZE, nullptr, nullptr, nullptr);
        Page fresh; memcpy(fresh.data(), file.data() + page * PAGE_SIZE, PAGE_SIZE);
        csValidatePage(btd, nullptr, page * PAGE_SIZE, fresh.data(), nullptr, nullptr, nullptr);
        assert(!memcmp(fresh.data(), expected.data() + page * PAGE_SIZE, PAGE_SIZE));
    }
    assert(patchRangeCount == ranges && !patchRangeOverflow);
    auto tampered = cache; tampered[0x6c8] ^= 1;
    assert(verifyCall(btd, 0, tampered.data(), tampered.size()) == 94);
    assert(verifyRestoreFailed == 1);
    puts("PASS: RPA build/boot/verify gates, page boundaries, counters and full verify-block restoration");
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
        osBuild = "25H100";
        reset(25, true, false); anyBuild = any; Patcher::init(0); msbcReady = true;
        Patcher::observeMsbcController(true);
        assert(anyBuildPatches == any);
        assert(!bluetoothdAddressRetryPatched);  // known-build scope, like advAddress
        Page p = originals(), expected = p;
        memcpy(expected.data() + 210, extAdvReplace, sizeof(extAdvReplace));
        if (any) {
            memcpy(expected.data() + 10, wbsReplace, sizeof(wbsReplace));
            memcpy(expected.data() + 110, hfpFillReplace, sizeof(hfpFillReplace));
            memcpy(expected.data() + 310, advAddressReplace, sizeof(advAddressReplace));
            memcpy(expected.data() + 410, advAddressRetryReplace, sizeof(advAddressRetryReplace));
        }
        apply(p);
        assert(p == expected && bluetoothdBuild == BuildOther && !strcmp(bluetoothdBuildName, "25H100"));
        assert(bluetoothdPatched == 1 && bluetoothdAddressRetryPatched == any);
        // Retry without advAddress stalls the set start: its guard calls a
        // helper that returns 0 without sending HCI or calling the callback
        // when the gate is false (verified on 25G332). ScopeKnownBuild, like advAddress.
        assert(bluetoothdAddressPatched == any && bluetoothdWbsPatched == any && bluetoothdHfpFillPatched == any);
        Page helpers = helperPage(); const Page original = helpers; apply(helpers);
        assert(helpers == original && !rpaHelpersReady && !bluetoothdRpaHelpersPatched);
    }
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
    // mSBC: the wideband gate page decided before the bridge was ready keeps
    // the sender page native too; never one of the pair alone.
    reset(25, true, true); Patcher::init(0);
    page = {}; memcpy(page.data() + 10, wbsFind, sizeof(wbsFind)); before = page; apply(page);
    assert(page == before && featureLatch[FeatureMsbc] == LatchOff && !bluetoothdWbsPatched);
    msbcReady = true; Patcher::observeMsbcController(true);
    Page fill {}; memcpy(fill.data() + 110, hfpFillFind, sizeof(hfpFillFind)); before = fill; apply(fill);
    assert(fill == before && !bluetoothdHfpFillPatched);
    csValidatePage(recycled, nullptr, 0, fill.data(), nullptr, nullptr, nullptr);
    assert(bluetoothdHfpFillPatched == 1 && featureLatch[FeatureMsbc] == LatchOn);
    // Decided on: a later controller veto cannot leave the gate native beside a patched sender.
    Patcher::observeMsbcController(false);
    page = {}; memcpy(page.data() + 10, wbsFind, sizeof(wbsFind));
    csValidatePage(recycled, nullptr, 0, page.data(), nullptr, nullptr, nullptr);
    assert(bluetoothdWbsPatched == 1);
    puts("PASS: masked sites patch 25G241 as before; unlisted builds get only build-independent sites (or all but RPA with -mtkbtanybuild); ambiguous and per-vnode decisions");
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
        assert(writingRounds == (step == VerifyOpNoWrite));
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
    memcpy(file.data() + blockOffset + 0x2b88, rpaHelpersFind25G332, sizeof(rpaHelpersFind25G332));
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
                       rpaHelpersReplace25G332, sizeof(rpaHelpersReplace25G332)));
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
        apply(page); assert(page == expected && bluetoothdWbsPatched == wideband && bluetoothdHfpFillPatched == wideband);
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
    assert(bluetoothdHfpFillPatched == 1); apply(fill241); assert(bluetoothdHfpFillPatched == 1);
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
    verifyTests();
    verifyOpTests();
    rpaTests();
    verifyRaceTests();
    puts("PASS: actual page hook gates unknown/unsupported/mixed controllers; supported IDs retain WBS; verification and advertising checks pass");
}
'''

declarations = "\n".join(array(name)[0] for name in names)
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
forward = "bool patchBluetoothdPage(void *data);\n"
wrappers = forward + "\n\n".join(function(name) for name in (
    "bool isMsbcReady()", "void recordPatchRange(", "void recordPageDiff(", "static __attribute__((noinline)) bool isBluetoothdPath(", "bool isBluetoothd(",
    "uint32_t decodeVnodeOpsOffset(", "void noteVerifyFailure(", "template <typename Verify>\nerrno_t verifyBluetoothdBlock(",
    "int verifyOp(void *arg)", "void installVerifyOp(", "void csValidatePage(", "SInt32 readBluetoothdBuild()",
    "bool siteScopeAllows(", "bool featureWanted(", "bool featureOn(", "bool patchBluetoothdPage(void *data)\n{",
    "void processPatcher(",
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
    cpp.write_text(scope_include + mock + declarations + history_code + state + "\n" + types + vop_mock +
                   real_code + "\n" + site_table + wrappers + tests)
    sanitizer = [] if args.no_sanitize else ["-fsanitize=" + ("thread" if args.thread_sanitize else "address,undefined"),
                                            "-fno-sanitize-recover=all"]
    subprocess.run([compiler, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "kext/BT"), "-I", str(ROOT / "kext/include"),
                    *sanitizer, str(cpp), str(rust_host / "debug/libmtk_ffi.a"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

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
