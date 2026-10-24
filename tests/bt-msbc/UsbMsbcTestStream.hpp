#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Deliberately simple transformer: these tests exercise actual USB plumbing,
// not the codec/framing implementation, which has its own independent tests.
class MtkMsbcStream {
    unsigned sequence {};
public:
    enum { kWirePacket = 63, kWireBytes = 945 };
    struct TxDiagnostics { uint64_t samples {}, nonzeroSamples {}; uint32_t peak {}; };
    struct RxDiagnostics {
        uint64_t decodedFrames {}, plcFrames {}, crcFailures {}, discardedBytes {};
        uint32_t lastParserReason {};
    };
    TxDiagnostics transmitDiagnostics() const { return {}; }
    RxDiagnostics receiveDiagnostics() const { return {}; }
    struct TxState { unsigned sequence; };
    inline static bool failTx {}, failRx {};
    inline static unsigned txCalls {}, rxCalls {}, resets {};
    void reset() { ++resets; sequence = 0; }
    void resetTransmit() { sequence = 0; }
    void resetReceive() {}
    void saveTransmit(TxState &s) const { s.sequence = sequence; }
    void restoreTransmit(const TxState &s) { sequence = s.sequence; }
    bool transmit(const uint8_t *, size_t, const uint16_t *, size_t n,
        uint8_t *wire, size_t cap, uint16_t *requested) {
        ++txCalls;
        if (failTx) return false;
        for (size_t i = 0; i < n; ++i) requested[i] = 0;
        requested[0] = requested[7] = 63;
        if (cap < 126) return false;
        std::memset(wire, 0xa5, 126);
        wire[0] = static_cast<uint8_t>(sequence++);
        return true;
    }
    bool receive(const uint8_t *, size_t, const uint16_t *requested,
        const uint16_t *completed, size_t n, uint8_t *logical,
        size_t cap, uint16_t *out) {
        ++rxCalls;
        if (failRx || completed == out) return false;
        size_t offset = 0;
        for (size_t i = 0; i < n; ++i) {
            if (requested[i] != 63 || offset + 33 > cap) return false;
            std::memset(logical + offset, 0x5a, 33);
            out[i] = 33; offset += 33;
        }
        return true;
    }
};
