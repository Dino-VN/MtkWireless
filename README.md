# MtkWireless

Wi-Fi and Bluetooth for MediaTek MT7921 / MT7922 cards on macOS, with the
protocol logic written in Rust. Two independent kexts:

| Kext | Does | Needs | License |
|---|---|---|---|
| `MtkWirelessWifi.kext` | PCIe Wi-Fi (IO80211/Skywalk), macOS 26 Tahoe | — | GPL-2.0 |
| `MtkWirelessBT.kext` | USB Bluetooth firmware + bluetoothd fixes | Lilu | GPL-3.0-only |

Successor of [SimpleMtkWlan](https://github.com/Dino-VN/SimpleMtkWlan) and
[SimpleMtkBtFw](https://github.com/flxxxxddd/SimpleMtkBtFw).

## Status

| | |
|---|---|
| **MtkWirelessBT** | Full replacement for SimpleMtkBtFw + SimpleMtkBtPatcher (firmware load, AirPods fixes, BLE feature-mask shim), protocol logic in Rust. Built and unit-tested; not yet run on hardware. See [docs/BLUETOOTH.md](docs/BLUETOOTH.md). |
| **MtkWirelessWifi** | AirportMtkWlan `dfca847` built here (Tahoe only) plus the Rust self-test; replaces AirportMtkWlan. Parts move to Rust phase by phase. See [docs/WIFI.md](docs/WIFI.md). |

After boot, `RustSelfTestFailed = 0` means the Rust kernel runtime works:

```sh
ioreg -l -w0 | grep -E 'RustCore|RustSelfTestFailed'
```

## Build

Needs Xcode and rustup.

```sh
scripts/bootstrap.sh          # MacKernelSDK submodule, Lilu SDK, pinned Rust nightly
scripts/build.sh Release      # build/Release/MtkWirelessWifi.kext, MtkWirelessBT.kext
scripts/package.sh Release    # dist/MtkWireless-<version>-RELEASE.zip
(cd rust && cargo test --workspace)
```

## Install (OpenCore)

Copy the kexts to `EFI/OC/Kexts` and merge `docs/config-snippet.plist`
(`Kernel → Add` in order, NVRAM variables for Bluetooth). Remove
SimpleMtkBtFw and SimpleMtkBtPatcher first. Details: [docs/BLUETOOTH.md](docs/BLUETOOTH.md).
