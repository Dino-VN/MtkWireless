# CLAUDE.md

MtkWireless: MediaTek MT7921 / MT7922 Wi-Fi + Bluetooth for macOS. Two
independent kexts, C++ shells around a Rust `no_std` core.

## Build

```sh
scripts/bootstrap.sh
scripts/build.sh Release      # or Debug; ends with scripts/check.sh
(cd rust && cargo test --workspace)
(cd rust && cargo test -p mtk-ffi --features wifi && cargo test -p mtk-ffi --features bt)
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
1. BT in Rust, parity with SimpleMtkBtFw 1.1.0 + LE-features shim.
2. Wi-Fi parsers: firmware patch/RAM, MCU messages and events.
3. Wi-Fi MAC: RXD/TXWI, tx-free, rates.
4. Wi-Fi control TLVs and HAL state machine.
5. DMA rings and interrupts in Rust.

Baseline for porting: SimpleMtkWlan `dfca847`, SimpleMtkBtFw `07a77db` plus
its uncommitted LE-features shim. Upstream behavior notes for Bluetooth are in
SimpleMtkBtFw's CLAUDE.md ("How macOS uses the controller").
