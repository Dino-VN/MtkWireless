//
//  BtLoader.hpp
//  MtkWireless
//
//  Matches the MediaTek USB Bluetooth device, runs the Rust bring-up
//  (firmware patch download, Bluetooth enable) and lets go of the device:
//  bluetoothd drives the controller from user space afterwards.
//

#ifndef MtkWirelessBT_BtLoader_hpp
#define MtkWirelessBT_BtLoader_hpp

#include <IOKit/IOService.h>
#include <IOKit/usb/IOUSBHostDevice.h>

class USBDeviceController;

class MtkWirelessBTLoader : public IOService {
    OSDeclareDefaultStructors(MtkWirelessBTLoader)

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn setPowerState(unsigned long state, IOService *device) override;

private:
    bool bringUp(char *firmware, size_t len);
    void publish(bool loaded, const char *firmware);
    void release();

    IOUSBHostDevice *m_device {nullptr};
    USBDeviceController *m_usb {nullptr};
};

#endif /* MtkWirelessBT_BtLoader_hpp */
