// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef MTK_AWDL_ROUTE_MESSAGE_HPP
#define MTK_AWDL_ROUTE_MESSAGE_HPP
// MtkWireless: PF_ROUTE messages for the awdl0 IPv6 link-local prefix route
// and static AWDL peer neighbours (issue #10). Header-only and free of IOKit
// so scripts/test-awdl-ndp.py builds this exact code against the host's
// <net/route.h>. The messages mirror the user-space commands that fixed
// AirDrop on hardware:
//   route -n add -inet6 -ifscope awdl0 fe80::%awdl0/64 -interface awdl0
//   ndp -s fe80::<peer EUI-64>%awdl0 <peer MAC>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <net/route.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <netinet/in.h>

namespace mtkawdl {
// The routing socket aligns every sockaddr to 4 bytes (XNU ROUNDUP32).
inline size_t routeSockaddrSpace(size_t length)
{
    return length ? 1+((length-1)|(sizeof(uint32_t)-1)) : sizeof(uint32_t);
}
// Modified EUI-64 link-local address of an AWDL peer: the address IPv6 on
// awdl0 uses for it (fe80::/64, U/L bit inverted, ff:fe in the middle).
inline void awdlLinkLocal(const uint8_t mac[6],uint8_t ip[16])
{
    memset(ip,0,16); ip[0]=0xfe; ip[1]=0x80;
    ip[8]=mac[0]^2; ip[9]=mac[1]; ip[10]=mac[2]; ip[11]=0xff; ip[12]=0xfe;
    ip[13]=mac[3]; ip[14]=mac[4]; ip[15]=mac[5];
}
struct RouteMessage {
    struct rt_msghdr header;
    uint8_t addresses[3*32]; // dst, gateway, netmask; each <= 28 bytes rounded
};
// Builds one message: RTA_DST = dst with the KAME embedded scope (bytes 2-3,
// big endian) plus sin6_scope_id, as route.c/ndp.c send it; RTA_GATEWAY (when
// lladdr is set) = AF_LINK address of ifindex carrying lladdr; RTA_NETMASK
// (when prefix64) = ffff:ffff:ffff:ffff::. RTF_IFSCOPE also sets rtm_index.
// Returns the message length (rtm_msglen).
inline size_t buildRouteMessage(RouteMessage &m,int type,int flags,uint16_t ifindex,
                                const uint8_t dst[16],bool prefix64,const uint8_t *lladdr,int seq)
{
    memset(&m,0,sizeof(m));
    uint8_t *p=m.addresses;
    int addrs=RTA_DST;
    struct sockaddr_in6 d;
    memset(&d,0,sizeof(d));
    d.sin6_len=sizeof(d); d.sin6_family=AF_INET6;
    memcpy(&d.sin6_addr,dst,16);
    d.sin6_addr.s6_addr[2]=uint8_t(ifindex>>8); d.sin6_addr.s6_addr[3]=uint8_t(ifindex);
    d.sin6_scope_id=ifindex;
    memcpy(p,&d,sizeof(d)); p+=routeSockaddrSpace(sizeof(d));
    if(lladdr) {
        struct sockaddr_dl g;
        memset(&g,0,sizeof(g));
        g.sdl_len=sizeof(g); g.sdl_family=AF_LINK; g.sdl_index=ifindex; g.sdl_type=IFT_ETHER;
        g.sdl_alen=6; memcpy(LLADDR(&g),lladdr,6);
        memcpy(p,&g,sizeof(g)); p+=routeSockaddrSpace(sizeof(g));
        addrs|=RTA_GATEWAY;
    }
    if(prefix64) {
        struct sockaddr_in6 n;
        memset(&n,0,sizeof(n));
        n.sin6_len=sizeof(n); n.sin6_family=AF_INET6;
        memset(&n.sin6_addr,0xff,8);
        memcpy(p,&n,sizeof(n)); p+=routeSockaddrSpace(sizeof(n));
        addrs|=RTA_NETMASK;
    }
    const size_t length=size_t(p-reinterpret_cast<uint8_t *>(&m));
    m.header.rtm_msglen=uint16_t(length);
    m.header.rtm_version=RTM_VERSION;
    m.header.rtm_type=uint8_t(type);
    m.header.rtm_flags=flags;
    m.header.rtm_addrs=addrs;
    m.header.rtm_seq=seq;
    if(flags&RTF_IFSCOPE) m.header.rtm_index=ifindex;
    return length;
}
} // namespace mtkawdl
#endif
