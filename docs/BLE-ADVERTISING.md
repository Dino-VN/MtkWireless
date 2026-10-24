# MediaTek BLE feature-mask USB compatibility

MtkWirelessBT handles the BCM LE feature-mask startup command that MediaTek
rejects. The shim is on by default on Monterey and later when all four
IOUSBHostFamily routes resolve (`-mtkbtnolefix` turns it off). The hook
symbols were checked on Tahoe 26.7.1 / 25G241; other versions are unverified.

The logic is in `rust/mtk-bt/src/le_features.rs` (ported from
SimpleMtkBtPatcher's `LeFeaturesShim.hpp`); `kext/BT/Patcher.cpp` moves the
bytes in and out of the USB transfers. Separate bluetoothd page patches enable
its standard advertising and per-set address paths. macOS owns the addresses;
the shim does not provision an IRK or implement Broadcom privacy commands.

## Status

0.1.9 has run on hardware on macOS 26.7.1 (25G241). All three page-patch
counters are 1, `LeFeaturesCommands=1`, `LeFeaturesCompletions=1` and
`LeFeaturesErrors=0`. bluetoothd reports `Started advertising successfully
status=0`, and the user confirms the iPhone now sees the Mac. AirDrop file
transfer still fails: endpoint resolution stalls and sharingd reports
`missingEndpoint` before transferring data. Advertising is verified;
end-to-end AirDrop is still unresolved.

In the earlier capture
(`airdrop-bt-20261023-212105`) `kexts.txt` lists SimpleMtkBtFw 1.0.0 but no
SimpleMtkBtPatcher, so the patcher that carried the shim was not loaded and
the trace shows the unpatched behavior. The collector now warns when
MtkWirelessBT is not loaded.

## 2026-10-24: bluetoothd disables its own advertiser

With the shim loaded (`LeFeaturesCompletions=1`) the HCI trace shows every
command succeeding and still no advertising command. bluetoothd logs
`LE_InitMultipleAdvertising failed with status STATUS 152` and
`LeBroadcaster.mm::stackDidStart() -- status = 0x0098`; every later start
request ends in `Stack is not ready`, and the iPhone does not see the Mac.

In the x86_64 bluetoothd of 26.7.1 (25G241), `initCoreStack` sets the
multiple-advertising flag from two chip-object checks, extended advertising
sets (vtable 0x918, `0x10033a6ec`) and BT-spec advertising sets (0x9c0,
`0x10033b212`). Both compare only the chip ID at `[this+0x334]` (true for
19-25, 2001-3999, 5000-5999, 6000), never the controller's LE features.
`LE_InitMultipleAdvertising` (`0x1001f7352`) returns 0x98 when the flag is 0.
`LE/SupportsMultipleAdvertising=true` is read ("Overriding Multiple
Advertising support - enabled") but feeds a different flag and changes
nothing; `AE/DisableExtAdvertising` and `AE/DisableSpecSets` can only
clear the checks.

MtkWirelessBT patches the extended-advertising check to return true (see
`Patcher.cpp`). The controller advertises LE Extended Advertising, the
standard path BlueZ uses. 0.1.5 registered the patch with Lilu's user patcher,
which never ran: the DEBUG Lilu log says `disabling user patcher on request!`
because Lilu sets `isUserDisabled` on Big Sur and later. 0.1.6 routes
`_cs_validate_page` instead and patches bluetoothd's page as it is validated,
like BlueToolFixup and RestrictEvents. Not yet run on hardware: check
`BluetoothdExtAdvRouted`, `BluetoothdPages` and `BluetoothdExtAdvPatched` in
ioreg, the absence of the 0x0098 log, and 0x2036-0x2039 in an HCI trace.

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

## 0.1.8: preserve per-set address updates

The 2026-10-24 12:55 capture shows repeated failures in
`multipleAdvStartCb` (718) and `setMultipleAdvertisingDataCompleteCb` (766)
after the extended-advertising patch was applied. Those are stack statuses;
this capture does not contain a complete HCI command trace.

Shell disassembly of the installed 25G241 x86_64 bluetoothd identifies another
chip policy in `initCoreStack` at `0x100510e99`. For chip IDs 0..3 it calls
the setter at `0x1001f79ec` with false, clearing the initially true byte at
`0x100b1cd00`. The before-enable path at `0x1001fa22d` skips per-set address
updates when that byte is false. `LE_UpdateMultiAdvertisingSetAddress` at
`0x1001f713b` also returns without sending a command. With the gate true and
extended advertising enabled, that function sends standard LE Set Advertising
Set Random Address (`0x2035`). A global LE Set Random Address (`0x2005`)
does not initialize each extended advertising set's random address.

0.1.8 removes only the seven-byte false-setter call sequence. It preserves
the chip lookup, branch and subsequent chip setup. macOS supplies its own
per-set addresses and sequences command completion before enabling the set;
the patch does not inject an address or synthesize an advertising success.
The exact 21-byte signature is scoped to bluetoothd validation pages and
disabled together with the advertising patch by `-mtkbtnoextadv`.
`BluetoothdAdvAddressPatched` reports matches in ioreg.

`python3 scripts/test-bt-page-patches.py` checks both signatures against the
installed x86_64 slice, their uniqueness and page boundaries, the setter's
target/default value, and the exact replacement instructions. Existing page
patches can be visible when reading the live vnode cache, so the validator
also recognizes the exact replacement signature. REA/Hopper analysis timed
out on this run; these findings come from llvm-objdump and Mach-O bytes,
not a successful REA decompilation.

After loading 0.1.8, verify both page-patch counters, successful `0x2035`
and `0x2039` completions, and actual iPhone discovery/transfers. The missing
address path is established statically; whether it explains all observed
718/766 failures remains a hardware question.

## 0.1.9: retry random address after the advertising set exists

The hardware capture `airdrop-bt-20261024-132612` confirms that 0.1.8 sends
standard advertising commands, but in this order (PacketLogger record IDs):

| Records | Command | Controller response |
| --- | --- | --- |
| 92–93 | `0x2035`, handle 0, before creation | `0x42`, Unknown Advertising Identifier |
| 94–95 | `0x2035`, handle 2, before creation | `0x42` |
| 106–107 | `0x2036`, handle 3 | success |
| 114–115 | `0x2035`, handle 3 | success |
| 118–119 | `0x2039` enable handle 3 | success |
| 122–125 | parameters/data for handle 0, own-address type 1 | success |
| 126–127 | enable handle 0, no successful per-set random address | `0x12`, Invalid HCI Command Parameters |

All 7,865 failed enables in this capture target handle 0 without a successful
per-set address initialization. Its early address call updates bluetoothd's
cache before the controller rejects it. The before-enable callback sees the
same cached address and skips reinitialization. The one handle with a
successful address command enables successfully. The global `0x2005` command
also succeeds, but does not initialize handle 0.

0.1.9 changes the before-enable cached-address guard: random addresses are
programmed again after parameters and data have been set, even if the cached
address matches. Public addresses retain the changed-address test. The
redundant address-support gate in this guard is replaced together with the
0.1.8 patch that preserves its true value. This is an exact 64-byte signature
at `0x1001fa20d` in 25G241 x86_64. The existing callback still waits for the
real `0x2035` result before enabling; no command response is synthesized.
`BluetoothdAdvAddressRetryPatched` counts matches; `-mtkbtnoextadv` disables
all three policy patches together.

The page-patch validator now executes the actual replacement guard machine
instructions against cached/changed random and public addresses on the x86_64
host. After booting 0.1.9, all three patches applied and bluetoothd reported
successful advertising. The iPhone sees the Mac, confirming discovery on
hardware. File transfer remains unresolved, with DNS resolution timeouts and
`missingEndpoint`; the advertising fix does not establish a working AirDrop
data connection.
