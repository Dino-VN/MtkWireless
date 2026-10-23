//
//  MtkWirelessWifi.hpp
//  MtkWireless
//
//  Phase 0 shell: attaches to IOResources, runs the Rust core self-test and
//  publishes the result. The PCIe/IO80211 driver replaces it in phase 1.
//

#ifndef MtkWirelessWifi_hpp
#define MtkWirelessWifi_hpp

#include <IOKit/IOService.h>

class MtkWirelessWifi : public IOService {
    OSDeclareDefaultStructors(MtkWirelessWifi)

public:
    bool start(IOService *provider) override;
};

#endif /* MtkWirelessWifi_hpp */
