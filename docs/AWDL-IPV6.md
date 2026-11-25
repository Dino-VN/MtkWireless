# AWDL IPv6 route and neighbours (issue #10)

AirDrop, Handoff and the rest run IPv6 over `awdl0`, to the peer's
link-local address (`fe80::` + modified EUI-64 of its AWDL MAC). With this
driver on macOS 26 the stack has neither of the entries that needs:

- the prefix route `fe80::%awdl0/64`, and
- a neighbour (NDP) entry for each AWDL peer.

Without them, unicast to the peer fails in the stack with `EHOSTUNREACH`
before it reaches the driver. Discovery still works because it runs on
multicast.

## Hardware report (MT7922 14c3:0616, 25G342, iOS 27.0.1, 2026-11-20)

- Synced (`MTKAWDLHostRole=2`), both sides saw each other in AirDrop, and
  transfers stayed at "Waiting". The iPhone's TCP SYNs arrived, but no reply
  left the stack: `MTKAWDLTXDataAcked` stayed 0, `NativeDroppedPackets`
  grew, and `ping6 <peer>%awdl0` returned "No route to host".
- `netstat -rn -f inet6` had no `fe80::%awdl0/64`, and `ndp -an` had no
  entry for the peer.
- Adding the route alone did not help.
- Adding the route and `ndp -s <peer EUI-64>%awdl0 <peer MAC>` made AirDrop
  work in both directions over awdl0.

A second machine (25G342, MtkWireless 1.1.5) shows the same gap: awdl0 has
its own `fe80::...%awdl0` address and host route, but no prefix route.

## Why the driver does it

The kernel has a KPI for exactly this: `ifnet_notice_node_presence` (and
`_absence`, both in `com.apple.kpi.private`) leads to `nd6_alt_node_present`.
It would still need the prefix route. On 25G342, no kext in the Boot KC
calls it directly, IO80211Family and AppleBCMWLANCoreMac included. The only
direct callers are the kernel's own wrappers. Indirect calls and the System
KC were not checked. Why a native Mac has the
prefix route and peer entries, and we don't, is still open. Possibly
wifip2pd adds them in reaction to something the Broadcom driver reports.

## What the driver does

`AwdlNeighbors` (`kext/Wifi/vendor/AirportMtkWlan/AwdlNeighbors.{hpp,cpp}`)
works through a kernel `PF_ROUTE` socket, from a thread call. It never runs
in the controller gate.

- **Desired state.** `AirportMtkAwdlInterface::updateNeighbors` runs once per
  second from `publishScheduler` while AWDL and its data path are up. It
  passes the interface name and MAC, and every live unicast data peer.
- **Prefix route.** `fe80::/64`, `RTF_UP|RTF_STATIC|RTF_IFSCOPE`, with an
  `AF_LINK` gateway for awdl0. This is the same entry as
  `route add -inet6 -ifscope awdl0 fe80::%awdl0/64 -interface awdl0`.
  If the add returns EEXIST, the route belongs to someone else: the driver
  uses it, never deletes it, and checks it again only on refresh passes.
- **Peer neighbours.** `RTF_HOST|RTF_STATIC` (the kernel adds `RTF_LLINFO`),
  with an `AF_LINK` gateway set to the peer MAC, as `ndp -s` sets them. An
  existing entry (EEXIST) is deleted and added once, as `ndp.c set()` does.
- **Removal.** Entries go 30 s after the peer was last live, when AWDL goes
  down (no update for 5 s), and on `stop()`. Native peer-cache removal
  (`PEER_CACHE_CONTROL op=2`) no longer deletes or suppresses the neighbour
  immediately: on 2026-11-25 hardware (`diagnostics/airdrop-benchmark-gpkoe571`),
  macOS issued `op=2` 0.5 s after `UPLOAD START`, and deleting the route caused
  `EHOSTUNREACH` and permanently closed QUIC's `awdl0` path
  (`validated -> unavailable`). The driver deletes only entries its own
  `RTM_ADD` created.
- **Refresh.** Every 15th pass (about 30 s) re-adds owned entries, in case
  something deleted them.
- **Re-attach.** The awdl0 index is resolved by name on every pass. Entries
  from an older attach are forgotten, not deleted, because the kernel
  dropped them with that interface.
- **Socket.** Read side shut down and `SO_USELOOPBACK=0`, so no route
  messages queue up on it. Non-blocking. `route_output()` returns its errno
  through `sock_send`.
- **Stop.** `stop()` runs before the gate teardown. It sets `stopping`, calls
  `thread_call_cancel_wait`, and then runs the final delete pass inline.
- **Boot-arg.** `amtk_awdl_ndp=0` turns it off.

ioreg on `AirportMtkAwdlInterface`: `MTKAWDLNdpEnabled`,
`MTKAWDLNdpRouteOwned`, `MTKAWDLNdpNeighbors` (entries owned now),
`MTKAWDLNdp{RouteAdds,NeighborAdds,NeighborDeletes,Errors,LastErrno,Passes}`.

Check on hardware:

```sh
netstat -rn -f inet6 | grep awdl0   # fe80::%awdl0/64 ... awdl0
ndp -an | grep awdl0                # fe80::<peer>%awdl0 <peer MAC> awdl0 permanent
```

Tests: `scripts/test-awdl-ndp.py` builds the production message builder and
`AwdlNeighbors.cpp` on the host with ASan/UBSan, against a modelled routing
table, thread call and ifnet. Unverified on hardware with this
implementation. The reporter's patch, which sends the same messages, was
verified (TXDataAcked 401-630, transfers in both directions, survives
reboots).

## AWDL scheduler in 1.1.6

The driver enables these changes on startup without extra boot arguments.

- Keep a live same-channel ROC when it covers an unserved window the greedy
  planner skipped. Forward recovery and the HAL's channel restrictions still
  apply. `MTKAWDLLiveGrantKept` counts retained grants and
  `MTKAWDLKeptGrantStarts` counts window starts those grants served.
- After three consecutive phase rejections, re-acquire the selected parent
  and root between windows and with no pending or live ROC. If three more
  samples fail within one second, double the two-second retry interval. Four
  such failures disable re-acquisition until the clock tree changes. See
  `MTKAWDLClockRejectStreak`, `MTKAWDLClockReacquires` and
  `MTKAWDLClockReacquireFailed`.
- Prefer the receive channel for a multicast IPv6 mDNS reply queued within
  350 ms of an admitted peer's query. A qualifying reply pauses the idle cap
  so the scheduler can take that channel's next window. Hard channel limits
  still apply. Use ordinary multicast scheduling if the channel is unavailable,
  the peer is no longer live, or the override expires after 1.1 s. See
  `MTKAWDLMdnsQueriesObserved` and `MTKAWDLMdnsRepliesOnQueryChannel`.
- Allow an AW start up to 9 TU (9216 µs) after its scheduled edge. Native AW
  duration and grant coverage still use the 10 TU tolerance.

`MTKAWDLAbortBy` is seven little-endian `uint32_t` counters for explicit ROC
aborts, in order: switch lead, idle-expired request, schedule change,
forward recovery, reservation invalidation, advertiser parent change, sync
state change. ROC issuance and grant timing remain in `MTKAWDLROCTrace`.

Test on macOS 26 with the Wi-Fi Release kext. Try AirDrop in both directions
with the iPhone unplugged and the Mac associated, and compare the ioreg
counters before and after. Hardware verification of this build is pending.
