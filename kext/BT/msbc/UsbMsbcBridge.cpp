#include "UsbMsbcBridge.hpp"
#include "UsbMsbcScope.hpp"

#ifndef USB_MSBC_BRIDGE_TEST
#include <IOKit/IOLib.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/OSByteOrder.h>
#endif

OSDefineMetaClassAndStructors(UsbMsbcBridge, OSObject)

struct UsbMsbcBridge::Transfer {
    UsbMsbcBridge *bridge;
    IOUSBHostPipe *pipe;
    IOMemoryDescriptor *originalData;
    IOBufferMemoryDescriptor *wireData;
    IOUSBHostIsochronousFrame *originalFrames;
    IOUSBHostIsochronousCompletion originalCompletion;
    IOUSBHostIsochronousFrame wireFrames[MaxFrames];
    uint16_t requested[MaxFrames];
    uint16_t wireRequested[MaxFrames];
    uint16_t completedCounts[MaxFrames];
    uint16_t logicalCompleted[MaxFrames];
    uint8_t logicalBytes[MaxBytes];
    MtkMsbcStream::TxState checkpoint;
    uint64_t generation;
    uint64_t firstFrame;
    uint32_t count;
    uint32_t logicalCapacity;
    uint32_t references;
    bool receive;
    bool completionClaimed;
    bool submissionPending;
    bool completionDeferred;
    IOReturn deferredStatus;
    IOUSBHostIsochronousFrame *deferredFrames;
    bool used;
    bool accepted;
    bool delivered;
    bool ownsRxDelivery;
};

UsbMsbcBridge *UsbMsbcBridge::create() {
    auto *bridge = new UsbMsbcBridge;
    if (bridge && !bridge->initialize()) {
        bridge->release();
        bridge = nullptr;
    }
    return bridge;
}

bool UsbMsbcBridge::initialize() {
    if (!OSObject::init()) return false;
    _guard = IOLockAlloc();
    _txLock = IOLockAlloc();
    _rxLock = IOLockAlloc();
    _transfers = static_cast<Transfer *>(IOMallocZero(sizeof(Transfer) * PoolSize));
    if (!_guard || !_txLock || !_rxLock || !_transfers) return false;
    for (uint32_t i = 0; i < PoolSize; ++i) {
        _transfers[i].wireData = IOBufferMemoryDescriptor::withCapacity(MaxBytes, kIODirectionInOut);
        if (!_transfers[i].wireData) return false;
    }
    _stream.reset();
    return true;
}

void UsbMsbcBridge::free() {
    // Every leased context retains this object until the submission and its
    // callback have both finished, including inline callbacks and rejections.
    if (_transfers) {
        for (uint32_t i = 0; i < PoolSize; ++i)
            if (_transfers[i].wireData) _transfers[i].wireData->release();
        IOFree(_transfers, sizeof(Transfer) * PoolSize);
    }
    if (_outPipe) _outPipe->release();
    if (_inPipe) _inPipe->release();
    if (_device) _device->release();
    if (_interface) _interface->release();
    if (_guard) IOLockFree(_guard);
    if (_txLock) IOLockFree(_txLock);
    if (_rxLock) IOLockFree(_rxLock);
    OSObject::free();
}

uint64_t UsbMsbcBridge::beginInterfaceChange() {
    IOLockLock(_guard);
    _active = false;
    __atomic_store_n(&_interfaceChanging, true, __ATOMIC_RELEASE);
    uint64_t generation = ++_generation;
    IOLockUnlock(_guard);
    publishStatus();
    return generation;
}

bool UsbMsbcBridge::bindPhysicalInterface(IOUSBHostInterface *interface, uint64_t generation) {
    if (!msbcCachedScoInterface(interface)) return false;
    auto *device = interface->getDevice();
    const auto *id = interface->getInterfaceDescriptor();
    if (!msbcCachedMtkDevice(device) || !id || id->bInterfaceNumber != 1 ||
        id->bAlternateSetting != 6) return false;
    IOLockLock(_guard);
    bool accepted = generation && generation == _generation && !_interface && !_device &&
        !_outPipe && !_inPipe;
    if (accepted) {
        device->retain(); _device = device;
        interface->retain(); __atomic_store_n(&_interface, interface, __ATOMIC_RELEASE);
        __atomic_store_n(&_interfaceChanging, false, __ATOMIC_RELEASE);
    }
    IOLockUnlock(_guard);
    return accepted;
}

bool UsbMsbcBridge::mapsPhysicalInterface(IOUSBHostInterface *interface) const {
    return ownsInterface(interface) && !__atomic_load_n(&_interfaceChanging, __ATOMIC_ACQUIRE);
}

bool UsbMsbcBridge::observePipe(IOUSBHostInterface *interface, IOUSBHostPipe *pipe) {
    if (!pipe || !msbcCachedScoInterface(interface)) return false;
    if (!mapsPhysicalInterface(interface)) return false;
    auto *device = interface->getDevice();
    if (!msbcCachedMtkDevice(device)) return false;
    const auto *id = interface->getInterfaceDescriptor();
    const auto *ep = pipe->getEndpointDescriptor();
    if (!id || !ep || id->bInterfaceNumber != 1 ||
        id->bAlternateSetting != 6 || (ep->bmAttributes & 3) != 1 ||
        OSSwapLittleToHostInt16(ep->wMaxPacketSize) != MtkMsbcStream::kWirePacket || ep->bInterval != 4 ||
        (ep->bEndpointAddress != 0x03 && ep->bEndpointAddress != 0x83)) return false;

    IOLockLock(_guard);
    auto **slot = ep->bEndpointAddress == 0x03 ? &_outPipe : &_inPipe;
    bool accepted = !_interfaceChanging && _device == device && _interface == interface &&
        (!*slot || *slot == pipe);
    if (accepted) {
        if (!*slot) { pipe->retain(); __atomic_store_n(slot, pipe, __ATOMIC_RELEASE); }
        if (!_active) { _active = true; ++_generation; }
    }
    IOLockUnlock(_guard);
    return accepted;
}

bool UsbMsbcBridge::ownsInterface(IOUSBHostInterface *interface) const {
    return interface && interface == __atomic_load_n(&_interface, __ATOMIC_ACQUIRE);
}

bool UsbMsbcBridge::handlesPipe(IOUSBHostPipe *pipe) const {
    return pipe && (pipe == __atomic_load_n(&_outPipe, __ATOMIC_ACQUIRE) ||
        pipe == __atomic_load_n(&_inPipe, __ATOMIC_ACQUIRE));
}

bool UsbMsbcBridge::ownsDevice(IOUSBHostDevice *device) {
    IOLockLock(_guard);
    bool matched = device && device == _device;
    IOLockUnlock(_guard);
    return matched;
}

void UsbMsbcBridge::setMsbcActive(bool active) {
    IOLockLock(_guard);
    if (active && (_interfaceChanging || !_interface || (!_outPipe && !_inPipe))) {
        IOLockUnlock(_guard);
        return;
    }
    if (!active || !_active) {
        _active = active;
        ++_generation;
    }
    IOLockUnlock(_guard);
    publishStatus();
}

void UsbMsbcBridge::stop() {
    setMsbcActive(false);
}

void UsbMsbcBridge::detach(uint64_t generation) {
    IOLockLock(_guard);
    if (generation && generation != _generation) { IOLockUnlock(_guard); return; }
    _active = false;
    if (!generation) ++_generation;
    __atomic_store_n(&_interfaceChanging, false, __ATOMIC_RELEASE);
    auto *outPipe = _outPipe;
    auto *inPipe = _inPipe;
    auto *device = _device;
    auto *interface = _interface;
    __atomic_store_n(&_outPipe, static_cast<IOUSBHostPipe *>(nullptr), __ATOMIC_RELEASE);
    __atomic_store_n(&_inPipe, static_cast<IOUSBHostPipe *>(nullptr), __ATOMIC_RELEASE);
    __atomic_store_n(&_interface, static_cast<IOUSBHostInterface *>(nullptr), __ATOMIC_RELEASE);
    _device = nullptr;
    IOLockUnlock(_guard);
    // The caller has already performed the real USB close/cancellation.
    if (outPipe) outPipe->release();
    if (inPipe) inPipe->release();
    if (device) device->release();
    if (interface) interface->release();
    publishStatus();
}

void UsbMsbcBridge::forgetPipe(IOUSBHostPipe *pipe) {
    IOLockLock(_guard);
    bool matched = pipe && (pipe == _outPipe || pipe == _inPipe);
    IOLockUnlock(_guard);
    if (matched) detach();
}

bool UsbMsbcBridge::quiescent() {
    IOLockLock(_guard);
    bool result = _inFlight == 0;
    IOLockUnlock(_guard);
    return result;
}

UsbMsbcBridge::Status UsbMsbcBridge::status() {
    IOLockLock(_guard);
    Status value {_txBatches, _rxBatches, _errors, _outOfOrder, _inFlight, _active,
        _txErrors, _rxErrors, _txZeroRequestFrames, _txZeroRequestErrors,
        _lastSubmitStatus, _lastSubmitFailureStatus, _lastTxFrameStatus, _lastTxZeroRequestStatus,
        _lastTxFrameIndex, _lastTxFrameRequested, _lastTxFrameActual, _lastSubmitReceive,
        _txAudio, _rxAudio};
    IOLockUnlock(_guard);
    return value;
}

void UsbMsbcBridge::publishStatus() {
#ifndef USB_MSBC_BRIDGE_TEST
    auto value = status();
    auto *resource = IOService::getResourceService();
    auto *dictionary = OSDictionary::withCapacity(28);
    if (!resource || !dictionary) { if (dictionary) dictionary->release(); return; }
    auto add = [dictionary](const char *name, uint64_t count) {
        auto *number = OSNumber::withNumber(count, 64);
        if (number) { dictionary->setObject(name, number); number->release(); }
    };
    add("Active", value.active);
    add("TxBatches", value.txBatches);
    add("RxBatches", value.rxBatches);
    add("Errors", value.errors);
    add("OutOfOrder", value.outOfOrder);
    add("InFlight", value.inFlight);
    add("TxErrors", value.txErrors);
    add("RxErrors", value.rxErrors);
    add("TxZeroRequestFrames", value.txZeroRequestFrames);
    add("TxZeroRequestErrors", value.txZeroRequestErrors);
    add("LastSubmitStatus", uint32_t(value.lastSubmitStatus));
    add("LastSubmitFailureStatus", uint32_t(value.lastSubmitFailureStatus));
    add("LastSubmitReceive", value.lastSubmitReceive);
    add("LastTxFrameStatus", uint32_t(value.lastTxFrameStatus));
    add("LastTxFrameIndex", value.lastTxFrameIndex);
    add("LastTxFrameRequested", value.lastTxFrameRequested);
    add("LastTxFrameActual", value.lastTxFrameActual);
    add("LastTxZeroRequestStatus", uint32_t(value.lastTxZeroRequestStatus));
    add("TxPcmSamples", value.txAudio.samples);
    add("TxPcmNonzeroSamples", value.txAudio.nonzeroSamples);
    add("TxPcmPeak", value.txAudio.peak);
    add("RxDecodedFrames", value.rxAudio.decodedFrames);
    add("RxPlcFrames", value.rxAudio.plcFrames);
    add("RxCrcFailures", value.rxAudio.crcFailures);
    add("RxParserDiscardedBytes", value.rxAudio.discardedBytes);
    add("RxLastParserReason", value.rxAudio.lastParserReason);
    resource->setProperty("MtkBtMsbcBridge", dictionary);
    dictionary->release();
#endif
}

UsbMsbcBridge::Transfer *UsbMsbcBridge::acquire(IOUSBHostPipe *pipe,
    IOMemoryDescriptor *data, IOUSBHostIsochronousFrame *frames, uint32_t count,
    IOUSBHostIsochronousCompletion *completion, uint64_t firstFrame) {
    IOLockLock(_guard);
    Transfer *transfer = nullptr;
    if (_active && (pipe == _outPipe || pipe == _inPipe)) {
        for (uint32_t i = 0; i < PoolSize; ++i) {
            if (!_transfers[i].used) { transfer = &_transfers[i]; break; }
        }
    }
    if (transfer) {
        transfer->used = true;
        transfer->bridge = this;
        transfer->pipe = pipe;
        transfer->originalData = data;
        transfer->originalFrames = frames;
        transfer->count = count;
        transfer->firstFrame = firstFrame;
        transfer->receive = pipe == _inPipe;
        transfer->generation = _generation;
        transfer->references = 2;
        transfer->completionClaimed = false;
        transfer->submissionPending = true;
        transfer->completionDeferred = false;
        transfer->deferredStatus = 0;
        transfer->deferredFrames = nullptr;
        transfer->accepted = false;
        transfer->delivered = false;
        transfer->ownsRxDelivery = false;
        transfer->originalCompletion = completion ? *completion : IOUSBHostIsochronousCompletion {};
        ++_inFlight;
        retain();
        pipe->retain();
        data->retain();
    }
    IOLockUnlock(_guard);
    return transfer;
}

bool UsbMsbcBridge::current(const Transfer *transfer) {
    IOLockLock(_guard);
    bool result = _active && transfer->generation == _generation;
    IOLockUnlock(_guard);
    return result;
}

bool UsbMsbcBridge::claimCompletion(Transfer *transfer) {
    IOLockLock(_guard);
    bool result = !transfer->completionClaimed;
    transfer->completionClaimed = true;
    IOLockUnlock(_guard);
    return result;
}

void UsbMsbcBridge::releaseTransfer(Transfer *transfer) {
    IOMemoryDescriptor *data = nullptr;
    IOUSBHostPipe *pipe = nullptr;
    IOLockLock(_guard);
    if (--transfer->references == 0) {
        data = transfer->originalData;
        pipe = transfer->pipe;
        transfer->originalData = nullptr;
        transfer->pipe = nullptr;
        transfer->used = false;
        if (transfer->ownsRxDelivery) _rxDeliveryActive = false;
        --_inFlight;
    }
    IOLockUnlock(_guard);
    if (data) {
        data->release();
        pipe->release();
        release();
    }
}

IOReturn UsbMsbcBridge::prepare(Transfer *transfer) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < transfer->count; ++i) {
        uint32_t requested = transfer->originalFrames[i].requestCount;
        if (requested != 33 || total > MaxBytes - requested) return kIOReturnBadArgument;
        total += requested;
        transfer->requested[i] = static_cast<uint16_t>(requested);
        transfer->wireRequested[i] = MtkMsbcStream::kWirePacket;
    }
    if (total > transfer->originalData->getLength()) return kIOReturnBadArgument;
    transfer->logicalCapacity = total;
    auto *wire = static_cast<uint8_t *>(transfer->wireData->getBytesNoCopy());
    bzero(wire, MaxBytes);
    if (!transfer->receive) {
        IOReturn result = transfer->originalData->prepare(kIODirectionOut);
        if (result != kIOReturnSuccess) return result;
        bool copied = transfer->originalData->readBytes(0, transfer->logicalBytes, total) == total;
        transfer->originalData->complete(kIODirectionOut);
        if (!copied) return kIOReturnIOError;
        bool valid = current(transfer) && _stream.transmit(transfer->logicalBytes, total,
            transfer->requested, transfer->count, wire, MaxBytes, transfer->wireRequested);
        IOLockLock(_guard);
        _txAudio = _stream.transmitDiagnostics();
        IOLockUnlock(_guard);
        if (!valid) return current(transfer) ? kIOReturnIOError : kIOReturnAborted;
    }
    total = 0;
    for (uint32_t i = 0; i < transfer->count; ++i) {
        uint16_t requested = transfer->wireRequested[i];
        if (requested > MtkMsbcStream::kWirePacket || total > MaxBytes - requested) return kIOReturnBadArgument;
        total += requested;
        transfer->wireFrames[i] = {};
        transfer->wireFrames[i].status = kIOReturnInvalid;
        transfer->wireFrames[i].requestCount = requested;
    }
    return current(transfer) ? kIOReturnSuccess : kIOReturnAborted;
}

IOReturn UsbMsbcBridge::finish(Transfer *transfer, IOReturn status) {
    if (transfer->receive) {
        // HostFamily documents workloop-serialized pipe completions. Also fail
        // closed for an atypical/deferred concurrent callback so successful PCM
        // deliveries cannot reverse after the decode lock has been released.
        IOLockLock(_guard);
        if (_rxDeliveryActive) {
            status = kIOReturnNoResources;
            _active = false;
            ++_generation;
        } else {
            _rxDeliveryActive = true;
            transfer->ownsRxDelivery = true;
        }
        IOLockUnlock(_guard);
    }
    if (status == kIOReturnSuccess && !current(transfer)) status = kIOReturnAborted;
    IOReturn frameStatus = status;
    if (status == kIOReturnSuccess) {
        uint32_t zeroFrames = 0, zeroErrors = 0;
        IOReturn zeroStatus = kIOReturnSuccess;
        bool txFailure = false;
        uint32_t failedIndex = 0;
        IOReturn failedStatus = kIOReturnSuccess;
        for (uint32_t i = 0; i < transfer->count; ++i) {
            const auto &frame = transfer->wireFrames[i];
            if (!transfer->receive) {
                if (!transfer->wireRequested[i]) {
                    ++zeroFrames;
                    if (frame.status != kIOReturnSuccess) { ++zeroErrors; zeroStatus = frame.status; }
                } else if (!txFailure && (frame.status != kIOReturnSuccess ||
                    frame.completeCount != frame.requestCount || frame.requestCount != transfer->wireRequested[i])) {
                    txFailure = true;
                    failedIndex = i;
                    failedStatus = frame.status != kIOReturnSuccess ? frame.status :
                        (frame.completeCount > frame.requestCount || frame.requestCount != transfer->wireRequested[i] ?
                        kIOReturnBadArgument : kIOReturnUnderrun);
                }
            }
            if (frame.completeCount > frame.requestCount || frame.requestCount != transfer->wireRequested[i]) {
                status = frameStatus = kIOReturnBadArgument;
                break;
            }
            transfer->completedCounts[i] = frame.status == kIOReturnSuccess ?
                static_cast<uint16_t>(frame.completeCount) : 0;
            if (!transfer->receive && frameStatus == kIOReturnSuccess &&
                (frame.status != kIOReturnSuccess || frame.completeCount != frame.requestCount))
                frameStatus = frame.status != kIOReturnSuccess ? frame.status : kIOReturnUnderrun;
        }
        if (!transfer->receive) {
            IOLockLock(_guard);
            _txZeroRequestFrames += zeroFrames;
            _txZeroRequestErrors += zeroErrors;
            if (zeroErrors) _lastTxZeroRequestStatus = zeroStatus;
            if (txFailure) {
                _lastTxFrameStatus = failedStatus;
                _lastTxFrameIndex = failedIndex;
                _lastTxFrameRequested = transfer->wireRequested[failedIndex];
                _lastTxFrameActual = transfer->wireFrames[failedIndex].completeCount;
            }
            IOLockUnlock(_guard);
        }
    }
    if (transfer->receive && status == kIOReturnSuccess) {
        bzero(transfer->logicalBytes, transfer->logicalCapacity);
        IOLockLock(_rxLock);
        if (current(transfer) && _rxGeneration != transfer->generation) {
            _stream.resetReceive();
            _rxGeneration = transfer->generation;
            _rxLastFrame = 0;
        }
        IOLockLock(_guard);
        bool ordered = transfer->firstFrame > _rxLastFrame;
        for (uint32_t i = 0; i < PoolSize && ordered; ++i) {
            const auto &other = _transfers[i];
            if (other.used && other.receive && !other.delivered &&
                other.generation == transfer->generation &&
                (other.submissionPending || other.accepted) && other.firstFrame > _rxLastFrame &&
                other.firstFrame < transfer->firstFrame)
                ordered = false;
        }
        if (!ordered) { ++_outOfOrder; _active = false; ++_generation; }
        IOLockUnlock(_guard);
        bool valid = ordered && current(transfer) && _stream.receive(
            static_cast<const uint8_t *>(transfer->wireData->getBytesNoCopy()), MaxBytes,
            transfer->wireRequested, transfer->completedCounts, transfer->count,
            transfer->logicalBytes, transfer->logicalCapacity, transfer->logicalCompleted);
        IOLockLock(_guard);
        _rxAudio = _stream.receiveDiagnostics();
        IOLockUnlock(_guard);
        if (valid) {
            for (uint32_t i = 0; i < transfer->count; ++i)
                if (transfer->logicalCompleted[i] > transfer->requested[i]) valid = false;
        }
        if (valid) _rxLastFrame = transfer->firstFrame;
        if (valid) {
            IOReturn prepared = transfer->originalData->prepare(kIODirectionIn);
            if (prepared != kIOReturnSuccess) { status = prepared; valid = false; }
            else {
                valid = transfer->originalData->writeBytes(0, transfer->logicalBytes,
                    transfer->logicalCapacity) == transfer->logicalCapacity;
                transfer->originalData->complete(kIODirectionIn);
            }
        }
        IOLockUnlock(_rxLock);
        if (!valid && status == kIOReturnSuccess)
            status = current(transfer) ? kIOReturnIOError : kIOReturnAborted;
        frameStatus = status;
    }
    for (uint32_t i = 0; i < transfer->count; ++i) {
        auto &logical = transfer->originalFrames[i];
        logical.status = frameStatus;
        logical.completeCount = frameStatus == kIOReturnSuccess ?
            (transfer->receive ? transfer->logicalCompleted[i] : transfer->requested[i]) : 0;
        logical.timeStamp = transfer->wireFrames[i].timeStamp;
    }
    return status;
}

void UsbMsbcBridge::completed(void *owner, void *parameter, IOReturn status,
    IOUSBHostIsochronousFrame *frames) {
    auto *bridge = static_cast<UsbMsbcBridge *>(owner);
    auto *transfer = static_cast<Transfer *>(parameter);
    if (!bridge->claimCompletion(transfer)) return;
    IOLockLock(bridge->_guard);
    bool defer = transfer->submissionPending;
    if (defer) {
        transfer->completionDeferred = true;
        transfer->deferredStatus = status;
        transfer->deferredFrames = frames;
    }
    IOLockUnlock(bridge->_guard);
    if (!defer) bridge->deliver(transfer, status, frames);
}

void UsbMsbcBridge::deliver(Transfer *transfer, IOReturn status,
    IOUSBHostIsochronousFrame *frames) {
    if (frames != transfer->wireFrames && status == kIOReturnSuccess) status = kIOReturnBadArgument;
    status = finish(transfer, status);
    status = finalize(transfer, status);
    auto completion = transfer->originalCompletion;
    if (completion.action)
        completion.action(completion.owner, completion.parameter, status, transfer->originalFrames);
    releaseTransfer(transfer);
}

IOReturn UsbMsbcBridge::finalize(Transfer *transfer, IOReturn status) {
    IOLockLock(_guard);
    // Linearize successful completion with stop/new-session generation changes.
    if (status == kIOReturnSuccess && (!_active || transfer->generation != _generation)) {
        status = kIOReturnAborted;
        for (uint32_t i = 0; i < transfer->count; ++i) {
            transfer->originalFrames[i].status = status;
            transfer->originalFrames[i].completeCount = 0;
        }
    }
    transfer->delivered = true;
    if (status != kIOReturnSuccess || transfer->originalFrames[0].status != kIOReturnSuccess) {
        ++_errors;
        if (transfer->receive) ++_rxErrors; else ++_txErrors;
    }
    else if (transfer->receive) ++_rxBatches;
    else ++_txBatches;
    bool publish = status != kIOReturnSuccess ||
        transfer->originalFrames[0].status != kIOReturnSuccess ||
        ((_txBatches + _rxBatches + _errors) & 63) == 0;
    IOLockUnlock(_guard);
    if (publish) publishStatus();
    return status;
}

IOReturn UsbMsbcBridge::io(IOUSBHostPipe *pipe, IOMemoryDescriptor *data,
    IOUSBHostIsochronousFrame *frames, uint32_t count, uint64_t firstFrame,
    IOUSBHostIsochronousCompletion *completion, IsoIo original) {
    if (!original) return kIOReturnBadArgument;
    if (!handlesPipe(pipe)) return original(pipe, data, frames, count, firstFrame, completion);
    IOLockLock(_guard);
    bool known = pipe && (pipe == _outPipe || pipe == _inPipe);
    bool active = _active;
    IOLockUnlock(_guard);
    if (!known) return original(pipe, data, frames, count, firstFrame, completion);
    if (!active) return kIOReturnAborted;
    if (!data || !frames || count != 15 || !firstFrame ||
        (completion && !completion->action)) return kIOReturnBadArgument;
    auto *transfer = acquire(pipe, data, frames, count, completion, firstFrame);
    if (!transfer) return kIOReturnNoResources;
    // Keep TX checkpoint/transform/submission serial. The wrapped completion
    // defers native callbacks until this lock is released, so inline completion
    // may safely resubmit another TX transfer from the native client's callback.
    if (!transfer->receive) {
        // A previous USB completion may already own the USB workloop and try
        // to resubmit while another thread is in original(). Never wait here.
        if (!IOLockTryLock(_txLock)) {
            IOLockLock(_guard);
            ++_errors;
            ++_txErrors;
            IOLockUnlock(_guard);
            claimCompletion(transfer);
            releaseTransfer(transfer);
            releaseTransfer(transfer);
            return kIOReturnNoResources;
        }
        if (current(transfer) && _txGeneration != transfer->generation) {
            _stream.resetTransmit();
            _txGeneration = transfer->generation;
        }
        _stream.saveTransmit(transfer->checkpoint);
    }
    IOReturn result = prepare(transfer);
    bool submitted = false;
    if (result == kIOReturnSuccess) {
        IOUSBHostIsochronousCompletion wrapped {this, completed, transfer};
        submitted = true;
        result = original(pipe, transfer->wireData, transfer->wireFrames, count, firstFrame,
            completion ? &wrapped : nullptr);
    }
    if (!transfer->receive) {
        if (result != kIOReturnSuccess) _stream.restoreTransmit(transfer->checkpoint);
        IOLockUnlock(_txLock);
    }
    IOLockLock(_guard);
    if (submitted) {
        _lastSubmitStatus = result;
        _lastSubmitReceive = transfer->receive;
        if (result != kIOReturnSuccess) _lastSubmitFailureStatus = result;
    }
    transfer->accepted = submitted && result == kIOReturnSuccess;
    transfer->submissionPending = false;
    bool deferred = transfer->completionDeferred;
    IOReturn deferredStatus = transfer->deferredStatus;
    auto *deferredFrames = transfer->deferredFrames;
    IOLockUnlock(_guard);
    if (deferred) deliver(transfer, deferredStatus, deferredFrames);
    if (!completion && claimCompletion(transfer)) {
        if (submitted) result = finish(transfer, result);
        result = finalize(transfer, result);
        releaseTransfer(transfer);
    }
    if (result != kIOReturnSuccess && claimCompletion(transfer)) {
        IOLockLock(_guard);
        ++_errors;
        if (transfer->receive) ++_rxErrors; else ++_txErrors;
        IOLockUnlock(_guard);
        releaseTransfer(transfer);
    }
    releaseTransfer(transfer);
    return result;
}
