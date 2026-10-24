#include "../UsbMsbcBridge.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <thread>

namespace {
constexpr uint16_t Handle = 0x0042;
struct Pending {
    IOMemoryDescriptor *data;
    IOUSBHostIsochronousFrame *frames;
    IOUSBHostIsochronousCompletion completion;
    uint64_t first;
    bool receive;
};
struct Callback {
    IOUSBHostIsochronousFrame *frames;
    IOMemoryDescriptor *data;
    unsigned calls {};
    bool receive;
    static void done(void *owner, void *parameter, IOReturn status,
                     IOUSBHostIsochronousFrame *frames)
    {
        auto *value = static_cast<Callback *>(owner);
        assert(parameter == value && frames == value->frames && status == 0);
        assert(value->data->retainCount() >= 2);
        unsigned total = 0;
        for (unsigned i = 0; i < 15; ++i) {
            assert(frames[i].status == 0 && frames[i].requestCount == 33);
            assert(frames[i].reserved == 0x7922);
            assert(frames[i].completeCount == (value->receive && i == 14 ? 24 : 33));
            total += frames[i].completeCount;
        }
        assert(total == (value->receive ? 486 : 495));
        ++value->calls;
    }
};
struct Harness {
    UsbMsbcBridge *bridge = UsbMsbcBridge::create();
    IOUSBHostDevice *device = new IOUSBHostDevice;
    IOUSBHostInterface *interface = new IOUSBHostInterface(device);
    IOUSBHostPipe *out = new IOUSBHostPipe, *in = new IOUSBHostPipe;
    IOMemoryDescriptor *tx = new IOMemoryDescriptor(495), *rx = new IOMemoryDescriptor(495);
    IOUSBHostIsochronousFrame txFrames[15] {}, rxFrames[15] {};
    Callback txCallback {txFrames, tx, 0, false}, rxCallback {rxFrames, rx, 0, true};
    IOUSBHostIsochronousCompletion txCompletion {&txCallback, Callback::done, &txCallback};
    IOUSBHostIsochronousCompletion rxCompletion {&rxCallback, Callback::done, &rxCallback};
    MtkMsbcCodec referenceEncoder, referenceDecoder;
    int16_t pcm[240] {}, expected[240] {};
    std::array<uint8_t, 126> packed {};
    std::vector<Pending> pending;
    bool inlineTx {}, inlineRx {}, wireReady {};
    unsigned txSubmissions {}, rxSubmissions {};

    Harness()
    {
        assert(bridge);
        auto ticket = bridge->beginInterfaceChange();
        bridge->detach(ticket);
        assert(bridge->bindPhysicalInterface(interface, ticket));
        in->descriptor.bEndpointAddress = 0x83;
        assert(bridge->observePipe(interface, out) && bridge->observePipe(interface, in));
        bridge->setMsbcActive(true);
    }
    ~Harness()
    {
        assert(pending.empty() && bridge->quiescent());
        bridge->stop(); bridge->detach(); bridge->release();
        assert(tx->prepared == 0 && rx->prepared == 0);
        tx->release(); rx->release(); in->release(); out->release(); interface->release(); device->release();
    }
    void complete(Pending item)
    {
        for (unsigned i = 0; i < 15; ++i) {
            item.frames[i].status = 0;
            item.frames[i].timeStamp = item.first + i;
            item.frames[i].completeCount = item.receive ?
                (i == 0 || i == 7 ? 63 : 0) : item.frames[i].requestCount;
        }
        item.completion.action(item.completion.owner, item.completion.parameter, 0, item.frames);
        item.data->release();
    }
    void drain()
    {
        assert(pending.size() == 1);
        Pending item = pending.front(); pending.clear();
        std::thread completion([this, item] { complete(item); });
        completion.join();
    }
    void input(unsigned batch)
    {
        for (unsigned i = 0; i < 240; ++i) {
            const unsigned sample = batch * 240 + i;
            pcm[i] = int16_t(12000 * std::sin(2 * 3.14159265358979323846 * 997 * sample / 16000) +
                            4000 * std::sin(2 * 3.14159265358979323846 * 211 * sample / 16000));
        }
        for (unsigned packet = 0; packet < 5; ++packet) {
            auto *bytes = tx->bytes.data() + packet * 99;
            bytes[0] = uint8_t(Handle); bytes[1] = uint8_t(Handle >> 8); bytes[2] = 96;
            for (unsigned i = 0; i < 48; ++i) {
                uint16_t sample = uint16_t(pcm[packet * 48 + i]);
                bytes[3 + i * 2] = uint8_t(sample);
                bytes[4 + i * 2] = uint8_t(sample >> 8);
            }
        }
        for (unsigned i = 0; i < 15; ++i) {
            txFrames[i] = {}; rxFrames[i] = {};
            txFrames[i].requestCount = rxFrames[i].requestCount = 33;
            txFrames[i].reserved = rxFrames[i].reserved = 0x7922;
        }
        std::fill(rx->bytes.begin(), rx->bytes.end(), 0xa5);
        wireReady = false;
    }
    void captureTx(IOMemoryDescriptor *data, IOUSBHostIsochronousFrame *frames)
    {
        ++txSubmissions;
        unsigned requested = 0;
        for (unsigned i = 0; i < 15; ++i) {
            assert(frames[i].requestCount == (i == 0 || i == 7 ? 63 : 0));
            requested += frames[i].requestCount;
        }
        assert(requested == 126 && data->bytes.size() >= 126);
        std::copy_n(data->bytes.begin(), packed.size(), packed.begin());
        for (unsigned frame = 0; frame < 2; ++frame) {
            const auto *packet = packed.data() + frame * 63;
            assert(packet[0] == uint8_t(Handle) && packet[1] == uint8_t(Handle >> 8) && packet[2] == 60);
            uint8_t reference[60];
            assert(referenceEncoder.encode(pcm + frame * 120, 120, reference, 60));
            assert(!std::memcmp(reference, packet + 3, 60));
            unsigned missing = 99;
            assert(referenceDecoder.decode(reference, 60, expected + frame * 120, 120, &missing) && missing == 0);
        }
        wireReady = true;
    }
    void supplyRx(IOMemoryDescriptor *data, IOUSBHostIsochronousFrame *frames)
    {
        ++rxSubmissions;
        assert(wireReady && data->bytes.size() >= 945);
        // HostFamily stores each IN completion at requested-count strides.
        std::fill(data->bytes.begin(), data->bytes.end(), 0xe7);
        unsigned offset = 0;
        for (unsigned i = 0; i < 15; ++i) {
            assert(frames[i].requestCount == 63);
            unsigned count = i == 0 || i == 7 ? 63 : 0;
            std::copy_n(packed.begin() + offset, count, data->bytes.begin() + i * 63);
            offset += count;
        }
        assert(offset == packed.size());
    }
    void checkOutput()
    {
        for (unsigned frame = 0; frame < 2; ++frame) {
            const auto *packet = rx->bytes.data() + frame * 243;
            assert(packet[0] == uint8_t(Handle) && packet[1] == uint8_t(Handle >> 8) && packet[2] == 240);
            for (unsigned i = 0; i < 120; ++i) {
                int16_t actual = int16_t(uint16_t(packet[3 + i * 2]) | (uint16_t(packet[4 + i * 2]) << 8));
                assert(actual == expected[frame * 120 + i]);
            }
        }
        for (unsigned i = 486; i < 495; ++i) assert(rx->bytes[i] == 0);
    }
};
Harness *active;

IOReturn backend(IOUSBHostPipe *pipe, IOMemoryDescriptor *data, IOUSBHostIsochronousFrame *frames,
                 uint32_t count, uint64_t first, IOUSBHostIsochronousCompletion *completion)
{
    assert(active && count == 15 && completion && completion->action);
    bool receive = pipe == active->in;
    assert(receive || pipe == active->out);
    if (receive) active->supplyRx(data, frames);
    else active->captureTx(data, frames);
    data->retain();
    Pending item {data, frames, *completion, first, receive};
    if (receive ? active->inlineRx : active->inlineTx) active->complete(item);
    else active->pending.push_back(item);
    return 0;
}
}

int main()
{
    {
        Harness harness; active = &harness;
        for (unsigned batch = 0; batch < 1000; ++batch) {
            harness.input(batch);
            harness.inlineTx = batch & 1;
            harness.inlineRx = batch & 2;
            uint64_t first = 100 + batch * 15;
            assert(harness.bridge->io(harness.out, harness.tx, harness.txFrames, 15, first,
                                     &harness.txCompletion, backend) == 0);
            if (!harness.inlineTx) harness.drain();
            assert(harness.txCallback.calls == batch + 1 && harness.bridge->quiescent());
            assert(harness.bridge->io(harness.in, harness.rx, harness.rxFrames, 15, first,
                                     &harness.rxCompletion, backend) == 0);
            if (!harness.inlineRx) harness.drain();
            assert(harness.rxCallback.calls == batch + 1 && harness.bridge->quiescent());
            harness.checkOutput();
        }
        auto status = harness.bridge->status();
        assert(status.active && status.txBatches == 1000 && status.rxBatches == 1000);
        assert(status.errors == 0 && status.outOfOrder == 0 && status.inFlight == 0);
        assert(status.txErrors == 0 && status.rxErrors == 0);
        assert(status.txZeroRequestFrames == 13000 && status.txZeroRequestErrors == 0);
        assert(status.txAudio.samples == 240000 && status.txAudio.nonzeroSamples > 230000 && status.txAudio.peak > 12000);
        assert(status.rxAudio.decodedFrames == 2000 && status.rxAudio.plcFrames == 0 && status.rxAudio.crcFailures == 0);
        assert(harness.txSubmissions == 1000 && harness.rxSubmissions == 1000);
        active = nullptr;
    }
    assert(OSObject::live == 0);
    std::puts("PASS: 1000 real USB/stream/codec duplex batches, inline/async callbacks, exact H2/PCM, TX495/RX486 logical completions and zero leaked objects");
}
