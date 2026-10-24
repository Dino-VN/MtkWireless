#include "MtkMsbcStream.hpp"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>

static const uint16_t logicalRequested[15] = {33,33,33,33,33,33,33,33,33,33,33,33,33,33,33};
static const uint16_t wireRequested[15] = {63,63,63,63,63,63,63,63,63,63,63,63,63,63,63};
static const uint8_t sequence[4] = {0x08,0x38,0xc8,0xf8};
static const sbc_frame msbc = {true, SBC_FREQ_16K, SBC_MODE_MONO, SBC_BAM_LOUDNESS,15,8,26};

// Use Google's raw codec API independently of the stream and codec wrappers.
struct Reference {
    sbc_t encoder {}, decoder {};
    unsigned next {};
    Reference() {
        sbc_reset(&encoder); sbc_reset(&decoder);
        decoder.nchannels = 1; decoder.nblocks = 15; decoder.nsubbands = 8;
    }
    void encode(const int16_t *pcm, uint8_t *packet) {
        packet[0] = 0x21; packet[1] = 0; packet[2] = 60;
        packet[3] = 1; packet[4] = sequence[next++ & 3]; packet[62] = 0;
        assert(sbc_encode(&encoder, pcm, 1, nullptr, 0, &msbc, packet+5, SBC_MSBC_SIZE) == 0);
    }
    std::vector<int16_t> decode(const uint8_t *packet, unsigned count) {
        std::vector<int16_t> output(count*120);
        for (unsigned i = 0; i < count; ++i) {
            sbc_frame frame;
            assert(sbc_decode(&decoder, packet+i*63+5, SBC_MSBC_SIZE, &frame,
                              output.data()+i*120, 1, nullptr, 0) == 0);
        }
        return output;
    }
};

static void makePcm(unsigned tick, int16_t *pcm, uint8_t *logical) {
    for (unsigned i = 0; i < 240; ++i) {
        pcm[i] = int16_t((i*211 + tick*83) % 16000 - 8000);
        const unsigned byte = (i/48)*99 + 3 + (i%48)*2;
        logical[byte] = uint8_t(pcm[i]); logical[byte+1] = uint8_t(uint16_t(pcm[i]) >> 8);
    }
    for (unsigned i = 0; i < 5; ++i) {
        logical[i*99] = 0x21; logical[i*99+1] = 0; logical[i*99+2] = 96;
    }
}

static std::vector<int16_t> receive(MtkMsbcStream &stream, const uint8_t *slots,
                                  const uint16_t *completed) {
    uint8_t logical[495]; uint16_t done[15];
    assert(stream.receive(slots, MtkMsbcStream::kWireBytes, wireRequested, completed,
                          15, logical, sizeof(logical), done));
    unsigned length = 0;
    for (unsigned i = 0; i < 15; ++i) {
        assert(done[i] <= 33); length += done[i];
    }
    assert(length <= 486 && length%243 == 0);
    unsigned remaining = length;
    for (unsigned i = 0; i < 15; ++i) {
        const unsigned count = remaining < 33 ? remaining : 33;
        assert(done[i] == count); remaining -= count;
    }
    for (unsigned i = length; i < sizeof(logical); ++i) assert(logical[i] == 0);
    std::vector<int16_t> pcm;
    for (unsigned packet = 0; packet < length; packet += 243) {
        assert(logical[packet] == 0x21 && logical[packet+1] == 0 && logical[packet+2] == 240);
        for (unsigned sample = 0; sample < 120; ++sample)
            pcm.push_back(int16_t(uint16_t(logical[packet+3+sample*2]) |
                                   (uint16_t(logical[packet+4+sample*2]) << 8)));
    }
    return pcm;
}

int main() {
    static_assert(MtkMsbcStream::kBytes == 495, "native logical capacity changed");
    static_assert(MtkMsbcStream::kWirePacket == 63 && MtkMsbcStream::kWireBytes == 945,
                  "physical alt 6 capacity changed");
    uint8_t logical[495], wire[128], referenceWire[126], slots[945];
    uint16_t requested[15], completed[15]; int16_t pcm[240];
    MtkMsbcStream stream; stream.reset();
    Reference remote, expectedTx, expectedRx;
    unsigned previousStart = 0;
    for (unsigned tick = 0; tick < 1000; ++tick) {
        makePcm(tick, pcm, logical);
        memset(wire, 0xa5, sizeof(wire));
        assert(stream.transmit(logical, sizeof(logical), logicalRequested, 15,
                               wire, 126, requested));
        assert(wire[126] == 0xa5 && wire[127] == 0xa5);
        unsigned offset = 0;
        for (unsigned i = 0; i < 15; ++i) {
            assert(requested[i] == (i == 0 || i == 7 ? 63 : 0));
            if (!requested[i]) continue;
            const unsigned start = tick*15 + i;
            if (start) assert(start-previousStart == (i == 7 ? 7 : 8));
            previousStart = start;
            expectedTx.encode(pcm+offset/63*120, referenceWire+offset);
            offset += requested[i];
        }
        assert(offset == 126 && memcmp(wire, referenceWire, 126) == 0);

        // Remote RX is independent of our TX wrapper; bytes outside completion
        // counts are poisoned so the physical stride is exercised explicitly.
        remote.encode(pcm, referenceWire); remote.encode(pcm+120, referenceWire+63);
        memset(slots, 0xff, sizeof(slots)); memset(completed, 0, sizeof(completed));
        memcpy(slots, referenceWire, 63); memcpy(slots+7*63, referenceWire+63, 63);
        completed[0] = completed[7] = 63;
        auto actual = receive(stream, slots, completed);
        assert(actual == expectedRx.decode(referenceWire, 2));
        assert(actual.size() == 240);
    }
    assert(stream.transmitDiagnostics().samples == 240000);
    assert(stream.receiveDiagnostics().decodedFrames == 2000);
    assert(stream.receiveDiagnostics().plcFrames == 0 && stream.receiveDiagnostics().crcFailures == 0);

    // Every legal completion count, including packets split across successive
    // USB calls, must preserve exactly the original 15x33 logical PCM contract.
    Reference encodeOne;
    makePcm(77, pcm, logical);
    encodeOne.encode(pcm, referenceWire); encodeOne.encode(pcm+120, referenceWire+63);
    for (unsigned chunk = 1; chunk <= 63; ++chunk) {
        MtkMsbcStream fragmented; fragmented.reset(); Reference reference;
        auto expected = reference.decode(referenceWire, 2);
        std::vector<int16_t> actual;
        for (unsigned offset = 0; offset < 126;) {
            memset(slots, 0xee, sizeof(slots)); memset(completed, 0, sizeof(completed));
            // A single occupied slot also exercises 0-byte completions between packets.
            const unsigned count = 126-offset < chunk ? 126-offset : chunk;
            memcpy(slots+11*63, referenceWire+offset, count); completed[11] = uint16_t(count);
            auto part = receive(fragmented, slots, completed);
            actual.insert(actual.end(), part.begin(), part.end()); offset += count;
        }
        assert(actual == expected);
    }
    puts("PASS: raw Google codec interop, 1,000 full-duplex alt6 batches, exact 7/8 ms TX schedule, contiguous 126-byte TX, 945-byte strided RX, completion counts 0..63, unchanged logical PCM shape");
}
