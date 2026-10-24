#include "MtkMsbcStream.hpp"
#include <string.h>

static unsigned h2Sequence(uint8_t byte)
{
    const uint8_t sequence[] = {0x08, 0x38, 0xc8, 0xf8};
    for (unsigned i = 0; i < 4; ++i) if (byte == sequence[i]) return i;
    return 4;
}

void MtkMsbcStream::reset()
{
    resetTransmit();
    resetReceive();
}

void MtkMsbcStream::resetTransmit()
{
    codec_.resetTransmit();
    txHandle_ = 0;
    txStarted_ = false;
}

void MtkMsbcStream::resetReceive()
{
    codec_.resetReceive();
    rxHandle_ = 0;
    rxStarted_ = damaged_ = discardPayload_ = false;
    h2DamageMask_ = 0;
    headerAligned_ = true;
    headerBytes_ = payloadLeft_ = h2Bytes_ = rxExpected_ = head_ = queued_ = 0;
    rxTick_ = lastFrameTick_ = 0;
}

void MtkMsbcStream::saveTransmit(TxState &state) const
{
    codec_.saveTransmit(state.codec);
    state.handle = txHandle_;
    state.started = txStarted_;
}

void MtkMsbcStream::restoreTransmit(const TxState &state)
{
    codec_.restoreTransmit(state.codec);
    txHandle_ = state.handle;
    txStarted_ = state.started;
}

bool MtkMsbcStream::transmit(const uint8_t *logical, size_t logicalCap,
    const uint16_t *requested, size_t frameCount,
    uint8_t *wire, size_t wireCap, uint16_t *wireRequested)
{
    if (!logical || !requested || !wire || !wireRequested ||
        frameCount != kFrames || logicalCap < kBytes || wireCap < 2 * kWirePacket) return false;
    for (unsigned i = 0; i < kFrames; ++i) if (requested[i] != 33) return false;
    const uint16_t handle = uint16_t(logical[0] | (uint16_t(logical[1]) << 8));
    if (handle > 0x0eff) return false;
    for (unsigned i = 0; i < 5; ++i) {
        const auto *packet = logical + i * 99;
        if (packet[0] != uint8_t(handle) || packet[1] != uint8_t(handle >> 8) ||
            packet[2] != 96) return false;
    }
    int16_t pcm[240];
    for (unsigned i = 0; i < 240; ++i) {
        const unsigned byte = (i / 48) * 99 + 3 + (i % 48) * 2;
        pcm[i] = int16_t(uint16_t(logical[byte]) | (uint16_t(logical[byte + 1]) << 8));
        ++txDiagnostics_.samples;
        if (pcm[i]) ++txDiagnostics_.nonzeroSamples;
        const uint32_t magnitude = pcm[i] < 0 ? uint32_t(-int32_t(pcm[i])) : uint32_t(pcm[i]);
        if (magnitude > txDiagnostics_.peak) txDiagnostics_.peak = magnitude;
    }
    if (!txStarted_ || txHandle_ != handle) {
        codec_.resetTransmit();
        txHandle_ = handle;
        txStarted_ = true;
    }
    for (unsigned i = 0; i < 2; ++i) {
        auto *packet = wire + i * kWirePacket;
        packet[0] = uint8_t(handle);
        packet[1] = uint8_t(handle >> 8);
        packet[2] = 60;
        if (!codec_.encode(pcm + i * 120, 120, packet + 3, 60)) return false;
    }
    memset(wireRequested, 0, kFrames * sizeof(*wireRequested));
    wireRequested[0] = wireRequested[7] = kWirePacket;
    return true;
}

bool MtkMsbcStream::pushFrame(bool conceal)
{
    // A damaged first frame cannot establish which SCO handle owns the call.
    if (conceal && !rxStarted_) return false;
    if (queued_ == kQueue) {
        rxDiagnostics_.lastParserReason = ParserQueueFull;
        // Keep at most 60 ms of PCM after a late callback; stale audio must
        // not permanently disable capture or grow the call's latency.
        head_ = (head_ + 1) % kQueue;
        --queued_;
    }
    auto *pcm = queue_[(head_ + queued_) % kQueue];
    bool good = conceal ? codec_.conceal(pcm, 120) : codec_.decode(h2_, 60, pcm, 120);
    if (!conceal) {
        if (good) ++rxDiagnostics_.decodedFrames;
        else {
            ++rxDiagnostics_.crcFailures;
            if (!rxStarted_) { codec_.resetReceive(); return false; }
            conceal = true; good = codec_.conceal(pcm, 120);
        }
    }
    if (!good) {
        rxDiagnostics_.lastParserReason = ParserConcealFailed;
        codec_.resetReceive();
        memset(pcm, 0, 120 * sizeof(*pcm));
        conceal = true;
    }
    if (conceal) ++rxDiagnostics_.plcFrames;
    ++queued_;
    return true;
}

void MtkMsbcStream::receivePayload(uint8_t byte)
{
    if (damaged_) h2DamageMask_ |= uint64_t(1) << h2Bytes_;
    h2_[h2Bytes_++] = byte;
    // Search only for the complete fixed mSBC prefix; corrupt SCO boundaries
    // must not turn arbitrary radio bytes into PCM samples.
    while ((h2Bytes_ >= 1 && h2_[0] != 1) ||
           (h2Bytes_ >= 2 && h2Sequence(h2_[1]) == 4) ||
           (h2Bytes_ >= 3 && h2_[2] != 0xad) ||
           (h2Bytes_ >= 4 && h2_[3] != 0) ||
           (h2Bytes_ >= 5 && h2_[4] != 0)) {
        ++rxDiagnostics_.discardedBytes;
        rxDiagnostics_.lastParserReason = ParserH2Prefix;
        --h2Bytes_;
        memmove(h2_, h2_ + 1, h2Bytes_);
        h2DamageMask_ >>= 1;
    }
    if (h2Bytes_ != 60) return;
    const unsigned sequence = h2Sequence(h2_[1]);
    unsigned missing = rxStarted_ ? (sequence - rxExpected_) & 3 : 0;
    // Two sequence bits alone cannot distinguish a duplicate from three lost
    // frames. A duplicate arriving inside 20 ms cannot be a 30 ms loss gap.
    if (missing == 3 && rxTick_ - lastFrameTick_ < 20) {
        h2Bytes_ = 0;
        h2DamageMask_ = 0;
        return;
    }
    while (missing--) pushFrame(true);
    if (pushFrame(h2DamageMask_ != 0)) {
        rxExpected_ = (sequence + 1) & 3;
        rxStarted_ = true;
        lastFrameTick_ = rxTick_;
    }
    h2Bytes_ = 0;
    h2DamageMask_ = 0;
}

void MtkMsbcStream::receiveByte(uint8_t byte)
{
    if (payloadLeft_) {
        if (discardPayload_) ++rxDiagnostics_.discardedBytes;
        else receivePayload(byte);
        --payloadLeft_;
        return;
    }
    header_[headerBytes_++] = byte;
    if (headerBytes_ < 3) return;
    const uint16_t raw = uint16_t(header_[0] | (uint16_t(header_[1]) << 8));
    const uint16_t handle = raw & 0x0fff;
    if ((raw & 0xc000) || handle > 0x0eff || header_[2] > 240) {
        ++rxDiagnostics_.discardedBytes;
        rxDiagnostics_.lastParserReason = ParserHciHeader;
        header_[0] = header_[1]; header_[1] = header_[2]; headerBytes_ = 2;
        headerAligned_ = false;
        h2Bytes_ = 0;
        h2DamageMask_ = 0;
        return;
    }
    if (rxStarted_ && handle != rxHandle_) {
        rxDiagnostics_.lastParserReason = ParserHandleChanged;
        if (headerAligned_) {
            rxDiagnostics_.discardedBytes += 3;
            // A framed foreign payload must never be searched for our handle.
            payloadLeft_ = header_[2];
            headerBytes_ = 0;
            discardPayload_ = true;
        } else {
            ++rxDiagnostics_.discardedBytes;
            header_[0] = header_[1]; header_[1] = header_[2]; headerBytes_ = 2;
        }
        h2Bytes_ = 0;
        h2DamageMask_ = 0;
        return;
    }
    if (handle != rxHandle_) {
        h2Bytes_ = 0;
        h2DamageMask_ = 0;
    }
    rxHandle_ = handle;
    headerAligned_ = true;
    discardPayload_ = false;
    damaged_ = (raw & 0x3000) != 0;
    payloadLeft_ = header_[2];
    headerBytes_ = 0;
}

bool MtkMsbcStream::receive(const uint8_t *wire, size_t wireCap,
    const uint16_t *requested, const uint16_t *completed, size_t frameCount,
    uint8_t *logical, size_t logicalCap, uint16_t *logicalCompleted)
{
    if (!wire || !requested || !completed || !logical || !logicalCompleted ||
        frameCount != kFrames || wireCap < kWireBytes || logicalCap < kBytes) {
        rxDiagnostics_.lastParserReason = ParserArguments;
        return false;
    }
    for (unsigned i = 0; i < kFrames; ++i)
        if (requested[i] != kWirePacket || completed[i] > kWirePacket) {
            rxDiagnostics_.lastParserReason = ParserFrameShape;
            return false;
        }
    memset(logical, 0, kBytes);
    memset(logicalCompleted, 0, kFrames * sizeof(*logicalCompleted));
    for (unsigned i = 0; i < kFrames; ++i) {
        for (unsigned j = 0; j < completed[i]; ++j) receiveByte(wire[i * kWirePacket + j]);
        ++rxTick_;
    }
    unsigned output = 0;
    for (unsigned packet = 0; packet < 2 && queued_; ++packet) {
        logical[output++] = uint8_t(rxHandle_);
        logical[output++] = uint8_t(rxHandle_ >> 8);
        logical[output++] = 240;
        const auto *pcm = queue_[head_];
        for (unsigned sample = 0; sample < 120; ++sample) {
            logical[output++] = uint8_t(pcm[sample]);
            logical[output++] = uint8_t(uint16_t(pcm[sample]) >> 8);
        }
        head_ = (head_ + 1) % kQueue;
        --queued_;
    }
    for (unsigned i = 0; i < kFrames; ++i) {
        const unsigned count = output > 33 ? 33 : output;
        logicalCompleted[i] = uint16_t(count);
        output -= count;
    }
    return true;
}
