# MtkWireless

Wi-Fi and Bluetooth for MediaTek MT7921 / MT7922 cards on macOS, with the
protocol logic written in Rust. Two independent kexts:

| Kext | Does | Needs | License |
|---|---|---|---|
| `MtkWirelessWifi.kext` | PCIe Wi-Fi (IO80211/Skywalk), macOS 26 Tahoe | — | GPL-2.0 |
| `MtkWirelessBT.kext` | USB Bluetooth firmware + bluetoothd fixes | Lilu | GPL-3.0-only |

## Status

Tested on one machine: macOS 26.7.1 (25G241) and 26.7.2 (25G332), SMBIOS MacPro7,1 (no real T2),
MT7921 PCIe Wi-Fi `14c3:7961` and its USB Bluetooth `0489:e0cd`, iPhone
`iPhone16,2` on iOS 18 and iOS 27. ✅ = seen working on that machine,
⚠️ = partly works, ❌ = fails, — = not tested yet.

### Supported macOS builds

The Wi-Fi kext needs macOS 26 Tahoe. Several Bluetooth fixes patch
bluetoothd's code byte for byte, so they only work on the exact macOS builds
listed here. On any other build they are skipped (ioreg
`Bluetoothd*Patched = 0`), and the features in the table stop working.

| macOS | Wi-Fi, BT pairing/audio | BLE advertising (AirDrop, Handoff) | HFP mSBC mic | Instant Hotspot |
|---|---|---|---|---|
| 26.7.2 (25G332) | ✅ | ✅ | ✅ | ✅ 1.0.7+ |
| 26.7.1 (25G241) | ✅ | ✅ | ✅ | ❌ no RPA patch for this build: the iPhone is never found over BLE, so the hotspot is not turned on. Update to 26.7.2 |
| other 26.x | ✅ | ⚠️ untested, patches may not match | ⚠️ untested | ❌ |

Check your build with `sw_vers -buildVersion`. After a macOS update, wait
for a MtkWireless release that adds the new build.

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
| BLE advertising | ✅ | 0.1.9+: the iPhone sees the Mac. Needs the bluetoothd page patch (macOS 26.7.1 25G241 and 26.7.2 25G332 only), see [docs/BLE-ADVERTISING.md](docs/BLE-ADVERTISING.md) |
| Mac connecting to an iPhone over BLE | - | |
| HFP microphone | ✅ | 1.0.2+: 16 kHz mSBC on MT7921 `0489:e0cd` and MT7922 `13d3:3610`, on by default (`-mtkbtnomsbc` off); CVSD on other IDs, see [wideband audio](docs/BLUETOOTH.md#wideband-microphone-tahoe) |

### Apple features

| Feature | Status | Notes |
|---|---|---|
| AirDrop iPhone <-> Mac, iOS 16 -> 27 | ✅ | 1.0.3: faster transfers and large files no longer stall (~4 MB/s average iPhone -> Mac, peaks ~7 MB/s). [Details](docs/AIRDROP-PERFORMANCE.md) |
| Handoff, Universal Clipboard, AirPlay to Mac | ✅ | |
| iPhone Mirroring with [Mirra](https://github.com/eiyuumiru/Mirra) | ⚠️ | Experimental on Intel: authentication, decoder creation and first video frame observed on 26.7.2 (25G332). Requires a separate jailbroken bridge iPhone; see below. MtkWireless alone does not supply T2/Secure Enclave support. |
| Instant Hotspot (same iCloud / Family) | ✅ | 1.0.7+, **macOS 26.7.2 (25G332) only**: the BLE RPA patch exists only for that build. The Mac sends Apple's hotspot IEs in the Association Request, so the iPhone starts the WPA2 handshake. See [diagnosis](docs/INSTANT-HOTSPOT.md). |
| Auto Unlock, Sidecar, Continuity Camera, Continuity Microphone  | — | |

### iPhone Mirroring with Mirra

MtkWireless can provide the Wi-Fi/Bluetooth and AWDL connectivity used by
[Mirra](https://github.com/eiyuumiru/Mirra), an experimental companion for
Apple's iPhone Mirroring on Intel Hackintosh. Mirra supplies separate native
compatibility and cryptographic-operation adapters; it is not bundled with
these kexts and does not emulate a complete T2 chip.

Requirements:

- A supported **macOS 26 Tahoe** Wi-Fi build; the tested configuration is
  **26.7.2 (25G332), x86_64**. Mirra's broader macOS support does not make this
  Wi-Fi kext compatible with Sequoia.
- A Wi-Fi build containing the native media IPv6 RX, replay-window rollover
  and UMI TX-completion fixes in the current source. Older release binaries
  may lack these changes.
- Mirra and Apple's compatible iPhone Mirroring app. Follow
  [Mirra's setup guide](https://github.com/eiyuumiru/Mirra#requirements).
- A **separate jailbroken bridge iPhone**, iOS 15 or later, with compatible
  jailbreak, OpenSSH and `ldid`. Mirra connects to the bridge through SSH over
  Wi-Fi or USB. **The iPhone being mirrored does not need a jailbreak.**
- Working Wi-Fi, Bluetooth, AWDL and H.264 VideoToolbox decoding. Keep the
  target iPhone locked and nearby; Apple's account, pairing and authentication
  requirements still apply. Do not restrict AWDL to the AP channel with
  `amtk_awdl_offchan=0` when the peer uses another channel.

On the test host, a development Wi-Fi build with Mirra reached successful
native authentication, H.264 decoder creation (`status=0`), the first received
video frame, and activated video/audio streams. **Stable video, audible audio,
input and reconnection are not yet verified end to end**, so this remains
experimental rather than a general compatibility guarantee. See the
[driver registration and RX notes](docs/IPHONE-MIRRORING.md) and
[Mirra's media documentation](https://github.com/eiyuumiru/Mirra/blob/main/docs/Media.md).

**AWDL coexistence warning:** In current experimental testing, running Mirra
may disrupt AirDrop and other AWDL-dependent Continuity features. Reliable
concurrent operation has not yet been verified, and the cause has not been
isolated to Mirra, MtkWireless or Apple's native services.

After boot, `RustSelfTestFailed = 0` means the Rust kernel runtime works:

```sh
ioreg -l -w0 | grep -E 'RustCore|RustSelfTestFailed'
```

## First hardware test

Suggested order on a new machine, one kext at a time, keeping a bootable
fallback EFI:

1. **MtkWirelessBT** (after Lilu and BlueToolFixup). Check `MtkBtLog` on the USB device, the
   `MtkWirelessBT` dictionary on IOResources (`RustSelfTestFailed = 0`,
   `LeFeaturesShimEnabled = true`, `BluetoothdExtAdvPatched` ≥ 1), pairing,
   AirPods audio. Then run `scripts/Collect-AirDrop-BT.command` for the BLE
   advertising check.
2. **MtkWirelessWifi**. Check `RustSelfTestFailed = 0` and `AwdlLayoutOk = Yes`, then scan,
   join, DHCP, throughput and `mtkctl log`. Then AirDrop both ways;
   `scripts/Collect-AirDrop-AWDL.command` records AWDL if it fails.

## Collect diagnostics

Run this **right after a failure**, without restarting Bluetooth or changing Wi-Fi.
One command downloads the standalone collector, asks for your Mac password, and
creates `MtkWireless-Logs-<date>-<random>.zip` on **your Desktop**, even when run
with `sudo`:

```sh
script="$(curl -fsSL https://raw.githubusercontent.com/Dino-VN/MtkWireless/main/scripts/collect-logs.sh)" && sudo bash -c "$script"
```

It collects:

- macOS/build, Wi-Fi and Bluetooth card details, PCI/USB hierarchy, audio and
  sleep/wake state;
- loaded kext versions/dependencies, kernel collection metadata and **kext/kernel
  loading logs since the current boot**, including failures;
- full IORegistry plus before/after Wi-Fi, AWDL, BT loader, page-patch, mSBC,
  verifier and LE-readiness counters;
- network interfaces, routes, DHCP, DNS, AWDL neighbors and relevant daemon state;
- the on-disk `bluetoothd` binary/signature, recent crash/panic reports and Lilu
  logs (last 3 days); existing HCI traces are **listed**, not copied by default;
- focused Bluetooth, Wi-Fi, AirDrop/Continuity/mDNS and driver log samples from
  the last **5 minutes**, at default/error/fault levels;
- relevant kext **metadata** and OpenCore Kernel/security/ACPI/driver sections from
  **already-mounted** EFIs. It does not mount EFI or copy the full `config.plist`.

**Bounded by default:** each text sample keeps at most **1 MiB output**, reads at
most **16 MiB input**, and gets **30 seconds**. The recent-log group has a
**90-second total budget**. Only the first three copies of each repeated message
are kept, with counts and timestamp ranges in the summary. Kernel/kext loading
samples come **before** slower daemon queries, so they survive an interrupted run.
Full kernel dumps, info/debug output, native archives, raw HCI traces and kext
bundle copies are not included by default.

Copied crash/Lilu files are sorted newest first: up to 10 per list, 4 MiB per
file and 16 MiB combined. Larger files are skipped, not truncated into invalid
reports/traces; their paths/sizes and reasons remain in the metadata and
`skipped.tsv`. The on-disk `bluetoothd` binary and structured hardware/IORegistry
snapshots are kept for build-specific diagnosis.

No Xcode, Python or extra helper is needed. It does **not** restart daemons,
load/unload kexts, scan, enable HCI/packet tracing, change preferences or upload
anything. `commands.tsv` distinguishes failures/time-budget exhaustion (**124**)
from intentionally limited samples (**125**); each sample has `.stats.txt` and,
when limited, `.limit` metadata. Collection continues and the ZIP is tested before
success is reported.

For a local copy: `bash scripts/collect-logs.sh`. Options also work after the
remote command, for example:

```sh
sudo bash -c "$script" -- --last 15m --verbose --notes "AirDrop Mac to iPhone stalls"
```

`--logarchive` opts into a native archive with a **64 MB log-data budget**;
macOS symbol metadata can make the archive larger. `--include-hci` and
`--include-kexts` opt into existing HCI traces (within the copy budget) and kext
bundle copies. These can make the ZIP larger; `--verbose` still obeys text-sample
limits. `--no-logarchive` is accepted for compatibility and is now the default.
The uncompressed folder is kept alongside the ZIP.

**Privacy:** the bundle is not anonymized. It can contain SSIDs, MAC/IP addresses,
serial numbers, device/user names, crash details and private activity from system
logs. Send it privately to someone you trust, not a public issue. Keychain and
Bluetooth link-key preferences are not copied. macOS redaction and missing
historical debug events cannot be undone by the collector.

## Build

Needs Xcode and rustup.

```sh
scripts/bootstrap.sh          # MacKernelSDK submodule, Lilu SDK, pinned Rust nightly
scripts/build.sh Release      # build/Release/MtkWirelessWifi.kext, MtkWirelessBT.kext
scripts/package.sh Release    # dist/MtkWireless-<version>-RELEASE.zip
(cd rust && cargo test --workspace)
```

## Install (OpenCore)

One command installs or updates everything. Paste it into Terminal:

```sh
sudo bash -c "$(curl -fsSL https://raw.githubusercontent.com/Dino-VN/MtkWireless/main/scripts/install.sh)"
```

The script:

- detects the MediaTek Wi-Fi card (`14c3:7961/0608/7922/0616`) and the USB
  Bluetooth controller, and installs only the kexts that have hardware;
- finds the EFI partition OpenCore booted from and mounts it;
- downloads the latest release and installs it, or updates it if the EFI
  has an older version (`--force` reinstalls, `--version v1.0.7` picks one);
- removes conflicting Wi-Fi/Bluetooth kexts (Intel itlwm/AirportItlwm/
  IntelBluetooth*, Broadcom BrcmPatchRAM*/BrcmFirmware*/BrcmBluetoothInjector*,
  other MediaTek kexts) from `EFI/OC/Kexts` and `Kernel → Add`;
- adds Lilu and BlueToolFixup if they are missing (latest acidanthera
  releases), puts them in the right order, and sets the Bluetooth NVRAM keys;
- prints which features work on your macOS build (see the table above).

It asks before changing anything and backs up `config.plist` and every
removed kext to `~/Desktop/MtkWireless-backup-<date>`. Preview without
changes: add `--dry-run`. Then reboot.

### Manual install

Download `MtkWireless-<version>-RELEASE.zip` from
[Releases](https://github.com/Dino-VN/MtkWireless/releases) and unzip it.
Keep a bootable copy of your EFI (USB stick) before changing it.

1. **Mount the EFI partition** OpenCore boots from:

   ```sh
   sudo diskutil mount disk0s1     # check the disk with: diskutil list
   ```

2. **Remove conflicting kexts** from `EFI/OC/Kexts` and their entries in
   `config.plist → Kernel → Add`: itlwm, AirportItlwm, IntelBluetooth*,
   BrcmPatchRAM*/BrcmFirmware*/BrcmBluetoothInjector*, any other MediaTek
   Wi-Fi/Bluetooth kext, and any SSDT that disables the Wi-Fi PCI device.

3. **Copy kexts** to `EFI/OC/Kexts`:
   - `MtkWirelessWifi.kext` (Wi-Fi)
   - `MtkWirelessBT.kext` (Bluetooth), plus
     [Lilu](https://github.com/acidanthera/Lilu/releases) and `BlueToolFixup.kext`
     from [BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM/releases)
     if you do not already have them.

4. **`Kernel → Add`** (ProperTree / OCAT, or merge
   [docs/config-snippet.plist](docs/config-snippet.plist)), in this order:

   | # | BundlePath | ExecutablePath | MinKernel | MaxKernel |
   |---|---|---|---|---|
   | 1 | `Lilu.kext` | `Contents/MacOS/Lilu` | | |
   | 2 | `BlueToolFixup.kext` | `Contents/MacOS/BlueToolFixup` | `21.0.0` | |
   | 3 | `MtkWirelessWifi.kext` | `Contents/MacOS/MtkWirelessWifi` | `25.0.0` | `25.99.99` |
   | 4 | `MtkWirelessBT.kext` | `Contents/MacOS/MtkWirelessBT` | | |

   `PlistPath` is `Contents/Info.plist`, `Arch` `x86_64`, `Enabled` true.
   Lilu must come before BlueToolFixup and MtkWirelessBT.

5. **NVRAM for Bluetooth**: under `NVRAM → Add →
   7C436110-AB2A-4BBB-A880-FE41995C9F82` add

   | Key | Type | Value |
   |---|---|---|
   | `bluetoothInternalControllerInfo` | Data | `0000000000000000000000000000` (14 zero bytes) |
   | `bluetoothExternalDongleFailed` | Data | `00` |

   and list both keys under `NVRAM → Delete` for the same GUID.

6. Save `config.plist`, run OpenCore's `ocvalidate` if you have it, then
   reboot (Reset NVRAM once from the OpenCore picker so the NVRAM keys apply).

7. **Check**:

   ```sh
   kmutil showloaded --list-only | grep MtkWireless
   ioreg -l -w0 | grep -E 'RustSelfTestFailed'      # should be 0
   system_profiler SPBluetoothDataType | grep -E 'Chipset|State'
   ```

   Bluetooth should show `Chipset: THIRD_PARTY_DONGLE` and `State: On`;
   the Wi-Fi menu should list networks.

**Updating**: replace the two kexts in `EFI/OC/Kexts` and reboot (or rerun
the installer). Details: [docs/WIFI.md](docs/WIFI.md),
[docs/BLUETOOTH.md](docs/BLUETOOTH.md).
