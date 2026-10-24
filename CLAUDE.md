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
  (`__IO80211_TARGET=__MAC_26_0`) only for now. Local edits (search for
  "MtkWireless:" / `mtk_wlan_`): `include/FwData.h` (embedded firmware
  lookup), `hal_mwx/MtkMwx.cpp`: `mwx_load_firmware` (image parsing in Rust),
  `mt7921_mac_fill_rx` (RX descriptor in Rust), `mwx_dma_rx_process` (packet
  class), `mwx_mac_tx_free` (TX-free walk), `mwx_dma_rx_dequeue` /
  `mwx_dma_tx_cleanup_locked` (hardware ring index and RX length checks),
  `mwx_intr` (all-ones status = device gone), NIC capability nss clamp,
  `awdlInfraChannel`/`awdlChannelReachable`/`awdlInfraSleep` (AWDL off the AP
  channel only with a PM handshake, see below), TXWI `SW_POWER_MGMT` for Null
  data, and
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
