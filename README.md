# MtkWireless

Wi-Fi and Bluetooth for MediaTek MT7921 / MT7922 cards on macOS, with the
protocol logic written in Rust. Two independent kexts:

| Kext | Does | Needs | License |
|---|---|---|---|
| `MtkWirelessWifi.kext` | PCIe Wi-Fi (IO80211/Skywalk), macOS 26 Tahoe | — | GPL-2.0 |
| `MtkWirelessBT.kext` | USB Bluetooth firmware + bluetoothd fixes | Lilu | GPL-3.0-only |

Successor of [SimpleMtkWlan](https://github.com/eiyuumiru/SimpleMtkWlan) and
[SimpleMtkBtFw](https://github.com/flxxxxddd/SimpleMtkBtFw).

## Status

Tested on one machine: macOS 26.7.1 (25G241), SMBIOS MacPro7,1 (no real T2),
MT7921 PCIe Wi-Fi `14c3:7961` and its USB Bluetooth `0489:e0cd`, iPhone
`iPhone16,2` on iOS 18 and iOS 27. ✅ = seen working on that machine,
⚠️ = partly works, ❌ = fails, — = not tested yet.

### Wi-Fi (`MtkWirelessWifi.kext`)

| Feature | Status | Notes |
|---|---|---|
| Wi-Fi menu, scan, join, DHCP | ✅ | WPA2 / WPA3-transition AP, 5 GHz channel 149, 80 MHz, 802.11ac |
| Join while AirDrop/AWDL is active | ✅ | 0.1.3+: scans and joins take the radio from AWDL until associated |
| 2.4 GHz, DFS channels, roaming | — | |
| Throughput, sleep/wake | — | |
| 802.11ax (`amtk_he=1`) | — | experimental |
| AWDL (Apple Wireless Direct Link) | ✅ | leaves the AP channel behind a power-save handshake (0.1.2+, `MTKAWDLInfraPS*`) |

Supported PCI IDs and boot arguments: [docs/WIFI.md](docs/WIFI.md).

### Bluetooth (`MtkWirelessBT.kext`)

| Feature | Status | Notes |
|---|---|---|
| Firmware load, pairing | ✅ | `Chipset: THIRD_PARTY_DONGLE`, needs the NVRAM keys in [docs/BLUETOOTH.md](docs/BLUETOOTH.md) |
| Audio (A2DP), AirPods AAC-ELD | ✅ | Read Clock and non-flushable ACL fixes |
| BLE scanning | ✅ | Mac sees the iPhone |
| BLE advertising | ✅ | 0.1.9+: the iPhone sees the Mac. Needs the bluetoothd page patch (macOS 26.7.1 only), see [docs/BLE-ADVERTISING.md](docs/BLE-ADVERTISING.md) |
| Mac connecting to an iPhone over BLE | ❌ | times out; this is what breaks iPhone Mirroring pairing |
| HFP microphone | ⚠️ | CVSD only, quiet and choppy |

### Apple features

| Feature | Status | Notes |
|---|---|---|
| AirDrop Mac → iPhone | ✅ | 0.1.11+ |
| AirDrop iPhone → Mac, iOS 18 | ✅ | |
| AirDrop iPhone → Mac, iOS 27 | ✅ | 0.1.13+: iOS 27 adds an 8-byte header to AWDL data frames; both framings are accepted |
| Handoff, Universal Clipboard | ✅ | |
| iPhone Mirroring | ❌ | passes the Mac checks, then pairing for iPhone unlock fails (Mac → iPhone BLE connect timeout). |
| Instant Hotspot (same iCloud / Family) | ⚠️ | iPhone activation succeeds; directed scan fails on 1.0.0. 1.0.1 fixes the scan path, pending hardware verification; see [diagnosis](docs/INSTANT-HOTSPOT.md). |
| Auto Unlock, Sidecar, AirPlay to Mac, Continuity Camera | — | |

After boot, `RustSelfTestFailed = 0` means the Rust kernel runtime works:

```sh
ioreg -l -w0 | grep -E 'RustCore|RustSelfTestFailed'
```

## First hardware test

Suggested order on a new machine, one kext at a time, keeping a bootable
fallback EFI:

1. **MtkWirelessBT** instead of SimpleMtkBtFw + SimpleMtkBtPatcher (OpenCore,
   after Lilu and BlueToolFixup). Check `MtkBtLog` on the USB device, the
   `MtkWirelessBT` dictionary on IOResources (`RustSelfTestFailed = 0`,
   `LeFeaturesShimEnabled = true`, `BluetoothdExtAdvPatched` ≥ 1), pairing,
   AirPods audio. Then run `scripts/Collect-AirDrop-BT.command` for the BLE
   advertising check.
2. **MtkWirelessWifi** instead of AirportMtkWlan (same load method as
   before). Check `RustSelfTestFailed = 0` and `AwdlLayoutOk = Yes`, then scan,
   join, DHCP, throughput and `mtkctl log`. Then AirDrop both ways;
   `scripts/Collect-AirDrop-AWDL.command` records AWDL if it fails.

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
