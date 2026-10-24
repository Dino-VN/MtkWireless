# Wi-Fi roaming

Status: implemented in 1.0.1, **not run on hardware**. The roam is a full
re-association (deauth, Open System auth, (re)assoc, PSK 4-way handshake),
so data stalls for the length of the handshake (expected well under a
second). The difference from before is that the link stays up: the BSD
interface keeps its IP configuration, and airportd/WCL see a roam instead of
a disconnect followed by an auto-join.

Covered: open networks and WPA/WPA2-Personal (PSK, SHA256-PSK, the PSK side
of a WPA3 transition network). Not covered: WEP, 802.1X (the PMK is cached for
the joined BSSID only), 802.11r FT, 802.11k/v and roaming to 6 GHz (off
unless `amtk_6ghz=1`, and 6 GHz BSSes take SAE H2E or OWE only; see
[WIFI-6GHZ.md](WIFI-6GHZ.md)). Boot-arg `amtk_roam=0` restores the old
behavior.

## How WCL sees a Broadcom roam (macOS 26.7.1 25G241)

Taken from IO80211Family and AppleBCMWLANCoreMac in
`/System/Library/KernelCollections/BootKernelExtensions.kc` (both keep their
symbols). WCL's bulletin-board event index equals the driver message id
(the join messages 211-214 the driver already sends prove it).

| Id | Name | Size | WCL handler | Payload |
|---|---|---|---|---|
| 137 | ROAM_START | 12 | `WCLRoamManager::handleRoamStartEvent` | u32 status, u32 reason (`0xe0820c00` + WLC reason), i16 RSSI |
| 139 | ROAM_PREP | 12 | `handleRoamPrepEvent` | u32 reason, target BSSID, i16 target RSSI |
| 73 | REASSOC | 8 | `handleReassocEvent` | u32 status (0 = ok), u32 reason |
| 80 | ROAM_DONE | 0xa8 | `handleRoamDoneEvent` (roam and net managers) | `apple80211_message_roam_status`, see `AirportWCL::RoamStatus` |
| 216 | LinkStatus | 16 | `WCLNetManager::linkStatusInd` | BSSID, up, inSleep, reason |

`ROAM_MANAGER` states: LINK_UP, ROAM_SCAN, ROAM_REASSOC, WAIT_ROAM_DONE.
START moves LINK_UP to ROAM_SCAN, PREP to ROAM_REASSOC, a successful REASSOC
to WAIT_ROAM_DONE, and ROAM_DONE back to LINK_UP from any of them (a nonzero
status is logged as "roam fail").

The BSS switch itself is not in the roam messages. In `NET_MANAGER` a
LinkStatus up while already LINK_UP runs `WCLNetManager::linkUp` ->
`updateBss`: it reads `WCL_BSS_INFO` (ioctl 433, 0x844 bytes), adds the beacon
to the scan cache, calls `setCurrentBSS` (with a BSSID-change reason) and posts
link-changed to user space. If BSS_INFO fails, WCL leaves the network
("Update Bss fail"). So the new BSS's beacon must be in BSS_INFO before
LinkStatus names it.

WCL can also ask for a roam: `WCL_REASSOC` (ioctl 426, `apple80211_reassoc`,
0xbc bytes: 50 chanspecs, BSSID list at 0x64, count at 0x90, channel count at
0x94, flags at 0x98, SSID at 0x9b/0xbb), sent by
`WCLNetManager::sendReassocCommand` for forced roams, BSS steering, band roams
and the host "lazy roam". `kIOReturnBusy` makes WCL keep the request and retry
it later.

## What the driver does

Roaming is driver-driven: net80211's background-scan roam (from OpenBSD),
which was disabled because the WCL join sets `IEEE80211_F_DESBSSID`.

1. `startWCLJoin` sets `ic_wcl_roam` for open and PSK joins.
2. Trigger (`ieee80211_input.c`): with `ic_wcl_roam`, a background scan is
   scheduled when the AP's RSSI is below about -70 dBm
   (`IEEE80211_WCL_ROAM_TRIGGER_RATIO`), backing off after scans that found
   nothing better, as upstream.
3. Selection (`ieee80211_node.c`): during a roam scan `ieee80211_match_bss`
   ignores the joined BSSID (or accepts only a `WCL_REASSOC` target). Same
   SSID, security and cipher checks as a join. A candidate replaces the
   current AP only if it is above the band's RSSI threshold or 8 dB stronger
   (`IEEE80211_WCL_ROAM_DELTA`).
4. Switch: net80211 sends deauth to the old AP, emits
   `IEEE80211_EVT_STA_ROAM_START`, then `mwx_bgscan_done_task` removes keys and
   `ieee80211_node_switch_bss` authenticates and associates with the new AP.
   Scans that keep the AP emit `IEEE80211_EVT_STA_ROAM_NONE`.
5. WCL glue (`WCL.cpp`, `beginWCLRoam` ...): on ROAM_START posts 137 (low
   RSSI) and 139, and holds the link: `AirportMtkWlan::setLinkStatus` ignores
   link changes while `holdWCLRoamLink` says a roam is pending, so neither the
   BSD interface nor WCL sees the link go down. LQM stays at its last value.
   Association with the new AP posts 73. Its beacon is staged for BSS_INFO.
6. When net80211 brings the link up again (RUN for open networks, port valid
   after the 4-way handshake for PSK) and the new beacon is staged:
   BSS_INFO switches to the new BSS, `joinBSSID` and `ic_des_bssid` follow,
   LinkStatus up names the new BSSID, then ROAM_DONE (status 0).
7. Failure (5 s timeout, deauth from the target, rejected association):
   ROAM_DONE with status `0xe0820401`, Deauth (203), leave, link down. airportd
   then auto-joins as it did before roaming existed.

`WCL_REASSOC` starts the same roam scan, restricted to the requested BSSID
when one is given (all channels are scanned; the chanspec list is not used),
and posts ROAM_START with reason "directed". If the scan keeps the current AP
the request ends with a failed ROAM_DONE and the link is untouched.

ioreg `MTKRoams` / `MTKRoamFailures` on the controller count roams. Roam
decisions are logged to the driver log ring (`mtkctl log`, prefix
`AirportMtkWlan-wcl: roam`).

## AWDL coexistence

- Roam scans are unsolicited background scans: with AWDL enabled and
  associated, `mt7921_mcu_hw_scan` suspends them, so no roam starts while
  AirDrop/AWDL is active (a `WCL_REASSOC` then returns busy and WCL retries).
- During the switch the station is in AUTH/ASSOC or RUN without a valid port,
  which `mwx_awdl_infra_joining` already treats as joining: no AWDL ROC.
- The AP power-save handshake state (`sc_awdl_infra_ps`) is cleared on every
  AUTH, so the first off-channel ROC after a roam sends Null data PM=1 to the
  new AP instead of assuming the old AP's state.
- `awdlInfraChannel` reads `ic_bss`, so AWDL follows the new AP's channel.

## Testing on hardware

Two APs with the same SSID and PSK on different channels; join one, walk away
or lower its TX power. Expect in `mtkctl log`: `roam ... -> ...`, then
`roam done status=0x0`; `ping` keeps running with a short gap; the Wi-Fi menu
shows the new BSSID (Option-click) without a disconnect. `log stream
--predicate 'subsystem == "com.apple.wifi"'` should show WCL's `Roam status`
lines from `printRoamStatus`.
