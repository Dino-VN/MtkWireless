# Large-file AirDrop stalls and throughput (2026-10-26)

The user reports roughly a minute for 10 MB from Mac to iPhone, and also
reproduced an iPhone-to-Mac stall with a 32 MB file during live sampling.
The running Wi-Fi driver is 1.0.1, UUID
`FA49826C-19FD-34E1-9FC2-EB6DE0642209`, including the earlier iOS 27
A-MSDU envelope fix. Its invalid-frame counter remains zero.

## Receive rollover

The initial snapshot has 5,508 too-old drops and 15 duplicates. During the
32 MB upload, accepted TID 0 AWDL sequences advance through 3691..3699.
The next retained burst contains sequences 447..458, with the old window's
high-water mark pinned at 4095. MPDU sequences advance from 2376..2380 to
2925..2932. Later new datagrams through 1351 are still rejected against 4095;
the too-old count ultimately reaches 6,995 (1,487 new drops during sampling).
The sampler retains only a 64-record ring,
so it misses some frames between the bursts, including the exact transition.
This is evidence of a 12-bit counter rollover during a continuous stream,
rather than malformed frames or merely a slow completion dialog.

1.0.3 learns 12-bit arithmetic when a stream near 4095 rolls into the lower
half of that range. A stream that supplies upper sequence bits retains
16-bit arithmetic; the historical TID 1 sequence 4096 remains supported.
The 64-MSDU bitmap, separate QoS/non-QoS domains, peer authorization and
three-second idle restart remain in place. A counter boundary can occur
partway through a file; there is no fixed file-size limit of 2–3 MB.

The inference is per stream: without explicit sequence-width negotiation,
a sufficiently delayed low sequence at the first boundary is ambiguous.
The observed continuous upload and pinned high-water mark establish the
need for the narrow-counter case; arbitrary backwards jumps away from the
boundary still fail. `MTKAWDLDataRx12BitStreams` and
`MTKAWDLDataRx16BitStreams` expose the learned modes. Rejection counters use
the selected modulus, while the version-1 trace CSV's `back` column remains
the raw 16-bit subtraction.

## Transmit throughput

The old AWDL TXWI path fixes every frame at legacy OFDM 6 Mbps. That is
0.75 MB/s before headers, contention, ACKs, retries and channel sharing.
It is a separate throughput ceiling even when no receive rollover occurs.

The first 1.0.3 build probed only 6/12/24 Mbps, and any single failed
report stepped down for 512 ACKs. The follow-up capture shows ~1 failure per
2,000 ACKs (the peer briefly off channel), so peer 0 flapped between 24 and
12 Mbps. The same capture shows the 64-frame AWDL queue; at 54 Mbps that is
~15 ms of airtime, while each off-channel ROC to channel 6 lasts ~220 ms.

The current 1.0.3 build:

- climbs OFDM 6/12/24/36/48/54 Mbps, then HT MCS 7 (65 Mbps, 20 MHz long
  GI, one stream), one rung per 32 ACKs. Boot-arg `amtk_awdl_ht=0` stops at
  54 Mbps.
- steps down only on a second failure within 128 ACKs, or on any failure
  while a new rung is unconfirmed (<32 ACKs). Holds start at 256 ACKs and
  double per failed probe (up to 16x); a confirmed probe resets them.
- compares TXS rate mode+index only; 128 reports at other rates in a row
  step down, so a rung the firmware never reports cannot stick.
- holds 256 queued AWDL frames instead of 64 (`kDataQueueSize`).

Discovery, BA actions and multicast remain at 6 Mbps. A-MPDU (BA) and
VHT/2-stream rates still need peer capability parsing.
`MTKAWDLTXPeer{0,1,2}RateMbps` report 6..65.

## Receive aggregation

Until this build every iPhone ADDBA request was declined (status 37), so
iPhone-to-Mac data arrived as single MPDUs, each waiting for its own ACK;
per-frame overhead then dominates whatever rate the iPhone picks. The
service loop now installs a firmware recipient session on the peer's AWDL
station (`MtkMwx::awdlRxBlockAck`: WTBL_BA + STA_REC_BA on the AWDL BSS, as
`mt7921_mcu_sta_rx_ba`) and answers status 0 with a window of at most 64
and the peer's A-MSDU bit. Without a station, or with boot-arg
`amtk_awdl_rxba=0`, it declines as before. An originator DELBA tears the
firmware session down. There is no reorder buffer: the AWDL MSDU replay
window admits out-of-order sequences within 64, so A-MPDU retries can
reach the stack reordered. Removing a station (membership expiry) drops
its session without a DELBA. ioreg: `MTKAWDLADDBAAccepted`,
`MTKAWDLRxBA{Started,Stopped,Failed}`.

## Presence during transfers

awdld keeps its idle sequence during AirDrop (2026-10-26:
`6,-,149,-,-,-,-,-,6,-,149,...`), present on the data channel in 2 of 16
EAWs, and peers send us data only in EAWs we advertise. Unassociated, each
channel switch also costs an 80-160 ms ROC grant; the capture moved
~50 KB/s. While a transfer runs (traffic registration, or at least 10 data
frames per 250 ms), `updatePresence` advertises and uses the data channel
in every idle EAW (`mtkawdl::denseSequence`); channel-6 and other host
slots stay. awdld's sequence returns 2 s after data stops. It is allowed
only unassociated or with the AP on the data channel
(`awdlDensePresenceAllowed`); boot-arg `amtk_awdl_dense=0` turns it off.
ioreg: `MTKAWDLDensePresence`, `MTKAWDLDensePresenceTransitions`,
`MTKAWDLTrafficActive` (awdld did not register traffic for AirDrop).

Measured on hardware (2026-10-26, AP on 149/80 MHz, iPhone also sending
the same file to another Mac): 245 MB iPhone -> Mac in ~62 s, ~4 MB/s
average, alternating ~5.5 and ~2.5 MB/s seconds, peak ~7 MB/s; 25 RX
Block Ack sessions, no DELBA, no firmware BA failure. A real Mac is still
several times faster. Not yet known: the PHY rate the iPhone uses toward
us (depends on the HT/VHT capabilities IO80211 advertises) and how much
the channel-6 excursions cost.

## Validation and installation

The regressions exercise 30,000 consecutive narrow-counter MSDUs with
duplicates across repeated rollovers, reordering at a rollover, missing
initial packets after rollover, and 70,000 full-width MSDUs. Rate tests
exercise promotion, failure backoff, stale completions, independent peers,
non-data status isolation and truncated status records.

Raw snapshots, the sampled-state ABI replay and build/test logs are under
ignored `diagnostics/airdrop-throughput-20261026/`. The replay restores each
captured pre-filter window separately; it is not a complete packet capture.
The initial 722 sampled records preserve 550 active-window accepts, recover
159 false old drops and retain the sampled duplicate rejection. Twelve
accepted reset cases need their idle state inferred because trace version 1
does not include last-RX time. Regressions separately test idle expiry.
All 54 mtk-wlan tests, workspace and Wi-Fi/BT FFI tests, 14 upstream Python
tests, the upstream protocol sanitizer test, 208 layout entries, RX trace
sanitizers, scan/RX admission regressions and Release kernel checks pass.

Install the Wi-Fi-only archive
`dist/MtkWirelessWifi-1.0.3-airdrop-dense.zip` by backing up and
replacing `MtkWirelessWifi.kext` in the EFI actually used to boot, then
reboot. It does not include a Bluetooth replacement. Repeat the same 32 MB
iPhone-to-Mac transfer and a 10 MB Mac-to-iPhone transfer, recording elapsed
time and confirming receive-all-bytes/completion in sharingd. The driver
changes require this hardware check before claiming a measured speedup.
The measured 1.0.3 Wi-Fi executable UUID is
`26946F19-4375-370A-B696-974674A9CF42` (earlier builds: RX Block Ack
`83186215-4716-3EF4-8A60-8E06A7714B14`, rate ladder
`05F53492-D98A-3450-9AD5-6AE0A30E59C5`) (the first 1.0.3 was
`64CE37DA-B9A1-3BA0-A64E-9BB4A8E7AF53`).

## ForbiddenItlwm comparison (2026-11-20, unverified on hardware)

ForbiddenItlwm 0.6.0 (Intel AWDL for macOS) reaches 20-26 MB/s both ways.
Its firmware time-shares a permanent second context, so it is not a design
we can copy (ours is ROC per window), but three of its findings apply:

- **TX packet pool.** A 128-packet `awdl0` TX pool (896 after Skywalk's
  rounding) ran dry at FQ-CoDel's peak (672 per flow + submission +
  completion); the flowswitch then drops the batch with ENOMEM ("failed to
  allocate slab") without telling QUIC, which stays flow controlled.
  Ours was 256 here; the 1.1.2 Skywalk work (docs/WIFI-SKYWALK-TEST.md,
  not on this branch) already requests 2048 with `amtk_awdl_txpool`, so
  this port leaves the pool alone.
- **PHY.** Its Mac -> iPhone speed comes from VHT80 two-stream A-MPDUs
  (`-itlawdlcoexbatx` + `-itlawdlcaps`); peer capabilities are assumed, not
  parsed. Ours topped out at HT MCS 7, 20 MHz, one stream (65 Mbps PHY).
  `amtk_awdl_phy` (default 1 since the 1.1.3 hardware test below):
  - 0: the old ladder and 20 MHz ROC.
  - 1: + VHT MCS 7/8 two-stream at 20 MHz (130/156 Mbps) in 5 GHz windows.
  - 2 (opt-in): + 80 MHz ROC on 36-48 / 149-161 (non-DFS, every 20 MHz channel
    enabled, VHT80 flag) and VHT MCS 4/7/9 two-stream at 80 MHz
    (351/585/780 Mbps). Associated, the block may not hold the AP's
    channel unless it is the AWDL channel at the AP's own 80 MHz.
  2.4 GHz windows use a separate ladder (HT at most), so channel-6 losses
  no longer demote the 5 GHz rate. Wide rungs match TXS by NSS and TXS
  bandwidth (bits 29..30) too. The TX ladder follows the granted window
  (`band2g`, `wide`). Linux's MT7921 ROC is always 20 MHz; three
  FirmwareStatus refusals of an 80 MHz acquire before any wide grant turn
  80 MHz off until AWDL is re-enabled. The peer WTBL gets WTBL_VHT for
  phy >= 1. ioreg on the controller: `MTKAWDLPhy`,
  `MTKAWDLROC{Wide,Narrow}Grants`, `MTKAWDLROCWideRejects`,
  `MTKAWDLROCWideOff`, `MTKAWDLWindowWide`; per-peer
  `MTKAWDLTXPeer*RateMbps` now reads up to 780. A/B: `amtk_awdl_phy=1`,
  then `0`. 1.0.16's "11ac/80 broke AirDrop" was the infrastructure knob,
  not AWDL; watch `MTKAWDLROCLatency*` for slower 80 MHz grants.
- **Scans during transfers.** A firmware scan holds the radio for its
  whole channel list and `awdlReserve` refuses every ROC meanwhile.
  While AWDL data moved within the last 3 s (TX status or, new, accepted
  RX: `MtkMwx::awdlNoteDataRx`), `mt7921_mcu_hw_scan` scans
  `amtk_scan_chunk` channels (default 4, 0 = whole list) per firmware
  request and leaves `amtk_scan_gap` ms (default 250) for AWDL between
  them; SCAN_DONE of the last chunk ends the logical scan. Joins (WCL
  channel or fallback) are never chunked. A fresh scan, stop/reset or the
  scan ending during a gap drops the rest (generation check in
  `mwx_scan_chunk_task`); a refused chunk ends the scan with what earlier
  chunks found. `deferScanForAWDL` still holds associated WCL scans while
  data flows (now in both directions); the one it lets through every 30 s
  is chunked. ioreg `MTKScanChunked`, `MTKScanChunks`,
  `MTKScanChunkAbandoned`, `MTKScanChunkSize`. Test:
  `scripts/test-wifi-scan-chunk.py`.

1.1.3 hardware (2026-11-20, AP "2.4 GHz ch 10", iPhone16,2, default
`amtk_awdl_phy=2`): AirDrop stuck at "Waiting". sharingd's application
service resolution timed out after 16 s. The firmware granted 91 80 MHz
ROCs on 149 (no refusal), yet every AWDL data frame received came on
channel 6, none on 149; only 5 unicast frames were queued and all expired
behind the peer gate (2336 NotReady). 1.1.4 makes 20 MHz the default and,
under `amtk_awdl_phy=2`, turns 80 MHz off for the AWDL session after 16
wide grants without one received AWDL frame. ioreg `MTKAWDLRxWide` /
`MTKAWDLRxNarrow5` count AWDL frames received inside 80 / 20 MHz 5 GHz
grants, `MTKAWDLWideAutoOff` the fallbacks.
