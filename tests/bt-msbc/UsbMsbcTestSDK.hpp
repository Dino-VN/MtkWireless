#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>
#include <string>
#include <vector>

using IOReturn = int32_t;
using AbsoluteTime = uint64_t;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnBadArgument = -1,
    kIOReturnAborted = -2, kIOReturnIOError = -3, kIOReturnUnderrun = -4,
    kIOReturnInvalid = -5, kIOReturnNoResources = -6, kIOReturnNotResponding = -7,
    kIOReturnIsoTooOld = -8, kIOReturnIsoTooNew = -9, kIOReturnOverrun = -10,
    kIOReturnNoBandwidth = -11;
constexpr unsigned kIODirectionIn = 1, kIODirectionOut = 2, kIODirectionInOut = 3;
inline uint16_t OSSwapLittleToHostInt16(uint16_t x) { return x; }
inline void *IOMallocZero(size_t n) { return std::calloc(1, n); }
inline void IOFree(void *p, size_t) { std::free(p); }
using IOLock = std::mutex;
inline IOLock *usbTestLatestLock;
inline void (*usbTestAfterUnlock)(IOLock *);
inline std::atomic<unsigned> usbTestLockCalls {0};
inline IOLock *IOLockAlloc() { return usbTestLatestLock = new IOLock; }
inline void IOLockFree(IOLock *p) { delete p; }
inline void IOLockLock(IOLock *p) { ++usbTestLockCalls; p->lock(); }
inline bool IOLockTryLock(IOLock *p) { return p->try_lock(); }
inline void IOLockUnlock(IOLock *p) { p->unlock(); if (usbTestAfterUnlock) usbTestAfterUnlock(p); }

class OSObject {
    std::atomic<unsigned> references {1};
public:
    inline static std::atomic<unsigned> live {0};
    OSObject() { ++live; }
    virtual ~OSObject() { --live; }
    virtual bool init() { return true; }
    virtual void free() {}
    void retain() { ++references; }
    void release() { if (--references == 0) { free(); delete this; } }
    unsigned retainCount() { return references; }
};
#define OSDeclareDefaultStructors(name) public: name() = default;
#define OSDefineMetaClassAndStructors(name, base)
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)

class OSNumber : public OSObject {
    uint32_t value;
public:
    explicit OSNumber(uint32_t number) : value(number) {}
    uint32_t unsigned32BitValue() { return value; }
};
class IORegistryEntry : public OSObject {
    std::map<std::string, OSObject *> properties;
public:
    unsigned propertyReads {};
    ~IORegistryEntry() override { for (auto &property : properties) property.second->release(); }
    OSObject *getProperty(const char *key) {
        ++propertyReads;
        auto found = properties.find(key);
        return found == properties.end() ? nullptr : found->second;
    }
    void setNumber(const char *key, uint32_t value) {
        auto &property = properties[key];
        if (property) property->release();
        property = new OSNumber(value);
    }
    void removeProperty(const char *key) {
        auto found = properties.find(key);
        if (found != properties.end()) { found->second->release(); properties.erase(found); }
    }
};

class IOMemoryDescriptor : public OSObject {
public:
    std::vector<uint8_t> bytes;
    IOReturn prepareResult {0};
    bool shortRead {}, shortWrite {};
    int prepared {};
    explicit IOMemoryDescriptor(size_t n) : bytes(n) {}
    size_t getLength() const { return bytes.size(); }
    IOReturn prepare(unsigned) { if (!prepareResult) ++prepared; return prepareResult; }
    IOReturn complete(unsigned) { assert(prepared > 0); --prepared; return 0; }
    size_t readBytes(size_t off, void *out, size_t n) {
        assert(off + n <= bytes.size());
        if (shortRead && n) --n;
        std::memcpy(out, bytes.data() + off, n); return n;
    }
    size_t writeBytes(size_t off, const void *in, size_t n) {
        assert(off + n <= bytes.size());
        if (shortWrite && n) --n;
        std::memcpy(bytes.data() + off, in, n); return n;
    }
};
class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
public:
    inline static int failAllocationAfter {-1};
    explicit IOBufferMemoryDescriptor(size_t n) : IOMemoryDescriptor(n) {}
    static IOBufferMemoryDescriptor *withCapacity(size_t n, unsigned) {
        if (failAllocationAfter == 0) return nullptr;
        if (failAllocationAfter > 0) --failAllocationAfter;
        return new IOBufferMemoryDescriptor(n);
    }
    void *getBytesNoCopy() { return bytes.data(); }
};
namespace StandardUSB {
struct DeviceDescriptor { uint16_t idVendor {0x13d3}, idProduct {0x3610}; };
struct InterfaceDescriptor { uint8_t bInterfaceNumber {1}, bAlternateSetting {6}; };
struct EndpointDescriptor {
    uint8_t bEndpointAddress {3}, bmAttributes {1};
    uint16_t wMaxPacketSize {63}; uint8_t bInterval {4};
};
}
class IOUSBHostDevice : public IORegistryEntry {
public:
    StandardUSB::DeviceDescriptor descriptor;
    IOUSBHostDevice() { setNumber("idVendor", 0x13d3); setNumber("idProduct", 0x3610); }
    const auto *getDeviceDescriptor() {
        assert(false && "device descriptor getter can reenter USB enumeration"); return &descriptor;
    }
};
class IOUSBHostInterface : public IORegistryEntry {
public:
    IOUSBHostDevice *device;
    StandardUSB::InterfaceDescriptor descriptor;
    bool poisonGetters {};
    unsigned deviceReads {}, descriptorReads {};
    explicit IOUSBHostInterface(IOUSBHostDevice *d) : device(d) {
        d->retain(); setNumber("idVendor", 0x13d3); setNumber("idProduct", 0x3610);
        setNumber("bInterfaceNumber", 1);
    }
    ~IOUSBHostInterface() override { device->release(); }
    IOUSBHostDevice *getDevice() { assert(!poisonGetters); ++deviceReads; return device; }
    const auto *getInterfaceDescriptor() { assert(!poisonGetters); ++descriptorReads; return &descriptor; }
};
class IOUSBHostPipe : public OSObject {
public:
    StandardUSB::EndpointDescriptor descriptor;
    bool poisonGetters {};
    unsigned descriptorReads {};
    const auto *getEndpointDescriptor() { assert(!poisonGetters); ++descriptorReads; return &descriptor; }
};
struct IOUSBHostIsochronousFrame {
    IOReturn status;
    uint32_t requestCount, completeCount, reserved;
    AbsoluteTime timeStamp;
} __attribute__((packed));
using IOUSBHostIsochronousCompletionAction = void (*)(void *, void *, IOReturn,
    IOUSBHostIsochronousFrame *);
struct IOUSBHostIsochronousCompletion {
    void *owner; IOUSBHostIsochronousCompletionAction action; void *parameter;
};
