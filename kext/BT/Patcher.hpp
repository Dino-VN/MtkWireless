//
//  Patcher.hpp
//  MtkWireless
//
//  Lilu routes on IOUSBHostFamily that adapt bluetoothd's traffic to MediaTek
//  (Read Clock, non-flushable ACL, LE feature-mask completion), plus the
//  pre-Monterey IOBluetoothFamily LE Start Encryption fix.
//

#ifndef MtkWirelessBT_Patcher_hpp
#define MtkWirelessBT_Patcher_hpp

#include <stdint.h>

namespace Patcher {

void init(uint32_t rustSelfTestFailed);
void observeMsbcController(bool supported);

} // namespace Patcher

#endif /* MtkWirelessBT_Patcher_hpp */
