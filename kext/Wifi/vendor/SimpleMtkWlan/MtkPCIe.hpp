//
//  MtkPCIe.hpp
//  SimpleMtkWlan
//
//  PCIe link setup shared by the SimpleMtkWlan and AirportMtkWlan controllers.
//

#ifndef MtkPCIe_hpp
#define MtkPCIe_hpp

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

/* Config-space values before/after, reported through the debug ioctl. */
extern uint32_t mtk_aspm_lnkctl_before, mtk_aspm_lnkctl_after;
extern uint32_t mtk_aspm_l1ss_before, mtk_aspm_l1ss_after;
extern uint32_t mtk_aspm_parent_l1ss_before;

void mtkDisableASPM(IOService *client, IOPCIDevice *dev);

#endif /* MtkPCIe_hpp */
