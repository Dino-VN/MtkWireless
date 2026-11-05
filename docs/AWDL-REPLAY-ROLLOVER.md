# AWDL replay rollover with a nonzero sequence prefix

## Failure

An iOS 27 iPhone Mirroring receive stream (QoS TID 5) advanced the AWDL replay high-water mark to `0x2fff`, then sent increasing sequence values `0x219e..0x2450`. Version 1.0.9 treats any upper bits as evidence of a 16-bit counter. It therefore rejects the resumed traffic as too old until the stream expires.

The affected capture reported 512,202 too-old drops. The sanitized regression fixture contains only the 500 captured `(sequence, previous, bitmap)` samples; no MAC addresses, device identifiers, payloads or keys.

## Change in 1.0.10

Learn a low-12-bit rollover near the existing boundary when the upper nibble stays the same. Retain the full sequence value so different domains are not aliased. Continue using full-width arithmetic for `0x2fff -> 0x3000` and `0xffff -> 0x0000` transitions.

Commit the candidate modulus only when advancing the accepted high-water mark. A rejected or reordered foreign-domain packet must not switch an established 12-bit window to 16-bit arithmetic.

The per-peer/per-TID windows, 64-packet duplicate bitmap and idle expiry are retained. No C++/Rust layout, radio scheduling, firmware, Bluetooth, membership or cryptographic validation changes are included.

## Regression coverage

- Nonzero-domain rollover and repeated rollover in all 16 upper-nibble domains.
- Reordered packets around a rollover, duplicate rejection and foreign-domain rejection.
- Genuine full-width counters and idle expiry, including the existing tests.
- All 500 sanitized capture samples are accepted once, then rejected as duplicates.

## Hardware result and limits

The 1.0.10 Wi-Fi build was installed on a macOS 26.7.2 test laptop. A 180-second sample received 60,786 AWDL frames with two duplicate drops and zero too-old drops. Motion was visibly smoother according to the tester. Residual missing video packets and occasional corrupted frames remain; this fix does not claim to make the radio link lossless.

This is an AWDL receive fix. It does not supply the T2/Apple silicon authentication required by native iPhone Mirroring; the test setup used a separate research authentication bridge.

## Validation on the rebased branch

Rust workspace tests (including all 64 `mtk-wlan` tests), Wi-Fi/BT FFI tests and the Release kext build pass. Kernel symbol/FPU checks pass and all 213 shared C++/Rust layout entries match.

The full `scripts/test-awdl.sh` suite is not green on this host: ASan binaries hit SIGILL, and `test-mi-discovery.py` references a missing `inputNativeView` stub method. With a local UBSan-only wrapper, 12 of 14 tests pass; the two remaining failures match the unmodified v1.0.9 baseline (`test-log-snapshot.py` SIGILL and the missing discovery stub). Existing upstream rustfmt differences are left outside this fix.
