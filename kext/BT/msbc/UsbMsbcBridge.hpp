#pragma once

#ifdef USB_MSBC_BRIDGE_TEST
#include "tests/UsbMsbcTestSDK.hpp"
#else
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#endif

#if defined(USB_MSBC_BRIDGE_TEST) && !defined(USB_MSBC_BRIDGE_REAL_STREAM_TEST)
#include "tests/UsbMsbcTestStream.hpp"
#else
#include "MtkMsbcStream.hpp"
#endif

// The caller routes the real IOUSBHostPipe isochronous overload and separately
// observes interface copyPipe/close/alternate-setting changes. No pipe layout
// casts, device enumeration, USB ownership changes or service restarts occur here.
class UsbMsbcBridge final : public OSObject {
    OSDeclareDefaultStructors(UsbMsbcBridge)
public:
    using IsoIo = IOReturn (*)(IOUSBHostPipe *, IOMemoryDescriptor *,
        IOUSBHostIsochronousFrame *, uint32_t, uint64_t,
        IOUSBHostIsochronousCompletion *);

    static UsbMsbcBridge *create();
    uint64_t beginInterfaceChange();
    bool bindPhysicalInterface(IOUSBHostInterface *interface, uint64_t generation);
    bool mapsPhysicalInterface(IOUSBHostInterface *interface) const;
    bool observePipe(IOUSBHostInterface *interface, IOUSBHostPipe *pipe);
    bool ownsInterface(IOUSBHostInterface *interface) const;
    bool handlesPipe(IOUSBHostPipe *pipe) const;
    bool ownsDevice(IOUSBHostDevice *device);
    void forgetPipe(IOUSBHostPipe *pipe);
    void setMsbcActive(bool active);
    // Call before the real interface closes/changes alternate setting; that
    // operation remains responsible for aborting and completing its USB I/O.
    void stop();
    // Invoke only after the real close/alternate-setting operation has aborted
    // old I/O. Keeping stopped pipes registered until then rejects late PCM I/O.
    void detach(uint64_t generation = 0);
    bool quiescent();
    struct Status {
        uint64_t txBatches, rxBatches, errors, outOfOrder;
        uint32_t inFlight;
        bool active;
        uint64_t txErrors, rxErrors, txZeroRequestFrames, txZeroRequestErrors;
        IOReturn lastSubmitStatus, lastSubmitFailureStatus, lastTxFrameStatus, lastTxZeroRequestStatus;
        uint32_t lastTxFrameIndex, lastTxFrameRequested, lastTxFrameActual;
        bool lastSubmitReceive;
        MtkMsbcStream::TxDiagnostics txAudio;
        MtkMsbcStream::RxDiagnostics rxAudio;
    };
    Status status();
    void publishStatus();

    IOReturn io(IOUSBHostPipe *pipe, IOMemoryDescriptor *data,
        IOUSBHostIsochronousFrame *frames, uint32_t frameCount,
        uint64_t firstFrame, IOUSBHostIsochronousCompletion *completion,
        IsoIo original);

private:
    static constexpr uint32_t MaxFrames = 15;
    static constexpr uint32_t MaxBytes = MtkMsbcStream::kWireBytes;
    static constexpr uint32_t PoolSize = 16;
    struct Transfer;
    Transfer *_transfers {};
    IOLock *_guard {};
    IOLock *_txLock {};
    IOLock *_rxLock {};
    IOUSBHostDevice *_device {};
    IOUSBHostInterface *_interface {};
    IOUSBHostPipe *_outPipe {};
    IOUSBHostPipe *_inPipe {};
    uint64_t _generation {1};
    uint64_t _txGeneration {};
    uint64_t _rxGeneration {};
    uint64_t _rxLastFrame {};
    uint64_t _txBatches {}, _rxBatches {}, _errors {}, _outOfOrder {};
    uint64_t _txErrors {}, _rxErrors {}, _txZeroRequestFrames {}, _txZeroRequestErrors {};
    IOReturn _lastSubmitStatus {}, _lastSubmitFailureStatus {}, _lastTxFrameStatus {}, _lastTxZeroRequestStatus {};
    uint32_t _lastTxFrameIndex {}, _lastTxFrameRequested {}, _lastTxFrameActual {};
    bool _lastSubmitReceive {};
    MtkMsbcStream::TxDiagnostics _txAudio;
    MtkMsbcStream::RxDiagnostics _rxAudio;
    uint32_t _inFlight {};
    bool _active {};
    bool _interfaceChanging {};
    bool _rxDeliveryActive {};
    MtkMsbcStream _stream;

    bool initialize();
    void free() override;
    Transfer *acquire(IOUSBHostPipe *pipe, IOMemoryDescriptor *data,
        IOUSBHostIsochronousFrame *frames, uint32_t count,
        IOUSBHostIsochronousCompletion *completion, uint64_t firstFrame);
    bool current(const Transfer *transfer);
    bool claimCompletion(Transfer *transfer);
    void releaseTransfer(Transfer *transfer);
    IOReturn prepare(Transfer *transfer);
    IOReturn finish(Transfer *transfer, IOReturn status);
    IOReturn finalize(Transfer *transfer, IOReturn status);
    void deliver(Transfer *transfer, IOReturn status, IOUSBHostIsochronousFrame *frames);
    static void completed(void *owner, void *parameter, IOReturn status,
        IOUSBHostIsochronousFrame *frames);
};
