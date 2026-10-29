# Bluetooth (MtkWirelessBT.kext)

Firmware loader and bluetoothd fixes for MediaTek MT7921 / MT7922 USB
Bluetooth. Replaces `SimpleMtkBtFw.kext` **and** `SimpleMtkBtPatcher.kext`:
never load them together with MtkWirelessBT (both would claim the device).

| Feature | Status |
|---|---|
| Firmware load, pairing | ✅ (same sequence as SimpleMtkBtFw 1.1.0) |
| A2DP (SBC/AAC), AirPods AAC-ELD, controls, switching | ✅ via Read Clock and ACL fixes |
| BLE scanning | ✅ |
| BLE advertising / AirDrop | ⚠️ feature-mask shim, see [BLE-ADVERTISING.md](BLE-ADVERTISING.md) |
| HFP microphone | ✅ 16 kHz mSBC on MT7921 `0489:e0cd` and MT7922 `13d3:3610` (below); CVSD elsewhere |

Rewritten in Rust (rust/mtk-bt): firmware parsing, WMT, the bring-up
sequence and every HCI rewrite. C++ keeps USB access, zlib and the Lilu routes.
Behavior is the same as SimpleMtkBtFw/SimpleMtkBtPatcher; the boot arguments
were renamed (see below).

## Install (OpenCore)

1. `EFI/OC/Kexts`: `Lilu.kext`, `BlueToolFixup.kext`
   ([BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM)),
   `MtkWirelessBT.kext`.
2. `Kernel → Add`, in this order: Lilu → BlueToolFixup (MinKernel `21.0.0`)
   → MtkWirelessBT. Entries are in `docs/config-snippet.plist`.
3. NVRAM (`NVRAM → Add` **and** `NVRAM → Delete`, GUID
   `7C436110-AB2A-4BBB-A880-FE41995C9F82`):

   | Key | Type | Value |
   |---|---|---|
   | `bluetoothInternalControllerInfo` | Data | 14 zero bytes |
   | `bluetoothExternalDongleFailed` | Data | `00` |

   Without them bluetoothd assumes Apple's Broadcom module and aborts on
   MediaTek's replies.
4. Reboot. `system_profiler SPBluetoothDataType` should show
   `Chipset: THIRD_PARTY_DONGLE` and `State: On`.

## Boot arguments

| Argument | Effect |
|---|---|
| `-mtkbtoff` | Disable the Lilu routes (the loader still runs) |
| `-mtkbtdbg` | Lilu debug logging for the plugin |
| `-mtkbtnolefix` | Disable the LE feature-mask shim only |
| `-mtkbtrandaddr` | Send a fixed LE random address before scanning (was `-smbtrandaddr`) |
| `-mtkbtnomsbc` | Disable the mSBC (wideband microphone) bridge; calls fall back to CVSD |

## Wideband microphone (Tahoe)

On by default since 1.0.2, on macOS 26 Tahoe, for the USB IDs it was tested
with: MT7922 `13d3:3610` (Galaxy Buds+) and MT7921 `0489:e0cd`, macOS 26.7.1
(25G241), x86_64. Other USB IDs keep CVSD; other macOS builds are untested.

Keep the normal Lilu and BlueToolFixup installation; no custom BlueToolFixup
build is needed. Add `-mtkbtnomsbc` to `boot-args` to return to CVSD.

This enables 16 kHz mono microphone and playback together. It is still call
audio: normal A2DP stereo returns when the microphone closes. On the tested
Buds, playback stayed clean with the microphone open, including a 60-second
duplex test, the usual calling-app/input test, and an ordinary reconnect.
The F1 USB mouse receiver and native Wi-Fi continued working. Multi-hour
stability and other headsets remain untested.

The firmware loader now qualifies each probed controller with the same
USB-ID predicate as the bridge. The global bluetoothd wideband/HFP patches
stay off until a supported controller is known, and an unsupported probe
vetoes them for the boot. This preserves CVSD on MT7922 `0489:e0d8`, which
previously negotiated mSBC even though its USB pipes bypassed the bridge.
This is a scope repair, not new E0D8 wideband support. CVSD uses narrowband
call audio. Since the page patches are global and persist in the file cache,
switching controller hardware after they have been installed still requires
a fresh boot; this change does not add dynamic patch rollback.

`MtkWirelessBT` on IOResources publishes `MsbcScopeFix = 1`,
`MsbcControllerScope` (0 unknown, 1 supported, 2 unsupported, 3 mixed), and
`MsbcWidebandAllowed`. E0D8 should report scope 2, allowed false and zero
`BluetoothdWbsPatched`/`BluetoothdHfpFillPatched` after a fresh boot.

The bridge encodes and decodes mSBC in software. It uses the controller's
USB alternate setting 6 with complete 63-byte HCI packets, while preserving
bluetoothd's 33-byte PCM view and timing. The two bluetoothd page patches
enable wideband audio and keep the sender's PCM reads to complete chunks.
They use exact 25G241 signatures and are applied only after the bridge is
ready. `-mtkbtnoextadv` still controls advertising independently.

After opening the Buds' microphone, inspect `MtkBtMsbcBridgeReady` and
`MtkBtMsbcBridge` on IOResources. The `MtkWirelessBT` counters
`BluetoothdWbsPatched` and `BluetoothdHfpFillPatched` report whether the
wideband and PCM sender page patches matched. Published
sample counts do not prove that playback is audible; check both directions
in the calling app too.

Host regression tests (no audio recording or driver loading):

```sh
python3 scripts/test-bt-msbc.py
python3 scripts/test-bt-msbc-integration.py
python3 scripts/test-bt-msbc-page-patches.py
```

## Page patches and the sealed volume

bluetoothd's page patches (MtkWirelessBT's and BlueToolFixup's) modify pages
in the shared file cache. macOS verifies compressed system files in 16 KB
blocks and hashes resident pages together with the ones it reads, so a
patched page left in memory made every later page-in of its neighbours fail:
bluetoothd crashed about 20 seconds after each launch, with
`apfs_announce_hash_mismatch ... (bluetoothd)` in the kernel log, until
reboot. MtkWirelessBT now records the original bytes of every patched
bluetoothd range and lets the verification hash a copy with them restored.
`-mtkbtnoverifyfix` disables this. `BluetoothdPatchRanges` should count
both kexts' patches (about six on 25G241) and `BluetoothdVerifyRestored` how
often a block needed it.

## Checking

```sh
ioreg -l -w0 | grep -A30 MtkBtLog                    # loader log on the USB device
ioreg -l -w0 -r -n IOResources | grep -A12 '"MtkWirelessBT"'
```

`MtkWirelessBT` on IOResources holds `RustCore`, `RustSelfTestFailed` (must
be 0), `ReadClockRewritten`, `AclPbRewritten` and the `LeFeatures*` counters.
If the key is missing, the kext is not loaded (Lilu, order, conflicting
loaders).

## Troubleshooting

* **Device missing after a reboot.** The M.2 card keeps standby power and its
  Bluetooth core can stay wedged; cut power for ~30 s.
* **bluetoothd keeps restarting.** Check the NVRAM variables. After many
  crashes launchd throttles it for 10–20 minutes:
  `sudo launchctl kickstart -k system/com.apple.bluetoothd`.
* **HCI trace.**

  ```sh
  sudo defaults write /Library/Preferences/com.apple.MobileBluetooth.debug HCITraces -dict HCIFileTraces -bool true
  sudo pkill bluetoothd
  # /private/var/root/Library/Logs/Bluetooth/bluetoothd-hci-latest.pklg
  sudo defaults delete /Library/Preferences/com.apple.MobileBluetooth.debug
  ```

* **Music sounds like a phone call.** That is HFP: the headphones are the
  selected microphone. Choose another input device.
