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
| WPA3-Personal (SAE), 802.11w PMF | — | 1.0.1+, untested on hardware: SAE group 19 (hunting-and-pecking) in the Rust core, PMF with IGTK/BIP and SA Query; WPA3-only and PMF-required networks join, transition networks prefer SAE. No H2E/SAE-PK (WPA3-only 6 GHz needs H2E). `amtk_pmf=0` returns to WPA2-PSK only. [docs/WIFI.md](docs/WIFI.md#wpa3-personal-and-pmf) |
| Join while AirDrop/AWDL is active | ✅ | 0.1.3+: scans and joins take the radio from AWDL until associated |
| 2.4 GHz, 5 GHz band choice | — | both bands scanned; airportd picks the BSS (it prefers 5 GHz when the signal is good) |
| DFS channels (52–144) | — | 1.0.1+: marked DFS/passive (passive scan, never used for AWDL); joining a DFS AP is untested |
| Roaming | — | 1.0.1+, untested on hardware: same-SSID roaming for open and WPA/WPA2-Personal networks keeps the link up and reports the roam to macOS (`WCL_REASSOC` supported). A full re-association, not 802.11r; no roam scans while AWDL is on; `amtk_roam=0` turns it off. [docs/ROAMING.md](docs/ROAMING.md) |
| Throughput, sleep/wake | — | A-MPDU/A-MSDU, SGI, 2×2 MCS 9 (11ac) / MCS 11 (11ax). Sleep stops the HAL, wake restarts it and reloads firmware if the restart fails (`MTKSleepStopStatus`, `MTKWakeStartStatus` in ioreg) |
| 802.11ax | — | 1.0.1+: offered by default (80 MHz, no TWT/MU-MIMO/160 MHz); `amtk_he=0` turns it off |
| 6 GHz (Wi-Fi 6E) | — | 1.0.1+, untested on hardware, off unless `amtk_6ghz=1`: MT7922/MT7921K only, HE up to 80 MHz, LPI power (12 dBm). 6 GHz networks need OWE or WPA3-SAE with hash-to-element, which joins do not do yet. AWDL never uses 6 GHz. [docs/WIFI-6GHZ.md](docs/WIFI-6GHZ.md) |
| AWDL (Apple Wireless Direct Link) | ✅ | leaves the AP channel behind a power-save handshake (0.1.2+, `MTKAWDLInfraPS*`) |

Supported PCI IDs and boot arguments: [docs/WIFI.md](docs/WIFI.md).

### Bluetooth (`MtkWirelessBT.kext`)

| Feature | Status | Notes |
|---|---|---|
| Firmware load, pairing | ✅ | `Chipset: THIRD_PARTY_DONGLE`, needs the NVRAM keys in [docs/BLUETOOTH.md](docs/BLUETOOTH.md) |
| Audio (A2DP), AirPods AAC-ELD | ✅ | Read Clock and non-flushable ACL fixes |
| BLE scanning | ✅ | Mac sees the iPhone |
| BLE advertising | ✅ | 0.1.9+: the iPhone sees the Mac. Needs the bluetoothd page patch (macOS 26.7.1 only), see [docs/BLE-ADVERTISING.md](docs/BLE-ADVERTISING.md) |
| Mac connecting to an iPhone over BLE | - | |
| HFP microphone | ⚠️ | CVSD by default; opt-in 16 kHz mSBC tested on MT7922 + Galaxy Buds+, see [wideband audio](docs/BLUETOOTH.md#wideband-microphone-tahoe) |

### Apple features

| Feature | Status | Notes |
|---|---|---|
| AirDrop iPhone <-> Mac, iOS 16 -> 27 | ✅ | |
| Handoff, Universal Clipboard, AirPlay to Mac | ✅ | |
| iPhone Mirroring | ❌ | not possible: needs Apple silicon or a T2 chip (Secure Enclave), which this machine does not have |
| Instant Hotspot (same iCloud / Family) | ⚠️ | iPhone activation succeeds; directed scan fails on 1.0.0. 1.0.1 fixes the scan path, pending hardware verification; see [diagnosis](docs/INSTANT-HOTSPOT.md). |
| Auto Unlock, Sidecar, Continuity Camera, Continuity Microphone  | — | |

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
