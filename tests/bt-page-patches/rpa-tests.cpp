// Runs the production x86_64 bytes with only native external calls/globals
// rebound to mocks. This tests policy and memory writes, not IRK cryptography
// or a controller. test-bt-page-patches.py validates the real call targets.
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
void rpa_clear(uint8_t *);
void rpa_scan_test(uint8_t *, uint8_t *);
void rpa_device_test(uint8_t *, uint8_t *, unsigned);
uint64_t rpa_decode_test(const uint8_t *);
uint64_t rpa_address_test(void *, void *, unsigned);
bool rpa_paired_test(void *, void *);
uint64_t rpa_original_selection(const uint8_t *, unsigned);
extern uint8_t *rpa_chip;
extern const uint8_t rpa_legacy_return, rpa_update_return;
}

using Typed = std::array<uint8_t, 7>;
const Typed identity {0, 0x10, 0xa2, 0xd3, 0x21, 0x31, 0xfe};
const Typed rpa1 {1, 0x74, 0xac, 0x69, 0xd1, 0x64, 0x95};
const Typed rpa2 {1, 0x6e, 0x42, 0x48, 0x27, 0x59, 0xd8};

template<class T> void put(uint8_t *p, T value) { memcpy(p, &value, sizeof(value)); }
uint64_t get(const uint8_t *p) { uint64_t v; memcpy(&v, p, sizeof(v)); return v; }
uint64_t encoded(const Typed &a) {
    uint64_t v = 0;
    for (uint8_t b : a) v = (v << 8) | b;
    return v;
}

unsigned resolveStatus, resolveCalls, sinkCalls, getterCalls, pairedCalls;
bool resolveMatch, pairedResult;
uint8_t *selectedDevice;
void *expectedManager, *expectedUuid;

extern "C" unsigned rpa_resolve_mock(const uint8_t *, uint8_t *out, uint8_t *matched) {
    ++resolveCalls;
    memcpy(out, identity.data(), identity.size());
    *matched = resolveMatch;
    return resolveStatus;
}
extern "C" void rpa_scan_sink(uint8_t *) { ++sinkCalls; }
extern "C" uint64_t rpa_getter_mock(void *manager, void *uuid, unsigned advertised) {
    ++getterCalls;
    assert(manager == expectedManager && uuid == expectedUuid && advertised <= 1);
    if (!selectedDevice) return 0;
    // The original native getter's checked selection, also executed separately.
    return rpa_original_selection(selectedDevice, advertised);
}
extern "C" bool rpa_paired_mock(void *manager, void *uuid) {
    ++pairedCalls;
    assert(manager == expectedManager && uuid == expectedUuid);
    return pairedResult;
}

struct Scan {
    std::vector<uint8_t> descriptor = std::vector<uint8_t>(0x50, 0x5a);
    std::vector<uint8_t> frameStorage = std::vector<uint8_t>(0x80);
    uint8_t *frame() { return frameStorage.data() + 0x50; }
    Scan() { put(frame() + 8, &rpa_legacy_return); }
    void address(const Typed &a) { memcpy(descriptor.data() + 1, a.data(), a.size()); }
    void clear() {
        const auto before = descriptor;
        const unsigned old = sinkCalls;
        rpa_clear(descriptor.data());
        assert(sinkCalls == old + 1);
        auto expected = before;
        memset(expected.data() + 0x33, 0, 7);
        assert(descriptor == expected); // including the live byte at +0x3a
    }
};

struct Record {
    std::vector<uint8_t> device = std::vector<uint8_t>(0x268, 0xa5);
    std::vector<uint8_t> advertisement = std::vector<uint8_t>(0x208, 0x5a);
    std::vector<uint8_t> parent = std::vector<uint8_t>(0x500);
    std::array<uint8_t, 16> setter {};
    std::array<uint8_t, 16> owner {};
    Record() {
        put(owner.data() + 8, advertisement.data());
        put(parent.data(), owner.data()); // parent frame's native -0x480 slot
        put(setter.data(), parent.data() + 0x480);
        put(setter.data() + 8, &rpa_update_return);
        put(device.data() + 0x10, encoded(identity));
        put(device.data() + 0x18, uint64_t(0));
    }
    void marker(const Typed &rpa) { put(advertisement.data() + 0xd0, encoded(rpa) | (uint64_t(1) << 55)); }
};

void scanTests() {
    for (unsigned type = 0; type < 256; ++type) {
        for (uint8_t first : {uint8_t(0x00), uint8_t(0x40), uint8_t(0x80), uint8_t(0xc0)}) {
            Scan scan;
            Typed address = rpa1; address[0] = type; address[1] = first;
            scan.descriptor[0] = 0;
            scan.address(address); scan.clear();
            const auto before = scan.descriptor;
            resolveStatus = 0; resolveMatch = true; resolveCalls = 0;
            rpa_scan_test(scan.descriptor.data(), scan.frame());
            const bool resolvable = type == 1 && first == 0x40;
            assert(resolveCalls == unsigned(resolvable));
            auto expected = before;
            if (resolvable) {
                memcpy(expected.data() + 1, identity.data(), 7);
                memcpy(expected.data() + 0x33, address.data(), 7);
                expected[0x33] = 0x81;
            }
            assert(scan.descriptor == expected);
        }
    }
    for (unsigned status = 0; status < 256; ++status) {
        for (bool match : {false, true}) {
            Scan scan; scan.descriptor[0] = 0; scan.address(rpa1); scan.clear();
            const auto before = scan.descriptor;
            resolveStatus = status; resolveMatch = match; resolveCalls = 0;
            rpa_scan_test(scan.descriptor.data(), scan.frame());
            auto expected = before;
            if (!status && match) {
                memcpy(expected.data() + 1, identity.data(), 7);
                memcpy(expected.data() + 0x33, rpa1.data(), 7); expected[0x33] = 0x81;
            }
            assert(resolveCalls == 1 && scan.descriptor == expected);
        }
    }
    // Extended/vendor callers and every legacy non-ADV_IND event keep the
    // native direct address; they may still resolve the primary identity.
    for (unsigned event = 0; event < 256; ++event) {
        for (bool legacy : {false, true}) {
            Scan scan; scan.descriptor[0] = event; scan.address(rpa1);
            put(scan.frame() + 8, legacy ? &rpa_legacy_return : &rpa_update_return);
            const auto before = scan.descriptor;
            resolveStatus = 0; resolveMatch = true;
            rpa_scan_test(scan.descriptor.data(), scan.frame());
            auto expected = before;
            memcpy(expected.data() + 1, identity.data(), 7);
            if (legacy && event == 0) {
                memcpy(expected.data() + 0x33, rpa1.data(), 7); expected[0x33] = 0x81;
            }
            assert(scan.descriptor == expected);
        }
    }
    // A following unmatched/public report cannot inherit a previous marker.
    Scan scan; scan.descriptor[0] = 0; scan.address(rpa1); scan.clear();
    resolveStatus = 0; resolveMatch = true;
    rpa_scan_test(scan.descriptor.data(), scan.frame());
    assert(scan.descriptor[0x33] == 0x81);
    scan.address(identity); scan.clear();
    rpa_scan_test(scan.descriptor.data(), scan.frame());
    for (unsigned i = 0; i < 7; ++i) assert(scan.descriptor[0x33 + i] == 0);
}

void decodeTests() {
    std::vector<uint8_t> parsed(0x1770);
    std::array<uint8_t, 8> direct {};
    put(parsed.data() + 0x1768, direct.data());
    for (unsigned type = 0; type < 256; ++type) {
        for (unsigned flag : {0u, 1u, 0x81u, 0xffu}) {
            memcpy(direct.data(), rpa1.data(), 7); direct[0] = type; direct[7] = flag;
            Typed typed; memcpy(typed.data(), direct.data(), 7);
            const uint64_t expected = type <= 1 || type == 0x81 ? encoded(typed) : 0;
            assert(rpa_decode_test(parsed.data()) == expected);
        }
    }
    // The internal marker must not widen native directed/nonconnectable/
    // extended report acceptance or admit a non-RPA random address.
    for (unsigned kind = 0; kind < 256; ++kind) for (unsigned extended : {0u, 1u, 0xffu})
    for (uint8_t first : {uint8_t(0x00), uint8_t(0x40), uint8_t(0x80), uint8_t(0xc0)}) {
        parsed[0] = kind; parsed[0x1756] = extended;
        memcpy(direct.data(), rpa1.data(), 7); direct[0] = 0x81; direct[1] = first;
        Typed typed; memcpy(typed.data(), direct.data(), 7);
        assert(rpa_decode_test(parsed.data()) ==
               (kind == 0 && extended == 0 && first == 0x40 ? encoded(typed) : 0));
    }
}

void recordTests() {
    for (bool flag : {false, true}) {
        Record record; record.marker(rpa1);
        const auto before = record.device;
        rpa_device_test(record.device.data(), record.setter.data(), flag);
        auto expected = before;
        put(expected.data() + 0x10, encoded(rpa1)); put(expected.data() + 0x18, encoded(identity));
        expected[0x61] = flag;
        assert(record.device == expected && get(record.advertisement.data() + 0xd0) == 0);
        record.marker(rpa2);
        rpa_device_test(record.device.data(), record.setter.data(), flag);
        put(expected.data() + 0x10, encoded(rpa2));
        assert(record.device == expected); // identity survives RPA rotation
    }
    // A valid directed address, an absent marker or another type stays native.
    for (unsigned type = 0; type < 256; ++type) {
        if (type == 0x81) continue;
        Record record; Typed direct = rpa1; direct[0] = type;
        const auto address = encoded(direct); put(record.advertisement.data() + 0xd0, address);
        auto expected = record.device; expected[0x61] = 1;
        rpa_device_test(record.device.data(), record.setter.data(), 1);
        assert(record.device == expected && get(record.advertisement.data() + 0xd0) == address);
    }
    // Existing bool callers never read the parent frame, even for a null
    // device, or for a bool register with unspecified upper bits.
    for (unsigned flag : {0u, 1u, 0x80000001u, 0xffffffffu}) {
        Record record; record.marker(rpa1);
        put(record.setter.data() + 8, &rpa_legacy_return);
        put(record.setter.data(), uintptr_t(1));
        auto expected = record.device; expected[0x61] = uint8_t(flag);
        rpa_device_test(record.device.data(), record.setter.data(), flag);
        assert(record.device == expected && get(record.advertisement.data() + 0xd0) != 0);
        rpa_device_test(nullptr, record.setter.data(), flag);
    }
    Record removed; removed.marker(rpa1);
    rpa_device_test(nullptr, removed.setter.data(), 1);
    assert(get(removed.advertisement.data() + 0xd0) == 0); // no marker escapes
}

void controllerTests() {
    std::vector<uint8_t> chip(0x338);
    Record record;
    expectedManager = chip.data(); expectedUuid = record.owner.data(); selectedDevice = record.device.data();
    for (unsigned id : {0u, 1u, 3u, 19u, 25u, 2001u, 3999u, 5000u, 5999u, 6000u, 0xffffffffu}) {
        for (bool present : {false, true}) {
            rpa_chip = present ? chip.data() : nullptr; put(chip.data() + 0x334, id);
            const bool fallback = present && id == 0;
            Scan scan; scan.descriptor[0] = 0; scan.address(rpa1); scan.clear();
            resolveStatus = 0; resolveMatch = true;
            rpa_scan_test(scan.descriptor.data(), scan.frame());
            assert(scan.descriptor[0x33] == (fallback ? 0x81 : 0));

            Record update; update.marker(rpa1); auto expected = update.device; expected[0x61] = 1;
            if (fallback) {
                put(expected.data() + 0x10, encoded(rpa1)); put(expected.data() + 0x18, encoded(identity));
            }
            rpa_device_test(update.device.data(), update.setter.data(), 1);
            assert(update.device == expected && get(update.advertisement.data() + 0xd0) == 0);

            put(record.device.data() + 0x10, encoded(rpa1)); put(record.device.data() + 0x18, encoded(identity));
            getterCalls = 0;
            assert(rpa_address_test(expectedManager, expectedUuid, 0xffffffffu) ==
                   encoded(fallback ? rpa1 : identity));
            assert(getterCalls == 1);
            put(record.device.data() + 0x18, uint64_t(0));
            assert(rpa_address_test(expectedManager, expectedUuid, 0) == encoded(rpa1));
            selectedDevice = nullptr;
            assert(rpa_address_test(expectedManager, expectedUuid, 0) == 0);
            selectedDevice = record.device.data();

            for (bool paired : {false, true}) {
                pairedCalls = 0; pairedResult = paired;
                assert(rpa_paired_test(expectedManager, expectedUuid) == (!fallback && paired));
                assert(pairedCalls == unsigned(!fallback));
            }
        }
    }
}

int main() {
    std::vector<uint8_t> chip(0x338);
    rpa_chip = chip.data(); // chip 0, for the first semantic groups
    scanTests(); decodeTests(); recordTests(); controllerTests();
}
