#!/usr/bin/env python3
"""Issue #10: awdl0 fe80::/64 route and AWDL peer neighbours (AwdlNeighbors).

Builds the production AwdlRouteMessage.hpp and AwdlNeighbors.cpp on the host
against the host's <net/route.h>, with ASan/UBSan. The kernel KPIs (thread
call, IOLock, socket, ifnet, clock, boot-args) are explicit host models: the
routing table below accepts the same RTM_ADD/RTM_DELETE outcomes (EEXIST,
ESRCH) route_output() returns, it is not XNU. Hardware behaviour (that these
entries make AirDrop work) comes from the issue's report, not this test.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
vendor = root / 'kext/Wifi/vendor'

stubs = {
    'IOKit/IOLocks.h': r'''
#pragma once
struct IOLock { int depth; };
IOLock *IOLockAlloc();
void IOLockFree(IOLock *);
void IOLockLock(IOLock *);
void IOLockUnlock(IOLock *);
''',
    'IOKit/IOLib.h': r'''
#pragma once
#include <stdint.h>
#include <IOKit/IOLocks.h>
void IOLog(const char *, ...) __attribute__((format(printf, 1, 2)));
uint64_t fake_mach_absolute_time();
#define mach_absolute_time fake_mach_absolute_time
void absolutetime_to_nanoseconds(uint64_t, uint64_t *);
''',
    'kern/thread_call.h': r'''
#pragma once
#include <stdint.h>
typedef struct fake_thread_call *thread_call_t;
typedef void *thread_call_param_t;
typedef void (*thread_call_func_t)(thread_call_param_t, thread_call_param_t);
typedef int boolean_t_;
enum { THREAD_CALL_PRIORITY_KERNEL = 2, THREAD_CALL_OPTIONS_ONCE = 1 };
enum { kSecondScale = 1000000000 };
thread_call_t thread_call_allocate_with_options(thread_call_func_t, thread_call_param_t, int, int);
int thread_call_enter(thread_call_t);
int thread_call_enter_delayed(thread_call_t, uint64_t);
int thread_call_cancel_wait(thread_call_t);
int thread_call_free(thread_call_t);
void clock_interval_to_deadline(uint32_t, uint32_t, uint64_t *);
''',
    'sys/kpi_socket.h': r'''
#pragma once
#include <sys/types.h>
#include <sys/socket.h>
typedef int errno_t;
typedef struct fake_socket *socket_t;
typedef void (*sock_upcall)(socket_t, void *, int);
errno_t sock_socket(int, int, int, sock_upcall, void *, socket_t *);
errno_t sock_setsockopt(socket_t, int, int, const void *, socklen_t);
errno_t sock_shutdown(socket_t, int);
errno_t sock_ioctl(socket_t, unsigned long, void *);
void sock_close(socket_t);
errno_t sock_send(socket_t, const struct msghdr *, int, size_t *);
''',
    'net/kpi_interface.h': r'''
#pragma once
#include <stdint.h>
#include <sys/kpi_socket.h>
typedef struct fake_ifnet *ifnet_t;
errno_t ifnet_find_by_name(const char *, ifnet_t *);
uint32_t ifnet_index(ifnet_t);
errno_t ifnet_release(ifnet_t);
''',
    'pexpert/pexpert.h': r'''
#pragma once
bool PE_parse_boot_argn(const char *, void *, int);
''',
}

fixture = r'''
#include "AwdlNeighbors.hpp"
#include <AWDL/AwdlRouteMessage.hpp>
#include <IOKit/IOLib.h>
#include <net/kpi_interface.h>
#include <pexpert/pexpert.h>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// ---- kernel models -------------------------------------------------------
static uint64_t nowNs = 1000000000ull;
uint64_t fake_mach_absolute_time() { return nowNs; }
void absolutetime_to_nanoseconds(uint64_t t, uint64_t *ns) { *ns = t; }
static void advance(double seconds) { nowNs += uint64_t(seconds * 1e9); }

static int lockDepth = 0;
IOLock *IOLockAlloc() { return new IOLock{0}; }
void IOLockFree(IOLock *l) { assert(!l->depth); delete l; }
void IOLockLock(IOLock *l) { assert(!l->depth); ++l->depth; ++lockDepth; }
void IOLockUnlock(IOLock *l) { assert(l->depth == 1); --l->depth; --lockDepth; }
void IOLog(const char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); }

static int bootArg = -1;   // -1: absent
bool PE_parse_boot_argn(const char *name, void *out, int size) {
    if (strcmp(name, "amtk_awdl_ndp") || bootArg < 0) return false;
    assert(size == sizeof(int)); *(int *)out = bootArg; return true;
}

struct fake_thread_call { thread_call_func_t func; thread_call_param_t param; bool pending, running, freed; uint64_t deadline; };
static fake_thread_call *theCall;
thread_call_t thread_call_allocate_with_options(thread_call_func_t f, thread_call_param_t p, int pri, int opt) {
    assert(pri == THREAD_CALL_PRIORITY_KERNEL && opt == THREAD_CALL_OPTIONS_ONCE);
    theCall = new fake_thread_call{f, p, false, false, false, 0}; return theCall;
}
int thread_call_enter(thread_call_t c) { assert(!c->freed); int was = c->pending; c->pending = true; c->deadline = 0; return was; }
void clock_interval_to_deadline(uint32_t n, uint32_t scale, uint64_t *d) { *d = nowNs + uint64_t(n) * scale; }
int thread_call_enter_delayed(thread_call_t c, uint64_t d) { assert(!c->freed); int was = c->pending; c->pending = true; c->deadline = d; return was; }
int thread_call_cancel_wait(thread_call_t c) { assert(!c->running); int was = c->pending; c->pending = false; return was; }
int thread_call_free(thread_call_t c) { assert(!c->pending && !c->running); c->freed = true; return 1; }
// Runs the call if it is due (ONCE: one invocation at a time).
static bool runCall() {
    if (!theCall || !theCall->pending || theCall->deadline > nowNs) return false;
    theCall->pending = false; theCall->running = true;
    theCall->func(theCall->param, nullptr);
    theCall->running = false; return true;
}

struct fake_ifnet { int unused; };
static fake_ifnet awdl0;
static bool attached = true;
static uint32_t ifIndex = 11;
errno_t ifnet_find_by_name(const char *name, ifnet_t *out) {
    if (!attached || strcmp(name, "awdl0")) return ENXIO;
    *out = &awdl0; return 0;
}
uint32_t ifnet_index(ifnet_t) { return ifIndex; }
errno_t ifnet_release(ifnet_t) { return 0; }

// Routing table model: key = ifindex + destination (+ /64).
struct fake_socket { bool shutRd, loopbackOff, nonBlocking; };
static int openSockets = 0, sends = 0;
static std::map<std::string, std::vector<uint8_t>> table;   // key -> gateway MAC
static std::vector<std::string> routeLog;
static void (*sendHook)() = nullptr;
errno_t sock_socket(int domain, int type, int proto, sock_upcall, void *, socket_t *out) {
    assert(domain == PF_ROUTE && type == SOCK_RAW && proto == 0);
    *out = new fake_socket{false, false, false}; ++openSockets; return 0;
}
errno_t sock_setsockopt(socket_t s, int level, int name, const void *v, socklen_t) {
    if (level == SOL_SOCKET && name == SO_USELOOPBACK && !*(const int *)v) s->loopbackOff = true; return 0;
}
errno_t sock_shutdown(socket_t s, int how) { if (how == SHUT_RD) s->shutRd = true; return 0; }
errno_t sock_ioctl(socket_t s, unsigned long, void *v) { s->nonBlocking = *(int *)v; return 0; }
void sock_close(socket_t s) { delete s; --openSockets; }
static std::string key(const uint8_t *addr, bool prefix) {
    char b[64]; std::string k;
    for (int i = 0; i < 16; ++i) { snprintf(b, sizeof(b), "%02x", addr[i]); k += b; }
    return k + (prefix ? "/64" : "");
}
errno_t sock_send(socket_t s, const struct msghdr *msg, int flags, size_t *sent) {
    assert(s->shutRd && s->loopbackOff && s->nonBlocking && flags == MSG_DONTWAIT);
    assert(!lockDepth);   // never under the AwdlNeighbors lock
    assert(msg->msg_iovlen == 1);
    const uint8_t *m = (const uint8_t *)msg->msg_iov[0].iov_base;
    auto h = (const rt_msghdr *)m;
    assert(h->rtm_msglen == msg->msg_iov[0].iov_len && h->rtm_version == RTM_VERSION);
    size_t at = sizeof(rt_msghdr);
    auto dst = (const sockaddr_in6 *)(m + at); at += mtkawdl::routeSockaddrSpace(sizeof(sockaddr_in6));
    assert(dst->sin6_family == AF_INET6 && dst->sin6_len == sizeof(sockaddr_in6));
    const uint32_t index = dst->sin6_scope_id;
    assert(dst->sin6_addr.s6_addr[0] == 0xfe && dst->sin6_addr.s6_addr[1] == 0x80);
    assert(dst->sin6_addr.s6_addr[2] == uint8_t(index >> 8) && dst->sin6_addr.s6_addr[3] == uint8_t(index));
    std::vector<uint8_t> gateway;
    if (h->rtm_addrs & RTA_GATEWAY) {
        auto g = (const sockaddr_dl *)(m + at); at += mtkawdl::routeSockaddrSpace(sizeof(sockaddr_dl));
        assert(g->sdl_family == AF_LINK && g->sdl_index == index && g->sdl_alen == 6);
        gateway.assign((const uint8_t *)LLADDR(g), (const uint8_t *)LLADDR(g) + 6);
    }
    bool prefix = false;
    if (h->rtm_addrs & RTA_NETMASK) {
        auto n = (const sockaddr_in6 *)(m + at); at += mtkawdl::routeSockaddrSpace(sizeof(sockaddr_in6));
        static const uint8_t m64[16] = {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
        assert(!memcmp(&n->sin6_addr, m64, 16)); prefix = true;
        assert(h->rtm_flags & RTF_IFSCOPE && h->rtm_index == index);
    } else assert(!(h->rtm_flags & RTF_IFSCOPE));
    assert(at == h->rtm_msglen);
    const std::string k = std::to_string(index) + "%" + key(dst->sin6_addr.s6_addr, prefix);
    ++sends; *sent = h->rtm_msglen;
    errno_t e = 0;
    if (h->rtm_type == RTM_ADD) {
        assert(gateway.size() == 6);
        if (prefix) assert(h->rtm_flags == (RTF_UP | RTF_STATIC | RTF_IFSCOPE));
        else assert(h->rtm_flags == (RTF_HOST | RTF_STATIC));
        if (table.count(k)) e = EEXIST; else table[k] = gateway;
    } else if (h->rtm_type == RTM_DELETE) {
        assert(gateway.empty());
        if (!table.erase(k)) e = ESRCH;
    } else assert(false);
    routeLog.push_back(std::string(h->rtm_type == RTM_ADD ? "add " : "del ") + k + (e ? " err" : ""));
    if (sendHook) sendHook();
    return e;
}

// ---- helpers ---------------------------------------------------------------
static const uint8_t ifMac[6] = {0x3e,0x7c,0xf9,0x82,0xec,0x84};
static const uint8_t peerA[6] = {0x6a,0x5d,0xb7,0x71,0x60,0x5b};
static const uint8_t peerB[6] = {0x02,0x11,0x22,0x33,0x44,0x55};
static std::string routeKey(uint32_t index) {
    static const uint8_t ll[16] = {0xfe,0x80};
    uint8_t a[16]; memcpy(a, ll, 16); a[2] = uint8_t(index >> 8); a[3] = uint8_t(index);
    return std::to_string(index) + "%" + key(a, true);
}
static std::string peerKey(const uint8_t *mac, uint32_t index) {
    uint8_t a[16]; mtkawdl::awdlLinkLocal(mac, a); a[2] = uint8_t(index >> 8); a[3] = uint8_t(index);
    return std::to_string(index) + "%" + key(a, false);
}
static void tick(AwdlNeighbors &n, std::vector<AwdlNeighbors::Peer> peers) {
    n.update("awdl0", ifMac, peers.data(), unsigned(peers.size()));
    while (runCall()) {}
}
// Advance in 1 s steps, calling update() like publishScheduler would.
static void run(AwdlNeighbors &n, double seconds, std::vector<AwdlNeighbors::Peer> peers) {
    for (double t = 0; t < seconds; t += 1) { advance(1); tick(n, peers); }
}
// Advance with AWDL down: no update(), only the self-armed passes.
static void idle(double seconds) {
    for (double t = 0; t < seconds; t += 0.5) { advance(0.5); while (runCall()) {} }
}
static AwdlNeighbors::Peer P(const uint8_t *mac, bool cached) { AwdlNeighbors::Peer p = {}; memcpy(p.mac, mac, 6); p.cached = cached; return p; }

static AwdlNeighbors *fresh() {
    table.clear(); routeLog.clear(); attached = true; ifIndex = 11; sendHook = nullptr;
    auto *n = new AwdlNeighbors(); assert(n->init()); return n;
}
static void done(AwdlNeighbors *n) { n->release(); delete n; assert(!openSockets); assert(!theCall || theCall->freed); theCall = nullptr; }

int main() {
    // 1. Message bytes.
    {
        mtkawdl::RouteMessage m;
        static const uint8_t ll[16] = {0xfe,0x80};
        size_t len = mtkawdl::buildRouteMessage(m, RTM_ADD, RTF_UP|RTF_STATIC|RTF_IFSCOPE, 11, ll, true, ifMac, 7);
        assert(len == sizeof(rt_msghdr) + 28 + 20 + 28 && m.header.rtm_msglen == len);
        assert(m.header.rtm_addrs == (RTA_DST|RTA_GATEWAY|RTA_NETMASK) && m.header.rtm_index == 11 && m.header.rtm_seq == 7);
        len = mtkawdl::buildRouteMessage(m, RTM_DELETE, 0, 0x1234, ll, false, nullptr, 8);
        auto d = (const sockaddr_in6 *)m.addresses;
        assert(len == sizeof(rt_msghdr) + 28 && m.header.rtm_index == 0 && m.header.rtm_addrs == RTA_DST);
        assert(d->sin6_addr.s6_addr[2] == 0x12 && d->sin6_addr.s6_addr[3] == 0x34 && d->sin6_scope_id == 0x1234);
        uint8_t ip[16]; mtkawdl::awdlLinkLocal(peerA, ip);
        const uint8_t want[16] = {0xfe,0x80,0,0,0,0,0,0,0x68,0x5d,0xb7,0xff,0xfe,0x71,0x60,0x5b};
        assert(!memcmp(ip, want, 16));
    }
    // 2. Boot-arg off: nothing allocated, update() is inert.
    {
        bootArg = 0; AwdlNeighbors n; assert(!n.init());
        auto p = P(peerA, true); n.update("awdl0", ifMac, &p, 1);
        assert(!theCall && table.empty() && !n.counters().enabled);
        n.stop(); n.release(); bootArg = -1;
    }
    // 3. Install, then stop removes exactly our entries.
    {
        auto *n = fresh();
        tick(*n, {P(peerA, true)});
        assert(table.size() == 2 && table.count(routeKey(11)) && table.count(peerKey(peerA, 11)));
        assert(table[routeKey(11)] == std::vector<uint8_t>(ifMac, ifMac + 6));
        assert(table[peerKey(peerA, 11)] == std::vector<uint8_t>(peerA, peerA + 6));
        auto c = n->counters();
        assert(c.routeOwned && c.neighbors == 1 && c.routeAdds == 1 && c.neighborAdds == 1 && !c.errors);
        table["7%other"] = {};   // unrelated entry survives
        int before = sends; run(*n, 3, {P(peerA, true)});
        assert(sends == before);   // steady state sends nothing
        n->stop();
        assert(table.size() == 1 && table.count("7%other") && !openSockets);
        n->update("awdl0", ifMac, nullptr, 0); assert(!theCall->pending);   // inert after stop
        done(n);
    }
    // 4. Somebody else's prefix route is used, never deleted; an existing
    //    (e.g. dynamic) neighbour is replaced once, like ndp -s.
    {
        auto *n = fresh();
        table[routeKey(11)] = {9,9,9,9,9,9};
        table[peerKey(peerA, 11)] = {1,1,1,1,1,1};
        tick(*n, {P(peerA, true)});
        assert(!n->counters().routeOwned && n->counters().neighbors == 1);
        assert(table[peerKey(peerA, 11)] == std::vector<uint8_t>(peerA, peerA + 6));
        int before = sends; run(*n, 5, {P(peerA, true)}); assert(sends == before);  // no ADD retry per pass
        n->stop();
        assert(table.size() == 1 && table[routeKey(11)] == std::vector<uint8_t>(6, 9));
        done(n);
    }
    // 5. Native cache removal deletes at once and holds re-adds for 10 s;
    //    a non-live peer is kept for 30 s.
    {
        auto *n = fresh();
        tick(*n, {P(peerA, true), P(peerB, false)});
        assert(table.count(peerKey(peerA, 11)) && table.count(peerKey(peerB, 11)));
        advance(1); tick(*n, {P(peerA, false), P(peerB, false)});
        assert(!table.count(peerKey(peerA, 11)) && table.count(peerKey(peerB, 11)));
        run(*n, 8, {P(peerA, false), P(peerB, false)});
        assert(!table.count(peerKey(peerA, 11)));
        run(*n, 3, {P(peerA, false), P(peerB, false)});
        assert(table.count(peerKey(peerA, 11)));   // hold over, live peer back
        run(*n, 29, {P(peerA, false)});
        assert(table.count(peerKey(peerB, 11)));   // within the 30 s grace
        run(*n, 3, {P(peerA, false)});
        assert(!table.count(peerKey(peerB, 11)) && table.count(peerKey(peerA, 11)));
        assert(n->counters().neighbors == 1);
        done(n);
        assert(table.empty());
    }
    // 6. AWDL down (no update for 5 s): everything is removed, socket closed,
    //    the call stops re-arming; coming back up reinstalls.
    {
        auto *n = fresh();
        tick(*n, {P(peerA, true)});
        idle(4); assert(table.size() == 2);
        idle(2.5); assert(table.empty() && !openSockets && !theCall->pending);
        assert(n->counters().neighbors == 0 && !n->counters().routeOwned);
        advance(1); tick(*n, {P(peerA, true)}); assert(table.size() == 2);
        done(n);
    }
    // 7. awdl0 re-attached with a new index: old entries are not touched
    //    (the kernel dropped them), new ones go on the new index.
    {
        auto *n = fresh();
        tick(*n, {P(peerA, true)});
        table.clear(); ifIndex = 12;
        advance(1); tick(*n, {P(peerA, true)});
        assert(table.size() == 2 && table.count(routeKey(12)) && table.count(peerKey(peerA, 12)));
        for (auto &l : routeLog) assert(l.rfind("del 11%", 0) != 0);
        attached = false; n->stop();   // detached: nothing to delete, no error
        assert(n->counters().neighbors == 0 && !openSockets);
        done(n);
    }
    // 8. Refresh repairs an owned entry deleted behind our back.
    {
        auto *n = fresh();
        tick(*n, {P(peerA, true)});
        table.erase(peerKey(peerA, 11)); table.erase(routeKey(11));
        run(*n, 16, {P(peerA, true)});
        assert(table.count(peerKey(peerA, 11)) && table.count(routeKey(11)));
        done(n);
    }
    // 9. A slot is never recycled while a pass works on its snapshot: the
    //    gate drops peer A and offers peer B while A's RTM_ADD is in flight.
    {
        auto *n = fresh();
        static AwdlNeighbors *target; target = n;
        sendHook = [] {
            sendHook = nullptr;
            advance(31);
            target->update("awdl0", ifMac, nullptr, 0);   // A past its grace
            auto p = P(peerB, false);
            target->update("awdl0", ifMac, &p, 1);        // must not take A's slot
        };
        tick(*n, {P(peerA, false)});
        // The in-flight ADD kept its slot, so the next pass deleted A (no leak).
        assert(!table.count(peerKey(peerA, 11)) && n->counters().neighbors == 0);
        assert(std::count(routeLog.begin(), routeLog.end(), "add " + peerKey(peerA, 11)) == 1);
        assert(std::count(routeLog.begin(), routeLog.end(), "del " + peerKey(peerA, 11)) == 1);
        run(*n, 2, {P(peerB, false)});
        assert(!table.count(peerKey(peerA, 11)) && table.count(peerKey(peerB, 11)) && table.size() == 2);
        done(n);
        assert(table.empty());
    }
    puts("PASS: AWDL neighbour messages, ownership, cache/grace/watchdog, reattach, refresh, stop");
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='mtk-ndp-') as tmp:
        tmp = Path(tmp)
        for name, text in stubs.items():
            path = tmp / 'stubs' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        (tmp / 'test.cpp').write_text(fixture)
        binary = tmp / 'test'
        cmd = ['clang++', '-std=c++14', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
               '-I', str(tmp / 'stubs'), '-I', str(vendor / 'include'),
               '-I', str(vendor / 'AirportMtkWlan'),
               str(tmp / 'test.cpp'), str(vendor / 'AirportMtkWlan/AwdlNeighbors.cpp'),
               '-o', str(binary)]
        subprocess.run(cmd, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    sys.exit(main())
