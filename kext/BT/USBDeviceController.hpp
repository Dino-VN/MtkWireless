//
//  USBDeviceController.hpp
//  MtkWireless
//
//  Based on SimpleMtkBtFw's USBDeviceController (qcwap, zxystd, laobamac).
//  Opens the HCI interface of the USB device and carries the vendor and HCI
//  control requests the Rust bring-up needs.
//

#ifndef MtkWirelessBT_USBDeviceController_hpp
#define MtkWirelessBT_USBDeviceController_hpp

#include <IOKit/IOService.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/USB.h>

#define HCI_INIT_TIMEOUT 10000 /* ms */

class USBDeviceController : public OSObject {
    OSDeclareDefaultStructors(USBDeviceController)

public:
    bool init(IOService *client, IOUSBHostDevice *dev);
    void free() override;

    bool initConfiguration();
    bool findInterface();
    bool findPipes();

    /* HCI command (opcode, length, parameters) as a class request on EP0. */
    IOReturn sendHCICommand(const uint8_t *cmd, uint32_t length, uint32_t timeout);
    IOReturn controlRequest(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                            void *data, uint16_t length, uint32_t timeout);
    /* MediaTek returns WMT events through vendor request 1, wValue 48. */
    IOReturn readWMTEvent(void *buf, uint32_t bufSize, uint32_t *size, uint32_t timeout);

    const char *stringFromReturn(IOReturn code);

private:
    IOUSBHostDevice *m_pDevice {nullptr};
    IOService *m_pClient {nullptr};
    IOUSBHostInterface *m_pInterface {nullptr};

    IOUSBHostPipe *m_pInterruptReadPipe {nullptr};
    IOUSBHostPipe *m_pBulkWritePipe {nullptr};
    IOUSBHostPipe *m_pBulkReadPipe {nullptr};
};

#endif /* MtkWirelessBT_USBDeviceController_hpp */
