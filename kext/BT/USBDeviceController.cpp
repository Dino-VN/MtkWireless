//
//  USBDeviceController.cpp
//  MtkWireless
//

#include "USBDeviceController.hpp"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>

#include "Log.h"

#define super OSObject
OSDefineMetaClassAndStructors(USBDeviceController, OSObject)

static bool isBluetoothControlInterface(const StandardUSB::InterfaceDescriptor *desc)
{
    return desc && desc->bInterfaceClass == 0xe0 && desc->bInterfaceSubClass == 0x01 &&
           desc->bInterfaceProtocol == 0x01;
}

static bool hasBluetoothHciEndpoints(IOUSBHostInterface *interface)
{
    const StandardUSB::ConfigurationDescriptor *config = interface->getConfigurationDescriptor();
    const StandardUSB::InterfaceDescriptor *desc = interface->getInterfaceDescriptor();
    if (!config || !desc)
        return false;

    bool interruptIn = false, bulkIn = false, bulkOut = false;
    const EndpointDescriptor *ep = nullptr;
    while ((ep = StandardUSB::getNextEndpointDescriptor(config, desc, ep))) {
        uint8_t dir = StandardUSB::getEndpointDirection(ep);
        uint8_t type = StandardUSB::getEndpointType(ep);
        interruptIn |= dir == kUSBIn && type == kUSBInterrupt;
        bulkIn |= dir == kUSBIn && type == kUSBBulk;
        bulkOut |= dir == kUSBOut && type == kUSBBulk;
    }
    return interruptIn && bulkIn && bulkOut;
}

bool USBDeviceController::init(IOService *client, IOUSBHostDevice *dev)
{
    if (!super::init())
        return false;
    m_pDevice = dev;
    m_pClient = client;
    return true;
}

void USBDeviceController::free()
{
    IOUSBHostPipe **pipes[] = { &m_pBulkWritePipe, &m_pBulkReadPipe, &m_pInterruptReadPipe };
    for (IOUSBHostPipe **pipe : pipes) {
        if (*pipe) {
            (*pipe)->abort();
            OSSafeReleaseNULL(*pipe);
        }
    }
    if (m_pInterface) {
        if (m_pClient && m_pInterface->isOpen(m_pClient))
            m_pInterface->close(m_pClient);
        OSSafeReleaseNULL(m_pInterface);
    }
    m_pClient = nullptr;
    m_pDevice = nullptr;
    super::free();
}

bool USBDeviceController::initConfiguration()
{
    if (m_pDevice->getDeviceDescriptor()->bNumConfigurations < 1) {
        BTLog("device has no configuration\n");
        return false;
    }
    const StandardUSB::ConfigurationDescriptor *config = m_pDevice->getConfigurationDescriptor(0);
    if (!config) {
        BTLog("getConfigurationDescriptor(0) failed\n");
        return false;
    }
    IOReturn ret = m_pDevice->setConfiguration(config->bConfigurationValue);
    if (ret != kIOReturnSuccess) {
        BTLog("set configuration %d failed: %s\n", config->bConfigurationValue, stringFromReturn(ret));
        return false;
    }
    return true;
}

bool USBDeviceController::findInterface()
{
    OSIterator *iterator = m_pDevice->getChildIterator(gIOServicePlane);
    if (!iterator) {
        BTLog("can not create child iterator\n");
        return false;
    }
    while (OSObject *candidate = iterator->getNextObject()) {
        IOUSBHostInterface *interface = OSDynamicCast(IOUSBHostInterface, candidate);
        if (!interface || !hasBluetoothHciEndpoints(interface))
            continue;
        const StandardUSB::InterfaceDescriptor *desc = interface->getInterfaceDescriptor();
        BTLog("interface %u class=0x%02x subclass=0x%02x protocol=0x%02x\n", desc->bInterfaceNumber,
              desc->bInterfaceClass, desc->bInterfaceSubClass, desc->bInterfaceProtocol);
        if (!m_pInterface || isBluetoothControlInterface(desc)) {
            OSSafeReleaseNULL(m_pInterface);
            interface->retain();
            m_pInterface = interface;
        }
        if (isBluetoothControlInterface(desc))
            break;
    }
    OSSafeReleaseNULL(iterator);
    if (!m_pInterface) {
        BTLog("no Bluetooth HCI interface\n");
        return false;
    }
    if (!m_pInterface->open(m_pClient)) {
        BTLog("can not open interface\n");
        OSSafeReleaseNULL(m_pInterface);
        return false;
    }
    return true;
}

bool USBDeviceController::findPipes()
{
    const StandardUSB::ConfigurationDescriptor *config = m_pInterface->getConfigurationDescriptor();
    const StandardUSB::InterfaceDescriptor *desc = m_pInterface->getInterfaceDescriptor();
    if (!config || !desc)
        return false;
    const EndpointDescriptor *ep = nullptr;
    while ((ep = StandardUSB::getNextEndpointDescriptor(config, desc, ep))) {
        uint8_t dir = StandardUSB::getEndpointDirection(ep);
        uint8_t type = StandardUSB::getEndpointType(ep);
        IOUSBHostPipe **slot = nullptr;
        if (dir == kUSBIn && type == kUSBInterrupt)
            slot = &m_pInterruptReadPipe;
        else if (dir == kUSBOut && type == kUSBBulk)
            slot = &m_pBulkWritePipe;
        else if (dir == kUSBIn && type == kUSBBulk)
            slot = &m_pBulkReadPipe;
        if (slot && !*slot) {
            *slot = m_pInterface->copyPipe(StandardUSB::getEndpointAddress(ep));
            if (!*slot) {
                BTLog("copyPipe 0x%02x failed\n", StandardUSB::getEndpointAddress(ep));
                return false;
            }
        }
    }
    return m_pInterruptReadPipe && m_pBulkWritePipe && m_pBulkReadPipe;
}

IOReturn USBDeviceController::sendHCICommand(const uint8_t *cmd, uint32_t length, uint32_t timeout)
{
    uint8_t buf[272];
    if (!cmd || length < 3 || length > sizeof(buf))
        return kIOReturnBadArgument;
    memcpy(buf, cmd, length);
    uint32_t transferred = 0;
    StandardUSB::DeviceRequest request = {
        .bmRequestType = makeDeviceRequestbmRequestType(kRequestDirectionOut, kRequestTypeClass,
                                                        kRequestRecipientDevice),
        .bRequest = 0,
        .wValue = 0,
        .wIndex = 0,
        .wLength = (uint16_t)length,
    };
    return m_pInterface->deviceRequest(request, buf, transferred, timeout);
}

IOReturn USBDeviceController::controlRequest(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                                             uint16_t wIndex, void *data, uint16_t length, uint32_t timeout)
{
    uint32_t transferred = 0;
    StandardUSB::DeviceRequest request = {
        .bmRequestType = bmRequestType,
        .bRequest = bRequest,
        .wValue = wValue,
        .wIndex = wIndex,
        .wLength = length,
    };
    return m_pDevice->deviceRequest(m_pClient, request, data, transferred, timeout);
}

IOReturn USBDeviceController::readWMTEvent(void *buf, uint32_t bufSize, uint32_t *size, uint32_t timeout)
{
    if (!buf || bufSize == 0)
        return kIOReturnBadArgument;

    uint8_t tmp[64];
    uint64_t deadline = 0;
    StandardUSB::DeviceRequest request = {
        .bmRequestType = makeDeviceRequestbmRequestType(kRequestDirectionIn, kRequestTypeVendor,
                                                        kRequestRecipientDevice),
        .bRequest = 1,
        .wValue = 48,
        .wIndex = 0,
        .wLength = sizeof(tmp),
    };
    clock_interval_to_deadline(timeout, kMillisecondScale, &deadline);
    while (true) {
        uint32_t transferred = 0;
        memset(tmp, 0, sizeof(tmp));
        IOReturn ret = m_pDevice->deviceRequest(m_pClient, request, tmp, transferred, 100);
        if (ret != kIOReturnSuccess && ret != kIOReturnTimeout)
            return ret;
        if (ret == kIOReturnSuccess && transferred > 0) {
            uint32_t n = min(min(transferred, bufSize), (uint32_t)sizeof(tmp));
            memcpy(buf, tmp, n);
            if (size)
                *size = n;
            return kIOReturnSuccess;
        }
        uint64_t now = 0;
        clock_get_uptime(&now);
        if (now >= deadline)
            return kIOReturnTimeout;
        IODelay(500);
    }
}

namespace {

struct EventRead {
    volatile UInt32 done;
    IOReturn status;
    uint32_t length;
};

void eventReadComplete(void *, void *parameter, IOReturn status, uint32_t bytesTransferred)
{
    auto *read = static_cast<EventRead *>(parameter);
    read->status = status;
    read->length = bytesTransferred;
    OSCompareAndSwap(0, 1, &read->done);
}

} // namespace

IOReturn USBDeviceController::readHCIEvent(void *buf, uint32_t bufSize, uint32_t *size, uint32_t timeout)
{
    if (!buf || bufSize == 0 || !m_pInterruptReadPipe)
        return kIOReturnBadArgument;
    // HCI event header + 255 parameter bytes; the transfer ends on a short packet.
    const uint32_t capacity = 260;
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::withCapacity(capacity, kIODirectionIn);
    if (!md)
        return kIOReturnNoMemory;
    IOReturn ret = md->prepare(kIODirectionIn);
    if (ret != kIOReturnSuccess) {
        md->release();
        return ret;
    }
    // Interrupt pipes take no completion timeout: wait here, then abort
    // synchronously so the completion has run before `read` goes away.
    EventRead read {};
    IOUSBHostCompletion completion = {
        .owner = this,
        .action = eventReadComplete,
        .parameter = &read,
    };
    ret = m_pInterruptReadPipe->io(md, capacity, &completion, 0);
    if (ret == kIOReturnSuccess) {
        uint64_t deadline = 0, now = 0;
        clock_interval_to_deadline(timeout, kMillisecondScale, &deadline);
        while (!read.done) {
            clock_get_uptime(&now);
            if (now >= deadline)
                break;
            IOSleep(5);
        }
        if (!read.done)
            m_pInterruptReadPipe->abort(IOUSBHostIOSource::kAbortSynchronous, kIOReturnTimeout);
        ret = read.done ? read.status : kIOReturnTimeout;
    }
    if (ret == kIOReturnSuccess) {
        uint32_t n = min(min(read.length, bufSize), capacity);
        md->readBytes(0, buf, n);
        if (size)
            *size = n;
    }
    md->complete(kIODirectionIn);
    md->release();
    return ret;
}

const char *USBDeviceController::stringFromReturn(IOReturn code)
{
    return m_pDevice ? m_pDevice->stringFromReturn(code) : "?";
}
