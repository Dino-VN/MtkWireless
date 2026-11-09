# Wi-Fi / AirDrop performance testing

`capture-wifi-performance.py` collects read-only, key-free driver telemetry
while **you** run a transfer. It neither generates traffic nor measures a
speedup. Baseline source: `main` at `3bb3ced`, `VERSION` 1.0.12. The collector
records the **loaded** MtkWireless Wi-Fi and Bluetooth versions, not the
version of a checkout or an archive.

## Capture

From the repository root, with Python 3 (standard library only):

```sh
python3 -B scripts/capture-wifi-performance.py
python3 -B scripts/capture-wifi-performance.py --duration 60 --interval 1
```

Defaults are 30 seconds and a nominal one-second start-to-start interval.
Both accept positive finite fractional seconds. Requests exceeding 10,000
nominal observations (`ceil(duration / interval) + 1`, including initial/final
observations) are rejected before queries or output creation. This is a
conservative sample-budget policy, not a byte-memory guarantee: samples remain
in memory and may contain multiple nodes; slow queries usually reduce their
number. Captures are never silently shortened or samples discarded.
Default output is a **new, unique** `build/diagnostics/wifi-performance-<UTC time>-<random suffix>/`
directory, already covered by `.gitignore`. To choose a directory:

```sh
python3 -B scripts/capture-wifi-performance.py --duration 60 --interval 1 \
  --output build/diagnostics/wifi-performance-before-mac-to-phone-01
```

An explicit directory must not exist; the collector will not overwrite
an earlier capture. Directory permissions are owner-only when created.
Ctrl-C saves completed samples and marks the summary `interrupted`.

No administrator privileges are needed. Do not add elevated privileges to
work around a failed query: unavailable tools, permission failures, missing
modules/interfaces and malformed output are reported without raw error
text. There is no dependency installation, listener, packet capture, log
stream, kernel logging, preference/boot-argument change, rate/bandwidth/
offload forcing, kext installation or reboot.

## Repeatable before / after procedure

1. Keep the devices, location, AP configuration, test data and other traffic
   the same. Keep Wi-Fi association and AirDrop state consistent between
   comparisons. Record those conditions privately; do not put SSIDs,
   addresses, personal device names or credentials in capture labels.
2. Use a separate capture for each direction and workload. Start it a few
   seconds before the manual transfer, and leave a short idle tail. Increase
   `--duration` if the complete transfer will not fit in the default window.
3. Repeat each workload at least three times before and after the separately
   managed candidate change. Use fresh neutral directory names such as
   `wifi-performance-before-phone-to-mac-01` and
   `wifi-performance-after-phone-to-mac-01`. Check loaded versions in each
   summary; the source tree version alone is not evidence of a running build.
4. Record only numeric results and a neutral direction/workload label in
   your comparison notes: payload bytes, transfer elapsed seconds, completion
   or failure, and the LAN iperf receiver result if used. Compare the same
   definitions and all repetitions, not just the fastest run. This collector
   makes no before/after improvement claim.

### AirDrop, both directions

Run Mac → phone and phone → Mac manually with the same non-sensitive data.
Record payload byte size and elapsed time using the same start/end events.
Record overall UI completion separately if it includes discovery, acceptance,
file moving/opening or other post-transfer work. An idle completion dialog is
not proof that the radio transfer stalled. A failed/partial transfer has no
successful delivered-file throughput result.

For a completed transfer, `payload_bytes / transfer_seconds` gives average
payload bytes/second; divide by 1,000,000 for MB/s, or multiply by 8 and divide
by 1,000,000 for Mbps. Use 1,048,576 instead if explicitly reporting MiB/s.
This is an external transfer measurement, **not** a collector-derived rate.
See [AIRDROP-PERFORMANCE.md](AIRDROP-PERFORMANCE.md) for historical RX,
aggregation, rate-ladder and coexistence evidence.

### Infrastructure Wi-Fi with your own LAN iperf server

If you already use iperf3, use an **already running server you own on your
LAN**. Run the collector in one terminal and the following clients yourself
in another; replace `LAN-SERVER` locally. Neither iperf nor a server is
installed or started by the collector. No public-host speed test is needed.

```sh
# Mac sends to your LAN server:
iperf3 -c LAN-SERVER -t 30
# Your LAN server sends to the Mac (separate capture):
iperf3 -c LAN-SERVER -t 30 -R
```

Keep the iperf version, duration and options identical for comparisons. Use
its receiver throughput and actual reported interval, not its sender rate
alone. Do not save raw iperf output in a key-free capture folder: it may
contain addresses. Copy only the numeric measurement into separate neutral
notes. A LAN test includes AP contention and the server/path bottlenecks; it
is not automatically a measurement of AWDL or AirDrop capacity.

## Files and interpretation

- **`samples.json`**: schema version 1 and timestamped, whitelisted samples.
  Each has UTC wall time, monotonic elapsed seconds, query duration and two
  sources: `infrastructure` and `awdl`. Queries are sequential, not atomic.
- **`samples.csv`**: long-form rows with the same timestamps, source/status,
  anonymous node number, metric, kind and value. Missing sources or nodes
  without valid metrics get a status row with blank metric/value fields.
  Missing metric values are not zero-filled.
- **`summary.json`**: first/last completed samples; requested timing, actual
  sample-start interval min/mean/max and query durations; loaded module
  observations at the beginning/end; per-node first/last metric observations,
  missing counts, counter deltas and reset/gap information. `sample_span_seconds`
  spans sample starts; `capture_elapsed_seconds` ends at the last completed
  query, excluding version queries and any interrupted incomplete query.

Per-counter `counter_delta` sums **only adjacent, nondecreasing observations
of the same node**. `delta_intervals` says how many intervals contributed.
A decrease is treated as a reset/wrap: its increment is unknown and excluded,
never negative. `resets` counts observed decreases (also across a gap), not
all possible resets. `unobserved_intervals` counts intervals with either
endpoint missing; they are not bridged. Hidden resets cannot be detected.
No comparable interval means `counter_delta: null`; an observed unchanged
counter gives zero. Never-observed metrics are listed in `unobserved_metrics`
and have null first/last values when a node exists. Node disappearance,
re-enumeration, failed queries or missing publications are not zero traffic.

Anonymous node numbers are stable only within a capture, using registry IDs
transiently in memory. Different controllers/re-enumerated nodes are never
merged; registry IDs/names/paths are not written. Without a registry ID each
observation receives a fresh node number, so its counters are not linked.
Do not compare anonymous node numbers across separate runs.

`throughput_bytes_per_second` is always null in this version: the inspected
publishers expose **no genuine cumulative delivered-byte counter**. Packet,
ACK, submission and replay counters describe events, not delivered file bytes.
`MTKAWDLTXPeer0RateMbps` through `MTKAWDLTXPeer2RateMbps` are retained per-slot
**AWDL TX ladder settings**, not infrastructure PHY measurements or application
throughput. An idle ladder rate is never active link capacity. Stream counts,
queue depth, statuses, channels, flags and maximum delays are gauges, not
cumulative counters; no deltas are calculated for them.

The infrastructure source includes native UMI timeout, cancellation, stale-TXS,
exhaustion and backpressure counters, plus active/terminal gauges.
`MTKAWDLUMILastReason` is a gauge: 0 none, 1 real ACK, 2 negative TXS,
3 local timeout, 4 cancellation, 5 tuple exhaustion, 6 table full,
7 identity/history unavailable, 8 submission rollback. Timeout and cancellation
are not on-air success, and none of these values measures delivered bytes.

Many AWDL/HAL properties publish at most once per second while their path
runs and can remain stale after it stops. Sampling below one second adds
queries without guaranteeing fresher data. Slow queries skip missed ticks;
the last query may finish after `--duration`. Missing AWDL nodes are normal
before native AWDL creates its interface. Version status `not_observed` or a
query failure does not prove a module is unloaded. Versions are not polled
between the two endpoint queries.

## Whitelist and privacy boundary

The explicit `COUNTERS`, `GAUGES` and `BOOLEANS` tables in the script are the
complete metric whitelist, checked against these publishers:

- `AirportMtkWlan.cpp`: power/state/capability observations and native link
  flags; AWDL enable/creation availability.
- `MtkMwx.cpp`: publishes on **AirportMtkWlan**, not the HAL's own node;
  Wi-Fi RX event counts, infrastructure power-save handshakes, ROC timing/
  failures/admission, AWDL TX feedback/rate slots and RX BA sessions.
- `AirportMtkAwdlInterface.cpp`: native/data RX/TX counts, rejection and replay
  counts, BA events, queue/stream/peer counts, scheduler/presence state and
  current offload flags (read, never changed).

The collector queries `ioreg -a -r -c AirportMtkWlan` and separately
`ioreg -a -r -c AirportMtkAwdlInterface`, following only registry child entries
and accepting only the matching classes' exact, typed integer/boolean
properties. `ioreg` has no scalar-property-only plist mode, so class-scoped
plists necessarily contain other data transiently in memory. They are never
saved or printed; only the whitelist is extracted. Join dictionaries and
other nested property dictionaries are not traversed. Histograms/arrays,
keys, SSIDs, MAC/BSSID/IP/peer addresses, payload/frame prefixes, raw traces
and arbitrary `MTK*` properties are excluded, even if numeric-looking.

Loaded-version queries use only `kmutil showloaded --list-only --show loaded
--bundle-identifier` for `com.dinovn.MtkWirelessWifi` and
`com.dinovn.MtkWirelessBT`. Only numeric bundle versions from matching loaded
rows are retained, never other modules, UUIDs, kernel addresses, stdout/stderr
or raw failure messages. No private user files or interface-address tools
are consulted.

## Offline regression tests

```sh
python3 -B scripts/test-wifi-performance.py
```

Tests use only the standard library, fake clocks and mocked plist/subprocess
results. Unmocked commands are forbidden. They exercise privacy/typing,
child traversal, separate AWDL roots, anonymous node continuity, missing/
invalid output, counter resets/gaps, CLI validation, timing, unique output,
no overwrites and side-effect-free import. They also check whitelist names
against the three repository publishers. They do **not** invoke real ioreg,
kmutil, iperf or hardware. Passing offline tests is not evidence of a measured
throughput improvement.

## Source audit on 2026-11-09

The working-tree corrections are correctness fixes, not measured speedups:

- Native AWDL IPv6 retry tags are inserted only after a successful Skywalk
  enqueue return. Allocation/preparation/enqueue errors leave that cache
  unchanged. The extraction models rejection before ownership transfer; the
  private native error/ownership contract remains unproved. This success-only
  cache is not an in-flight reservation. Decoded-attempt counters are still
  not delivered-byte counters.
- Canonical AWDL replay now has an allocation-only preflight. A failed or
  NULL allocation does not consume a prospectively accepted sequence. Genuine
  replay rejection still refreshes its ordinary `lastRx` and trace without
  allocation. After allocation, authoritative admission rechecks generation,
  readiness, originating resources, MAC, transmitter membership and the current
  subframe destination; nested authoritative decisions invalidate older
  candidates. Preparation, decode and enqueue failures **still consume replay**.
  This is not a full native delivery transaction or a proven Skywalk teardown
  exclusion contract, and no replay snapshot rollback was added.
- DELBA uses its complete four-bit wire TID; unsupported TIDs 8–15 no longer
  alias recipient sessions 0–7. Queue admission also requires the original
  30-byte frame: a missing/partial reason code is no longer zero-padded into
  a valid-looking teardown. The existing originator/direction and action
  envelope checks remain. Shared pending-queue overflow is now published as
  `MTKAWDLDELBADropped`; this makes lost admission observable, not lossless.
- WCL compatibility fallback requires a positively classified PSK join.
  Open/WEP/Enterprise joins no longer lose HE/PMF merely because their BSSID
  is locally administered or their SSID matches a prior PSK stall. SAE
  admission reads baseline PMF capability, not a prior PSK profile's temporary
  clear. Actual restoration follows the final admission guards, before RSN
  configuration; a busy/deferred request cannot restore an in-flight profile.
  Immediate SCAN-start failure also restores its abandoned profile. Originally
  disabled PMF stays disabled, and SAE remains excluded from fallback.
- Infrastructure RX/TX Block Ack programming requires firmware status zero
  for both STA_REC/WTBL updates. Nonzero status is an error, not a successful
  agreement just because the command wait completed. Transport errors,
  deferred admission, command ordering and generic MCU callers are unchanged.
- The controller no longer advertises magic-packet wake based on PCI PME.
  Enabling an unimplemented Wi-Fi wake path returns unsupported; disabling
  it remains idempotent. PCI initialization and sleep/wake ordering are not
  changed.
- Native UMI metadata no longer remains occupied indefinitely when TXS is
  missing. One-second timeout produces a local failure; lifecycle cancellation
  discards obsolete results. Polling removes the raw completion-callback
  lifetime dependency. Wire tuples are never recycled within the loaded
  module, so delayed TXS cannot complete a new reservation using the same
  tuple. This recovery is finite, with explicit tuple-exhaustion backpressure;
  it does not establish a device-reset or module-reload fence. Failed peer
  removal preserves active and already-recorded completions; cancellation
  follows a successful firmware peer update.

### Performance limits and missing/partial features

| Area | Source-established status / remaining work |
| --- | --- |
| Infrastructure Wi-Fi | Firmware rate adaptation and TX/RX A-MPDU exist. Offered HT/VHT/HE profiles support up to two streams and 80 MHz; actual negotiated mode and throughput require hardware measurements. Hardware TX A-MSDU is disabled; 160 MHz and MU-MIMO are not enabled profiles. |
| PHY capabilities | HT/VHT short GI and HE LDPC are offered. HT/VHT LDPC, STBC and beamforming are not offered; enabling bits alone is not a safe optimization. |
| Locally administered PSK BSSIDs | The existing hotspot compatibility experiment can still select a legacy profile (`amtk_hotspot_level`, default 3). A local BSSID does not prove the AP is an iPhone. This audit preserves PSK levels/defaults rather than claiming the Apple-IE fix proves every modern PHY profile safe. |
| AWDL / AirDrop TX | The current ladder tops out at HT MCS7, 20 MHz, one stream, long GI (65 Mbps PHY). TX A-MPDU/A-MSDU and negotiated VHT/multiple-stream/wider-channel profiles remain work, not an advertised application rate. |
| AWDL / AirDrop RX | Firmware recipient BA sessions support TIDs 0–7 and windows up to 64. Whole-frame/A-MSDU, MAC/BSSID, exact IPv6-length/MTU and replay validation remain mandatory. |
| Canonical AWDL RX delivery | Allocation failure/NULL no longer consumes a prospectively accepted sequence. A pure probe precedes allocation, then fresh generation/readiness/resources/MAC/membership/destination checks precede authoritative admission; recursive replay decisions cancel older candidates. Preparation/decode/enqueue failures still consume canonical replay. Retains protect object lifetime, not private Skywalk finalize/stop exclusion; non-NULL error-return ownership remains unestablished. A full delivery transaction remains deferred. No snapshot rollback or validation-after-enqueue workaround was added. |
| AWDL BA admission | Four shared ADDBA/DELBA slots remain best-effort. Overflow is counted, but reliable teardown intent needs session-generation-aware ordering/coalescing; firmware failures are not retried by this queue. |
| Native UMI lifecycle | Bounded containment now expires active metadata after one second and cancels it on lifecycle changes. Terminal results are polled rather than delivered through a raw interface callback. A module-lifetime 48 KiB tuple history prevents late TXS from matching a reused metadata slot; each WCID/12-bit-sequence permits only 32 issued PIDs before explicit refusal. History is not cleared on reset, peer removal or reattachment. This is finite recovery, not indefinite recycling; no hardware/module-reload fence is established. Timeout is local failure, never an ACK, and metadata retirement does not reclaim DMA ownership. Hardware verification remains pending. |
| 6 GHz | Experimental/off by default. SAE H2E and OWE are missing, so compliant production 6 GHz security is incomplete; regulatory/CLC/RNR work and hardware validation remain. Plain MT7921 has no 6 GHz radio; chip support must still follow the family tables. |
| Roaming / security | Open/PSK full-reassociation roaming, Enterprise EAPOL transport and SAE hunting-and-pecking/PMF paths exist, but broader hardware verification is pending. This is not 802.11r/k/v fast roaming. |
| Other offloads / modes | No functional hardware checksum/TSO or WoWLAN path was established. AP/Internet Sharing setters remain unsupported. Software networking assists are separate. |

For throughput work, first collect completed, bidirectional infrastructure
and AirDrop measurements with the procedure above. The larger AWDL TX change
needs peer QoS/HT/VHT/MCS/NSS/bandwidth/GI capability intersection, an originator
BA handshake and matching firmware records before aggregation/rate expansion.
Do not force capabilities, remove replay validation or bypass the AP power-save
handshake to increase a displayed rate. Native TX copy reduction is a future
CPU optimization candidate, not an established bottleneck.

Focused production-extraction regressions (ASan/UBSan) are local to `scripts/`;
upstream AWDL tests are intentionally unchanged:

```sh
python3 -I -B scripts/test-native-mirroring-rx.py
python3 -I -B scripts/test-awdl-native-tx-reservations.py
python3 -I -B scripts/test-awdl-ba.py
python3 -I -B scripts/test-wifi-ba.py
python3 -I -B scripts/test-wifi-policy.py
python3 -I -B scripts/test-wifi-scan.py
```

Relevant implementation history and hardware limitations are in
[AIRDROP-PERFORMANCE.md](AIRDROP-PERFORMANCE.md),
[WIFI-6GHZ.md](WIFI-6GHZ.md), [ROAMING.md](ROAMING.md) and
[WIFI-ENTERPRISE.md](WIFI-ENTERPRISE.md).

## Validation on 2026-11-09

- All 28 offline collector tests pass, including differential summary and
  operation-count regressions. Aggregation now visits observed entries once
  instead of rescanning every sample for each historical node/metric.
- At the earlier audit checkpoint, all 13 focused host scripts passed: Wi-Fi
  capabilities, EAPOL, Hotspot, roaming, RX, timing, scan, policy, infrastructure
  BA, native Mirroring RX, AWDL BA, cache diagnostics and RX trace. This predates
  the follow-up UMI/RX changes, not a final current-tree matrix. Logs and the
  result index are under `build/diagnostics/wifi-audit-final-host.zpx2zua1/`.
- Native UMI reservation and full-production peer-removal extraction pass
  ASan/UBSan, including seven allocation/transport/firmware failures preserving
  Active attempts and recorded ACKs, scoped replacement, epoch/session guards
  and delayed TXS. Restoring premature cancellation in memory fails the negative
  control. Evidence: `diagnostics/awdl-peer-removal-20261109T161204-3g4vjqvm/`.
  Its supplementary native RX/AWDL BA runs used cached FFI only; they are not
  fresh validation of the concurrently edited RX source.
- At that earlier checkpoint, the Rust workspace passed 117 unit tests;
  separate FFI Wi-Fi and Bluetooth feature runs passed four and one tests
  respectively. These were separate builds, not a combined Wi-Fi/Bluetooth
  binary; they do not validate the later RX changes.
- The earlier `scripts/build.sh Release` succeeded for the separate Wi-Fi and
  Bluetooth kexts. Kernel checks resolved all 1,129 Wi-Fi and 372 Bluetooth
  undefined symbols and passed SSE/AVX/x87 instruction checks. Both Rust builds
  emitted the core future-incompatibility warning. This build predates the
  follow-up UMI/RX corrections. Evidence is under
  `build/diagnostics/wifi-audit-final-build.20261109T151044.1w3xb4xl/`.
- At the earlier checkpoint the unchanged upstream AWDL suite matched all 213
  layout entries and passed its sanitized protocol test plus 11/13 Python
  fixtures. The suite exited 1: `test-mi-discovery.py` lacked cache diagnostic
  stubs and had a void/IOReturn controller-message mismatch;
  `test-partial-init.py` lacked TX-completion/lock stubs. Both also failed in
  baseline validation. Full compiler output is retained in the host-log
  directory. The later follow-up has additional fixture failures listed below.
- An earlier separate read-only smoke capture (`--duration 2 --interval 1`)
  recorded three samples, found both registry classes, and observed loaded
  Wi-Fi 1.0.15 and Bluetooth 1.0.13 at both endpoints. This checks query-format
  compatibility on this host only, not the later offline summary refactor.

### Canonical RX allocation-only checkpoint

- The expanded production extraction with real Rust FFI passed 11 ASan/UBSan
  test groups: allocation error/NULL and success/NULL, retry, ordinary duplicate
  refresh, nested acceptance/rejection, replay streams/rollover, mutation and
  lifecycle invalidation, origin cleanup, aggregate validation and the
  intentionally consumed preparation/decode/enqueue failures. The C++ extraction
  was sanitized; the linked Rust host library was not sanitizer-instrumented.
- Separate offline WLAN and FFI Wi-Fi runs passed 70 and six tests respectively.
  Native RX trace, AWDL BA and native UMI reservation scripts also passed.
  Full native AWDL translation-unit syntax checking passed,
  all 213 layout entries matched, and the sanitized upstream protocol passed.
- `scripts/test-awdl.sh` still exited 1: seven upstream Python fixtures passed
  and six failed extraction compilation (`mi-discovery`, `native-controls`,
  `native-start`, `native-windows`, `partial-init`, `sync-transition`). The local
  `scripts/test-awdl-cache-diagnostics.py` also failed extraction compilation
  because its mocks lacked the command gate/gated worker. These additional
  failures are not all established baseline failures; reconciliation is pending.
  The upstream fixture files remain unchanged.
- Commands, statuses and full logs:
  `build/diagnostics/awdl-rx-allocation-20261109T092338Z-_c93aqq4/commands.jsonl`.
  Latest successful extraction: `native-rx-03.log` in the same directory.
  This checkpoint did not run a Release link/check or hardware validation;
  independent verification and a final Release build are pending.

### Current-tree offline matrix (10:26–10:28 UTC)

- Fresh, separate diagnostic Cargo targets passed 120 workspace tests
  (BT 44/common 4/FFI 1/kernel 1/WLAN 70), six FFI Wi-Fi tests and one FFI
  Bluetooth test. Wi-Fi and Bluetooth were never linked into one binary.
- Fourteen of 15 focused Python scripts passed, including all 28 collector
  tests. The local cache-diagnostic script failed with three compilation
  errors: `_fCommandGate` twice and `peerCacheGated` once. The unchanged
  upstream runner again exited 1: 213 layout entries matched, the protocol
  passed, and seven of 13 Python fixtures passed; the same six failed
  compilation. Nothing was skipped or relabeled as baseline.
- Native RX built a previously absent diagnostic host FFI archive and passed
  its 11 ASan/UBSan groups. Capabilities, AWDL BA, cache diagnostics and the
  upstream runner reused the existing `build/rust-host/debug/libmtk_ffi.a`;
  those archive builds are not claimed as fresh validation. The C++ sanitizer
  run does not imply that Rust or private native routines were instrumented.
- All 328 fingerprinted source/test files retained identical SHA-256 and
  mtimes, including the 18 dirty production/script files. Documentation was
  excluded from the snapshot because it was updated independently.
- Exact argv, timestamps, exit codes, full logs, archive fingerprints and
  per-fixture outputs are indexed in
  `build/diagnostics/offline-rx-matrix-20261109T102331Z-8z7jq4xw/command-index.json`.
  This matrix did not run a Release build or hardware/throughput test.

### Cache fixture repair and coverage follow-up

The local cache diagnostic fixture now extracts the complete production setter,
gated worker and mutation/programming helper chain, with real Rust DataPeers.
Its explicit gate model releases and restores recursive depth during a modeled
firmware wait; it does not establish private native scheduling or ownership.

- The repair passed 11 ASan/UBSan groups and four assertion-based in-memory
  negative controls, independently reproduced. A separate offline Cargo build
  proved both mtk-ffi and mtk-wlan rebuilt rather than reusing artifacts.
  Evidence: `build/diagnostics/awdl-cache-20261109T111705Z-87km8hgf/` and
  `build/diagnostics/awdl-cache-independent-20261109T112323Z-t6v4gsnz/`.
- Independent mutation checks exposed two coverage gaps, not additional
  production defects: traffic-ownership generation invalidation and the
  programming helper's own address copy during a wait. Both assertions are now
  incorporated. The expanded fixture passes 13 ASan/UBSan groups and six
  negative controls; each mutant fails its designated assertion without a
  sanitizer error. Cargo validated the current offline Wi-Fi dependency graph;
  this follow-up did not require a fresh rebuild.
  Evidence: `build/diagnostics/awdl-cache-20261109T122612Z-lfogveap/`.
- Upstream fixtures remain unchanged. Repairing this local script does not
  repair or relabel the six upstream extraction compilation failures.

### Lifecycle companion and packed-sequence correction

The new local `scripts/test-awdl-native-lifecycle.py` companion passes all nine
strict ASan/UBSan scenarios, independently rerun. It exercises startup failures,
lifecycle/control transitions, recursive gate waits, admission generations,
originating-resource cleanup, HAL result/session boundaries, BSD methods and
packed sequence initialization. Its three host programs retain
`-fno-sanitize-recover=all`; recovery/suppression is not used to obtain a pass.

Actual Tahoe translation-unit record-layout and object compilation established
that `apple80211_channel` has alignment 4 and stride 12, while its packed
sequence has alignment 1. In that source-compiled interface, sequence
descriptors are at offsets `46633 + 12*i` (modulo 4 = 1). The host superclass
model differs but reproduces that misalignment. The initialization loop now
accesses fields through the packed container rather than binding a naturally
aligned descriptor reference. This corrects C++ undefined behavior without
changing declarations, packing, wire layout, versions or unused bytes; it is
not evidence of a hardware crash or of private superclass ABI correctness.

- A byte-exact 400-byte regression verifies intended channel/flag changes only.
  Restoring the old reference in deliberately misaligned storage fails with
  the expected UBSan reference-binding diagnostic.
- All four BSD helper bodies execute with assertions covering attach/clear/
  replace, assist reporting and clearing, preparation options, native return
  statuses, failure/readback handling and diagnostics. Four separate body-entry
  negative controls prove those methods are reached.
- All 13 unchanged upstream extractors reran: seven pass and the same six fail
  compilation. The overall script correctly exits 1 despite its nine clean
  companion scenarios; it does not make the upstream suite green.
- The modeled `awdlEnable` supplies its own epoch behavior, so it does not
  establish production HAL quiesce/epoch correctness. Private ownership/copy/
  stop/reentrancy contracts remain unproved. Rust is not sanitizer-instrumented;
  there is no TSan or kernel/hardware execution.

Evidence: `build/diagnostics/awdl-native-lifecycle-finish-20261109T121941Z-9wpuh29g/`
and independent rerun
`build/diagnostics/awdl-native-lifecycle-20261109T122647Z-fg64ovsf/`.
Protected source/upstream files and archive provenance remained unchanged during
the verification runs. Final Release validation is recorded below.

### Final stable-source Release validation

The final matrix and Release build passed their applicable checks on the same
source snapshot: 335 tracked/untracked source files were unchanged before and
after execution (documentation excluded).

- Fresh separate Cargo targets: workspace 120 tests, FFI Wi-Fi six, FFI
  Bluetooth one; zero failures. The GPL-separated kext features were never
  combined into one binary.
- Native RX, UMI reservations, AWDL BA, infrastructure BA, Wi-Fi policy, scan,
  performance (28 tests) and cache (13 groups/six negative controls) passed.
- All nine strict lifecycle companion scenarios passed; its overall invocation
  still exited 1 because six unchanged upstream extractors failed compilation.
- The unchanged AWDL runner also exited 1: all 213 layouts matched, protocol
  passed, seven upstream extractors passed and six failed. Failures remain
  `mi-discovery`, `native-controls`, `native-start`, `native-windows`,
  `partial-init` and `sync-transition`; this is not a fully green test suite.
- `scripts/build.sh Release` exited 0, including its final kernel checks.
  An independent `scripts/check.sh Release` also exited 0. All 1,129 Wi-Fi and
  372 Bluetooth undefined symbols resolved; both kexts passed SSE/AVX/x87
  checks. The expected core soft-float/SSE future-incompatibility warning
  appeared once per Rust kext build.
- Outputs: `build/Release/MtkWirelessWifi.kext` and
  `build/Release/MtkWirelessBT.kext`, with matching dSYM bundles. They have not
  been installed or loaded by this validation; no hardware or throughput
  improvement is established.

Exact commands, statuses, timings, logs, source snapshots, generated-output
inventory and artifact hashes:
`build/diagnostics/stable-release-20261109-192908/command-index.json` and its
containing directory. Full upstream fixture logs are retained under
`build/diagnostics/awdl-native-lifecycle-20261109T123007Z-ruvme11e/`.

### Independent RX verification

Independent source and production-extraction verification found no additional
source-established defect in the canonical allocation-only RX correction. It
confirmed the local cache-test compilation regression: the fixture still
extracted only the public setter and omitted its new gated transaction/helper
chain. The local cache fixture has since been repaired and independently
verified; the supplemental lifecycle companion now has nine strict clean
scenarios, with its separate upstream failures retained as described above.
The second fixture-reconciliation worker returned no result after an API error;
that branch is not counted as completed verification.

The RX verifier linked the pre-existing Wi-Fi Rust archive and independently
inspected the live pure-probe implementation. Its harness did not execute full
native start/init/free, the action receive arm or the service loop, and its
firmware-wait mock did not release the gate for real cross-thread scheduling.
Those limits are not evidence of native exclusion or complete lifecycle
coverage. Diagnostic reproductions: `build/diagnostics/awdl-rx-review.eU1Nmr/`.

Injecting identical native-tag RX recursively at allocation/preparation/enqueue
produced two deliveries in the extracted model: a success-only cache is not an
in-flight reservation. The private Skywalk reentrancy needed to reach this
condition on hardware remains unproved, so this is a conditional concern, not
an established runtime regression or a claim that native duplicate handling is
fully transactional.

A read-only attempt to inspect this host's native Skywalk RX implementation
obtained no method bodies: `dyld_info` aborted on the kernel collection and
Hopper startup failed with `Cannot launch Hopper! Connection is invalid.`
The retained records in `diagnostics/native-rx-contract-20261109-QDKvA6/`
document unresolved questions, not ownership or non-reentrancy guarantees.
REA reports no open target but pending cleanup; authenticated Hopper closure
was not confirmed. No provider setup or installation was attempted.

No file transfer or iperf test ran; throughput remained null. Other hosts
and macOS builds still need their own capture, and actual before/after
performance requires completed transfers. No candidate kext was installed
and no reboot was performed by this audit.
