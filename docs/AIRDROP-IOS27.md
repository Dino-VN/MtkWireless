# iOS 27 AirDrop receive stalls (2026-10-26)

Observed with macOS 26.7.1 (25G241), MtkWireless 1.0.1. The user reports
slow iPhone iOS 27 -> Mac transfers and the Mac progress indicator remaining
around halfway after the iPhone reports completion.

## Confirmed driver defect

The existing iOS 27 LLC length-envelope support applied only to non-aggregated
QoS frames. Today's rejected-frame trace also contains A-MSDU subframes with:

```
802.11 QoS header (A-MSDU bit set)
  destination / source / BE16 subframe length
    aa aa 03 00 00 00 / BE16 inner length
      AWDL SNAP / AWDL header / IPv6 packet
```

The old aggregate path expected AWDL SNAP immediately after the subframe
header and rejected these packets. The two initial fully retained frames are
146 bytes: 26-byte MAC header, 14-byte subframe header, 8-byte LLC envelope,
and 98-byte AWDL MSDU. The inner length and IPv6 payload length match exactly.
The later snapshot supplies another 16 complete rejected frames. All 18 now
pass the production parser through its C ABI; truncated larger-frame prefixes
are not treated as proof of whole-frame validity.

The fix recognizes the same envelope separately within each QoS MSDU,
bounding the Reader to the subframe body rather than the remaining aggregate.
Outer length, padding, subframe source, AWDL, IPv6, and duplicate validation
remain in place. Original plain/QoS and unwrapped A-MSDU framing stays supported.
No scheduling or Bluetooth behavior changes are needed for this defect.

## Completion status and timing

Recorded QUIC connections establish in approximately 15-34 ms once they reach
sharingd. This measurement does not include discovery or sender preparation.
The latest recorded QUIC upload starts at 11:47:30.274; the inspected log through
11:50:20 lacks its receive-all-bytes/decompression-success event. Repeated
progress events alone cannot establish that the file is complete. The receiver
invalid-frame count rises from 821 to 1306 between snapshots.

A separate successful TCP/TLS transfer in the same capture illustrates a
second source of UI delay: receive-all-bytes at 11:46:40.498, decompression
success/upload response at 11:46:41.008, followed by an open/move response wait
that times out at 11:46:48.937. That transfer ends at 11:46:48.945. This is a
post-transfer macOS UI stage; it must not be attributed to the latest stuck
QUIC transfer or used to claim that the latest transfer already received all
bytes. The impact of the driver fix on the user's completion mismatch still
requires a new hardware transfer.

## Validation and deployment

- New regression tests fail before the fix and pass afterward.
- 51 mtk-wlan Rust tests pass, including all Ethernet sizes 54..1514,
  mixed wrapped/plain aggregate subframes, padding and malformed lengths.
- Upstream AWDL protocol sanitizer test, all 14 Python tests and 201 layout
  entries pass unchanged.
- 18 complete rejected hardware frames pass replay through the production ABI.
- Release build and kernel symbol/instruction checks pass.

The bundle keeps numeric version 1.0.1. The Wi-Fi-only archive is
`dist/MtkWirelessWifi-1.0.1-airdrop-ios27-amsdu.zip`. It excludes the unfinished
Continuity Camera/Bluetooth work. Replace the Wi-Fi kext in the actual boot
EFI with a backup, then reboot and repeat the iOS 27 -> Mac transfer to verify.
At packaging time the previously used FD14-LITE USB is not mounted/present;
no EFI files were replaced and the running driver is still the previous build.

After the user installed the archive and restarted, the running Wi-Fi UUID
was FA49826C-19FD-34E1-9FC2-EB6DE0642209, matching this build. Rust self-tests
and AWDL layout checks passed, but AirportMtkWlan stopped during startup
before publishing a network interface. Accessible logs do not retain the
exact failure reason. A full power removal was suggested to distinguish a
warm-restart controller state issue; its result and AirDrop verification are
still pending. Boot evidence is under
`diagnostics/wifi-start-20261026-115604/`.

Raw evidence and test logs are retained under ignored
`diagnostics/airdrop-latency-20261026-1146/`.
