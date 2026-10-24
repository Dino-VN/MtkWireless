#pragma once
#include "MtkMsbcCodec.hpp"

// Tahoe keeps 15x33-byte PCM requests; physical alt 6 carries whole SCO packets.
class MtkMsbcStream {
public:
    enum { kFrames = 15, kBytes = 495, kWirePacket = 63, kWireBytes = kFrames * kWirePacket };
    // Lifetime totals include attempted TX submissions and survive session resets.
    struct TxDiagnostics {
        uint64_t samples {}, nonzeroSamples {};
        uint32_t peak {};
    };
    enum ParserReason {
        ParserNone, ParserArguments, ParserFrameShape, ParserQueueFull,
        ParserConcealFailed, ParserHandleChanged, ParserHciHeader, ParserH2Prefix
    };
    struct RxDiagnostics {
        uint64_t decodedFrames {}, plcFrames {}, crcFailures {}, discardedBytes {};
        uint32_t lastParserReason {};
    };
    TxDiagnostics transmitDiagnostics() const { return txDiagnostics_; }
    RxDiagnostics receiveDiagnostics() const { return rxDiagnostics_; }
    struct TxState {
        MtkMsbcCodec::TxState codec;
        uint16_t handle;
        bool started;
    };
    void reset();
    void resetTransmit();
    void resetReceive();
    void saveTransmit(TxState &state) const;
    void restoreTransmit(const TxState &state);
    bool transmit(const uint8_t *logical, size_t logicalCap,
        const uint16_t *requested, size_t frameCount,
        uint8_t *wire, size_t wireCap, uint16_t *wireRequested);
    bool receive(const uint8_t *wire, size_t wireCap,
        const uint16_t *requested, const uint16_t *completed, size_t frameCount,
        uint8_t *logical, size_t logicalCap, uint16_t *logicalCompleted);

private:
    enum { kQueue = 8 };
    MtkMsbcCodec codec_;
    TxDiagnostics txDiagnostics_;
    RxDiagnostics rxDiagnostics_;
    uint16_t txHandle_ {};
    bool txStarted_ {};
    uint8_t header_[3] {}, headerBytes_ {};
    unsigned payloadLeft_ {};
    uint16_t rxHandle_ {};
    bool damaged_ {}, discardPayload_ {}, headerAligned_ {true};
    uint64_t h2DamageMask_ {};
    uint8_t h2_[60] {};
    unsigned h2Bytes_ {}, rxExpected_ {};
    bool rxStarted_ {};
    uint64_t rxTick_ {}, lastFrameTick_ {};
    int16_t queue_[kQueue][120] {};
    unsigned head_ {}, queued_ {};
    void receiveByte(uint8_t byte);
    void receivePayload(uint8_t byte);
    bool pushFrame(bool conceal);
};
