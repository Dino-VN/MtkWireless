# Wi-Fi Skywalk test — 1.1.2

Experimental Wi-Fi-only build, 2026-11-17. No hardware speedup or stall fix is
claimed. The previous uncommitted scan-probe/firmware-candidate work was
explicitly discarded before this experiment.

## Changes

- Call the exported, non-virtual
  `IOSkywalkNetworkInterface::reportLinkQuality(unsigned)` in addition to the
  existing `setLQM` path. Keep the previous RSSI thresholds. Cache only successful
  reports, preserve quality during a held roam and report off on explicit
  link-down/stop, including a stop during a held roam.
- Use the existing gated 1-second watchdog for an RSSI-only WCL update every two
  ticks. Only an authorized RUN link with matching BSSID, link up and a running
  BSD interface may supply RSSI. The 0x1dc message keeps snapshot, beacon-counter
  and unavailable-measurement flags clear. No new virtual slot or timer.
- Request 2048 TX pool packets/buffers instead of 256. RX stays at 256; buffer
  size, pool type, submission/completion queues and the 256-frame protocol queue
  stay unchanged. Requested counts are not a measurement of Skywalk's actual
  rounded capacity. A larger pool does not repair a full protocol queue.
- Publish host-visible queue-pressure counters and capture network-path state
  and targeted Skywalk allocation/flow-control logs with the AWDL collector.
  Allocation failures upstream of our submission callback are NOT counted as
  driver-observed pool failures.

No firmware image, PHY rate, bandwidth, aggregation policy, scan/ROC ownership,
PM handshake, channel-switch timing, Bluetooth source or installed kext changes.
VHT80/2SS and firmware peer-queue pause are deliberately deferred to separate
experiments.

Design references, not transplanted Intel firmware commands:
[ForbiddenItlwm 8562a66 LQM](https://github.com/retropineaple/ForbiddenItlwm/blob/8562a661c0adb86a8b87e4b25a19911a4bd6f706/ForbiddenItlwm/WCL/WCL.cpp#L712),
[TX pool](https://github.com/retropineaple/ForbiddenItlwm/blob/8562a661c0adb86a8b87e4b25a19911a4bd6f706/ForbiddenItlwm/AirportItlwmV2.cpp#L800),
[network-path diagnostic](https://github.com/retropineaple/ForbiddenItlwm/blob/8562a661c0adb86a8b87e4b25a19911a4bd6f706/tools/compat/check_compat.sh#L469).

## Install and test

1. Back up the known-working Wi-Fi kext from the EFI actually used to boot.
2. Replace **only `MtkWirelessWifi.kext`**, keep Bluetooth and all existing boot
   arguments unchanged, then reboot. No additional boot arguments are needed.
3. Verify the loaded executable UUID against `BINARY-IDENTITY.txt` in the package;
   version alone does not establish binary identity.
4. Keep AP channel, phone Wi-Fi state, distance and test file constant. Test one
   large Mac-to-iPhone transfer, then a separate iPhone-to-Mac transfer. Record
   actual file completion, elapsed time, progress at any stall, and whether the
   AP link remains usable. Do not combine directions in one capture.
5. Run `scripts/Collect-AirDrop-AWDL.command 180` for each attempt. It prompts for
   sudo only for packet capture; it does not restart services or change settings.
   Capture files contain private data and must remain local unless reviewed.
   Its initial message says Mac-to-iPhone; use a separate run for the reverse
   direction and record that direction explicitly.

The packaged `scripts/` folder is self-contained for this collector; results go
into the package's `diagnostics/` directory. The full repo collector works too.

Read-only check, without sudo:

```sh
bash scripts/check-airdrop-network.sh
```

On the development host, before installing this experiment, the read-only check
reported `net.filter_state=pf`, `enable_netagent=0`, and LinkQuality 100 on en0 and
en1. This does not identify which is the active Wi-Fi route or establish a
firewall/OpenCore root cause. The new driver does not enable netagent, disable pf
or change OpenCore. Keep those settings fixed for the first driver A/B.

## Optional A/B controls — edit boot configuration separately, then reboot

- Default: Skywalk LQM/RSSI updates on, requested TX pool 2048.
- `amtk_lqm=0`: old setLQM-only path; no periodic RSSI-only updates.
- `amtk_awdl_txpool=256`: old requested pool size; RX unchanged.
- Both arguments together disable both experimental performance changes. The
  explicit stop-during-roam reporting correction and diagnostics remain.
- `amtk_awdl_txpool` accepts 256 through 8192; out-of-range values use 2048.

Change one control at a time to distinguish pool effects from LQM effects.
Rollback is the backed-up Wi-Fi kext plus the original boot arguments and reboot.
Never reset USB, re-enumerate Bluetooth, or kill bluetoothd for this experiment.

## Diagnostics

On AirportMtkWlan:

- `MTKWiFiSkywalkLQMEnabled`: selected LQM experiment.
- `MTKWiFiLinkQuality`: last successfully reported numeric metric, or the legacy
  metric when opted out; 32-bit signed -2 is the off value.
- `MTKWiFiLinkQualityResult` / `Attempts`: last exported-call result and count;
  failed calls are not cached and are retried.
- `MTKWiFiLQMRSSIPosts` / `LastRSSI`: RSSI-post attempts and last observed dBm,
  not an acknowledgement that WCL consumed the post.

On AirportMtkAwdlInterface:

- `MTKAWDL{Tx,Rx}PoolRequestedPackets`: requested factory counts only.
- `MTKAWDLNativeTxQueueFullDrops`: submitted packets rejected while the active
  protocol queue was full, distinct from disabled-path rejection.
- `MTKAWDLNativeTxCompletionBackpressure`: callbacks with a remaining batch
  blocked by the 128 pending-completion limit, not dropped-packet count.
- `MTKAWDLNativeTxQueueHighWater` / `CompletionHighWater`: maxima observed at
  native submission, not continuous maxima of every enqueue path.

The collector saves `network-path-before/after.txt` and `skywalk-pressure.log`.
Empty OS logs do not prove that no allocation failure happened.

## Offline validation

Build: `MODULE_VERSION=1.1.2 bash scripts/build.sh Release`. VERSION remains
numeric and unchanged in source. The test archive contains Wi-Fi only even
though the standard build script also rebuilds BT.

Tests include:

```sh
python3 -I scripts/test-wifi-skywalk-lqm.py
python3 -I scripts/test-airdrop-network.py
python3 -I scripts/test-wifi-roam.py
python3 -I scripts/test-wifi-hotspot.py
python3 -I scripts/test-wifi-hal-safety.py
python3 -I scripts/test-wifi-capabilities.py
python3 -I scripts/test-awdl-collector.py
python3 -I scripts/test-awdl-roc-preparation.py
python3 -I scripts/test-awdl-service-deadlines.py
python3 -I scripts/test-awdl-sync-recovery.py
python3 -I -B scripts/test-awdl-native-lifecycle.py --fresh-ffi --compare-head-upstream
```

The lifecycle companion tests extract the actual creation/submission paths and
exercise pool defaults, all range boundaries, factory failures, pressure-counter
semantics and resource cleanup with C++ ASan/UBSan and a fresh Rust Wi-Fi FFI.
They model OS/firmware boundaries and cannot establish native/RF correctness.
The comparison run reports unchanged upstream extractor failures separately;
companion success does not make the entire upstream suite green.

Final offline run: Release build and kernel checks passed (1096 Wi-Fi imports
resolved; no SSE/x87 rejection). Focused LQM/network-path, roaming, Hotspot, HAL
safety, capability, collector, ROC preparation, service-deadline and sync-recovery
tests passed. All nine lifecycle companion scenarios passed, including the new
pool/pressure assertions, with no protected source changes during the run.

The lifecycle comparison command still exits 1: six unchanged upstream
extractors fail to compile (`mi-discovery`, `native-controls`, `native-start`,
`native-windows`, `partial-init`, `sync-transition`). The same six suites fail
with production .cpp reads redirected to HEAD; `native-start` additionally lacks
mocks for the new fields/boot argument. Those harnesses were not weakened or
reported as passing. This comparison is not a complete clean-HEAD build.
See `build/diagnostics/forbidden-ideas-{build,lifecycle-head}.log` for results.
