// SPDX-License-Identifier: GPL-2.0-or-later
// MtkWireless (issue #10): driver-installed awdl0 link-local prefix route and
// AWDL peer neighbours. See AwdlNeighbors.hpp.
//
// Hardware (MT7922, 25G342, iOS 27.0.1, 2026-11-20, reporter): with neither
// entry, TCP SYNs from the iPhone arrived but no reply left the stack
// (TXDataAcked 0, ping6 "No route to host"). The route alone did not help;
// route plus `ndp -s <EUI-64>%awdl0 <mac>` made AirDrop work both ways.
//
// Entries: fe80::/64 ifscoped to awdl0 (RTF_UP|RTF_STATIC|RTF_IFSCOPE, AF_LINK
// gateway = awdl0, like `route add -interface`), and one permanent neighbour
// per live data peer (RTF_HOST|RTF_STATIC, AF_LINK gateway = peer MAC, like
// `ndp -s`; the kernel adds RTF_LLINFO). Only entries our own RTM_ADD created
// are deleted: on native peer-cache removal, 30 s without the peer, AWDL down
// (no update() for 5 s) or stop. An EEXIST prefix route belongs to someone
// else and is left alone. An EEXIST neighbour is replaced once, as ndp.c set()
// does. Every 15th pass re-adds owned entries in case something deleted them.
#include "AwdlNeighbors.hpp"
#include <AWDL/AwdlRouteMessage.hpp>
#include <IOKit/IOLib.h>
#include <net/kpi_interface.h>
#include <sys/errno.h>
#include <sys/filio.h>
#include <pexpert/pexpert.h>

static uint64_t neighborsNowUS()
{
    uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(),&ns); return ns/1000;
}

bool AwdlNeighbors::init()
{
    if(lock) return enabled;
    int value=1;
    PE_parse_boot_argn("amtk_awdl_ndp",&value,sizeof(value));
    if(!value) return false;
    lock=IOLockAlloc();
    if(!lock) return false;
    call=thread_call_allocate_with_options(&AwdlNeighbors::threadCall,this,
        THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
    if(!call) { IOLockFree(lock); lock=nullptr; return false; }
    enabled=stats.enabled=true;
    return true;
}

void AwdlNeighbors::threadCall(thread_call_param_t self,thread_call_param_t)
{
    static_cast<AwdlNeighbors *>(self)->pass(false);
}

// Caller holds lock.
void AwdlNeighbors::armLocked(uint32_t delaySeconds)
{
    if(!call || stopping) return;
    if(!delaySeconds) {
        immediatePending=true;
        thread_call_enter(call);
        return;
    }
    uint64_t deadline=0;
    clock_interval_to_deadline(delaySeconds,kSecondScale,&deadline);
    thread_call_enter_delayed(call,deadline);
}

void AwdlNeighbors::update(const char *name,const uint8_t mac[6],const Peer *peers,unsigned count)
{
    if(!enabled || !name || !name[0]) return;
    const uint64_t now=neighborsNowUS();
    IOLockLock(lock);
    if(stopping) { IOLockUnlock(lock); return; }
    strlcpy(ifName,name,sizeof(ifName));
    memcpy(ifMac,mac,6);
    upUntilUS=now+kUpWatchdogUS;
    bool live[kSlots]={}, cachedNow[kSlots]={};
    for(unsigned i=0;i<count;++i) {
        const uint8_t *peer=peers[i].mac;
        if(!mtkawdl::unicast(peer)) continue;
        int slot=-1, spare=-1;
        for(unsigned j=0;j<kSlots;++j) {
            if(slots[j].used && !memcmp(slots[j].mac,peer,6)) { slot=int(j); break; }
            if(spare<0 && (!slots[j].used || (!slots[j].want && !slots[j].owned))) spare=int(j);
        }
        // Never recycle a slot while a pass works on a snapshot of it: its
        // write-back matches slots by index and MAC.
        if(slot<0) {
            if(spare<0 || running) continue;
            slot=spare;
            slots[slot]={};
            memcpy(slots[slot].mac,peer,6);
            slots[slot].used=true;
        }
        Slot &s=slots[slot];
        live[slot]=true; cachedNow[slot]=peers[i].cached; s.lastLiveUS=now;
        if(peers[i].cached || now>=s.suppressUntilUS) s.want=true;
    }
    for(unsigned j=0;j<kSlots;++j) {
        Slot &s=slots[j];
        if(!s.used) continue;
        if(s.cached && !cachedNow[j]) {
            // Native removed the peer from its cache: delete now and do not
            // re-add for 10 s unless native caches it again.
            s.want=false; s.suppressUntilUS=now+kUncachedHoldUS;
        } else if(!live[j] && now-s.lastLiveUS>kPeerGraceUS) s.want=false;
        s.cached=cachedNow[j];
        if(!s.want && !s.owned && !running && now>=s.suppressUntilUS) s.used=false;
    }
    armLocked(0);
    IOLockUnlock(lock);
}

errno_t AwdlNeighbors::send(int type,int flags,uint16_t index,const uint8_t dst[16],bool prefix64,const uint8_t *lladdr)
{
    if(!routeSocket) return ENOTCONN;
    mtkawdl::RouteMessage m;
    const size_t length=mtkawdl::buildRouteMessage(m,type,flags,index,dst,prefix64,lladdr,int(++seq));
    struct iovec iov={&m,length};
    struct msghdr msg={};
    msg.msg_iov=&iov; msg.msg_iovlen=1;
    size_t sent=0;
    // route_output() runs synchronously; its errno is the send's result.
    return sock_send(routeSocket,&msg,MSG_DONTWAIT,&sent);
}

// Serialized: thread call (THREAD_CALL_OPTIONS_ONCE) or stop() after
// thread_call_cancel_wait. Socket I/O runs without the lock.
void AwdlNeighbors::pass(bool final)
{
    IOLockLock(lock);
    if(running) { IOLockUnlock(lock); return; }
    running=true; immediatePending=false;
    const uint64_t now=neighborsNowUS();
    const bool up=!final && !stopping && upUntilUS && now<upUntilUS;
    char name[IFNAMSIZ]; memcpy(name,ifName,sizeof(name));
    uint8_t mac[6]; memcpy(mac,ifMac,6);
    Slot snap[kSlots]; memcpy(snap,slots,sizeof(snap));
    uint32_t route=routeOwned;
    bool external=routeExternal;
    const bool refresh=++passCount%kRefreshPasses==0;
    IOLockUnlock(lock);

    // Resolve awdl0 every pass; an index from an older attach is never used.
    uint16_t index=0;
    if(name[0]) {
        ifnet_t ifp=nullptr;
        if(!ifnet_find_by_name(name,&ifp) && ifp) { index=uint16_t(ifnet_index(ifp)); ifnet_release(ifp); }
    }
    if(route && route!=index) route=0;   // the kernel dropped it with that interface
    for(auto &s:snap) if(s.owned && s.owned!=index) s.owned=0;
    bool anyOwned=route;
    for(auto &s:snap) anyOwned|=s.owned!=0;

    uint64_t routeAdds=0, adds=0, deletes=0, errors=0;
    int lastErrno=0;
    auto fail=[&](errno_t e) { ++errors; lastErrno=e; };
    if(!routeSocket && index && (up || anyOwned)) {
        socket_t so=nullptr;
        errno_t e=sock_socket(PF_ROUTE,SOCK_RAW,0,nullptr,nullptr,&so);
        if(!e && so) {
            int zero=0, on=1;
            sock_setsockopt(so,SOL_SOCKET,SO_USELOOPBACK,&zero,sizeof(zero)); // no replies queued to us
            sock_shutdown(so,SHUT_RD);  // nor every system route message
            sock_ioctl(so,FIONBIO,&on);
            routeSocket=so;
        } else fail(e ? e : ENOMEM);
    }
    static const uint8_t linkLocal[16]={0xfe,0x80};
    if(routeSocket && up && index) {
        if((!route && (!external || refresh)) || (route && refresh)) {
            errno_t e=send(RTM_ADD,RTF_UP|RTF_STATIC|RTF_IFSCOPE,index,linkLocal,true,mac);
            if(!e) { ++routeAdds; route=index; external=false; }
            else if(e==EEXIST) { if(!route) external=true; }
            else fail(e);
        }
    } else external=false;
    for(auto &s:snap) {
        if(!s.used) continue;
        uint8_t ip[16];
        mtkawdl::awdlLinkLocal(s.mac,ip);
        if(routeSocket && up && index && s.want) {
            if(!s.owned) {
                errno_t e=send(RTM_ADD,RTF_HOST|RTF_STATIC,index,ip,false,s.mac);
                if(e==EEXIST) {
                    send(RTM_DELETE,0,index,ip,false,nullptr);
                    e=send(RTM_ADD,RTF_HOST|RTF_STATIC,index,ip,false,s.mac);
                }
                if(!e) { s.owned=index; ++adds; } else fail(e);
            } else if(refresh) {
                errno_t e=send(RTM_ADD,RTF_HOST|RTF_STATIC,index,ip,false,s.mac);
                if(!e) ++adds;
                else if(e!=EEXIST) fail(e);
            }
        } else if(s.owned) {
            errno_t e=send(RTM_DELETE,0,index,ip,false,nullptr);
            if(!e || e==ESRCH) { ++deletes; s.owned=0; }
            else {
                fail(e);
                if(!up) s.owned=0; // do not retry forever once AWDL is down
            }
        }
    }
    if(route && !up) {
        errno_t e=send(RTM_DELETE,RTF_IFSCOPE,index,linkLocal,true,nullptr);
        if(e && e!=ESRCH) fail(e);
        route=0;
    }
    bool owned=route;
    for(auto &s:snap) owned|=s.owned!=0;
    if(routeSocket && (final || (!up && !owned))) { sock_close(routeSocket); routeSocket=nullptr; }

    IOLockLock(lock);
    uint32_t ownedCount=0;
    for(unsigned i=0;i<kSlots;++i) {
        Slot &s=slots[i];
        if(s.used && !memcmp(s.mac,snap[i].mac,6)) s.owned=snap[i].owned;
        if(!up) { s.want=false; if(!s.owned) s.used=false; }
        if(s.used && s.owned) ++ownedCount;
    }
    routeOwned=route; routeExternal=external;
    stats.routeAdds+=routeAdds; stats.neighborAdds+=adds; stats.neighborDeletes+=deletes;
    stats.errors+=errors; ++stats.passes;
    if(errors) stats.lastErrno=lastErrno;
    stats.neighbors=ownedCount; stats.routeOwned=route!=0;
    // While up, a pass every 2 s also runs the down watchdog; while entries
    // remain after AWDL went down, retry their deletion.
    if(!immediatePending && (up || owned)) armLocked(kPassSeconds);
    running=false;
    IOLockUnlock(lock);
    if(errors && !up) IOLog("MtkWireless: AWDL neighbour cleanup: %llu error(s), last errno %d\n",errors,lastErrno);
}

void AwdlNeighbors::stop()
{
    if(!lock) return;
    IOLockLock(lock);
    stopping=true;
    IOLockUnlock(lock);
    // A running pass cannot re-arm once stopping is set.
    thread_call_cancel_wait(call);
    pass(true);
}

void AwdlNeighbors::release()
{
    if(!lock) return;
    stop();
    thread_call_free(call); call=nullptr;
    IOLockFree(lock); lock=nullptr;
    enabled=false;
}

AwdlNeighbors::Counters AwdlNeighbors::counters()
{
    if(!lock) return stats;
    IOLockLock(lock);
    Counters c=stats;
    IOLockUnlock(lock);
    return c;
}
