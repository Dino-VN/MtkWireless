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
- HFP wideband (`-mtkbtmsbc`, Tahoe, USB `13d3:3610` only; `kext/BT/msbc`):
  bluetoothd keeps a 33-byte PCM SCO view; `UsbMsbcBridge` maps interface 1
  alt 4 to alt 6, encodes/decodes mSBC in software (Google libsbc,
  Apache-2.0, vendored) into whole 63-byte HCI packets, and reports 33 bytes
  from `GetPipeProperties`. The `wbsFind`/`hfpFillFind` page patches apply
  only once the bridge routes are ready. Codec code is built without SSE/
  vectorization. Tests: `scripts/test-bt-msbc*.py`. Unverified on hardware
  as packaged here (tested via a local BlueToolFixup build).
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
