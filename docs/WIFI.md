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
| `amtk_he=0` | do not offer 802.11ax (offered by default since 1.0.1, 80 MHz; `mtkctl knob 0x1002` does the same at runtime for the next join) |
| `amtk_awdl=0` | disable native AWDL (on by default on Tahoe) |
| `amtk_6ghz=1` | scan and report 6 GHz on chips that have it (MT7922, MT7921K); off by default: 6 GHz joins need SAE hash-to-element or OWE, not implemented ([WIFI-6GHZ.md](WIFI-6GHZ.md)) |
| `amtk_roam=0` | no in-driver roaming: stay on the joined BSSID until the link drops ([ROAMING.md](ROAMING.md)) |
| `amtk_awdl_offchan=0` | keep AWDL on the AP channel while associated (no PM handshake; AirDrop to phones on other channels fails) |
| `amtk_pmf=0` | no 802.11w PMF and therefore no WPA3-SAE: WPA3-transition networks are shown to macOS as WPA2-PSK again, WPA3-only and PMF-required networks cannot be joined (the 1.0.0 behaviour) |

## WPA3-Personal and PMF

1.0.1+, **unverified on hardware**.

- airportd (IO80211.framework `Apple80211Associate2`, Tahoe 25G) only
  offers WPA3-SAE when `CARD_CAPABILITIES` byte 5 bit 3 is set, and PMF when
  byte 2 has `0x11`; the driver sets byte 5 `0x08` and byte 2 `0x01` while
  PMF is on. The WCL join then carries the password itself
  (`apple80211_key` cipher 10, `APPLE80211_CIPHER_PWD`, 1..63 bytes),
  because SAE cannot start from a PMK.
- SAE (group 19, hunting-and-pecking PWE, anti-clogging tokens) runs in
  `rust/mtk-wlan/src/sae` (P-256, SHA-256, KDF; tested against IEEE
  802.11-2020 Annex J.10 and checked again by the kext self-test);
  `net80211/ieee80211_sae.c` sends Commit/Confirm (authentication
  algorithm 3) before association and puts PMK/PMKID in the PMKSA cache.
  The 4-way handshake then uses AKM 8: KDF-SHA-256, AES-CMAC MIC, EAPOL-Key
  descriptor version 0.
- Transition networks (PSK + SAE) use SAE when macOS passes a password;
  with a PMK only, they join as WPA2-PSK (with PMF when the AP offers it).
  A BSS of the same network that lacks SAE gets the PBKDF2 PSK.
- PMF: the MAC encrypts and decrypts unicast robust management frames
  (Deauth, Disassoc, Action) with the pairwise key; the IGTK goes to the
  firmware as mt76 sends it (group CCMP + BIP-CMAC-128) and to net80211's
  software BIP, which checks broadcast Deauth/Disassoc. An unprotected
  Deauth/Disassoc from the AP starts an SA Query (3 × 201 ms); no answer
  ends the association.
- Not supported: H2E (hash-to-element, required for WPA3 on 6 GHz and by
  APs set to H2E only), SAE-PK, password identifiers, other groups,
  WPA3-Enterprise, FT-SAE, PMKSA caching across joins.
- ioreg (published with the AWDL diagnostics): `MTKWiFiPMF`,
  `MTKWiFiSAEAccepted`, `MTKWiFiSAEFailed`, `MTKWiFiSAELastStatus` (the AP's
  status code of the last failure: 77 = group refused, 126 = AP wants H2E).
  A wrong password shows as SAE timeouts: the AP drops a bad Confirm
  silently.

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
