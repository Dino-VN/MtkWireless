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
  class), `mwx_mac_tx_free` (TX-free walk), NIC capability nss clamp, and
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
5. DMA rings and interrupts in Rust.
6. AWDL protocol, election/clock, timeline and ROC planning (done; all 14
   upstream tests pass against the Rust core).

Nothing has been run on hardware yet.

Baseline for porting: SimpleMtkWlan `dfca847`, SimpleMtkBtFw `07a77db` plus
its uncommitted LE-features shim. Upstream behavior notes for Bluetooth are in
SimpleMtkBtFw's CLAUDE.md ("How macOS uses the controller").
