# MediaTek BLE feature-mask USB compatibility

MtkWirelessBT handles the BCM LE feature-mask startup command that MediaTek
rejects. The shim is on by default on Monterey and later when all four
IOUSBHostFamily routes resolve (`-mtkbtnolefix` turns it off). The hook
symbols were checked on Tahoe 26.7.1 / 25G241; other versions are unverified.

The logic is in `rust/mtk-bt/src/le_features.rs` (ported from
SimpleMtkBtPatcher's `LeFeaturesShim.hpp`); `kext/BT/Patcher.cpp` moves the
bytes in and out of the USB transfers. It does not modify bluetoothd, force an
address policy, provision an IRK or implement Broadcom privacy commands.

## Status

The shim has not run on hardware yet. In the only capture so far
(`airdrop-bt-20261023-212105`) `kexts.txt` lists SimpleMtkBtFw 1.0.0 but no
SimpleMtkBtPatcher, so the patcher that carried the shim was not loaded and
the trace shows the unpatched behavior. The collector now warns when
MtkWirelessBT is not loaded.

## Evidence and behavior

The retained `airdrop-bt-20261023-212105` capture shows BLE scan commands
succeeding, an eight-byte `0xFD3C` feature-mask request followed by Command Status
`0x01` (Unknown HCI Command), and no advertising data or enable commands.
`rapportd` reports NeedsCLink advertiser startup while `bluetoothd` repeatedly
reports `Stack is not ready`. The upper-layer startup log does not establish
that an advertising packet reached the controller or the air.

Native disassembly confirms that `_BCM_WriteLELocalSupportedFeatures` copies
exactly eight feature bytes into a vendor request. In the inspected x86_64
slice, that function begins at `0x10019a2d5` and sends OCF `0x13C`; the common
sender at `0x1001e3616` ORs in `0xFC00`, producing opcode `0xFD3C`.
These are shell disassembly observations, not runtime execution evidence.
Other vendor paths can assign a different meaning to the same opcode, so the
shim also requires the exact startup payload captured here:
`fd fd 01 ff 09 00 00 00`.

The shim tracks that command on EP0 and the event pipe on HCI interface 0.
It scopes both to a USB registry ID with the `MtkBtLog` marker and successful
`FirmwareLoaded` property published by MtkWirelessBTLoader.
On the same device's interrupt IN, it converts only the corresponding
Unknown HCI Command response:

```text
Command Status:   0F 04 01 NN 3C FD
Command Complete: 0E 04 NN 3C FD 00
```

Both events are six bytes; `NN` is the controller's original command credit.
The command is still sent to MediaTek, and the rejected response is replaced
in its existing transfer. There is no extra USB transfer or duplicate HCI
response. Other statuses, existing Command Complete responses, other opcodes,
other feature masks and unrelated devices are passed through.

Synchronous reads are handled after they complete. Asynchronous reads retain
the descriptor, transform the received bytes before the original callback,
and forward its original owner, parameter, USB status and byte count.
Command state is cleared by HCI Reset, replacement of the event pipe, or a
failed EP0 submission/completion. Generation tokens prevent a late failed
completion from cancelling a newer request. Complete HCI framing is checked
before rewriting; truncated events and matching bytes inside payloads are not
interpreted as command responses.

This supplies the successful vendor compatibility completion that the host
expects. It does not establish that this rejected command is the only cause of
advertiser readiness failure. Successful BLE advertising and iPhone/AWDL
behavior must still be measured with the new kext loaded.

## Linux reference and controller capabilities

The Linux sources reviewed on 2026-10-24 were from revision
`734bef548a96e8774d6eef5dfa2026c6edc4e80b`.
The suggested [mt7921 driver](https://github.com/torvalds/linux/tree/734bef548a96e8774d6eef5dfa2026c6edc4e80b/drivers/net/wireless/mediatek/mt76/mt7921)
handles Wi-Fi through mac80211. Its `mt7921_mcu_set_roc` function manages
remaining on a Wi-Fi channel, which can inform the Wi-Fi side of discovery,
but does not initialize BLE advertising or implement Apple's AWDL protocol.

For Bluetooth, [btmtk_usb_setup](https://github.com/torvalds/linux/blob/734bef548a96e8774d6eef5dfa2026c6edc4e80b/drivers/bluetooth/btmtk.c#L1768)
loads the MT7922/MT7961 firmware, configures endpoint reset and enables the
Bluetooth protocol with WMT. The corresponding setup sequence already exists
in `rust/mtk-bt/src/setup.rs`. No IRK provisioning or `0xFD3C` handler was found in the
reviewed `btmtk.c` and `btmtk.h`.

Linux implements privacy in the generic Bluetooth host stack:
[hci_le_add_resolve_list_sync](https://github.com/torvalds/linux/blob/734bef548a96e8774d6eef5dfa2026c6edc4e80b/net/bluetooth/hci_sync.c#L2507)
programs peer and local IRKs through LE Add Device To Resolving List (`0x2027`),
and enables resolution through `0x202D` when supported.
[hci_update_random_address_sync](https://github.com/torvalds/linux/blob/734bef548a96e8774d6eef5dfa2026c6edc4e80b/net/bluetooth/hci_sync.c#L1071)
also generates an RPA from the host's IRK and sets it through `0x2005`.
These paths require the actual host identity key and correct command ordering.

The supplied capture contains a successful LE Read Local Supported Features
response (`0x2003`) with bits `0x0000000eff01fdfd`. It reports LL Privacy (bit 6)
and LE Extended Advertising (bit 12). The Read Local Supported Commands response
also reports support for `0x2027` (octet 34, bit 3) and `0x202D` (octet 35, bit 1).
These advertised capabilities support investigating a standard HCI privacy path;
they do not prove that programming the resolving list succeeds, that macOS uses
that path, or that `0xFD3C` can be translated into either command. The current
feature-mask shim does not program the resolving list or perform that translation.

## Validation

Offline (no hardware):

```sh
cd rust && cargo test -p mtk-bt le_features
MTK_BT_LE_CAPTURE=/path/to/hci.pklg cargo test -p mtk-bt capture_replay
```

The tests cover every HCI status and command credit, truncated and coalesced
events, embedded opcode bytes, stale cancellation tokens, reset, duplicate
processing, independent controllers and event-pipe rebinding. Replaying the
2026-10-23 capture yields exactly one synthesized completion. Captures contain
device addresses and keys; keep them out of the repository.

On the machine, after installing MtkWirelessBT and rebooting:

```sh
ioreg -l -w0 -r -n IOResources | grep -A12 '"MtkWirelessBT"'
```

`LeFeaturesShimEnabled` must be true. `LeFeaturesCommands` counts recognized
startup requests, `LeFeaturesCompletions` replacement completions; both should
become nonzero. `LeFeaturesErrors` counts allocation, submission/completion
and descriptor-write failures. Commands without completions point at pipe
binding or event delivery.

Then capture an attempt (iPhone AirDrop set to Everyone for 10 Minutes):

```sh
scripts/Collect-AirDrop-BT.command
python3 scripts/analyze_airdrop_hci.py diagnostics/<capture>/hci.pklg
```

Check that `0xFD3C` gets Command Complete with status 0, that the advertiser
becomes ready, and that advertising commands (`0x2006`, `0x2008`, `0x200A` or
the extended `0x2036`–`0x2039`) succeed. Then test iPhone discovery and
transfers both ways. A NeedsCLink startup log or the synthetic completion
alone is not proof.

If advertising still fails with the shim active, the next candidate is the
standard HCI privacy path (resolving list `0x2027`/`0x202D`, RPA via `0x2005`),
which the controller advertises (see above); that needs the host IRK and is
not implemented.
