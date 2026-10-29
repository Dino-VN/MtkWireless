#pragma once
#include <stdint.h>

// bluetoothd's page patches are global for this boot. Keep them off until
// a controller has been identified, and fail closed if any probed MediaTek
// controller is outside the USB bridge's scope. Repeated probes are harmless.
struct MsbcControllerScope {
    enum : uint32_t { Supported = 1, Unsupported = 2 };

    void observe(bool supported) {
        __atomic_fetch_or(&seen, supported ? Supported : Unsupported, __ATOMIC_RELEASE);
    }

    uint32_t observed() const {
        return __atomic_load_n(&seen, __ATOMIC_ACQUIRE);
    }

    bool allowsWideband() const {
        return observed() == Supported;
    }

private:
    uint32_t seen {};
};
