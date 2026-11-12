//
//  BtLoader.hpp
//  MtkWireless
//
//  Matches the MediaTek USB Bluetooth device, runs the Rust bring-up
//  (firmware patch download, Bluetooth enable) and lets go of the device:
//  bluetoothd drives the controller from user space afterwards. At shutdown
//  and restart it takes the device back for one WMT FUNC_CTRL off.
//

#ifndef MtkWirelessBT_BtLoader_hpp
#define MtkWirelessBT_BtLoader_hpp

#include <IOKit/IOService.h>
#include <kern/thread_call.h>
#include <IOKit/usb/IOUSBHostDevice.h>

class USBDeviceController;

class MtkWirelessBTLoader : public IOService {
    OSDeclareDefaultStructors(MtkWirelessBTLoader)

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    IOReturn setPowerState(unsigned long state, IOService *device) override;
    void systemWillShutdown(IOOptionBits specifier) override;

private:
    bool bringUp(char *firmware, size_t len);
    void publish(bool loaded, const char *firmware);
    void release();
    void powerOffController(IOOptionBits specifier);
    void armOpenWatchdog();
    static void openWatchdogFired(thread_call_param_t self, thread_call_param_t);
    static bool hciInterfaceOpen(IOUSBHostDevice *device);

    IOUSBHostDevice *m_device {nullptr};
    USBDeviceController *m_usb {nullptr};
    thread_call_t m_openWatchdog {nullptr};
};

#endif /* MtkWirelessBT_BtLoader_hpp */
