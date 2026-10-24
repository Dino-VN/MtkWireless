#include "../UsbMsbcBridge.hpp"
#include "../UsbMsbcScope.hpp"
#include <cstdio>
#include <condition_variable>
#include <thread>

namespace {
struct Pending {
    IOMemoryDescriptor *data;
    IOUSBHostIsochronousFrame *frames;
    uint32_t count;
    IOUSBHostIsochronousCompletion completion;
};
enum Mode { Queue, Inline, Reject };
Mode mode = Queue;
std::vector<Pending> pending;
unsigned originalCalls;
uint64_t observedFirstFrame;
IOReturn backendError = -123;
UsbMsbcBridge *stopOnSubmit;
IOLock *pauseRxLock;
std::mutex pauseMutex;
std::condition_variable pauseCondition;
bool paused {}, resume {};
bool pauseBackend {}, backendEntered {}, backendResume {};
std::thread::id pausedThread;
void pauseAfterRxUnlock(IOLock *lock) {
    if (lock != pauseRxLock || std::this_thread::get_id() != pausedThread) return;
    std::unique_lock<std::mutex> guard(pauseMutex);
    paused = true; pauseCondition.notify_all();
    pauseCondition.wait(guard, [] { return resume; });
}

void complete(Pending p, IOReturn status = 0, bool shortFrame = false,
    uint32_t errorFrame = UINT32_MAX, IOReturn frameError = 0) {
    for (uint32_t i = 0; i < p.count; ++i) {
        p.frames[i].status = status;
        p.frames[i].completeCount = status ? 0 : p.frames[i].requestCount;
        p.frames[i].timeStamp = 1000 + i;
    }
    if (shortFrame && p.frames[0].completeCount) --p.frames[0].completeCount;
    if (errorFrame < p.count) {
        p.frames[errorFrame].status = frameError;
        p.frames[errorFrame].completeCount = 0;
    }
    p.completion.action(p.completion.owner, p.completion.parameter, status, p.frames);
    p.data->release();
}
IOReturn backend(IOUSBHostPipe *, IOMemoryDescriptor *data,
    IOUSBHostIsochronousFrame *frames, uint32_t n, uint64_t first,
    IOUSBHostIsochronousCompletion *completion) {
    ++originalCalls; observedFirstFrame = first;
    if (pauseBackend) {
        std::unique_lock<std::mutex> guard(pauseMutex);
        backendEntered = true; pauseCondition.notify_all();
        pauseCondition.wait(guard, [] { return backendResume; });
    }
    if (stopOnSubmit) { stopOnSubmit->stop(); stopOnSubmit = nullptr; }
    if (mode == Reject) return backendError;
    if (!completion) {
        for (uint32_t i = 0; i < n; ++i) {
            frames[i].status = 0; frames[i].completeCount = frames[i].requestCount;
        }
        return 0;
    }
    data->retain();
    Pending p {data, frames, n, *completion};
    if (mode == Inline) complete(p);
    else pending.push_back(p);
    return 0;
}
struct Fixture {
    UsbMsbcBridge *bridge = UsbMsbcBridge::create();
    IOUSBHostDevice *device = new IOUSBHostDevice;
    IOUSBHostInterface *interface = new IOUSBHostInterface(device);
    IOUSBHostPipe *out = new IOUSBHostPipe, *in = new IOUSBHostPipe;
    IOMemoryDescriptor *data = new IOMemoryDescriptor(15 * 33);
    IOUSBHostIsochronousFrame frames[15] {};
    unsigned callbacks {};
    bool resubmitInCallback {};
    IOReturn status {};
    IOUSBHostIsochronousCompletion callback {this, done, this};
    Fixture() {
        assert(bridge);
        auto ticket = bridge->beginInterfaceChange();
        bridge->detach(ticket);
        assert(bridge->bindPhysicalInterface(interface, ticket));
        in->descriptor.bEndpointAddress = 0x83;
        assert(bridge->observePipe(interface, out));
        assert(bridge->observePipe(interface, in));
        assert(bridge->ownsInterface(interface) && bridge->handlesPipe(out) && bridge->handlesPipe(in));
        bridge->setMsbcActive(true);
        for (auto &frame : frames) { frame.requestCount = 33; frame.reserved = 0xbeef; }
    }
    ~Fixture() {
        assert(pending.empty());
        if (bridge) { assert(bridge->quiescent()); bridge->stop(); bridge->detach(); bridge->release(); }
        assert(data->prepared == 0);
        data->release(); in->release(); out->release(); interface->release(); device->release();
    }
    static void done(void *owner, void *parameter, IOReturn status,
        IOUSBHostIsochronousFrame *frames) {
        auto *f = static_cast<Fixture *>(owner);
        assert(parameter == f && frames == f->frames);
        assert(f->data->retainCount() >= 2);
        ++f->callbacks; f->status = status;
        if (f->resubmitInCallback) {
            f->resubmitInCallback = false;
            assert(f->submit() == 0);
        }
    }
    IOReturn submit(bool receive = false, bool synchronous = false) {
        return bridge->io(receive ? in : out, data, frames, 15, 999,
            synchronous ? nullptr : &callback, backend);
    }
    void drain(IOReturn result = 0, bool shortFrame = false) {
        assert(!pending.empty());
        auto p = pending.front(); pending.erase(pending.begin()); complete(p, result, shortFrame);
    }
};
}

int main() {
    {
        auto *bridge = UsbMsbcBridge::create();
        auto *device = new IOUSBHostDevice;
        auto *interface = new IOUSBHostInterface(device);
        auto *pipe = new IOUSBHostPipe;
        assert(!bridge->observePipe(interface, pipe)); // No successful physical selection yet.
        auto old = bridge->beginInterfaceChange();
        bridge->detach(old);
        assert(bridge->bindPhysicalInterface(interface, old));
        assert(bridge->observePipe(interface, pipe) && bridge->status().active);
        old = bridge->beginInterfaceChange();
        assert(!bridge->mapsPhysicalInterface(interface) && !bridge->observePipe(interface, pipe));
        bridge->setMsbcActive(true);
        assert(!bridge->status().active);
        // A newer lifecycle operation wins even if the older USB call returns later.
        auto newer = bridge->beginInterfaceChange();
        bridge->detach(newer);
        assert(bridge->bindPhysicalInterface(interface, newer));
        std::thread stale([&] {
            bridge->detach(old);
            assert(!bridge->bindPhysicalInterface(interface, old));
        });
        stale.join();
        assert(bridge->mapsPhysicalInterface(interface));
        auto ticket = bridge->beginInterfaceChange();
        bridge->detach(ticket);
        interface->descriptor.bAlternateSetting = 4;
        assert(!bridge->bindPhysicalInterface(interface, ticket) && !bridge->ownsInterface(interface));
        interface->setNumber("idVendor", 0x1d57);
        interface->setNumber("idProduct", 0xfa60);
        interface->poisonGetters = true;
        const auto locks = usbTestLockCalls.load();
        const auto deviceReads = interface->deviceReads, descriptorReads = interface->descriptorReads;
        assert(!bridge->bindPhysicalInterface(interface, ticket));
        assert(usbTestLockCalls == locks && interface->deviceReads == deviceReads &&
               interface->descriptorReads == descriptorReads);
        bridge->release(); pipe->release(); interface->release(); device->release();
    }
    {
        Fixture f;
        assert(f.submit() == 0 && f.callbacks == 0 && !f.bridge->quiescent());
        assert(observedFirstFrame == 999 && pending[0].frames != f.frames && pending[0].data != f.data);
        assert(pending[0].frames[0].requestCount == 63 && pending[0].frames[1].requestCount == 0);
        assert(pending[0].frames[2].requestCount == 0);
        f.drain();
        assert(f.callbacks == 1 && f.status == 0 && f.bridge->quiescent());
        for (auto &frame : f.frames) assert(frame.completeCount == 33 && frame.reserved == 0xbeef);
    }
    {
        Fixture f; mode = Inline;
        assert(f.submit() == 0 && f.callbacks == 1 && f.bridge->quiescent());
        assert(f.submit(true) == 0 && f.callbacks == 2);
        for (auto byte : f.data->bytes) assert(byte == 0x5a);
        assert(f.submit(false, true) == 0 && f.callbacks == 2 && f.bridge->quiescent());
        mode = Queue;
    }
    {
        Fixture f;
        pauseBackend = true; backendEntered = backendResume = false;
        std::thread worker([&f] { assert(f.submit() == 0); });
        {
            std::unique_lock<std::mutex> guard(pauseMutex);
            pauseCondition.wait(guard, [] { return backendEntered; });
        }
        unsigned before = originalCalls;
        assert(f.submit() == kIOReturnNoResources && originalCalls == before);
        {
            std::lock_guard<std::mutex> guard(pauseMutex); backendResume = true;
        }
        pauseCondition.notify_all(); worker.join(); pauseBackend = false;
        f.drain();
        assert(f.submit() == 0 && pending[0].data->bytes[0] == 1);
        f.drain(); assert(f.bridge->quiescent());
    }
    {
        Fixture f; mode = Inline;
        f.resubmitInCallback = true;
        assert(f.submit() == 0 && f.callbacks == 2 && f.bridge->quiescent());
        stopOnSubmit = f.bridge;
        assert(f.submit() == 0 && f.status == kIOReturnAborted);
        mode = Queue;
    }
    {
        Fixture f; mode = Reject;
        assert(f.submit() == backendError && f.callbacks == 0 && f.bridge->quiescent());
        auto rejected = f.bridge->status();
        assert(rejected.txErrors == 1 && rejected.rxErrors == 0);
        assert(rejected.lastSubmitStatus == backendError && rejected.lastSubmitFailureStatus == backendError);
        assert(!rejected.lastSubmitReceive);
        mode = Queue;
        assert(f.submit() == 0 && pending[0].data->bytes[0] == 0); // rejected TX rolled back.
        f.drain();
        assert(f.bridge->status().lastSubmitStatus == 0 &&
            f.bridge->status().lastSubmitFailureStatus == backendError);
        f.data->shortRead = true;
        assert(f.submit() == kIOReturnIOError && f.bridge->quiescent());
        f.data->shortRead = false;
        f.frames[0].requestCount = 34;
        unsigned before = originalCalls;
        assert(f.submit() == kIOReturnBadArgument && originalCalls == before);
    }
    {
        Fixture f;
        assert(f.submit(true) == 0);
        f.bridge->stop();
        assert(f.submit() == kIOReturnAborted); // stopped pipes stay known until real close.
        unsigned rxBefore = MtkMsbcStream::rxCalls;
        f.drain();
        assert(f.status == kIOReturnAborted && MtkMsbcStream::rxCalls == rxBefore);
        assert(f.bridge->quiescent());
    }
    {
        Fixture f;
        assert(f.submit(true) == 0);
        auto *secondData = new IOMemoryDescriptor(495);
        IOUSBHostIsochronousFrame secondFrames[15] {};
        for (auto &frame : secondFrames) frame.requestCount = 33;
        IOUSBHostIsochronousCompletion ignore {nullptr,
            [](void *, void *, IOReturn, IOUSBHostIsochronousFrame *) {}, nullptr};
        assert(f.bridge->io(f.in, secondData, secondFrames, 15, 1014, &ignore, backend) == 0);
        auto second = pending.back(); pending.pop_back(); complete(second);
        assert(!f.bridge->status().active && f.bridge->status().outOfOrder == 1);
        f.drain();
        assert(f.status == kIOReturnAborted);
        secondData->release();
    }
    {
        Fixture f;
        pauseRxLock = usbTestLatestLock; // initialize() allocates guard, TX, then RX.
        assert(f.submit(true) == 0);
        auto *secondData = new IOMemoryDescriptor(495);
        IOUSBHostIsochronousFrame secondFrames[15] {};
        for (auto &frame : secondFrames) frame.requestCount = 33;
        IOUSBHostIsochronousCompletion ignore {nullptr,
            [](void *, void *, IOReturn status, IOUSBHostIsochronousFrame *) {
                assert(status == kIOReturnNoResources);
            }, nullptr};
        assert(f.bridge->io(f.in, secondData, secondFrames, 15, 1014, &ignore, backend) == 0);
        Pending first = pending[0], second = pending[1]; pending.clear();
        paused = resume = false;
        usbTestAfterUnlock = pauseAfterRxUnlock;
        std::thread worker([first] { pausedThread = std::this_thread::get_id(); complete(first); });
        {
            std::unique_lock<std::mutex> guard(pauseMutex);
            pauseCondition.wait(guard, [] { return paused; });
        }
        // First RX decoded and unlocked, but has not finalized/delivered. A
        // concurrent second delivery must fail closed rather than swap PCM batches.
        complete(second);
        assert(!f.bridge->status().active && f.bridge->status().errors == 1);
        {
            std::lock_guard<std::mutex> guard(pauseMutex); resume = true;
        }
        pauseCondition.notify_all(); worker.join(); usbTestAfterUnlock = nullptr;
        assert(f.status == kIOReturnAborted);
        secondData->release();
    }
    {
        Fixture f; pauseRxLock = usbTestLatestLock;
        assert(f.submit(true) == 0);
        Pending first = pending.front(); pending.clear();
        paused = resume = false; usbTestAfterUnlock = pauseAfterRxUnlock;
        std::thread worker([first] { pausedThread = std::this_thread::get_id(); complete(first); });
        {
            std::unique_lock<std::mutex> guard(pauseMutex);
            pauseCondition.wait(guard, [] { return paused; });
        }
        f.bridge->stop(); // decoded but not finalized must not report stale success.
        {
            std::lock_guard<std::mutex> guard(pauseMutex); resume = true;
        }
        pauseCondition.notify_all(); worker.join(); usbTestAfterUnlock = nullptr;
        assert(f.status == kIOReturnAborted);
        for (auto &frame : f.frames) assert(frame.completeCount == 0);
    }
    {
        Fixture f;
        assert(f.submit() == 0); f.drain(0, true);
        assert(f.status == 0); // preserve overall backend result; surface short frame in frame status.
        for (auto &frame : f.frames) assert(frame.status == kIOReturnUnderrun && frame.completeCount == 0);
        assert(f.submit(true) == 0); f.drain(backendError);
        assert(f.status == backendError);
        auto value = f.bridge->status();
        assert(value.txErrors == 1 && value.rxErrors == 1 && value.errors == 2);
        assert(value.lastTxFrameStatus == kIOReturnUnderrun && value.lastTxFrameIndex == 0);
        assert(value.lastTxFrameRequested == 63 && value.lastTxFrameActual == 62);
        assert(value.txZeroRequestFrames == 13 && value.txZeroRequestErrors == 0);
    }
    {
        Fixture f;
        assert(f.submit() == 0);
        auto p = pending.front(); pending.erase(pending.begin());
        complete(p, 0, false, 7, backendError);
        auto value = f.bridge->status();
        assert(value.txErrors == 1 && value.rxErrors == 0);
        assert(value.lastTxFrameStatus == backendError && value.lastTxFrameIndex == 7);
        assert(value.lastTxFrameRequested == 63 && value.lastTxFrameActual == 0);
        assert(value.txZeroRequestFrames == 13 && value.txZeroRequestErrors == 0);
    }
    {
        Fixture f;
        assert(f.submit() == 0);
        auto p = pending.front(); pending.erase(pending.begin());
        complete(p, 0, false, 2, backendError);
        auto value = f.bridge->status();
        assert(value.txErrors == 1 && value.rxErrors == 0);
        assert(value.lastTxFrameStatus == 0 && value.txZeroRequestFrames == 13);
        assert(value.txZeroRequestErrors == 1 && value.lastTxZeroRequestStatus == backendError);
        for (auto &frame : f.frames) assert(frame.status == backendError && frame.completeCount == 0);
    }
    {
        Fixture f;
        assert(f.submit(true) == 0);
        f.data->shortWrite = true; f.drain();
        assert(f.status == kIOReturnIOError && f.bridge->quiescent());
    }
    {
        Fixture f;
        IOUSBHostPipe *foreign = new IOUSBHostPipe;
        foreign->poisonGetters = true;
        const auto locksBefore = usbTestLockCalls.load();
        const auto originalBefore = originalCalls;
        assert(f.bridge->io(foreign, f.data, f.frames, 15, 999, nullptr, backend) == 0);
        assert(usbTestLockCalls == locksBefore && originalCalls == originalBefore + 1);
        assert(foreign->descriptorReads == 0);
        foreign->release();
        f.interface->descriptor.bAlternateSetting = 2;
        assert(!f.bridge->observePipe(f.interface, f.out));
        f.interface->descriptor.bAlternateSetting = 6;
        f.interface->setNumber("idProduct", 0x9999);
        assert(!f.bridge->observePipe(f.interface, f.out));
    }
    {
        Fixture f;
        auto *foreignDevice = new IOUSBHostDevice;
        auto *foreignInterface = new IOUSBHostInterface(foreignDevice);
        auto *foreignPipe = new IOUSBHostPipe;
        foreignDevice->setNumber("idVendor", 0x1d57);
        foreignDevice->setNumber("idProduct", 0xfa60);
        foreignInterface->setNumber("idVendor", 0x1d57);
        foreignInterface->setNumber("idProduct", 0xfa60);
        foreignInterface->poisonGetters = foreignPipe->poisonGetters = true;
        const auto locksBefore = usbTestLockCalls.load();
        for (unsigned i = 0; i < 1000; ++i) {
            assert(!f.bridge->observePipe(foreignInterface, foreignPipe));
            assert(!f.bridge->ownsInterface(foreignInterface));
            assert(!f.bridge->handlesPipe(foreignPipe));
        }
        assert(usbTestLockCalls == locksBefore);
        assert(foreignInterface->deviceReads == 0 && foreignInterface->descriptorReads == 0 &&
            foreignPipe->descriptorReads == 0 && foreignDevice->propertyReads == 0);
        // A descriptorless enumeration placeholder is also a cache-only miss.
        foreignInterface->removeProperty("idVendor");
        assert(!f.bridge->observePipe(foreignInterface, foreignPipe));
        foreignPipe->release(); foreignInterface->release(); foreignDevice->release();
    }
    {
        Fixture f;
        auto *interface = new IOUSBHostInterface(f.device);
        auto *pipe = new IOUSBHostPipe;
        interface->setNumber("bInterfaceNumber", 2); // LE ISO, not SCO.
        interface->poisonGetters = pipe->poisonGetters = true;
        const auto locksBefore = usbTestLockCalls.load();
        assert(!f.bridge->observePipe(interface, pipe));
        assert(usbTestLockCalls == locksBefore && interface->deviceReads == 0);
        pipe->release(); interface->release();
        assert(f.bridge->ownsInterface(f.interface));
        f.bridge->stop(); f.bridge->detach();
        assert(!f.bridge->ownsInterface(f.interface) && !f.bridge->handlesPipe(f.in));
    }
    {
        Fixture f;
        for (unsigned i = 0; i < 16; ++i) assert(f.submit() == 0);
        assert(f.submit() == kIOReturnNoResources);
        while (!pending.empty()) f.drain();
        assert(f.callbacks == 16 && f.bridge->quiescent());
    }
    {
        auto baseline = OSObject::live.load();
        IOBufferMemoryDescriptor::failAllocationAfter = 3;
        assert(!UsbMsbcBridge::create());
        IOBufferMemoryDescriptor::failAllocationAfter = -1;
        assert(OSObject::live == baseline);
    }
    assert(OSObject::live == 0);
    std::puts("PASS: real USB wrapper inline/async/sync/reject, per-transfer retention, isolation, bounds, cancellation, pool exhaustion and partial allocation cleanup");
}
