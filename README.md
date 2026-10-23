# MtkWireless

Wi-Fi and Bluetooth for MediaTek MT7921 / MT7922 cards on macOS, with the
protocol logic written in Rust. Two independent kexts:

| Kext | Does | Needs | License |
|---|---|---|---|
| `MtkWirelessWifi.kext` | PCIe Wi-Fi (IO80211/Skywalk) | — | GPL-2.0 |
| `MtkWirelessBT.kext` | USB Bluetooth firmware + bluetoothd fixes | Lilu | GPL-3.0-only |

Successor of [SimpleMtkWlan](https://github.com/Dino-VN/SimpleMtkWlan) and
[SimpleMtkBtFw](https://github.com/flxxxxddd/SimpleMtkBtFw).

## Status

**Phase 0 (toolchain).** Both kexts attach to `IOResources`, run the Rust
core self-test and publish the result; they do not drive the hardware yet.
They are safe to load next to SimpleMtkWlan / SimpleMtkBtFw.

Check after boot:

```sh
ioreg -l -w0 | grep -E 'RustCore|RustSelfTestFailed'   # Wi-Fi: on the MtkWirelessWifi entry
ioreg -l -w0 -n IOResources | grep -A3 MtkWirelessBT     # BT: on IOResources
```

`RustSelfTestFailed = 0` means the kernel runtime works (heap, aligned heap,
formatting, 128-bit math, bounds checks, chip tables).

## Build

Needs Xcode and rustup.

```sh
scripts/bootstrap.sh          # MacKernelSDK submodule, Lilu SDK, pinned Rust nightly
scripts/build.sh Release      # build/Release/MtkWirelessWifi.kext, MtkWirelessBT.kext
scripts/package.sh Release    # dist/MtkWireless-<version>-RELEASE.zip
(cd rust && cargo test --workspace)
```

## Install (OpenCore)

Copy both kexts to `EFI/OC/Kexts` and add the entries from
`docs/config-snippet.plist` to `Kernel → Add` (Lilu first).
