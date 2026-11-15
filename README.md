# MtkWireless

[Tiếng Việt](README.vi.md) · [Releases](https://github.com/Dino-VN/MtkWireless/releases)

[![Support on Ko-fi](https://img.shields.io/badge/Ko--fi-Support%20the%20project-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/dinovn)

**Quick links:** [Requirements](#requirements) · [What works](#what-works) · [Install](#install) · [Troubleshooting](#check-that-it-loaded) · [Report a problem](#reporting-a-problem) · [Mirroring](#iphone-mirroring-with-mirra) · [Build](#build-from-source) · [Credits](#credits) · [License](#license) · [Support](#support)

Wi-Fi and Bluetooth for MediaTek MT7921 / MT7922 cards on macOS 26 Tahoe
(OpenCore, Intel x86_64). The protocol logic is written in Rust; C++ is kept
for the IOKit, USB and Lilu glue.

The project ships two kexts that work independently of each other:

| Kext | What it does | Depends on | License |
|---|---|---|---|
| `MtkWirelessWifi.kext` | PCIe Wi-Fi through the native stack (Wi-Fi menu, airportd, AWDL) | nothing | GPL-2.0 |
| `MtkWirelessBT.kext` | Loads the USB Bluetooth firmware and patches `bluetoothd` so it accepts a MediaTek controller | Lilu, BlueToolFixup | GPL-3.0-only |

## Requirements

- **macOS 26 Tahoe** (Darwin 25). The Wi-Fi kext does not attach on Sequoia or
  older; that code was removed.
- **OpenCore** on an Intel machine.
- A supported card:
  - Wi-Fi (PCI): `14c3:7961` MT7921, `14c3:0608` MT7921K / RZ608,
    `14c3:7922` MT7922, `14c3:0616` MT7922 / RZ616.
  - Bluetooth (USB): the MT7921 / MT7922 IDs listed in
    [`kext/BT/Info.plist`](kext/BT/Info.plist).
- No other Wi-Fi or Bluetooth kext for the same card (itlwm, AirportItlwm,
  IntelBluetooth\*, BrcmPatchRAM\*, other MediaTek kexts), and no SSDT that
  disables the Wi-Fi PCI device.

## What works

Everything below was tested on **one machine**: macOS 26.7.1 (25G241) and
26.7.2 (25G332), SMBIOS MacPro7,1, MT7921 Wi-Fi `14c3:7961` with Bluetooth
`0489:e0cd`, and an iPhone 15 Pro Max (`iPhone16,2`) on iOS 18 and iOS 27.
"Untested" means the code exists but nobody has confirmed it on hardware yet.

### By macOS build

Bluetooth patches `bluetoothd` in memory, so features depend on the exact
build. Check yours with `sw_vers -buildVersion`.

| macOS | Wi-Fi, pairing, audio | AirDrop / Handoff (BLE advertising) | Wideband mic (HFP mSBC) | Instant Hotspot |
|---|---|---|---|---|
| 26.7.2 (25G332) | Works | Works | Works | Works |
| 26.7.1 (25G241) | Works | Works | Works | No. The patch needs 25G332; update to 26.7.2 |
| 26.7.3 (25G342) | Untested | Untested | Untested | Untested |
| Other 26.x | Untested | Untested | Untested | Untested |

On 25G342 the patches (1.1.1+) match the `bluetoothd` binary in offline
tests, but nothing has been run on real hardware yet. On any build the kext
checks every signature before patching and leaves a feature unpatched when
the check fails, so an unsupported build loses that feature instead of
crashing `bluetoothd`.

### Wi-Fi

| Feature | Status | Notes |
|---|---|---|
| Scan, join, DHCP | Works | WPA2 and WPA3-transition AP, 5 GHz channel 149, 80 MHz, 802.11ac |
| AWDL | Works | Leaves the AP channel using a power-save handshake |
| Joining while AirDrop is active | Works | Scans and joins take the radio from AWDL until associated |
| WPA3-Personal (SAE), 802.11w PMF | Untested | SAE group 19, hunting-and-pecking only. No H2E, no SAE-PK. `amtk_pmf=0` goes back to WPA2-PSK only |
| 802.11ax | Untested | Offered by default at 80 MHz, no TWT / MU-MIMO / 160 MHz. `amtk_he=0` turns it off |
| 6 GHz (Wi-Fi 6E) | Untested | Off unless `amtk_6ghz=1`, MT7922 / MT7921K only. Joining is not possible yet because 6 GHz needs H2E or OWE |
| DFS channels (52–144) | Untested | Passive scan only, never used for AWDL |
| Roaming | Untested | Same-SSID re-association, not 802.11r. `amtk_roam=0` turns it off |
| Sleep / wake | Untested | Wake restarts the HAL and reloads firmware if the restart fails |

Details and all boot arguments: [docs/WIFI.md](docs/WIFI.md),
[docs/ROAMING.md](docs/ROAMING.md), [docs/WIFI-6GHZ.md](docs/WIFI-6GHZ.md).

### Bluetooth

| Feature | Status | Notes |
|---|---|---|
| Firmware load, pairing | Works | Shows up as `Chipset: THIRD_PARTY_DONGLE`. Needs the NVRAM keys from the install steps |
| A2DP audio, AirPods (AAC-ELD) | Works | |
| BLE scanning | Works | The Mac sees the iPhone |
| BLE advertising | Works | The iPhone sees the Mac. Needs 25G241 or newer |
| HFP microphone | Works | 16 kHz mSBC on `0489:e0cd` and `13d3:3610` only; other IDs fall back to narrowband CVSD. `-mtkbtnomsbc` forces CVSD |
| Mac connecting to an iPhone over BLE | Untested | |

Details and all boot arguments: [docs/BLUETOOTH.md](docs/BLUETOOTH.md),
[docs/BLE-ADVERTISING.md](docs/BLE-ADVERTISING.md).

### Apple features

| Feature | Status | Notes |
|---|---|---|
| AirDrop, both directions, iOS 16–27 | Works | About 4 MB/s average iPhone → Mac. [Measurements](docs/AIRDROP-PERFORMANCE.md) |
| Handoff, Universal Clipboard, AirPlay to Mac | Works | |
| Instant Hotspot (same iCloud account or Family) | Works | 25G332 or newer. [How it was diagnosed](docs/INSTANT-HOTSPOT.md) |
| iPhone Mirroring | Experimental | Only with [Mirra](https://github.com/eiyuumiru/Mirra), see below |
| Auto Unlock, Sidecar, Continuity Camera / Microphone | Untested | |

## Install

Keep a bootable copy of your current EFI on a USB stick before you start.

### Installer script

```sh
sudo bash -c "$(curl -fsSL https://raw.githubusercontent.com/Dino-VN/MtkWireless/main/scripts/install.sh)"
```

The script asks before it changes anything, and backs up `config.plist` and
every kext it removes to `~/Desktop/MtkWireless-backup-<date>`. It:

1. detects the MediaTek Wi-Fi and Bluetooth hardware and installs only the
   kexts that have matching hardware;
2. finds and mounts the EFI partition OpenCore booted from;
3. downloads the latest release, or updates an older installed version;
4. removes conflicting Intel, Broadcom and MediaTek kexts from
   `EFI/OC/Kexts` and `Kernel → Add`;
5. adds Lilu and BlueToolFixup if missing, fixes the load order and sets the
   Bluetooth NVRAM keys;
6. prints which features your macOS build supports.

Options: `--dry-run` shows what would change, `--force` reinstalls,
`--version v1.0.7` installs a specific release. Reboot when it finishes.

### Manual install

Download `MtkWireless-<version>-RELEASE.zip` from
[Releases](https://github.com/Dino-VN/MtkWireless/releases).

1. Mount the EFI partition OpenCore boots from:

   ```sh
   diskutil list
   sudo diskutil mount disk0s1
   ```

2. Remove the conflicting kexts listed under [Requirements](#requirements)
   from `EFI/OC/Kexts` and from `config.plist → Kernel → Add`.

3. Copy `MtkWirelessWifi.kext` and `MtkWirelessBT.kext` to `EFI/OC/Kexts`.
   For Bluetooth you also need
   [Lilu](https://github.com/acidanthera/Lilu/releases) and
   `BlueToolFixup.kext` from
   [BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM/releases).

4. Add the entries to `Kernel → Add` in this order (or merge
   [docs/config-snippet.plist](docs/config-snippet.plist)):

   | # | BundlePath | ExecutablePath | MinKernel | MaxKernel |
   |---|---|---|---|---|
   | 1 | `Lilu.kext` | `Contents/MacOS/Lilu` | | |
   | 2 | `BlueToolFixup.kext` | `Contents/MacOS/BlueToolFixup` | `21.0.0` | |
   | 3 | `MtkWirelessWifi.kext` | `Contents/MacOS/MtkWirelessWifi` | `25.0.0` | `25.99.99` |
   | 4 | `MtkWirelessBT.kext` | `Contents/MacOS/MtkWirelessBT` | | |

   For every entry: `PlistPath` = `Contents/Info.plist`, `Arch` = `x86_64`,
   `Enabled` = true. Lilu must come before BlueToolFixup and MtkWirelessBT.

5. For Bluetooth, add these under
   `NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82`, and list both keys
   under `NVRAM → Delete` for the same GUID:

   | Key | Type | Value |
   |---|---|---|
   | `bluetoothInternalControllerInfo` | Data | `0000000000000000000000000000` (14 zero bytes) |
   | `bluetoothExternalDongleFailed` | Data | `00` |

   Without them `bluetoothd` assumes Apple's Broadcom module and aborts on
   MediaTek's replies.

6. Save, reboot, and choose Reset NVRAM once in the OpenCore picker so the
   new keys apply.

To update, replace the two kexts in `EFI/OC/Kexts` and reboot, or run the
installer again.

### Check that it loaded

```sh
kmutil showloaded --list-only | grep MtkWireless
ioreg -l -w0 | grep RustSelfTestFailed                   # expect 0
system_profiler SPBluetoothDataType | grep -E 'Chipset|State'
```

Bluetooth should report `Chipset: THIRD_PARTY_DONGLE` and `State: On`, and
the Wi-Fi menu should list networks.

On a new machine, bring up one kext at a time. Start with Bluetooth: pair a
device, play audio, then try AirDrop. Add Wi-Fi after that: join a network,
then try AirDrop in both directions.

| Symptom | What to do |
|---|---|
| No Wi-Fi interface | Make sure no other Wi-Fi kext claims the card, then look for kext load errors |
| Wi-Fi is gone and a restart does not bring it back | The firmware crashed. Shut down, wait 10–15 seconds, power on |
| Bluetooth disappeared together with Wi-Fi | A chip reset takes Bluetooth down too. Reboot |

## iPhone Mirroring with Mirra

[Mirra](https://github.com/eiyuumiru/Mirra) is a separate project that makes
Apple's iPhone Mirroring run on Intel Hackintoshes. MtkWireless only provides
the Wi-Fi, Bluetooth and AWDL link it needs. It does not emulate a T2 chip,
and Mirra is not bundled with these kexts.

You need:

- macOS 26.7.2 (25G332), the only tested build;
- a Wi-Fi kext built from current source (older release zips may lack the
  required RX and TX fixes);
- Mirra, set up according to
  [its guide](https://github.com/eiyuumiru/Mirra#requirements);
- a **second, jailbroken iPhone** (iOS 15+, OpenSSH, `ldid`) that Mirra uses
  as a bridge. The iPhone you mirror does not need a jailbreak.

Current state on the test machine: authentication succeeds, the H.264 decoder
is created and the first video frame arrives. Stable video, audio, input and
reconnection are not verified. Running Mirra can also break AirDrop and other
AWDL features while it is active; the cause is not known yet. Driver notes:
[docs/IPHONE-MIRRORING.md](docs/IPHONE-MIRRORING.md).

## Reporting a problem

Run the collector **right after the failure**, before restarting Bluetooth or
toggling Wi-Fi:

```sh
script="$(curl -fsSL https://raw.githubusercontent.com/Dino-VN/MtkWireless/main/scripts/collect-logs.sh)" && sudo bash -c "$script"
```

It writes `MtkWireless-Logs-<date>-<random>.zip` to your Desktop. The bundle
contains the macOS build, card and kext details, the IORegistry, driver
counters, network state, the `bluetoothd` binary, recent crash reports and the
last 5 minutes of Wi-Fi / Bluetooth / AirDrop logs. It only reads: it does not
restart daemons, load kexts, change settings or upload anything. It needs no
Xcode or Python.

Useful options, added after `--`:

```sh
sudo bash -c "$script" -- --last 15m --verbose --notes "AirDrop Mac to iPhone stalls"
```

| Option | Effect |
|---|---|
| `--last 15m` | Collect a longer log window than the default 5 minutes |
| `--notes "..."` | Attach a description of what went wrong |
| `--verbose` | Include info/debug log levels |
| `--logarchive` | Add a native log archive (larger zip) |
| `--include-hci` | Copy existing HCI traces instead of only listing them |
| `--include-kexts` | Copy the kext bundles |

**The bundle is not anonymized.** It can contain SSIDs, MAC and IP addresses,
serial numbers, device and user names. Send it privately; do not attach it to
a public issue. Keychain data and Bluetooth link keys are not copied.

## Build from source

Requires Xcode and rustup.

```sh
scripts/bootstrap.sh          # MacKernelSDK submodule, Lilu SDK, pinned Rust nightly
scripts/build.sh Release      # build/Release/MtkWirelessWifi.kext, MtkWirelessBT.kext
scripts/package.sh Release    # dist/MtkWireless-<version>-RELEASE.zip
(cd rust && cargo test --workspace)
```

| Path | Contents |
|---|---|
| `rust/mtk-wlan` | Wi-Fi: firmware parsing, RX/TX descriptors, scan, join, SAE, AWDL |
| `rust/mtk-bt` | Bluetooth: firmware, WMT, HCI rewrites, `bluetoothd` patch resolver |
| `rust/mtk-ffi`, `rust/mtk-kernel` | C interface and kernel runtime for the Rust code |
| `kext/Wifi` | Wi-Fi kext; `vendor/` is the SimpleMtkWlan HAL and OpenBSD net80211 |
| `kext/BT` | Bluetooth kext: USB loader, Lilu plugin, mSBC bridge |
| `firmware/` | MediaTek firmware images embedded into the kexts |
| `scripts/`, `tests/` | Build, install, diagnostics and host-side regression tests |
| `docs/` | Per-feature notes and investigation logs |

## Credits

- [SimpleMtkWlan](https://github.com/laobamac/SimpleMtkWlan) by laobamac,
  built on zxystd's macOS WLAN framework, with contributions from Raboebie,
  mavizao and eiyuumiru. The Wi-Fi HAL comes from it; see
  [kext/Wifi/vendor/CREDITS.md](kext/Wifi/vendor/CREDITS.md).
- SimpleMtkBtFw by zxystd and laobamac, the base of the Bluetooth firmware
  loader.
- OpenBSD `net80211` and the `if_mwx` driver.
- Google's libsbc, used for the mSBC codec.
- [Lilu](https://github.com/acidanthera/Lilu),
  [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) and
  BlueToolFixup by acidanthera.
- [Mirra](https://github.com/eiyuumiru/Mirra) by eiyuumiru.
- Everyone who sent patches and logs.

## License

`MtkWirelessWifi.kext` is GPL-2.0 and `MtkWirelessBT.kext` is GPL-3.0-only;
the texts are in [LICENSES](LICENSES). Vendored files keep their own license
headers.

## Support

I work on this in my free time and test on my own hardware. If the kexts are
useful to you, you can support the project on Ko-fi:
<https://ko-fi.com/dinovn>
