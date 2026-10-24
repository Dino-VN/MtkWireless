# Instant Hotspot

## Observed failure

On macOS 26.7.1 (25G241), MT7921, MtkWireless 1.0.0, the iPhone appears
under Personal Hotspot in the Wi-Fi menu. Clicking Dino fails. The
2026-10-24 21:13:13 reproduction shows sharingd successfully activating the
iPhone hotspot and returning its network credentials and channel 149.
The next step fails: airportd's Apple80211Scan returns -3903 within
6–52 microseconds, retries, then the tether join times out with -3905.
Ordinary network-list scans still complete.

## Scan contract and 1.0.1 change

The installed Apple IO80211 userspace library checks CARD_CAPABILITIES
bit 73 (byte 9, mask 0x02) to encode SCAN_SSID_LIST in modern IOC 10
SCAN_REQ (0x1598 bytes). Without that bit it chooses legacy IOC 86, then
rejects the request because legacy multiple-scan capability bit 22 is
absent. 1.0.1 advertises bit 73; bit 22 remains clear.

The native kernel's IO80211ScanRequest::getScanRequestForDriver preserves
the list in the 0x1550-byte driver-facing request:

| Field | Offset / size |
|---|---|
| Primary SSID length / bytes | 0x1c / 0x20, up to 32 bytes |
| Channel count / entries | 0x54 / 0x58, 12 bytes per entry |
| SSID list count / entries | 0x1318 / 0x131c, up to ten 56-byte entries |
| List entry | version u32, length u32, SSID[32], remaining 16 bytes |

Named entries use version 1. Apple also appends an empty wildcard entry
with version 0. Rust parses and validates the list with mtk_common::Reader,
deduplicates the primary SSID, and preserves the wildcard. The HAL sends
up to four primary and six extended SSIDs to MediaTek firmware v1. A
named list sets ssid_type=4; ssid_type_ext is 0 when wildcard probing is
also requested, otherwise 1. Passive requests clear all named targets.
Existing channel restrictions and AWDL scan preemption remain in effect.

These offsets and the capability selection were checked against the
installed Apple binaries. A local helper exercised the userspace capability
gate only; it could not bind a live driver and is not a successful hardware
scan. Kernel image provenance and reproduction logs are retained locally
in diagnostics/hotspot-20261024-211313 (ignored by Git).

## Validation and remaining hardware check

The regressions exercise the production capability method, Rust parser,
WCL decoder, firmware target encoder, and scan ownership transitions:

```sh
python3 scripts/test-wifi-capabilities.py
python3 scripts/test-wifi-scan.py
python3 scripts/test-wifi-rx.py
(cd rust && cargo test --workspace)
(cd rust && cargo test -p mtk-ffi --features wifi && cargo test -p mtk-ffi --features bt)
scripts/test-awdl.sh
scripts/build.sh Release
```

After booting 1.0.1, click Dino while Personal Hotspot is initially off.
Confirm that the hotspot activates, the directed scan no longer returns
-3903, Wi-Fi associates, DHCP completes, and traffic uses the hotspot.
The driver scan log now includes primary/extended SSID counts and
ssid_type_ext without printing SSID contents or credentials.

End-to-end operation and Family Sharing behavior still require hardware
verification. Activation success in this reproduction does not establish
that every same-account or Family Sharing configuration works.
