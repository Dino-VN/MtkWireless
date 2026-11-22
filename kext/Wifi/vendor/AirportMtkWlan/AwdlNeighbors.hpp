// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef MTK_AWDL_NEIGHBORS_HPP
#define MTK_AWDL_NEIGHBORS_HPP
// MtkWireless (issue #10): macOS 26 installs neither the awdl0 link-local
// prefix route (fe80::%awdl0/64) nor neighbour entries for AWDL peers with
// this driver; no kext in the 25G342 Boot KC calls ifnet_notice_node_presence
// directly (docs/AWDL-IPV6.md). IPv6 unicast to a peer then fails
// with EHOSTUNREACH before it reaches the driver and AirDrop never moves
// data. This installs both through a kernel PF_ROUTE socket, from a thread
// call only, never in the controller gate. Boot-arg amtk_awdl_ndp=0 off.
#include <stdint.h>
#include <net/if.h>
#include <sys/kpi_socket.h>
#include <kern/thread_call.h>
#include <IOKit/IOLocks.h>
#include <AWDL/AwdlProtocol.hpp>

class AwdlNeighbors {
public:
    struct Peer { uint8_t mac[6]; bool cached; };
    struct Counters {
        uint64_t routeAdds, neighborAdds, neighborDeletes, errors, passes;
        int32_t lastErrno;
        uint32_t neighbors;  // entries currently owned
        bool routeOwned, enabled;
    };
    // Reads the boot-arg and allocates; false leaves the feature off.
    bool init();
    // Controller gate, about once per second while AWDL runs: the interface
    // name/MAC and the live unicast data peers. No socket I/O here.
    void update(const char *ifname,const uint8_t ifmac[6],const Peer *peers,unsigned count);
    // Blocking, no gate held: deletes every entry this driver added and
    // waits until no pass is queued or running. Idempotent.
    void stop();
    // stop() plus freeing; for the interface's free().
    void release();
    Counters counters();

private:
    struct Slot {
        uint8_t mac[6];
        uint64_t lastLiveUS, suppressUntilUS;
        uint32_t owned;  // ifindex our RTM_ADD succeeded on, 0 = not ours
        bool want, cached, used;
    };
    static constexpr unsigned kSlots = mtkawdl::maxPeers;
    static constexpr uint64_t kUpWatchdogUS = 5000000;   // no update for 5 s = AWDL down
    static constexpr uint64_t kPeerGraceUS = 30000000;   // keep an entry 30 s after the peer
    static constexpr uint64_t kUncachedHoldUS = 10000000; // native cache removal: no re-add 10 s
    static constexpr unsigned kRefreshPasses = 15;        // re-ADD owned entries (~30 s)
    static constexpr uint32_t kPassSeconds = 2;

    static void threadCall(thread_call_param_t,thread_call_param_t);
    void armLocked(uint32_t delaySeconds);
    void pass(bool final);
    errno_t send(int type,int flags,uint16_t ifindex,const uint8_t dst[16],bool prefix64,const uint8_t *lladdr);

    IOLock *lock = nullptr;
    thread_call_t call = nullptr;
    socket_t routeSocket = nullptr;     // pass() only; passes never overlap
    Slot slots[kSlots] = {};
    char ifName[IFNAMSIZ] = {};
    uint8_t ifMac[6] = {};
    uint64_t upUntilUS = 0;
    uint32_t routeOwned = 0, seq = 0, passCount = 0;
    bool enabled = false, stopping = false, running = false;
    bool routeExternal = false;   // EEXIST: a prefix route we did not add
    bool immediatePending = false;
    Counters stats = {};
};
#endif
