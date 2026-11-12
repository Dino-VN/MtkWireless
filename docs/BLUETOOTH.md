# Bluetooth (MtkWirelessBT.kext)

Firmware loader and bluetoothd fixes for MediaTek MT7921 / MT7922 USB
Bluetooth. Do not load any other MediaTek Bluetooth firmware kext together
with it (both would claim the device).

| Feature | Status |
|---|---|
| Firmware load, pairing | ✅ |
| A2DP (SBC/AAC), AirPods AAC-ELD, controls, switching | ✅ via Read Clock and ACL fixes |
| BLE scanning | ✅ |
| BLE advertising / AirDrop | ⚠️ feature-mask shim, see [BLE-ADVERTISING.md](BLE-ADVERTISING.md) |
| HFP microphone | ✅ 16 kHz mSBC on MT7921 `0489:e0cd` and MT7922 `13d3:3610` (below); CVSD elsewhere |

Rewritten in Rust (rust/mtk-bt): firmware parsing, WMT, the bring-up
sequence and every HCI rewrite. C++ keeps USB access, zlib and the Lilu routes.

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
| `-mtkbtrandaddr` | Send a fixed LE random address before scanning |
| `-mtkbtnomsbc` | Disable the mSBC (wideband microphone) bridge; calls fall back to CVSD |
| `-mtkbtnoopenwatchdog` | No re-enumeration when bluetoothd has not opened the HCI interface 20 s after a post-boot bring-up |
| `-mtkbtnoshutdownoff` | Leave the controller on at shutdown/restart (no WMT FUNC_CTRL off) |

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

## Page patches across macOS builds

Every bluetoothd page patch is one entry in `bluetoothdSites`
(`kext/BT/Patcher.cpp`), searched with a masked matcher in Rust
(`mtk_bt::page_patch`): rel32/RIP displacements that move between builds are
wildcards, and a site must match exactly once in its page or it is left
native. Each site has a scope, keyed on `kern.osversion` (ioreg
`BluetoothdBuild`):

| Scope | Sites | Builds |
|---|---|---|
| any build | extended-advertising check, random address before enable | any Tahoe build where the pattern matches |
| known build | per-set address gate, HFP wideband gate, HFP sender | 25G241, 25G332; others with boot-arg `-mtkbtanybuild` |
| RPA | Instant Hotspot helpers and their 6 call sites | 25G332 only |

A feature (advertising, mSBC, RPA) is decided at its first matching site and
kept for the bluetoothd vnode, so a gate that flips later (mSBC bridge
readiness, controller scope) cannot leave one site of a pair patched.
`BluetoothdPatchFeatures` shows the decisions, 2 bits each (advertising,
mSBC, RPA): 1 on, 2 off. `BluetoothdSitesAmbiguous` counts sites refused for
matching twice.

Page patches now fail closed: without the verifier restore (hook state not
1, or `-mtkbtnoverifyfix`) no page is patched (`BluetoothdPagesUnpatched`),
since a resident patched page breaks its neighbours' page-ins.

To add a new macOS build to the known list, check an unpatched copy of its
bluetoothd (the live one may show patched cache pages):

```sh
python3 scripts/test-bt-page-patches.py --binary /path/to/bluetoothd
```

It checks every masked site for exactly one same-page match, that the masked
sites turn the historical 25G241 signatures into their old patches, and that
the NOPed address call still targets the default-true gate setter. If it
passes, add the build to `readBluetoothdBuild`. RPA still needs its own arrays
per build. `-mtkbtanybuild` lets a user try the known-build sites before
that; RPA is never enabled by it.

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

Since 1.0.8 the page hook and verification hook share a lock. Without it, a
neighbour's page-in could hash a page after its bytes were patched but before
their originals were recorded, including the first page before the cached
vnode was published. Verification also keeps an untouched shared buffer
locked until its hash finishes; restored private copies can be hashed after
unlocking. The 2026-11-02 regression pauses the production hook in this gap
and verifies the 16 KB block containing the RPA helpers and the adjacent
`__DATA_CONST` page, using the universal binary's 16 KB slice offset. It
fails before the fix and passes with the lock. Run it with ASan/UBSan by
default, or with `python3 scripts/test-bt-msbc-page-patches.py --thread-sanitize`.
1.0.8 still lost Bluetooth after sleep (2026-11-03 17:31). On wake the
USB port had been powered off, the controller re-enumerated and took the
~22 s cold firmware download; bluetoothd aborted itself ("updateAdvertisement
busy with address change for more than 10000ms") and every relaunch died at
dyld start with SIGBUS. The kernel log named the block: "Data hash mismatch
for 16384 bytes at offset 11157504" (file offset 0xaa4000: the RPA helper
page resident, `__DATA_CONST`'s `__got` page absent), and the hash APFS
computed is exactly that of the block with our helper patch applied. The
verification hook had never run: the 25G332 kernel inlines `VNOP_VERIFY`
into `decmpfs_pagein_compressed` and calls APFS's verifier through the
vnode's operations vector, so the `_VNOP_VERIFY` route only saw other
callers. Since 1.0.9 the hook replaces the `vnop_verify` entry of the
bluetoothd vnode's operations vector (APFS's, shared by its regular files)
at bluetoothd's first validated page; the `v_op` offset is decoded from
`_VNOP_VERIFY` (0xd0 on 25G332) and checked against `vnode_mount` and
`vnode_fsnode`. Expect `BluetoothdVerifyHookState = 1` once bluetoothd runs
(2-6: the failed step, page patches then stay without RPA and
recording), `BluetoothdVnodeOpsOffset = 208`, and `BluetoothdVerifyRestored`
counting blocks that needed restoring (one after each bluetoothd relaunch
whose `__got` page was evicted). `BluetoothdVerifyFailed` counts bluetoothd
blocks that still failed with no recorded patch bytes in them
(`BluetoothdVerifyLastFailOffset` is the last one). These counters are
published from a thread call, so they stay current while bluetoothd is
crashing. Hardware verification after sleep is pending.

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
