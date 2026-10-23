//
//  MtkPCIEDeviceWrapper.hpp
//  AirportMtkWlan-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef MtkPCIEDeviceWrapper_hpp
#define MtkPCIEDeviceWrapper_hpp

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOTypes.h>

#include <HAL/MtkHalService.hpp>

class MtkPCIEDeviceWrapper : public IOService {
    OSDeclareDefaultStructors(MtkPCIEDeviceWrapper)
    
public:
    virtual IOService* probe(IOService* provider, SInt32* score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual IOWorkLoop* getWorkLoop() const override;
    virtual IOReturn setPowerState(
        unsigned long powerStateOrdinal,
                                   IOService *   whatDevice ) override;
    
public:
    MtkHalService *fHalService;
    IOPCIDevice *pciNub;
};

#endif /* MtkPCIEDeviceWrapper_hpp */
