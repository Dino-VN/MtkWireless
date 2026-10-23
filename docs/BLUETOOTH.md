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
| HFP microphone | ⚠️ CVSD only, quiet and choppy |

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
