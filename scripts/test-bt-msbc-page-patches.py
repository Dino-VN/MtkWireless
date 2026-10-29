#!/usr/bin/env python3
"""Exercise the actual opt-in bluetoothd page hook; optionally check a binary."""
import argparse
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "kext/BT/Patcher.cpp").read_text()
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--binary", type=Path, help="optional Tahoe x86_64 bluetoothd to check, without modifying it")
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


names = ("wbsFind", "wbsReplace", "hfpFillFind", "hfpFillReplace",
         "extAdvFind", "extAdvReplace", "advAddressFind", "advAddressReplace",
         "advAddressRetryFind", "advAddressRetryReplace")
arrays = {name: array(name)[1] for name in names}
wbs, wbs_patch = arrays["wbsFind"], arrays["wbsReplace"]
fill, fill_patch = arrays["hfpFillFind"], arrays["hfpFillReplace"]
assert len(wbs) == len(wbs_patch) == 76
assert [(i, a, b) for i, (a, b) in enumerate(zip(wbs, wbs_patch)) if a != b] == [(17, 255, 0), (19, 206, 207)]
assert len(fill) == len(fill_patch) == 31
assert fill[:4] == bytes.fromhex("480fafd0") and fill_patch[:4] == bytes.fromhex("0f1f4000")
assert fill[4:] == fill_patch[4:]
assert "-btlfxmtkwbs" not in source and "-smbtmsbc" not in source
assert 'number("BluetoothdWbsPatched", (uint32_t)bluetoothdWbsPatched);' in source
assert 'number("BluetoothdHfpFillPatched", (uint32_t)bluetoothdHfpFillPatched);' in source

mock = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using mach_vm_address_t = uintptr_t;
using vnode_t = const char *;
using memory_object_t = void *;
using memory_object_offset_t = uint64_t;
using errno_t = int;
using vfs_context_t = void *;
typedef void IOLock;
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
int32_t bluetoothdPages, bluetoothdPatched, bluetoothdAddressPatched, bluetoothdAddressRetryPatched;
int32_t bluetoothdWbsPatched, bluetoothdHfpFillPatched;
uint32_t rustFailed;
uint32_t patchRangeCount;
int32_t patchRangeOverflow, verifyRestored, verifyRestoreFailed;
bool verifyFixEnabled, verifyFixRouted;
void *leLock;
int getKernelVersion() { return kernel; }
bool checkKernelArgument(const char *name) {
    if (!strcmp(name, "-mtkbtnomsbc")) return !flag;
    if (!strcmp(name, "-mtkbtnoverifyfix")) return noVerifyFix;
    assert(!strcmp(name, "-mtkbtnoextadv")); return noExtAdv;
}
void *IOLockAlloc() { return reinterpret_cast<void *>(1); }
int lockDepth;
void IOLockLock(IOLock *) { assert(!lockDepth++); }
void IOLockUnlock(IOLock *) { assert(lockDepth-- == 1); }
void *IOMalloc(size_t size) { return malloc(size); }
void IOFree(void *p, size_t) { free(p); }
void OSIncrementAtomic(int32_t *value) { ++*value; }
void publishCounters() {}
int pathError, validations, searches;
int vn_getpath(vnode_t path, char *out, int *length) {
    if (pathError) return pathError;
    assert(strlen(path) + 1 <= size_t(*length)); strcpy(out, path); return 0;
}
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
errno_t originalVerify(vnode_t, off_t offset, uint8_t *buf, size_t size, size_t *, void **, uint32_t, vfs_context_t, void *) {
    ++verifies;
    return buf && memcmp(buf, pristine + offset, size) ? 94 : 0;
}
mach_vm_address_t orgCsValidatePage, orgVnopVerify;
struct KernelPatcher {
    static constexpr int KernelID = 0;
    struct RouteRequest {
        const char *symbol;
        mach_vm_address_t *original;
        template<class Fn> RouteRequest(const char *s, Fn, mach_vm_address_t &o) : symbol(s), original(&o) {}
    };
    bool routeFails = false, verifyRouteFails = false;
    bool routeMultipleLong(int id, RouteRequest *routes, size_t count) {
        assert(id == KernelID && count == 1);
        if (!strcmp(routes[0].symbol, "_VNOP_VERIFY")) {
            if (verifyRouteFails) return false;
            *routes[0].original = reinterpret_cast<mach_vm_address_t>(originalVerify); return true;
        }
        assert(!strcmp(routes[0].symbol, "_cs_validate_page"));
        if (routeFails) return false;
        *routes[0].original = reinterpret_cast<mach_vm_address_t>(originalValidate); return true;
    }
    void clearError() {}
    static bool findAndReplace(void *data, size_t size, const void *find, size_t findSize,
                               const void *replace, size_t replaceSize) {
        assert(validations && findSize == replaceSize && size == PAGE_SIZE && lockDepth == verifyFixRouted); ++searches;
        auto *bytes = static_cast<uint8_t *>(data);
        const auto *needle = static_cast<const uint8_t *>(find);
        auto *hit = std::search(bytes, bytes + size, needle, needle + findSize);
        if (hit == bytes + size) return false;
        memcpy(hit, replace, replaceSize); return true;
    }
};
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
namespace Patcher { void init(uint32_t); }
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
    free(pageSnapshot); pageSnapshot = nullptr; pageLock = nullptr; lilu.patcher.verifyRouteFails = false;
    msbcReady = extAdvPatchEnabled = extAdvPatchRouted = msbcPatchEnabled = false;
    bluetoothdPages = bluetoothdPatched = bluetoothdAddressPatched = bluetoothdAddressRetryPatched = 0;
    bluetoothdWbsPatched = bluetoothdHfpFillPatched = 0;
    pathError = validations = searches = 0;
    orgCsValidatePage = reinterpret_cast<mach_vm_address_t>(originalValidate);
    lilu.pageRegistrations = 0; lilu.patcher.routeFails = false;
}
// One 16 KB verify block: page 1 holds our and a foreign patch site, page 0 none.
void verifyTests() {
    const char *btd = "/usr/sbin/bluetoothd", *other = "/usr/sbin/BlueTool";
    std::vector<uint8_t> file(PAGE_SIZE * 4, 0x5a);
    memcpy(file.data() + PAGE_SIZE + 210, extAdvFind, sizeof(extAdvFind));
    memcpy(file.data() + PAGE_SIZE + 3000, foreignFind, sizeof(foreignFind));
    pristine = file.data();
    size_t blockSize = 16384;
    auto verify = [&](vnode_t vp, off_t offset, uint8_t *buf, size_t size, uint32_t flags = 0) {
        return vnopVerify(vp, offset, buf, size, &blockSize, nullptr, flags, nullptr, nullptr);
    };

    reset(25, true, false); Patcher::init(0);
    assert(verifyFixRouted && extAdvPatchRouted);
    std::vector<uint8_t> cache = file; // the file cache: page 1 resident and patched
    csValidatePage(btd, nullptr, PAGE_SIZE, cache.data() + PAGE_SIZE, nullptr, nullptr, nullptr);
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

    // Range table full: later ranges are counted, not stored.
    reset(25, true, true); Patcher::init(0);
    for (int round = 0; round < 40; ++round) {
        std::vector<uint8_t> page(PAGE_SIZE, 0);
        memcpy(page.data() + round * 100, foreignFind, sizeof(foreignFind));
        csValidatePage(btd, nullptr, 0, page.data(), nullptr, nullptr, nullptr);
    }
    assert(patchRangeCount == 32 && patchRangeOverflow == 8);

    // Long differences split into ranges of at most kPatchRangeMax bytes.
    reset(25, true, true); Patcher::init(0);
    Page before {}, after {}; for (size_t i = 0; i < 200; ++i) after[100 + i] = 0xcc;
    recordPageDiff(0, before.data(), after.data());
    assert(patchRangeCount == 3 && patchRanges[0].length == 96 && patchRanges[2].length == 8);
    assert(patchRanges[0].fileOffset == 100 && patchRanges[2].fileOffset == 292);

    // Opt-out and route failure leave the plain page hook.
    reset(25, true, false, true); Patcher::init(0);
    assert(!verifyFixRouted && extAdvPatchRouted && !pageSnapshot);
    cache = file; csValidatePage(btd, nullptr, PAGE_SIZE, cache.data() + PAGE_SIZE, nullptr, nullptr, nullptr);
    assert(bluetoothdPatched == 1 && patchRangeCount == 0 && !bluetoothdVnode);
    reset(25, true, false); lilu.patcher.verifyRouteFails = true; Patcher::init(0);
    assert(!verifyFixRouted && extAdvPatchRouted);
    cache = file; csValidatePage(btd, nullptr, PAGE_SIZE, cache.data() + PAGE_SIZE, nullptr, nullptr, nullptr);
    assert(patchRangeCount == 0 && verify(btd, 0, cache.data(), cache.size()) == 94);
    puts("PASS: VNOP_VERIFY restores recorded patch ranges (ours and a wrapped foreign patcher) only in bluetoothd blocks");
}
int main() {
    for (int version : {20, 21, 24, 25, 26}) for (bool enabled : {false, true})
    for (bool disableAdv : {false, true}) for (bool ready : {false, true}) {
        reset(version, enabled, disableAdv); Patcher::init(0); msbcReady = ready;
        const bool adv = version >= 21 && !disableAdv;
        const bool wideband = version == 25 && enabled && ready;
        assert(lilu.pageRegistrations == int(version >= 21));
        assert(verifyFixRouted == (version >= 21));
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
    verifyTests();
    puts("PASS: actual init/page hook opt-in, Tahoe/readiness gates, foreign paths, exact bytes, page limits, idempotency and advertising opt-out");
}
'''

declarations = "\n".join(array(name)[0] for name in names)
state_start = source.index("constexpr size_t kPatchRangeMax")
state = source[state_start:source.index("\n", source.index("vnode_t bluetoothdVnode;"))]
wrappers = "\n\n".join(function(name) for name in (
    "bool isMsbcReady()", "void recordPatchRange(", "void recordPageDiff(", "bool isBluetoothd(",
    "errno_t vnopVerify(", "void patchBluetoothdPage(const void *data)\n{", "void csValidatePage(", "void processPatcher(",
    "void Patcher::init("))
compiler = shutil.which("clang++")
assert compiler, "clang++ is required"
with tempfile.TemporaryDirectory(prefix="mtk-msbc-pages-") as directory:
    directory = Path(directory)
    cpp, binary = directory / "pages.cpp", directory / "pages"
    cpp.write_text(mock + declarations + state + "\n" + wrappers + tests)
    subprocess.run([compiler, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", str(cpp), "-o", str(binary)], check=True)
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
    data = args.binary.read_bytes()
    for label, original, replacement in (("WBS", wbs, wbs_patch), ("HFP fill", fill, fill_patch)):
        originals, replacements = data.count(original), data.count(replacement)
        assert originals + replacements == 1, label + ": expected one original or live-cache-patched signature"
        offset = data.index(original if originals else replacement)
        assert offset % 4096 + len(original) <= 4096, label + ": signature crosses validation page"
        print(f"PASS: {label} unique same-page signature at file offset {offset:#x}" +
              (" (live file cache already patched)" if replacements else ""))
