# CLAUDE.md

MtkWireless: MediaTek MT7921 / MT7922 Wi-Fi + Bluetooth for macOS. Two
independent kexts, C++ shells around a Rust `no_std` core.

## Build

```sh
scripts/bootstrap.sh
scripts/build.sh Release      # or Debug; ends with scripts/check.sh
(cd rust && cargo test --workspace)
(cd rust && cargo test -p mtk-ffi --features wifi && cargo test -p mtk-ffi --features bt)
scripts/test-awdl.sh   # upstream AWDL C++/Python tests against the Rust core
scripts/test-wifi-roam.py   # WCL roaming wire formats
```

- Plain shell + clang, no Xcode project. Needs full Xcode; the scripts set
  `DEVELOPER_DIR` when `xcode-select` points at the Command Line Tools.
- `VERSION` (or `MODULE_VERSION`) must stay purely numeric.
- `scripts/check.sh` fails on undefined symbols the kernel/Lilu does not
  export and on SSE/x87 instructions. Run it after any change that pulls new
  code from `core`/`alloc`.

## Layout

- `rust/targets/x86_64-apple-kext.json`: kext target (static relocation, no
  red zone, soft-float), the Rust equivalent of `clang -mkernel`. Toolchain
  pinned in `rust/rust-toolchain.toml`.
- `rust/mtk-kernel`: IOMalloc allocator, panic → kernel `panic()`, `klog!`.
  Kernel-only code is behind `cfg(kext)` (set by build.rs), so everything
  else builds and tests on the host.
- `rust/mtk-common`: bounds-checked `Reader`/`Writer`. Parse anything from
  hardware, firmware or user space with these, never raw pointer offsets.
- `rust/mtk-wlan` (GPL-2.0-only), `rust/mtk-bt` (GPL-3.0-only).
- `rust/mtk-ffi`: the staticlib, built once per kext with feature `wifi` or
  `bt`. C header: `kext/include/mtk_rust.h` (keep in sync by hand).
- `kext/Wifi`, `kext/BT`: C++ shells and Info.plists (`@MODULE_VERSION@`).
  BT: `BtLoader` (IOUSBHostDevice, runs `mtk_bt_setup`), `Patcher` (Lilu
  routes), `BtFirmware` (kernel zlib), `mtk_bt.h` (Rust ABI).
- `kext/Wifi/vendor`: SimpleMtkWlan `dfca847` (AirportMtkWlan target), built
  by `scripts/build.sh` with the Xcode target's flags: macOS 15.2 deployment
  target, MacKernelSDK before the SDK's PrivateHeaders, header-map dirs as
  `-iquote`. Changing any of these changes IOKit vtable layouts; diff
  `nm -u` against an Xcode build after touching the flags. Tahoe
  (`__IO80211_TARGET=__MAC_26_0`) only. Only files the build, `tests/awdl`,
  `tools/mtkctl.c` or `scripts/test-*` use are kept, and the pre-Tahoe /
  non-Airport `#if` branches (`__MAC_15_2`, `!AIRPORT_WCL`, `!AIRPORT`,
  `!IO80211FAMILY_V2`) were removed with unifdef; take anything else from
  upstream `dfca847`. Local edits (search for
  "MtkWireless:" / `mtk_wlan_`): `include/FwData.h` (embedded firmware
  lookup), `hal_mwx/MtkMwx.cpp`: `mwx_load_firmware` (image parsing in Rust),
  `mt7921_mac_fill_rx` (RX descriptor in Rust), `mwx_dma_rx_process` (packet
  class), `mwx_mac_tx_free` (TX-free walk), `mwx_dma_rx_dequeue` /
  `mwx_dma_tx_cleanup_locked` (hardware ring index and RX length checks),
  `mwx_intr` (all-ones status = device gone), NIC capability nss clamp,
  `awdlInfraChannel`/`awdlChannelReachable`/`awdlInfraSleep` (AWDL off the AP
  channel only with a PM handshake, see below), TXWI `SW_POWER_MGMT` for Null
  data, DFS channels 52-144 flagged `IEEE80211_CHAN_DFS | PASSIVE` in
  `mwx_preinit` and sent as Linux `IEEE80211_CHAN_RADAR` in
  `mt7921_mcu_set_channel_domain` (`mwx_chan_is_dfs`), HE offered by default
  (`AirportMtkWlan.cpp`, `amtk_he=0` off), PMF (`IEEE80211_C_MFP`,
  `amtk_pmf=0` off; IGTK as mt76 sends it in `mt7921_mcu_sta_bip_update`,
  plus net80211 software BIP; `RXMGMTPROT` set at port valid), net80211
  WPA3-SAE (`ieee80211_sae.c`, AKM `IEEE80211_AKM_SAE`, EAPOL descriptor
  version 0, HW-decrypted robust mgmt frames accepted in
  `ieee80211_input`, STA-side SA Query moved out of `IEEE80211_STA_ONLY`),
  WCL SAE joins (`LegacyAuth::Sae`, cipher 10 password,
  `selectWPA2TransitionMode` only without PMF), and
  `include/AWDL/*.hpp`: same structs, logic forwarded to Rust
  (`rust/mtk-wlan/src/awdl`, ABI `rust/mtk-ffi/src/awdl.rs`).
- AWDL structs are shared by C++ and Rust. When a field changes, change both
  and the two layout tables (`AwdlLayout.hpp`, `LAYOUT` in awdl.rs);
  `scripts/test-awdl.sh` and the kext self-test (`AwdlLayoutOk`) compare them.
  `tests/awdl` and `tools/awdl` are upstream's, run unchanged.
  Rust ABI for the HAL: `kext/Wifi/mtk_wlan.h`.
- `kext/common/EmbeddedFirmware.cpp`: zlib firmware table shared by both kexts.
- `firmware/wifi/*`: extracted from SimpleMtkWlan's `FwBinary.cpp`.
- `firmware/bt/*.bin`: embedded by `scripts/embed_firmware.py` (zlib +
  `.incbin`) into `build/gen/bt`.

## Wi-Fi + AWDL coexistence (do not regress)

- While associated, AWDL may leave the AP's channel only behind a power-save
  handshake: `MtkMwx::awdlInfraSleep` sends Null data PM=1 to the AP before an
  off-channel ROC (awdlReserve waits 2 ms after it and caps the ROC at 300 ms;
  the grant counts from its arrival, and 40 ms expired before the window),
  `awdlInfraWakeIfIdle` sends PM=0 once no such ROC is pending/granted. An
  unannounced switch got us disassociated within ~2 s (2026-10-24: ROC to
  channel 6 on AP channel 149). Without leaving, AirDrop fails: the iPhone is
  only heard on channel 6, peers flap and Bonjour resolve times out
  (`missingEndpoint`). Boot-arg `amtk_awdl_offchan=0` restores the AP-channel
  only mode (`awdlOnlyChannel`, `planRadio`'s `onlyChannel`). Unverified on
  hardware for AirDrop; 0.1.3 ran 81 handshakes with the AP link intact.
  ioreg `MTKAWDLInfraPS*` counts the handshakes.
- Unassociated, Wi-Fi scans and joins win over AWDL: `mt7921_mcu_hw_scan`
  suspends scans for AWDL only while associated (RUN), otherwise it aborts
  the AWDL ROC and scans; `awdlReserve` refuses ROC during a scan and while
  `mwx_awdl_infra_joining` (AUTH/ASSOC, or RUN with the RSN port not yet
  valid). Suspending every scan while AWDL was on made all joins fail with
  -3905 while AirDrop was open (2026-10-24).
- Firmware ROC grant latency while associated (0.1.3 histogram): on the AP
  channel 20-40 ms, off it 80-160 ms (one >160 ms); the lead estimator then
  plans ~156 ms ahead. While associated, `planRadio`'s
  `homeChannel` (the AP channel) lets one grant span trailing idle slots up
  to the next off-channel window instead of a ROC per window. ioreg
  `MTKAWDLROCLatency{Unassoc,APChannel,OffChannel}` histograms grant latency
  (<5/10/20/40/80/160/>=160 ms).
- `sudo dmesg` is useless for driver printf: system IOMemoryDescriptor
  backtrace spam overwrites the buffer within seconds. Publish diagnostics
  to ioreg.
- With AWDL on, the `other_bss` RX filter lets neighbor BSS traffic in; it
  shows up as `rx drop reason=amsdu-err` with wcid 0x3ff. That is noise.
- iOS 27 AWDL data (0.1.13): non-aggregated QoS frames from the iPhone
  carry an 8-byte envelope `aa aa 03 00 00 00` + BE16 length before the AWDL
  SNAP (capture 2026-10-24 15:02: all 44 rejected frames, length = exactly
  the rest of the MSDU). iOS 18 sends the SNAP right after the QoS header.
  `data_view` accepts the envelope only on QoS non-aggregated frames with an
  exact length and keeps every AWDL/IPv6 check; it never scans for an inner
  SNAP. Before this fix iOS 27 to Mac stalled after QUIC HELLO.

## Wi-Fi roaming (docs/ROAMING.md, unverified on hardware)

- Join LinkStatus up (first beacon) is posted only once `ic_state == RUN`:
  WCL then reads `EXTENDED_BSS_INFO` (RUN only) and leaves on failure
  ("Update Bss fail"). On an 80 MHz AP the HAL RUN task took ~2 s after the
  Association Response (2026-10-31, 1.0.7, every join to that AP failed).
- WCL learns a BSS switch only from LinkStatus (216) up naming the new BSSID
  while already linked: `WCLNetManager::linkUp` re-reads `WCL_BSS_INFO` and
  leaves the network if that fails, so stage the new beacon first. Roam
  messages 137/139/73/80 drive `WCLRoamManager` (IO80211Family 26.7.1).
- net80211 roams by background scan (`ic_wcl_roam`, open/PSK joins only);
  `holdWCLRoamLink` keeps `setLinkStatus` from dropping the BSD/WCL link
  during the switch. Any new path that releases a held roam must call
  `setLinkStatus(kIONetworkLinkValid)` itself: net80211 already lowered
  `if_link_state`. Roam scans stay suspended while AWDL is on, and
  `sc_awdl_infra_ps` is cleared on AUTH. `amtk_roam=0` turns it off.

## WPA3 / PMF (do not regress)

- 1.0.15 Hotspot Wi-Fi (25G332, 2026-10-31 22:10): 1.0.14 passes the
  whole outgoing BLE path; Wi-Fi times out after association. AUTH now
  cancels existing AWDL ROC and admission covers queued AUTH/ASSOC/RUN
  during MCU waits, not just committed ic_state. Key commands retain real
  firmware status; port-valid waits for the queued GTK/IGTK batch. RSN OUI
  pointers use permanent storage (C++ ASan found compound-literal lifetime
  UB). PSK+PMF remains AKM :2 / descriptor 2; SHA256-PSK 3, SAE 0.
  `MTKWiFiJoin` / `MTKWiFiJoinLastFailed` on AirportMtkWlan retain key-free
  per-attempt diagnostics independently of AWDL. Regression:
  `scripts/test-wifi-hotspot.py`, Rust join/FFI tests. Hotspot hardware
  verification pending; `amtk_pmf=0` remains an A/B test, see
  docs/INSTANT-HOTSPOT.md. Do not infer a specific M1/M3 failure from WCL
  association/LinkStatus alone.
- 1.0.15 hardware (2026-11-01 11:45): same failure. Each join first ran a
  full net80211 scan (~5.8 s of airportd's 10 s budget, measured from
  SUSPEND AWDL), then link up and ~4 s with no completion. 1.0.15 never
  recorded link-up failures in `MTKWiFiJoinLastFailed` (only attempts that
  never reached link up), so a later scan-only attempt was all ioreg showed.
  1.0.16: `mtk_join_record_failure` keeps every failure without a valid
  port, and associated ones also in `MTKWiFiJoinLastAssocFailed`
  (diagnostic version 2). The join's first firmware scan visits only WCL's
  `channelSpec` channel (`ic_wcl_join_chan`); a miss falls back to the
  full scan. Unverified on hardware.
- 1.0.16 hardware (2026-11-01 12:59): scan 0.26 s, auth/assoc status 0,
  RUN, then 8.5 s without one EAPOL frame from the iPhone (`EapolRaw*` 0:
  nothing reached ieee80211_input). The home WPA2 AP on the same channel
  got M1 34 ms after association; it negotiated neither HE nor PMF, the
  hotspot both. 1.0.17: `mtk_join_eapol_stall` (associated, RUN, no
  EAPOL) marks the SSID; later PSK joins to it clear `IEEE80211_F_HEON`
  and `IEEE80211_C_MFP` for that join only (`applyJoinCompat`, restored on
  leave/next join; never for SAE). airportd's retry is then the compat
  join. ioreg `MTKWiFiJoinCompat` 1|2 HE|4 PMF. `amtk_hotspot_compat=0`
  off. 1.0.17 hardware (13:15): the compat join had no MFP but still HE
  (`mwx_apply_phy_knobs` re-sets F_HEON at AUTH) and still got no EAPOL in
  8.5 s, so PMF alone is not the cause. 1.0.18: `ic_wcl_compat_no_he`
  keeps HE off through the AUTH knobs. 1.0.18 hardware (14:40): compat
  join `Compat` 7, NodeFlags 0xFC402 (no HE, no MFP, VHT 80), assoc 0,
  RUN, still no EAPOL in 8.5 s: neither HE nor PMF is the cause. 1.0.19
  adds post-association RX counters to the join diagnostics: frames with
  TA = BSSID reaching ieee80211_input (`RxBss*`: beacons, other mgmt,
  data, protected, to-me, last FC/LLC), data to us from another TA
  (`RxOtherTA*`), and U2M frames the HAL drops (`RxHalDrop*`: amsdu-err,
  icv, other, last RXD1-3; RXD1 low 10 bits = wcid, 0x3ff unknown peer).
  1.0.19 hardware (14:55, non-compat join with HE+PMF): after assoc 31
  beacons and 28 protected data frames from the BSSID reached
  ieee80211_input, none addressed to us; no unicast frame at all and no
  U2M HAL drop. The phone transmits, but no M1 arrives (not sent, or lost
  in firmware/PHY before the host). 1.0.20: compat level per stalled
  join to the SSID (`joinCompatLevel`, `ic_wcl_compat_phy` read by
  `mwx_apply_phy_knobs`): 1 HE/PMF off, 2 + VHT/40 MHz off, 3 + HT off;
  `Compat` bits 8 (level 2) and 16 (level 3). Also `RxBssDataMcast`,
  `RxBssLastA1`, `RxBssLastRate`/`RxBssMgmtRate` (P-RXV word 0).
  Likely cause (1.0.19 dump: `MTKAWDLInfraPowerSave` Yes, PSSleeps 30 /
  Wakes 29): the AWDL scheduler calls `awdlInfraSleep` before
  `awdlReserve`, and it sent Null PM=1 in RUN before the port was valid;
  the AP then buffers M1 for a dozing STA (only beacons/group data
  arrive). 1.0.21: `awdlInfraSleep` refuses while
  `mwx_awdl_infra_joining`, `awdlInfraWakeIfIdle` wakes at once when AWDL
  is off/stopping or a join runs, AWDL disable calls it before canceling
  its tick. Join diag `InfraPS{Sleeps,Wakes,Refused,AtUS,AtStart}`.
  1.0.22 hardware (18:20-18:37): PM=1 ruled out (InfraPSSleeps 0 in the
  stalled joins). First working Instant Hotspot join (18:37:45, attempt
  22, 5 GHz ch 149 WPA3-transition AP, PSK side): `Compat` 31 = level 3,
  HT/VHT/HE/PMF all off (legacy 11a), M1 29 ms after assoc, port valid,
  DHCP 172.20.10.2, ping OK. Level 1 (HT+VHT, no HE/PMF) always stalled;
  level 2 (HT20) never ran properly because the retry join did not scan
  (`ScanStartedAtUS` 0, SCAN->SCAN while MWX_FLAG_SCANNING). So the
  iPhone's unicast frames are lost when we negotiate HT/VHT: look at the
  HT/VHT STA_REC / RA / LDPC/STBC/A-MPDU setup for this peer. Also: WCL
  leave goes RUN->SCAN, which sends no Deauth, so the phone keeps the old
  association and the next PMF join gets status 30.
  1.0.23: leaveWCLNetwork sends Deauth (AUTH_LEAVE) from ASSOC/RUN first;
  a WCL join's SCAN->SCAN restarts a scan started for another attempt
  (`sc_scan_join_attempt`; diag `ScanRestarted`/`ScanSkippedBusy`/
  `ScanSuspendedAWDL`); the first stall starts compat at level 2 (HT20).
  1.0.24: the Association Request no longer carries the QoS Capability
  element (46) and puts WMM last (after HT/VHT/HE), as mac80211 does;
  level 3 dropped both together with HT, so it did not prove HT itself
  is at fault. Join diag `RunBw`/`RunCenter`/`RunSwitchError` (first RUN
  channel switch; a failure silently falls back to 20 MHz). A/B without
  reboots: boot-arg `amtk_hotspot_compat=0`, then `mtkctl knob` 0x22
  (HT20), 0x12 (HT40), 0x202, 0x402. 1.0.24 hardware (19:30): level 2
  and even level 3 stalled (RunBw 0, no switch error) on a BSSID that had
  already seen a stalled join from our MAC; after the user restarted the
  hotspot (new BSSID) the first, level-3 join worked (M1 28 ms, DHCP,
  ping). The phone runs the 4-way handshake only for a MAC's first
  association to a BSSID; a stall poisons that BSSID until the hotspot
  restarts, so every earlier "compat retry" result was meaningless.
  1.0.25: joins to a locally administered BSSID start at
  `amtk_hotspot_level` (default 3, legacy; 0 = old behaviour). To find
  the HT bug, test levels 0-2 only on a freshly restarted hotspot.
  1.0.25 hardware (19:45-19:47): Instant-Hotspot-activated (BLE) hotspots
  stalled even on the first, level-3 join to a new BSSID, so the
  "first association" theory is wrong too. Both successes so far came
  after the user enabled Personal Hotspot by hand on the phone; every
  BLE-activated one stalled. sharingd sends `com.sharing.hotspot.request`
  (8 keys) before the join; the phone may admit only what it learns
  there (MAC?). Unresolved. Also found: a user disconnect during a RUN
  background scan left `IEEE80211_F_BGSCAN` set in SCAN, so every WCL
  scan returned busy (Wi-Fi dead until reboot). 1.0.26 clears it on
  leave and in beginWCLScan outside RUN.
  1.0.26 hardware (20:04-20:19): ~25 more stalls (Instant Hotspot and
  plain joins, AirDrop/Handoff off, native AWDL "off"), same pattern; the
  two successes are not explained by MAC, BSSID freshness, compat level,
  manual vs BLE activation or native AWDL state. 1.0.27: net80211 sends
  EAPOL-Start (802.1X v2, type 1) 300 ms after RUN without EAPOL, then at
  +1 s and +2 s (`ic_eapol_start_to`, ieee80211_pae_output.c; diag
  `EapolStartTx`/`EapolStartAtUS`). 1.0.27 hardware (2026-11-01 22:14):
  association status 0, RUN in 19 ms, 3 Starts submitted to DMA, zero raw
  or parsed M1 and no BSS data for ~10 s; no ROC/infra-PS activity. This
  experiment did not fix the failure. 1.0.28 fixes diagnostic ASSOC DMA
  overcounting (subtype 0 also counted data/Starts), and records firmware
  TX-free ACK/failure/count/raw/time for unicast Auth/Assoc/EAPOL, scoped
  to the submitting join attempt. It changes no radio/RX/RSN behavior;
  hardware TX-free capture pending. See docs/INSTANT-HOTSPOT.md.
  1.0.29 source review: mt7921_mac_set_timing hard-coded is_2ghz=1
  (SIFS=10 even at 5 GHz) and slot=9. Match MT76's band selection
  (2 GHz 10 us, 5/6 GHz 16 us) and net80211's 2 GHz ERP slot flag
  (9/20 us, always 9 otherwise). Wire ic_updateslot and refresh after
  RUN BSS/peer setup even at 20 MHz. Join MacTiming* retains written
  timing/readback. This is a configuration correction, not proof of
  hotspot root cause; hotspot/AWDL hardware verification pending.
  1.0.30 (REA of 25G332, 2026-11-02): a native Mac's Association
  Request carries Apple vendor IEs we never sent: `dd 0b 00 17 f2 0a 00
  01 04 00 00 00 00` (airportd installs it at Wi-Fi power-on via request
  85; our `setIE` returns unsupported, log "configure Apple device IE
  failed") and, for Instant Hotspot joins, subtype 0x0b with flag 2
  (manual) / 1 (auto) from the WCL candidate flags at `+0x1e0`
  (`AppleBCMWLANJoinAdapter::performJoin` -> `addInstantHSAppleDeviceIE`).
  `decodeJoin` keeps `assocFlags`, `ieee80211_get_assoc_req` appends both
  (`ic_wcl_apple_ie`, cleared on leave). `amtk_apple_ie` bit 0 = 0x0a,
  bit 1 = 0x0b (default 3, 0 off). Join diag `AppleJoinFlags`,
  `AppleIE` (mask | flag << 8). Repo's custom-IE struct
  (`apple80211_ioctl.h`) has a bogus pad (Tahoe: 0x814 bytes, data at
  +20); fix it before implementing `setIE`. Hardware (2026-11-02,
  Instant Hotspot, dev build 1.0.30, released as 1.0.7): `AppleJoinFlags`
  2, `AppleIE` 515, M1 received, port valid, DHCP 172.20.10.2. The missing
  Apple IEs were the cause of the hotspot stalls.

- airportd offers WPA3-SAE only with `CARD_CAPABILITIES` byte 5 bit 3 and
  PMF only with byte 2 `& 0x11` (IO80211.framework `Apple80211IsAuthSelAllowed`,
  `_getMFPCaps`, Tahoe 25G x86_64); the join then carries the SAE password
  as `apple80211_key` cipher 10 (`APPLE80211_CIPHER_PWD`), not a PMK.
- SAE math lives in Rust (`mtk_wlan::sae`, Annex J.10 vector in tests and
  in the kext self-test); C keeps frames, state and the PMKSA entry.
  `mtk_wlan_sae` is an opaque 512-byte C buffer (size asserted in Rust).
- Turning on `IEEE80211_C_MFP` changes WPA2 joins too: APs with MFPC now
  negotiate PMF. `amtk_pmf=0` is the escape hatch. Unverified on hardware.

## Wi-Fi 6 GHz (docs/WIFI-6GHZ.md, unverified on hardware)

- Off unless boot-arg `amtk_6ghz=1` (`mwx_6ghz_enabled`): 6 GHz BSSes need
  OWE or SAE with H2E (ieee80211_sae.c is hunting-and-pecking only), with
  PMF. 6 GHz channel n is `ic_channels[IEEE80211_CHAN_6GHZ_BASE +
  n]` (flag `IEEE80211_CHAN_6GHZ`); `ieee80211_chan2ieee` gives the number,
  `ieee80211_chan2idx` the index. Use the index for every `ic_chan_*` and
  WCL channel bitmap; never index `ic_channels` by a frame's channel number
  without its band.
- Apple chanspec band bits 14-15: 0 = 2.4, 3 = 5, 1 = 6 GHz (`0x5000|ch`);
  `apple80211_channel` flag 0x2000. AWDL never goes to 6 GHz; associated on
  6 GHz, `awdlInfraChannel()` is 0xff so every AWDL ROC takes the PM
  handshake.

## How macOS uses the Bluetooth controller (do not regress)

- `bluetoothd` (bm3_usb) talks to the USB device from user space; the kernel
  only loads firmware. HCI commands = EP0 class requests, events = interrupt
  IN, ACL = bulk, SCO = isochronous interface 1.
- Leave the USB "Product Name" alone: `Found_USB_Dongle()` skips devices named
  "Bluetooth USB Host Controller" (Apple's internal module, assumed Broadcom).
  The third-party dongle path needs NVRAM `bluetoothInternalControllerInfo`
  (14 bytes) and `bluetoothExternalDongleFailed` (`00`).
- `HCI_Read_Clock(handle, 0)`: MediaTek returns handle 0 and AAC-ELD to
  AirPods never plays; the route sends `Which_Clock = 1`.
- ACL to MediaTek must be non-flushable (PB 0b00) or AAC-ELD frames break.
- `0xFD3C` LE feature mask is rejected by MediaTek; the shim turns the
  rejection into Command Complete (docs/BLE-ADVERTISING.md). Unverified on
  hardware.
- BLE advertising (AirDrop, Nearby) never started: bluetoothd's chip table
  (`[this+0x334]` chip ID) gives a THIRD_PARTY_DONGLE no extended / BT-spec
  advertising sets, so `LE_InitMultipleAdvertising` returns 0x98 and
  LeBroadcaster logs `status = 0x0098`; no HCI command fails. Prefs cannot
  fix it (`LE/SupportsMultipleAdvertising` sets another flag,
  `AE/DisableExtAdvertising` only disables). `Patcher.cpp` routes
  `_cs_validate_page` and patches bluetoothd's extended-advertising check to
  return true as its page is validated (77-byte x86_64 pattern, macOS 26.7.1
  25G241; `-mtkbtnoextadv` off). Lilu's user patcher (`onProcLoad`) is off on
  Big Sur+ (`isUserDisabled` in Lilu's kern_start.cpp), so it cannot be used;
  BlueToolFixup/RestrictEvents use the same page route. ioreg `MtkWirelessBT`
  `BluetoothdExtAdvRouted`/`BluetoothdPages`/`BluetoothdExtAdvPatched`.
  Unverified on hardware. Lilu DEBUG + `-liludbg liludump=120` writes
  `/var/log/Lilu_*.txt`, the only way to see Lilu/plugin SYSLOG here.
- bluetoothd page patches (ours and BlueToolFixup's) live in the shared file
  cache. The sealed volume hashes compressed files per 16 KB block, and
  `decmpfs_pagein_compressed` hashes absent pages together with resident
  ones in place, so a resident patched page breaks every later page-in of a
  neighbour: bluetoothd SIGBUS ~20 s after each launch, kernel log
  `apfs_announce_hash_mismatch ... (bluetoothd)` (2026-10-27/29,
  BlueToolFixup's 15.4 chipset patch at 0x1e3737, fault 0x1e2327).
  `csValidatePage` snapshots each bluetoothd page before any patcher runs
  and records the changed ranges; APFS's verifier then hashes a copy of
  the block with those ranges restored (only where the block holds exactly
  the patched bytes). Our route must be installed after BlueToolFixup's to
  see its patches (Lilu plugin order). `-mtkbtnoverifyfix` off.
  Do not route `_VNOP_VERIFY` for this: 25G332 inlines it into
  `decmpfs_pagein_compressed` (`callq *vp->v_op[vdesc_offset]`), so 1.0.4-1.0.8
  never saw a page-in. 1.0.8 hardware (2026-11-03 17:31, after sleep):
  bluetoothd aborted, every relaunch SIGBUSed at dyld start ("Data hash
  mismatch ... offset 11157504": RPA helper page resident, `__got` absent;
  APFS's hash = block with exactly our helper patch), launchd throttled it,
  Bluetooth dead until reboot. 1.0.9 (`installVerifyOp`, at bluetoothd's
  first validated page, under `pageLock`) replaces the `vnop_verify` entry of
  the bluetoothd vnode's operations vector (APFS's, shared by its regular
  files) with `verifyOp`; `v_op`'s offset is decoded from `_VNOP_VERIFY`
  (0xd0) and checked against `vnode_mount`/`vnode_fsnode`; `vdesc_offset` is
  read then, not at patcher load (bsd_init numbers it). RPA and range
  recording need it installed. ioreg `BluetoothdVerifyHookState` (1 in,
  2-6 failed step), `BluetoothdVnodeOpsOffset`, `BluetoothdPatchRanges`,
  `BluetoothdVerifyRestored`, `BluetoothdVerifyFailed`(+`LastFailOffset`),
  published from a thread call so they update while bluetoothd crashes.
  Test: `scripts/test-bt-msbc-page-patches.py` (decodes this host's kernel).
  Unverified on hardware.
- bluetoothd page patches are one table (`bluetoothdSites`, Patcher.cpp)
  matched by `mtk_bt::page_patch` (masked: displacements are wildcards; one
  match per page or none). Scopes by `kern.osversion` (`readBluetoothdBuild`):
  extAdv + advAddressRetry on any build; advAddress, wbs, hfpFill only on
  listed builds (25G241, 25G332) or with `-mtkbtanybuild`; RPA 25G332 only.
  Features latch per bluetoothd vnode (`featureLatch`). No verifier restore
  means no page patch at all (fail closed, `BluetoothdPagesUnpatched`). New
  build: run `scripts/test-bt-page-patches.py --binary` on an unpatched copy,
  then list it. Canonical arrays are 25G332's; 25G241's live in the tests.
- HFP wideband (Tahoe, on unless `-mtkbtnomsbc`; USB `13d3:3610` and
  `0489:e0cd` only, `UsbMsbcScope.hpp`; `kext/BT/msbc`):
  bluetoothd keeps a 33-byte PCM SCO view; `UsbMsbcBridge` maps interface 1
  alt 4 to alt 6, encodes/decodes mSBC in software (Google libsbc,
  Apache-2.0, vendored) into whole 63-byte HCI packets, and reports 33 bytes
  from `GetPipeProperties`. The `wbsFind`/`hfpFillFind` page patches apply
  only once the bridge routes are ready. Codec code is built without SSE/
  vectorization. Tests: `scripts/test-bt-msbc*.py`. RX judges each isoc
  frame like `btusb_isoc_complete`: MT7921 fails whole transfers with
  `kIOReturnNotResponding` for one missed frame, and dropping the batch
  killed the mic (bluetoothd then resubmits stale frames, `IsoTooOld`).
  Verified on MT7921 `0489:e0cd` (2026-10-26) and MT7922 `13d3:3610`.
- Sleep on the Mac Pro cuts USB power, so after wake the controller
  re-enumerates and needs a cold patch download (`PatchUndone`; at boot it is
  usually `already patched`). After one (2026-10-30, 13:30 and 18:38) MT7961
  answered every WMT command, FUNC_CTRL included, then timed out every HCI
  EP0 request: bluetoothd fell back to `BCM_4350C2` and its watchdog aborted
  it every ~30 s. `mtk_bt::setup` therefore ends with an HCI_Reset probe
  (event read from the interrupt pipe); unanswered, it resets the subsystem
  and downloads once more (Linux `BTMTK_FIRMWARE_DL_RETRY`). Still silent is
  only logged (`MtkBtLog`). Unverified on hardware.
- Advertising address patches (`advAddress*`) are exact bytes per build:
  `*25G332` variants for macOS 26.7.2. On 25G332 without them
  (`BluetoothdAdvAddress*Patched=0`) bluetoothd looped on `multipleAdvStartCb`
  STATUS 718 and Instant Hotspot failed with `BLE client connect timeout`
  (2026-10-31). After a macOS update run `scripts/test-bt-page-patches.py`.
- Instant Hotspot outgoing BLE (25G332): chip 0 has no initialized controller
  privacy backend, while the host AH resolver overwrites the on-air RPA with
  its identity. The accept list then cannot match the phone. `rpa*25G332`
  retains verified legacy ADV_IND RPAs, keeps identity/UUID/security intact,
  and selects the last advertised address at the two initiating-list sites.
  Device updates use the native mutex. `-mtkbtnorpa` off (also needs advertising
  and verify routes); ioreg `BluetoothdRpa*Patched`. On only when
  `kern.osversion` is 25G332 (25G241 shares child signatures), read at the
  first bluetoothd page: launchd sets it after the kext starts, so 1.0.12's
  check in `init` always saw an empty string and left RPA off. Helpers live
  in end-of-__TEXT padding after `__eh_frame` (0x100aa2c00). Never put them
  on the header/load-command pages: in 1.0.10 they sat right after the load
  commands (0x1000026c8) and every re-exec of bluetoothd failed
  (`OS_REASON_EXEC`, launchd spawn failed, Bluetooth dead until reboot).
  The helper page is faulted lazily, so children do not wait for it (1.0.10
  gated them and patched only 2/7); a child's call faults it in and page
  validation installs the helpers first. No `0x2027/0x202D` is added:
  hardware must show a current random RPA in `0x2011` and a real central-role
  LE connection, then hotspot activation. See docs/INSTANT-HOTSPOT.md.
  1.0.13 hardware (2026-10-31 20:25): RPA counters all 1, real central-role
  connection and encryption succeed; Hotspot still stalls in wait-for-phy.
  HFP's `hfpFill*25G332`
  also preserves the native call displacement, with the existing mSBC scope.
- Instant Hotspot LE readiness (1.0.14): 25G332's generic backend selects
  LE event mask `0x1f` (`0x100171183`, constant `0x100a74481`), disabling
  Data Length Change (0x07) and PHY Update Complete (0x0c), although the
  remote-features callback waits for them. The MediaTek EP0 shim ORs `0x840`
  into nonzero `0x2001` masks; zero still disables events. No completion is
  synthesized and no new bluetoothd page is patched. `-mtkbtnolereadymask`
  disables only the mask repair. Rust bounds-checks commands/events;
  IOResources `MtkWirelessBT` publishes `LeEventMask*`,
  `Le{SetEventMask,SetDataLength,SetPhy,ReadPhy}*` and
  `Le{DataLengthChange,PhyUpdateComplete,ChannelSelection,RemoteFeatures}*`.
  Offline Rust/USB tests cover original event ordering, asynchronous errors
  and 0xFD3C coexistence. 1.0.14 hardware passes BLE readiness, rapportd
  PSM 0x81 and hotspot activation; Wi-Fi still times out (1.0.15 diagnostics);
  see docs/INSTANT-HOTSPOT.md and scripts/test-bt-le-ready.py.
- `A2DP.DisableAACELD` / `FastConnect.Disable*` debug prefs do nothing for
  AirPods. A wedged controller may need a real power cut. Repeated bluetoothd
  crashes make launchd throttle it (640–1200 s).
- The core's `sse` future-incompat warning from `core` is expected on a
  soft-float target and harmless.

## Rules

- Never link `mtk-wlan` and `mtk-bt` into one binary (GPL-2.0 vs
  GPL-3.0-only); `mtk-ffi` refuses both features.
- C++ owns IOKit objects, memory descriptors, DMA memory, locks, Lilu
  routes. Rust gets slices and a callback table, never C++ objects.
- No Rust panics on data from outside: return `mtk_common::Error`. A panic
  is a kernel panic.
- Kernel stack is ~16 KB: no large arrays on the stack.
- Chip support goes through `mtk_wlan::chip` / `mtk_bt::chip` tables and
  per-family modules; do not hard-code MT7921 in shared code.

## Phases

0. Toolchain, self-test stubs (done).
1. BT in Rust, parity with SimpleMtkBtFw 1.1.0 + LE-features shim (done,
   not yet run on hardware).
2. Wi-Fi parsers: firmware patch/RAM (done; `cargo run -p mtk-wlan --example
   dump_regions -- ../firmware/wifi/*` prints the download plan), MCU
   messages and events.
3. Wi-Fi MAC: RXD, packet class, TX-free (done), TXWI, TXS, rates.
4. Wi-Fi control TLVs and HAL state machine.
5. DMA rings and interrupts: hardware index/length validation in Rust (done);
   ring memory management stays in C.
6. AWDL protocol, election/clock, timeline and ROC planning (done; all 14
   upstream tests pass against the Rust core).

Nothing has been run on hardware yet.

Baseline for porting: SimpleMtkWlan `dfca847`, SimpleMtkBtFw `07a77db` plus
its uncommitted LE-features shim. Upstream behavior notes for Bluetooth are in
SimpleMtkBtFw's CLAUDE.md ("How macOS uses the controller").
