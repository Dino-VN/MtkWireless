#ifndef IO80211P2PInterface_h
#define IO80211P2PInterface_h

#include <Airport/macOS26/IO80211AWDLProtocol.h>

// Tahoe's native P2P object uses the same virtual-interface storage and AWDL
// ioctl surface in this target. Keep a distinct class identity for the role-6
// interface without redeclaring the pre-Tahoe P2P vtable.
class IO80211P2PInterface : public IO80211AWDLProtocol {
    OSDeclareAbstractStructors(IO80211P2PInterface)
};

static_assert(sizeof(IO80211P2PInterface) == 0x130, "Tahoe P2P interface ABI");

#endif /* IO80211P2PInterface_h */
