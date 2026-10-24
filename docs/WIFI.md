# Wi-Fi (MtkWirelessWifi.kext)

Native Wi-Fi (Wi-Fi menu, System Settings, airportd) for MediaTek MT7921 /
MT7922 PCIe on **macOS 26 Tahoe**. It is AirportMtkWlan (SimpleMtkWlan
`dfca847`, vendored in `kext/Wifi/vendor`) built by this repository, plus the
Rust core self-test. The driver is moved to Rust piece by piece (CLAUDE.md,
phases 2–5); until then behavior is that of AirportMtkWlan `dfca847`.

It replaces AirportMtkWlan: the IOKit class names are the same (so `mtkctl`
keeps working), and the two must never be loaded together. Remove itlwm,
AirportItlwm, AirportMtkWlan and any SSDT that disables the Wi-Fi PCI device.

Supported PCI IDs: `14c3:7961` (MT7921), `14c3:0608` (MT7921K / RZ608),
`14c3:7922` (MT7922), `14c3:0616` (MT7922 / RZ616).

macOS 26 Tahoe only. Code for older macOS (Sequoia's IO80211 ABI) was removed;
a Tahoe build never attaches on older systems.

## Install

The kext is unsigned; SIP must allow unsigned and unapproved kexts.

Load after boot (how AirportMtkWlan was tested):

```sh
sudo mkdir -p "/Library/Application Support/MtkWireless"
sudo cp -R MtkWirelessWifi.kext "/Library/Application Support/MtkWireless/"
sudo chown -R root:wheel "/Library/Application Support/MtkWireless/MtkWirelessWifi.kext"
sudo kmutil load -p "/Library/Application Support/MtkWireless/MtkWirelessWifi.kext"
```

The first load is usually refused: Allow it in System Settings › Privacy &
Security, reboot, run `kmutil load` again. Every new build needs approving
again. Approving a kext rebuilds the Auxiliary Kernel Collection; check that
other unsigned kexts are still loaded afterwards.

OpenCore (untested): `Kernel → Add` with `ExecutablePath`
`Contents/MacOS/MtkWirelessWifi`, `MinKernel` `25.0.0`, `MaxKernel`
`25.99.99`. It does not need Lilu.

## Boot arguments

| Boot-arg | Effect |
|---|---|
| `-novht` | disable 802.11ac |
| `-noht40` | disable 40 MHz channels |
| `amtk_cc=XX` | override the country code |
| `amtk_he=1` | offer 802.11ax (experimental, 80 MHz) |
| `amtk_awdl=0` | disable native AWDL (on by default on Tahoe) |
| `amtk_awdl_offchan=0` | keep AWDL on the AP channel while associated (no PM handshake; AirDrop to phones on other channels fails) |

## Checking

```sh
kmutil showloaded --list-only | grep MtkWireless
ioreg -l -w0 | grep -E 'RustCore|RustSelfTestFailed'
mtkctl sta; mtkctl log     # tools/mtkctl.c in kext/Wifi/vendor
```

| Symptom | Fix |
|---|---|
| No Wi-Fi interface | Check that AirportMtkWlan is not loaded, then load/approval errors and PCI matching. |
| Wi-Fi died and does not come back after restart | Firmware crash: shut down, wait 10–15 s, power on. |
| Bluetooth disappeared with Wi-Fi | A chip reset also takes down Bluetooth. Reboot. |
