# HAL AWDL TX trace v1

Read-only `MTKAWDLTXTrace*` properties on the Wi-Fi controller. The existing
watchdog -> AWDL cache -> HAL diagnostic publisher refreshes them even after
native sync is off. No new ioctl, boot argument, radio policy, rate/ACK change,
UMI completion, queue action or DMA ownership change is involved.

## What the evidence means

- Ticket/outcome scope: ordinary AWDL unicast data, submitting peer WCID 15–17
  and software PID 125. Infrastructure, multicast, action and native UMI TX
  generate no submitting tickets or ACK/failure outcomes. Shared-pool safety
  bookkeeping covers **all** token occupants; identity-free idle/unattributed
  reports and token-taint counts are not measurements of AWDL traffic.
- Each attempt's actual `mwx_txwi` owns a **ticket**, captured from the cached
  TXWI the HAL just built. Its submission time is immediately before the
  successful DMA doorbell, after the last admission failure. Mapping/admission
  failures have no submission timestamp and are not counted as submitted.
- `Outstanding` counts live host TXWI tickets submitted but not reclaimed.
  DMA-ring consumption does **not** complete them. Increasing count/oldest age
  distinguishes tokens awaiting firmware TX-free from completed traffic; it is
  not a measurement of airtime or proof that a frame went over the air.
- Only attributable **real TX-free** reports increment `Acked`/`Failed`.
  Status 0 means firmware-reported success; other statuses mean failure.
  `Transmissions` is the sum of reported nonzero counts; `Retries` sums
  `count - 1` for those reports. Count 0 is **unknown**, not zero transmissions.
  Latency is host doorbell-to-TX-free-observation time, including firmware
  queueing and host event delivery. It is not per-attempt RF retry timing.
- `requestedRate` is TXD6, never an actual transmitted rate. The separate raw
  TXS ring retains reported bits, error bits, format, WCID, sequence and PID.
  Its session/incarnation/channel fields describe the **observer at receipt**.
  TXS carries no token/session/incarnation; even a matching sequence/WCID/PID
  must not be labeled authoritative submission identity. Formats 2/3 are PPDU
  records, not MPDU ACKs; raw rate bits are retained without interpreting them.
- A late TX-free for an unreclaimed old ticket retains that ticket's old
  session/epoch/incarnation and is marked not-current. It cannot affect RA or
  the replacement peer. Current/not-current counters are cumulative outcomes
  at observation time, not counters reset for the latest session.
- Sync-off/reset does not erase history or decrement pending counts. A later
  plain reclaim records **unobserved completion**, not firmware success.
  A per-token in-flight witness is set at successful DMA admission for **every**
  shared-pool occupant, not just traced AWDL unicast, and cleared on its actual
  TX-free. Reclaim without that free permanently makes the token unsafe for
  diagnostic attribution for this HAL attachment. Mapping/admission failure
  before the doorbell does not create a witness or poison otherwise clean reuse.
  After unsafe reuse, a raw free gets an identity-free row and the new AWDL
  ticket gets an unobserved-reclaim row. DMA still reuses it normally.
  Any detected in-range idle report also makes the token unsafe, even before
  its first AWDL use. A clean untraced completion does not taint the token.
  Saved diagnostic tickets cannot clear a newer actual HAL object's witness.
- **Limit:** TX-free supplies only a token. A duplicate delayed past an otherwise
  normally completed token's reuse is indistinguishable on the wire. Normal
  attributable results rely on the firmware's existing token-ownership contract;
  no device-status fence is claimed. Known unobserved reclaims fail closed.

## Publication and bounds

`Available` distinguishes allocation failure from an empty trace. `Faults == 0`
is required to interpret gauges/counters as complete tracking. Nonzero faults
latch diagnostic-only failure; IDs never wrap, counters saturate, and no new
submission identities are issued. No diagnostic failure blocks actual TX.
`AtUS` is the host monotonic microsecond snapshot time. Properties are separate
registry copies; `Summary` contains the counts/time for its coherent snapshot.
Counters/IDs persist through disable/reset and end at HAL detach, not reboot-
persistent storage. Take before/after snapshots to isolate a single attempt.

The heap object is 41,312 bytes, including a separate watchdog snapshot, plus
56 bytes per TXWI (at most 512). No per-packet allocation; no large stack arrays.
Packet hooks do constant bounded work. Watchdog copying/scanning is bounded to
512 token slots, 128 events and 64 TXS records. No frame payload, MAC address,
SSID, key, pointer or DMA address is retained.

- `MTKAWDLTXTraceVersion`: 1.
- `MTKAWDLTXTraceCount` / `...TXSCount`: monotonic event/raw-TXS IDs.
- `MTKAWDLTXTrace`: 128 × 96-byte event rows (12,288 bytes).
- `MTKAWDLTXTraceTXS`: 64 × 88-byte raw observation rows (5,632 bytes).
- `MTKAWDLTXTraceSummary`: 424 bytes, defined below.
- `...Oldest` / `...OldestCurrent`: 56-byte token snapshots. Ticket ID 0 means
  absent; otherwise the lowest outstanding ticket ID in that scope.
- Scalar mirrors include `Outstanding`, `CurrentOutstanding`,
  `AmbiguousOutstanding`, `OldestAgeUS`, `Submitted`, `Acked`, `Failed`,
  `NotCurrent`, `Unattributed`, `UnobservedReclaim`, `Transmissions`, `Retries`,
  `CountUnknown` and `MaxLatencyUS` (all prefixed `MTKAWDLTXTrace`).

All binary integers are **little-endian**, unsigned, at the offsets below.
Reserved bytes are zero. Nonzero row ID `n` belongs in slot `(n-1) % capacity`;
read retained nonzero rows sorted by ID, not physical array order. Completions
append new event IDs containing their original ticket IDs, even if their submit
row was evicted. They never edit a historical/overwritten submit slot.

### Token (56 bytes)

| Offset | Type | Field |
|---:|---|---|
| 0, 8, 16, 24 | u64 each | ticket ID, submitting session, peer incarnation, submittedAtUS |
| 32, 36 | u32 each | HAL epoch, frame length |
| 40, 42, 44, 46 | u16 each | token index, WCID, 802.11 sequence, **requested** rate bits |
| 48, 49, 50, 51 | u8 each | channel, PID, state, token flags |
| 52 | 4 bytes | reserved |

State: 0 empty, 1 prepared, 2 submitted. Token flags: 1 sequence known,
2 requested rate known, 4 channel known, 8 submitting context known,
16 ambiguous token, 32 unassigned free observed, 64 length known.
Zero sequence/rate/epoch can be legitimate; consult flags. Time 0 is unknown.
Incarnations advance only on successful peer commits/removals, never failed
MCU commands, and never reset with the rate ladder or session.

### Event (96 bytes)

| Offset | Type | Field |
|---:|---|---|
| 0, 8, 16 | u64 each | event ID, observation time US, completion latency US |
| 24 | Token (56 bytes) | submitting ticket; identity-free rows retain only token index |
| 80, 84 | u32 each | real TX-free raw word, host failure errno |
| 88 | u16 | real TX-free transmission count |
| 90, 91 | u8 each | firmware status, event kind |
| 92 | u32 | record flags |

Kind: 1 submit, 2 submission failure, 3 attributable TX-free, 4 unobserved
reclaim, 5 unattributed raw TX-free. Firmware status **0xff = unobserved**.
An abandoned prepared ticket has host errno `0xffffffff` (unknown).
Flags: 1 real free present, 2 free attributable, 4 submitting context current,
8 latency known, 16 transmission count known, 32 session changed/unknown,
64 epoch changed, 128 incarnation changed/unknown, 256 context inactive,
512 tracking unavailable, 1024 unsafe token. Current context and unsafe token
are independent: a new current submission can occupy an ambiguous token.

### Raw TXS observation (88 bytes)

| Offset | Type | Field |
|---:|---|---|
| 0, 8, 16, 24 | u64 each | raw observation ID, time US, **observer** session, **observer** incarnation |
| 32 | 8 × u32 | raw TXS dwords (MCU TX_DONE's absent eighth word remains zero) |
| 64, 68 | u32 each | observer HAL epoch, raw error mask (`word0 & 0x70000`) |
| 72, 74, 76 | u16 each | raw reported rate (`word0 & 0x3fff`), WCID, sequence |
| 78, 79, 80, 81 | u8 each | PID, format, observer flags, last observer ROC channel |
| 82 | 6 bytes | reserved |

Observer flags: 1 enabled, 2 stopping, 4 resetting, 8 detaching,
16 peer present, 32 interface running. Last ROC channel is not measured RF.
The Reader-based Rust parser validates all 32 bytes, including unaligned input;
short input clears the FFI output. It does not alter existing TXS adaptation.

### Summary (53 × u64 = 424 bytes)

Word indices (multiply by 8 for byte offsets):

- 0–5: version, snapshot time US, event count, ticket count, TXS count, faults.
- 6–8: observer session, observer HAL epoch, observer flags.
- 9–11: peer incarnations for WCIDs 15–17.
- 12–15: outstanding, current outstanding, ambiguous outstanding, prepared.
- 16–18: oldest age US, oldest-current age US, age-known mask (1 all, 2 current).
- 19–23: prepared attempts, submissions, submission failures, free ACKs, free failures.
- 24–27: current frees, not-current frees, unattributed frees, unobserved reclaims.
- 28–32: token taints, rejected metadata, skipped tracking, idle frees,
  out-of-range frees. Taints and idle/out-of-range reports cover the shared
  HAL pool, including untraced occupants; they are not proven AWDL transmissions.
- 33–35: transmissions, retries, unknown transmission counts.
- 36–39: known-latency samples, latency sum US, maximum latency US, invalid clocks.
- 40–43: context-end count, last context-end time US, prior session, prior epoch.
- 44: malformed TXS observations.
- 45–52: latency bins `<1 / <5 / <10 / <25 / <50 / <100 / <500 / >=500 ms`.

Fault bits: 1 counter saturation, 2 ticket IDs exhausted, 4 event IDs exhausted,
8 TXS IDs exhausted, 16 incarnation exhausted, 32 membership lost,
64 snapshot/token-membership mismatch. A zero gauge with faults is **not proof
of zero outstanding firmware work**. Ambiguous outstanding is a subset, not a
separate firmware queue. Prepared tickets have not reached the doorbell.

## Offline regression

`python3 -I -B scripts/test-awdl-tx-trace.py` creates a new evidence directory under
`build/diagnostics`, builds Wi-Fi FFI offline with the installed pinned toolchain,
and runs production-excerpt C++ ASan/UBSan and Rust parser tests. It covers ring
wrap/full 509-token pressure, real TX-free PAIR parsing, submission failures,
peer/session/epoch/token reuse, unobserved reclaim, current success controls,
raw TXS observer-only identity, saturation and disabled publication. The 32-case
shared-pool matrix covers infrastructure, multicast, action and native UMI,
with/without prior AWDL use: unobserved reset, observed idle duplicate, clean
completion and mapping failure before admission. OS locks,
registers, MCU transport, mbufs and time are explicit host models; Rust is not
sanitizer-instrumented. This is diagnostic validation, **not an AirDrop fix or
hardware verification**. The unrelated conditional stale-peer TXS adaptation
remains unchanged and is still reproduced by the test.
