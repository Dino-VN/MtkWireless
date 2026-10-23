//
//  MtkPCIe.cpp
//  SimpleMtkWlan
//
//  PCIe link setup shared by the SimpleMtkWlan and AirportMtkWlan controllers.
//

#include "MtkPCIe.hpp"

uint32_t mtk_aspm_lnkctl_before, mtk_aspm_lnkctl_after;
uint32_t mtk_aspm_l1ss_before, mtk_aspm_l1ss_after;
uint32_t mtk_aspm_parent_l1ss_before;

/*
 * Linux mt7921e disables ASPM: with L1/L1 substates enabled the device stops
 * issuing DMA and MSI writes after the link idles. Firmware leaves them on
 * for Windows, so clear L1SS on both ends of the link and ASPM on the device.
 */
void mtkDisableASPM(IOService *client, IOPCIDevice *dev)
{
    IOByteCount l1ss = 0;
    UInt8 exp = 0;

    dev->setASPMState(client, 0);

    if (dev->extendedFindPCICapability(-0x1EUL, &l1ss) && l1ss) {
        UInt32 ctl1 = dev->configRead32(l1ss + 0x8);
        mtk_aspm_l1ss_before = ctl1 | 0x80000000;
        dev->configWrite32(l1ss + 0x8, ctl1 & ~0xFU);
        mtk_aspm_l1ss_after = dev->configRead32(l1ss + 0x8) | 0x80000000;
    }

    IOPCIDevice *parent = OSDynamicCast(IOPCIDevice, dev->getParentEntry(gIOServicePlane));
    if (parent) {
        IOByteCount pl1ss = 0;
        if (parent->extendedFindPCICapability(-0x1EUL, &pl1ss) && pl1ss) {
            UInt32 ctl1 = parent->configRead32(pl1ss + 0x8);
            mtk_aspm_parent_l1ss_before = ctl1 | 0x80000000;
            parent->configWrite32(pl1ss + 0x8, ctl1 & ~0xFU);
        }
    }

    if (dev->findPCICapability(kIOPCIPCIExpressCapability, &exp) && exp) {
        UInt16 lnkctl = dev->configRead16(exp + 0x10);
        mtk_aspm_lnkctl_before = lnkctl | 0x80000000;
        dev->configWrite16(exp + 0x10, lnkctl & ~0x3);
        mtk_aspm_lnkctl_after = dev->configRead16(exp + 0x10) | 0x80000000;
    }
}
