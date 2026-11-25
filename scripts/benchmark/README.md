# AirDrop benchmark

Run on the sending Mac from a local Terminal, with Wi-Fi and Bluetooth hardware available. The launcher uses Python 3 and the macOS Command Line Tools. Allow Accessibility for Terminal in System Settings → Privacy & Security so the benchmark can cancel a stuck transfer. Keep the receiving device discoverable, select the same recipient in the macOS AirDrop window for each trial, and accept the transfer on that device if prompted.

```sh
scripts/benchmark/AirDrop-Benchmark.command
```

To select the same device automatically in each trial, give its exact AirDrop label and allow Accessibility for Terminal in System Settings → Privacy & Security. The tool uses the native AirDrop picker and acts only on one matching device button. A duplicate label ends the trial. The receiver still needs to accept when macOS or iOS requests it.

```sh
scripts/benchmark/AirDrop-Benchmark.command --recipient 'iPhone của Dino' \
  --recipient-timeout 15 --connect-timeout 30 --stall-timeout 15 --timeout 300
```

These are the default limits. Recipient timeout applies to finding the device in automatic mode. Connect timeout covers the time from selecting the device to seeing outgoing bulk data, including any wait for acceptance. In manual mode it also covers time spent selecting in the picker. The transfer limit is measured from bulk startup. If bulk TX stops progressing for 15 seconds while macOS is still waiting for completion, the trial ends early with `transfer_stalled`. If capture on the last TX interface has exited, the result is `transfer_unobservable`. During a transfer the benchmark presses the active recipient tile to cancel and observes the tile leaving its active state or AppKit ending the session. It saves the cancellation result and stops the suite if the end remains unconfirmed. Use `--stall-timeout 0` to let macOS continue until it returns a result or reaches the transfer limit.

With live macOS logs, the benchmark waits for `sharingd` to start the upload before starting the transfer timer or using packets to trigger a Wi-Fi action. By default, only AWDL capture can drive packet-based decisions. The Wi-Fi interface capture remains available for diagnosis. A confirmed delivery requires the receiver's upload response, the matching upload end, and a successful send metric for the exact file size before cancellation. The benchmark closes the picker on confirmation and uses the upload timestamps to calculate file throughput. An AppKit completion without matching delivery evidence is recorded as `delivery_unconfirmed`.

The tool asks you to confirm **before interrupting network access**. It uses sudo to take physical Ethernet interfaces down and capture packets. It uses AppKit to send the file and CoreWLAN to read and control Wi-Fi. A trial sends the same 64 MiB random file by default. Give it more time on fast links with `--mib 256`. You can pass `--file /path/to/file` to send your own file.

| Default trial | What it measures |
|---|---|
| `wifi` | AirDrop while associated with the current access point |
| `wifi-scan` | AirDrop while macOS runs a Wi-Fi scan |
| `wifi-disconnect` | AirDrop while leaving the access point with the radio still on |
| `direct` | AirDrop with the radio on and no access-point association |

The scan and disconnect start after the capture sees outgoing bulk traffic on AWDL. If the transfer finishes before the action runs, or only travels over a LAN interface, the action may be reported as `not_triggered`. The report records whether the action trigger agrees with the analyzed flow. Keep other AirDrop transfers idle during the benchmark. Use `--interface` to explicitly choose a different capture and trigger interface.

For additional comparisons, repeat `--scenario` to select the cases and their order. The band-specific cases prompt you to choose the matching network in the macOS Wi-Fi menu when needed. The optional `wifi-radio-off` turns off the radio mid-transfer and records how AirDrop ends.

```sh
scripts/benchmark/AirDrop-Benchmark.command --mib 256 --runs 2 \
  --scenario wifi-2.4 --scenario wifi-5 --scenario direct
scripts/benchmark/AirDrop-Benchmark.command --mib 256 --scenario wifi-radio-off
```

The tool checks for Ethernet during each run and attempts to restore the original Ethernet interfaces and Wi-Fi state after completion, cancellation or interruption. Wi-Fi rejoin uses saved macOS settings. If the original AP does not rejoin, use the Wi-Fi menu. If the process is force-killed or restoration fails, see `network-recovery.txt` in the output folder.

## Results

The console prints the path under `diagnostics/airdrop-benchmark-*/`. Open `overview.md` for the result, AP channel, connection and startup times, total elapsed time, speed, and P95 and maximum TX gap for each trial. The overview reports sharing and network conditions separately. The detailed outputs for each trial are:

- `report.md` and `summary.json` with phases, active TX speed, min/mean/P50/P95/P99/max packet gaps, the longest pauses, and the gap around the Wi-Fi action.
- `packets.csv` with a row for each attributable unicast TCP/UDP packet, its direction, timestamps, payload size and gap from the previous packet for that flow and direction.
- `timeline.html` and `timeline.csv` with per-second payload throughput and the placement of the Wi-Fi action. The chart includes zero-traffic seconds through the end of the capture window.
- `wifi.jsonl` with one-second macOS observations of the access point's channel, band, width, RSSI, and PHY rate when available. The native snapshots, packet captures, and logs use the local wall clock for correlation.
- `flows.csv`, the header-limited packet captures, `sharing-events.jsonl`, `action.log` when applicable, and a filtered macOS `system.log`.
- `system-live.log` streams macOS AirDrop, networking and Wi-Fi logs from before the picker opens through trial cleanup. `trial-events.jsonl` records bulk startup, one-second progress snapshots, Wi-Fi actions, stall detection and cancellation. `cancellation-events.jsonl` records the native dialog action when attempted. `native_ended` means AppKit reported a terminal result during cancellation. Only delivery evidence from before the cancel request can supersede a stall or timeout.
- `awdl0-routes.log` records native macOS routing messages for `awdl0` while the trial runs. Compare their timestamps with QUIC path changes in `system-live.log`.

The overall file rate includes time spent selecting and accepting the transfer. In automatic mode, the detailed report also shows picker-to-selection, selection-to-first-flow-packet, and selection-to-bulk time. The last interval includes any wait for receiver acceptance and uses observed transport traffic as its endpoint. The active rate is calculated from captured transport payload, including headers inside encrypted transport and retransmissions. Gaps are intervals at the sending Mac's capture interface. Use independent receiver or radio measurements for one-way delay, over-the-air retries, or QUIC RTT. Traffic attribution uses the dominant captured flow as a candidate. If a transfer is ambiguous or changes flows during a trial, inspect `flows.csv` and reanalyze the relevant flows individually.

```sh
scripts/benchmark/AirDrop-Benchmark.command --analyze /path/to/one/trial --flow 2
```

Results can contain IP and MAC addresses and device information in macOS logs. The benchmark saves the files locally with private permissions.

Stall detection follows observed outgoing payload to the peer that triggered startup. An early stall requires at least 512 KiB of captured payload, or a quarter of the file for smaller transfers. Once captured TX reaches the file size, the benchmark waits for the macOS result or the overall transfer deadline. Each additional batch of up to 16 KiB resets the idle timer. Small control packets and inbound ACK traffic alone cannot keep resetting the timer. Repeated TCP sequence ranges are ignored once bulk tracking starts. Encrypted UDP retransmissions can still look like TX progress, and the sender capture cannot confirm how much data reached the receiver.
