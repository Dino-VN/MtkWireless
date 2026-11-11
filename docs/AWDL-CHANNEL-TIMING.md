# AWDL scheduling — 1.0.18 test build

This Wi-Fi-only test build adds selective recovery for schedule channels missed
by the greedy planner, and bounded HAL TX diagnostics. **Hardware verification
is pending; the large-file stall around the displayed 131 KB is not proved fixed.**
No MTU/offload/rate/ACK changes or simultaneous-channel enablement are included.

The running 1.0.17 image was matched to its supplied test binary on 2026-11-10.
Its PM-preparation correction was insufficient to fix the user's bulk upload.

## Selective recovery in 1.0.18

The existing greedy planner remains in charge while channels receive native
windows. A bounded history tracks distinct reachable schedule channels. A channel
with no native-window progress for a whole schedule cycle becomes eligible for
a fixed future recovery target; attempts rotate fairly among eligible channels.
There is no special case for channel 6 and no unconditional round-robin policy.

- A recovery intent is a plan, **not a radio grant**. It neither emits a native
  window nor authorizes TX. Useful intermediate work may continue if its original
  promised window ends before the recovery preparation edge.
- An open native window, a pending firmware response, and an original future or
  short grant remain protected. Only unused coalesced grant surplus may be
  relinquished, after its original window ends and recovery preparation is due.
- Session, AP/join context, schedule, intent and call identity are rechecked after
  callbacks or gate-releasing calls. A stale invocation cannot adopt a newer
  session's grant or overwrite its timer. Expired targets do not slide forever.
- A response that actually times out temporarily excludes its channel from
  speculative intermediate work for one schedule cycle. Its own recovery turn
  remains eligible; a successful grant clears the hold. Idle/unreachable-slot
  cleanup records a current-context timeout before cancellation clears its
  reservation, so an unanswered channel cannot evade this hold at that boundary.
  This does not cancel a still-pending response or globally disable the channel.
- Retained latency history is used only for recovery. The original estimator's
  other-channel aging, outlier resampling and greedy/off-mode behavior remain.
- A newly faster channel can leave a cold outlier in that original history.
  Repeated recovery would refresh the outlier before ordinary aging removes it.
  Recovery therefore allows settling only when both the current-context history
  and the legacy history are fresh, have multiple samples, and put the latest
  latency sample more than 8 ms below their retained lead. This does not change
  either estimator or discard samples. The grace ends after eight schedule cycles
  without native progress; repeated observations and polling cannot extend it.
  It postpones recovery selection, not an already-owned native window or grant.

`amtk_awdl_forward=0` disables selective recovery for an A/B test after reboot;
leave boot arguments unchanged for the first 1.0.18 hardware test. This is not a
new DBDC/channel-context mode or a promise of higher throughput.

`MTKAWDLForward*` on `AirportMtkAwdlInterface` records enabled/active state,
selected/expired/invalidated counts, preemptions, observed response timeouts,
intent ID and the last target/request/expiry timestamps. `ForwardIssued` counts
**calls into `awdlReserve`**, including refused calls, not firmware submissions.
Use the correlated ROC trace's issued flag/time for actual request evidence.
`ForwardSettlingTicks` counts planning ticks on which at least one eligible
channel received the settling grace, not attempts, time or missed windows.
Target fields can retain a completed intent: consult `ForwardActive`.

The new [HAL TX trace](AWDL-TX-TRACE.md) distinguishes host submission, real
firmware TX-free, raw TXS observations and unobserved reclaim. Safety bookkeeping
covers all occupants of the shared token pool, even when no AWDL ticket is
recorded. Old reports after known-unsafe reuse are never counted as new ACKs.

Country reporting is now consistent and refuses unapplied country changes, but
**`00` / `Unknown` remains**: this driver has no real regulatory-domain setter or
verified country source. See [Country code and Location](WIFI.md#country-code-and-location).

## Preserved PM preparation and safeguards

The scheduler now wakes at the PM preparation edge (request deadline minus
3,000 us), then at the request deadline. Previously a 2,000-us poll could cross
that preparation edge and submit PM=1 less than 2,000 us before the request.
The HAL correctly refused that request, adding a retry even with no timer jitter.
Clipping the poll removes this avoidable phase error.

The HAL still requires **2,000 us after successful Null PM=1 submission** before
an associated off-channel ROC. This is a transmission allowance, not verification
of an AP ACK. Late ticks or failed PM submissions still defer the request. The
3,000-us preparation lead leaves only 1,000 us for timer/work/submission delay.

Unchanged: the 300-ms off-channel cap; PM=0 wake/10-ms preparation grace;
scan/join/unfinished-RSN exclusions; matching token/channel/grant admission;
abort ownership and token quarantine; native AW pairing; the final 2-ms TX guard;
per-channel lead estimation and same-channel grant reuse. Station/BA maintenance
has not been moved behind planning's early returns.

If firmware resets while reservation submission releases the controller gate,
partial timing is retained and a new tick performs transport-reset cleanup. The
old invocation cannot adopt stale HAL state or rearm a replacement native session.

## Firmware capability observations

On `AirportMtkWlan`:

- `MTKWiFiPHYCapabilitiesValid`, `MTKWiFiPHYCapabilities` (12 raw bytes),
  `MTKWiFiPHYDBDC`, `MTKWiFiPHYHWPath`, `MTKWiFiPHYRawNSS` retain the NIC reply.
  Raw NSS is separate from the existing effective clamp to four chains.
- `MTKWiFiFirmwareFeaturesKnown`, `MTKWiFiFirmwareFeaturesStatus`,
  `MTKWiFiFirmwareFeatures`, `MTKWiFiFirmwareFeatureSegment`,
  `MTKWiFiFirmwareCNM` describe optional metadata in the image successfully
  downloaded by this driver. `Known=false` is not proof of no capability.
  Metadata parse errors do not block otherwise valid firmware loading.

Both bundled WM RAM images contain segment 2 / flags `0xc0`, including CNM bit
`0x80`. Linux v6.12 uses CNM to advertise **two logical channel contexts** in
certain interface combinations. That is not proof of simultaneous RF, nor proof
that our P2P-discovery AWDL context can use those combinations. DBDC remains a raw
firmware observation. No band index, interface role, channel policy or multichannel
enablement changes here.

Reference: Linux v6.12 `mt792x_get_offload_capability` in
[mt792x_core.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/mediatek/mt76/mt792x_core.c).

## Correlated host trace

On `AirportMtkAwdlInterface`, `MTKAWDLROCTraceVersion=1`,
`MTKAWDLROCTraceCount`, `MTKAWDLROCTrace` and `MTKAWDLROCTraceLast` retain the
last 32 reservation calls. They survive sync-off, and the controller watchdog
publishes them after the AWDL timer stops. This is a bounded sample, not a full
capture. Sort by nonzero `id`; the ring index is `(id-1) % 32`.

Each little-endian record is 192 bytes, defined and size-checked by
`include/AWDL/AwdlRocTrace.hpp`. Its field order is:

- 19 × `uint64_t`: `id`, `session`, `serviceAtUS`, `planAtUS`, `requestAtUS`,
  `idealAtUS`, `callAtUS`, `returnAtUS`, `issuedAtUS`, `queueReturnedAtUS`,
  `grantAtUS`, `pmAtUS`, `readyDelayUS`, `targetStartUS`, `targetEndUS`,
  `firstStartUS`, `firstStartDispatchUS`, `firstEndUS`, `firstEndDispatchUS`.
- 8 × `uint32_t`: `epoch`, `leadUS`, `durationMS`, `status`, `queueStatus`,
  `invalidReason`, `starts`, `ends`.
- `uint16_t counter`; four bytes `token`, `channel`, `infraChannel`, `flags`;
  two reserved zero bytes.

Flags: 1 = call returned, 2 = request issued, 4 = lifecycle changed during call.
`status` is IOReturn (`0xffffffff` before return); `queueStatus` is the send
helper's errno and is meaningful only with flag 2. Zero optional timestamps mean
unobserved, not zero latency. A refused call cannot inherit an older request.
Session/epoch/token/issue identity keeps stale completions out of newer records.

Interpretation:

- `planAtUS-serviceAtUS`: gated service work before planning, including station
  maintenance. It excludes `service()`'s earlier TX-status/completion drains.
- `callAtUS-idealAtUS` (signed): request lateness versus the selected channel's
  lead. `requestAtUS` is the planner's already-clamped edge; by itself it hides
  deadlines missed before planning began.
- `issuedAtUS-pmAtUS`, when PM exists: submission age when the ROC is issued.
- `queueReturnedAtUS-issuedAtUS`: host send-helper duration. Its return is only
  an **upper bound** on actual enqueue, not a firmware or DMA timestamp.
- `readyDelayUS = max(returnAtUS, grantAtUS)-callAtUS`: host request readiness,
  counting an early grant during a blocked send only once. Diagnostic only;
  this does not change the lead estimator.
- `firstStartUS` / `firstEndUS`: effective native timestamps for the **first**
  window on that grant. Dispatch fields are fresh host observations before
  callbacks, so delayed delivery is visible. `starts` / `ends` count all windows
  served by the grant; subsequent windows do not overwrite the first pair.

Existing HAL latency histograms and new `MTKAWDLROCPMBlocked` distinguish PM-age
refusals from firmware grant delays. HAL `MTKAWDLROCTimingSession/Epoch/Token`,
`MTKAWDLROCIssuedAtUS`, `MTKAWDLROCQueueReturnedAtUS` and
`MTKAWDLROCPMSubmittedAtUS` describe the last issued request, not every refused call.

## Hardware test

1. Back up the currently working Wi-Fi kext. Replace **only**
   `MtkWirelessWifi.kext` in the EFI actually used to boot, then reboot. Leave
   Bluetooth and boot arguments unchanged. The bundle version should be 1.0.18.
2. Stay associated to the usual Wi-Fi AP. For the bulk-stall reproduction, start
   `scripts/Collect-AirDrop-AWDL.command`, then send **one large file from Mac to
   iPhone**. Leave that attempt running through the stall/error; do not start
   another transfer or change network settings during the capture. Record the
   file size, direction, progress shown, completion/error and AP connectivity.
   Test the opposite direction in a separate capture, not in the same run.
3. The collector now runs for 180 seconds by default (optional argument 30–600),
   long enough for the observed approximately 122-second upload timeout. It saves
   paired frontend/HAL plist samples, before/after interface state, explicit-time
   process logs and `awdl0` IPv6 packets. `clocks.jsonl` brackets each sample with
   wall/monotonic observations and records the clock implementation. Do not derive
   wall time by adding raw driver timestamps to the boot date. No other interface's
   traffic is captured, even if AirDrop selects an Ethernet candidate.
4. Keep the diagnostics directory local: it contains private packets and device
   information. The output directory is unique and private; capture files are
   pre-created as the invoking user so their private permissions do not prevent
   later inspection. Check `window.txt` and `tcpdump.log` for capture errors.
   If the AP link regresses, restore the backed-up Wi-Fi kext and reboot.

## Bulk-stall evidence boundary (2026-11-10)

A small 4,255-byte item completed over AWDL. A later bulk attempt completed
HELLO/ASK and an MTU-1484 probe, then failed after approximately 122 seconds with
no upload response. The logs do not expose 131 KB as uniquely delivered file
bytes. A separate `EMSGSIZE` failure selected Ethernet; it does not justify
changing AWDL MTU. Cumulative counters and a bounded RX tail are not a complete
per-transfer trace.

The retained ROC ring has three channel-6 grants arriving approximately 71 ms
**after the entire target window ended**. Native-window admission and late
planning reproduce these missed windows on the host. However, the retained bulk
RX data is on channel 149: correcting channel-6 coverage has not yet been shown
to correct the bulk-transfer failure. A host model with fixed firmware latency
cannot prove that earlier submission will advance a real grant by the same amount.

Do not relax replay protection, change checksumming/MTU, or replace rate-feedback
semantics on this evidence. Those paths were investigated without establishing
this stall's root cause. Further tests must correlate actual request/grant/native
windows and TX/RX observations with a single attempt's acknowledged progress.

## Offline validation

`scripts/test-awdl-forward-admission.py` runs the extracted production scheduler
with fresh Wi-Fi FFI, paired baseline/selective runs of equal duration, and a
retention-only negative control in a copied Rust workspace. It exercises recovery
fairness, healthy schedules, original reservation protection and callback/context
changes. Unchanged native-window assertions run with recovery enabled and disabled;
a separate copied-header control removes only settling and must fail the original
cold-outlier assertion. Neither control modifies production inputs. Additional
24-cycle sparse runs cover idle and unreachable slots at two schedule phases,
checking timeout observation before cancellation and repeated healthy-channel
service while another channel never responds. The new assertion reproduced the
missing-observation failure before the correction; the real off mode is retained
as a baseline, not credited with recovery it does not provide.

A sparse slow-first/fast-later host companion still serves fewer channel-149
native windows with recovery than with recovery disabled. Settling prevents a
larger loss; it does not make every schedule baseline-equivalent. These are
host-model native-window observations, **not transfer-speed or RF measurements**.
The final candidate's test record lists the executed results and coverage tradeoffs,
and retains upstream extraction failures rather than calling every test a pass.

The preparation regression runs the extracted production deadline block with
real Rust helpers, including the old block as a failing negative control.
The trace/lifecycle companions exercise host observations and callback models,
including one-shot timer consumption, HAL-only reset during reserve, and cleanup
without an external wake. Removing the reset rearm is a failing negative control.
They do not establish actual RF retune time, AP ACK receipt or private native
runtime behavior. Existing raw upstream extractors are kept unchanged even when
their older handwritten mocks no longer compile against current production.
