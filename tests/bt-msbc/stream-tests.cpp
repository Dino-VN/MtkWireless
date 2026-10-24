#include "MtkMsbcStream.hpp"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <vector>

static const uint16_t requested[15] = {33,33,33,33,33,33,33,33,33,33,33,33,33,33,33};
static const uint16_t physicalRequested[15] = {63,63,63,63,63,63,63,63,63,63,63,63,63,63,63};
static void logicalPcm(uint8_t *logical, unsigned seed)
{
    for (unsigned packet = 0; packet < 5; ++packet) {
        auto *p = logical + packet * 99;
        p[0] = 0x21; p[1] = 0; p[2] = 96;
        for (unsigned i = 0; i < 48; ++i) {
            seed = seed * 1664525 + 1013904223;
            int16_t value = int16_t((seed >> 18) - 8192);
            p[3+i*2] = uint8_t(value); p[4+i*2] = uint8_t(uint16_t(value) >> 8);
        }
    }
}

static std::vector<int16_t> receive(MtkMsbcStream &stream, const uint8_t *bytes, size_t size,
                                   unsigned maxChunk = 63)
{
    assert(size <= 15 * maxChunk);
    uint8_t wire[MtkMsbcStream::kWireBytes] {}, logical[495];
    uint16_t complete[15] {}, output[15];
    size_t off = 0;
    for (unsigned i = 0; i < 15 && off < size; ++i) {
        size_t count = size-off < maxChunk ? size-off : maxChunk;
        memcpy(wire+i*MtkMsbcStream::kWirePacket, bytes+off, count); complete[i] = uint16_t(count); off += count;
    }
    assert(stream.receive(wire, sizeof(wire), physicalRequested, complete, 15,
                          logical, sizeof(logical), output));
    unsigned length = 0;
    for (unsigned i = 0; i < 15; ++i) { assert(output[i] <= 33); length += output[i]; }
    assert(length % 243 == 0);
    std::vector<int16_t> result;
    for (unsigned i = 0; i < length; i += 243) {
        assert(logical[i] == 0x21 && logical[i+1] == 0 && logical[i+2] == 240);
        for (unsigned j = 0; j < 120; ++j)
            result.push_back(int16_t(uint16_t(logical[i+3+j*2]) | (uint16_t(logical[i+4+j*2]) << 8)));
    }
    return result;
}

int main()
{
    {
        MtkMsbcStream diagnostic; diagnostic.reset();
        uint8_t input[495] {}, encoded[495]; uint16_t sizes[15];
        for (unsigned i = 0; i < 5; ++i) { input[i * 99] = 0x21; input[i * 99 + 2] = 96; }
        input[4] = 0x80;
        assert(diagnostic.transmit(input, 495, requested, 15, encoded, 495, sizes));
        auto tx = diagnostic.transmitDiagnostics();
        assert(tx.samples == 240 && tx.nonzeroSamples == 1 && tx.peak == 32768);
        auto good = receive(diagnostic, encoded, 126);
        assert(good.size() == 240 && diagnostic.receiveDiagnostics().decodedFrames == 2);
        diagnostic.reset(); // Diagnostics cover the object's lifetime, including session resets.
        encoded[8] ^= 0xff;
        assert(receive(diagnostic, encoded, 63).empty());
        auto rx = diagnostic.receiveDiagnostics();
        assert(rx.decodedFrames == 2 && rx.plcFrames == 0 && rx.crcFailures == 1);
        uint8_t invalid[3] = {0xff, 0xff, 0xff};
        assert(receive(diagnostic, invalid, 3).empty());
        rx = diagnostic.receiveDiagnostics();
        assert(rx.discardedBytes == 1 && rx.lastParserReason == MtkMsbcStream::ParserHciHeader);
        assert(diagnostic.transmitDiagnostics().samples == 240);
    }
    uint8_t logical[495], wire[495], repeated[495];
    uint16_t counts[15], repeatedCounts[15];
    logicalPcm(logical, 23);
    MtkMsbcStream encoder; encoder.reset();
    MtkMsbcStream::TxState before;
    encoder.saveTransmit(before);
    assert(encoder.transmit(logical, 495, requested, 15, wire, 495, counts));
    unsigned length = 0;
    for (unsigned i = 0; i < 15; ++i) {
        assert(counts[i] == (i == 0 || i == 7 ? 63 : 0));
        length += counts[i];
    }
    assert(length == 126 && wire[2] == 60 && wire[65] == 60);
    encoder.restoreTransmit(before);
    assert(encoder.transmit(logical, 495, requested, 15, repeated, 495, repeatedCounts));
    assert(!memcmp(wire, repeated, 126));
    encoder.restoreTransmit(before);
    logical[101] = 95;
    assert(!encoder.transmit(logical, 495, requested, 15, repeated, 495, repeatedCounts));
    logical[101] = 96;
    assert(!encoder.transmit(logical, 494, requested, 15, repeated, 495, repeatedCounts));
    assert(!encoder.transmit(logical, 495, requested, 14, repeated, 495, repeatedCounts));
    assert(encoder.transmit(logical, 495, requested, 15, repeated, 495, repeatedCounts));
    assert(!memcmp(wire, repeated, 126));

    MtkMsbcCodec reference;
    int16_t pcm[240];
    assert(reference.decode(wire+3, 60, pcm, 120));
    assert(reference.decode(wire+66, 60, pcm+120, 120));
    // Every possible HCI header/payload boundary across successive USB calls.
    for (unsigned split = 0; split <= 126; ++split) {
        MtkMsbcStream decoder; decoder.reset();
        auto first = receive(decoder, wire, split);
        auto second = receive(decoder, wire+split, 126-split);
        first.insert(first.end(), second.begin(), second.end());
        assert(first.size() == 240 && !memcmp(first.data(), pcm, sizeof(pcm)));
    }
    // Long continuous duplex stream: codec filter history and H2 wrap survive.
    MtkMsbcStream duplex; duplex.reset();
    MtkMsbcCodec expected;
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
        logicalPcm(logical, iteration);
        assert(duplex.transmit(logical, 495, requested, 15, wire, 495, counts));
        auto result = receive(duplex, wire, 126, 9 + iteration % 25);
        assert(expected.decode(wire+3, 60, pcm, 120));
        assert(expected.decode(wire+66, 60, pcm+120, 120));
        assert(result.size() == 240 && !memcmp(result.data(), pcm, sizeof(pcm)));
    }
    // Duplicate packets are dropped; a sequence gap inserts exactly one PLC frame.
    MtkMsbcStream lossEncoder, lossDecoder;
    lossEncoder.reset(); lossDecoder.reset();
    logicalPcm(logical, 99);
    assert(lossEncoder.transmit(logical, 495, requested, 15, wire, 495, counts));
    auto first = receive(lossDecoder, wire, 63);
    assert(first.size() == 120);
    assert(receive(lossDecoder, wire, 63).empty());
    assert(lossEncoder.transmit(logical, 495, requested, 15, repeated, 495, counts));
    auto gap = receive(lossDecoder, repeated, 63);
    assert(gap.size() == 240);
    // Preserve packet-status damage across HCI fragmentation of one H2 unit.
    uint8_t fragments[66];
    fragments[0] = 0x21; fragments[1] = 0x10; fragments[2] = 30;
    memcpy(fragments+3, wire+66, 30);
    fragments[33] = 0x21; fragments[34] = 0; fragments[35] = 30;
    memcpy(fragments+36, wire+96, 30);
    lossDecoder.reset();
    assert(receive(lossDecoder, wire, 63).size() == 120);
    const auto plcBeforeDamage = lossDecoder.receiveDiagnostics().plcFrames;
    auto damaged = receive(lossDecoder, fragments, sizeof(fragments));
    assert(damaged.size() == 120);
    assert(lossDecoder.receiveDiagnostics().plcFrames == plcBeforeDamage + 1);
    // Three H2 units coalesced in one HCI packet preserve the third until next IO.
    uint8_t coalesced[183];
    coalesced[0] = 0x21; coalesced[1] = 0; coalesced[2] = 180;
    memcpy(coalesced+3, wire+3, 60);
    memcpy(coalesced+63, wire+66, 60);
    memcpy(coalesced+123, repeated+3, 60);
    lossDecoder.reset();
    assert(receive(lossDecoder, coalesced, sizeof(coalesced)).size() == 240);
    assert(receive(lossDecoder, nullptr, 0).size() == 120);
    assert(receive(lossDecoder, nullptr, 0).empty());
    // CRC corruption emits concealed PCM rather than compressed bytes or noise.
    lossDecoder.reset();
    assert(receive(lossDecoder, wire, 63).size() == 120);
    wire[71] ^= 0xff;
    const auto plcBeforeCrc = lossDecoder.receiveDiagnostics().plcFrames;
    const auto crcBefore = lossDecoder.receiveDiagnostics().crcFailures;
    auto corrupt = receive(lossDecoder, wire + 63, 63);
    assert(corrupt.size() == 120);
    assert(lossDecoder.receiveDiagnostics().plcFrames == plcBeforeCrc + 1);
    assert(lossDecoder.receiveDiagnostics().crcFailures == crcBefore + 1);
    // Malformed USB lengths are rejected before changing RX state.
    uint8_t blank[MtkMsbcStream::kWireBytes] {}, out[495];
    uint16_t complete[15] {}, done[15];
    complete[0] = 64;
    assert(!lossDecoder.receive(blank, sizeof(blank), physicalRequested, complete, 15, out, 495, done));
    assert(!lossDecoder.receive(blank, sizeof(blank)-1, physicalRequested, physicalRequested, 15, out, 495, done));
    assert(!lossDecoder.receive(blank, sizeof(blank), requested, complete, 15, out, 495, done));
    // Untrusted controller bytes: bounded parser/queue under sanitizers.
    unsigned random = 7;
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
        MtkMsbcStream fuzz; fuzz.reset();
        for (unsigned j = 0; j < sizeof(blank); ++j) { random = random*1664525+1013904223; blank[j] = uint8_t(random >> 24); }
        assert(fuzz.receive(blank, sizeof(blank), physicalRequested, physicalRequested, 15, out, 495, done));
    }
    puts("PASS: 1,000 duplex batches, all 127 HCI split boundaries, alt6 strided USB, TX rollback, malformed input, duplicate/loss/CRC handling");
}
