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
  `createDataPath()` now requests 2048 TX packets/buffers by default
  (`amtk_awdl_txpool` 256..8192; RX stays 256).
- **PHY.** Its Mac -> iPhone speed comes from VHT80 two-stream A-MPDUs
  (`-itlawdlcoexbatx` + `-itlawdlcaps`); peer capabilities are assumed, not
  parsed. Ours topped out at HT MCS 7, 20 MHz, one stream (65 Mbps PHY).
  `amtk_awdl_phy` (default 1):
  - 0: the old ladder and 20 MHz ROC.
  - 1 (default): + VHT MCS 7/8 two-stream at 20 MHz (130/156 Mbps) in 5 GHz
    windows with 20 MHz ROC.
  - 2 (opt-in): begin at 20 MHz. An active peer (via native traffic
    registration or recent unicast AWDL data within 5 s, including AirDrop)
    with a recent 80 MHz operating class and a VHT RX map supporting two
    streams at MCS 9 can enable 80 MHz ROC on 36-48 / 149-161 (non-DFS, every
    20 MHz channel enabled, VHT80 flag) and VHT MCS 4/7/9 two-stream at
    80 MHz (351/585/780 Mbps). On 2026-11-25 MT7922 hardware
    (`diagnostics/airdrop-benchmark-gpkoe571`), firmware accepted 116 80 MHz
    ROC grants on channel 149 (center 155, bw 2) but received 0 frames
    (`MTKAWDLRxWide=0`) and failed all 10 channel-149 TX frames after 15
    retries, while 20 MHz grants received 609 frames (`MTKAWDLRxNarrow5=609`).
    If opted into `amtk_awdl_phy=2`, 6 wide grants with 0 wide RX also trip
    `sc_awdl_wide_off` back to 20 MHz. Associated, the block may not hold the
    AP's channel unless it is the AWDL channel at the AP's own 80 MHz.
    The driver advertises the allowed width through a separate operating
    class schedule (TLV 18) alongside the VHT capability (TLV 17).
    `MTKAWDLVHT80Channels` reports the negotiated primary-channel bitmap.
  2.4 GHz windows use a separate ladder (HT at most), so channel-6 losses
  no longer demote the 5 GHz rate. Wide rungs match TXS by NSS and TXS
  bandwidth (bits 29..30) too. The 80 MHz TX ladder requires the firmware
  grant to report 80 MHz and the requested center. Linux's MT7921 ROC is
  always 20 MHz. Three
  FirmwareStatus refusals of an 80 MHz acquire before any wide grant turn
  80 MHz off until AWDL is re-enabled. The peer WTBL gets WTBL_VHT for
  phy >= 1. ioreg on the controller: `MTKAWDLPhy`,
  `MTKAWDLROC{Wide,Narrow}Grants`, `MTKAWDLROCWideRejects`,
  `MTKAWDLROCWideOff`, `MTKAWDLWindowWide`; per-peer
  `MTKAWDLTXPeer*RateMbps` now reads up to 780. 1.0.16's "11ac/80 broke
  AirDrop" was the infrastructure knob,
  not AWDL; watch `MTKAWDLROCLatency*` for slower 80 MHz grants.
- **Scans during transfers.** A firmware scan holds the radio for its
  whole channel list and `awdlReserve` refuses every ROC meanwhile.
  While AWDL is enabled, explicit WCL discovery scans are chunked even
  before a peer is visible. Other scans are chunked if AWDL data moved
  within the last 15 s (TX enqueue/status or accepted RX) or a valid peer action
  frame arrived within 3 s. In those cases,
  `mt7921_mcu_hw_scan` scans
  `amtk_scan_chunk` channels (default 4, 0 = whole list) per firmware
  request and leaves `amtk_scan_gap` ms (default 400) for AWDL between
  them; SCAN_DONE of the last chunk ends the logical scan. Joins (WCL
  channel or fallback) are never chunked. A fresh scan, stop/reset or the
  scan ending during a gap drops the rest (generation check in
  `mwx_scan_chunk_task`); a refused chunk ends the scan with what earlier
  chunks found. `deferScanForAWDL` holds associated WCL scans for 5 s after
  AWDL data activity (RX, TXS, or live-peer unicast enqueue); the one it lets
  through every 30 s is chunked. ioreg `MTKScanChunked`, `MTKScanChunks`,
  `MTKScanChunkAbandoned`, `MTKScanChunkSize`. Test:
  `scripts/test-wifi-scan-chunk.py`.

  On 1.1.6, an iPhone-to-Mac AirDrop session completed PairVerify at
  00:51:42. An automatic 4.059 s scan and an immediate 5.009 s scan
  followed. macOS removed the peer from NDP during the first scan after
  6.614 s without an action frame. The peer became routable again 151 ms
  after the second scan finished, but the existing TCP socket failed with
  `EHOSTUNREACH`. The previous 3 s window let a scan take the radio while
  the session waited for a reply. A recent peer also protects discovery before
  any payload has moved, following the scan scheduling approach in
  [RTLAirLink](https://github.com/xnpty/RTLAirLink/blob/c073f4c8b1e20b10f88648338ecb1084f1716d19/Source-Build/src/compat_rtw89/rtw89_glue.c#L3607-L3622).
  The new scan admission rules need hardware verification.

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

On the 1.1.6 November 25 capture, 16 accepted wide requests with zero RX
in those windows triggered the old silence-based fallback. The accepted
request count was based on the submitted width. The current code keeps
negotiated 80 MHz enabled across empty windows. Compare
`MTKAWDLROCWideGrants` (accepted requests) with `MTKAWDLROCWideConfirmed`
(matching width and center). `MTKAWDLROCWideLast*` retains the last wide
request's grant, even after a narrow/social window. `MTKAWDLRxVHT80Data`
counts received data frames whose P-RXV reports VHT80. Those fields need
hardware verification.

In the 01:21 November 25 AirDrop attempt, the first Rapport TCP SYN went
out immediately after recipient selection and the SYN-ACK arrived 1.93 s
later. The same 98-byte TCP segment was sent seven times on `awdl0` over
17.35 s before the remote ACK advanced. Native logs report no peer action
frames for 6.06 s and removal from NDP during that interval. The Rapport
session-start response took 20.79 s, followed by 2.04 s resolving the
AirDrop service and 3.03 s to receive HELLO. In the 10:48 attempt, Rapport
opened in 0.13 s and HELLO arrived in 1.46 s. The captures locate the
largest delay in the early control exchange. They do not identify why
each TCP retry went unacknowledged.

## 2026-11-25 3.31 MiB/s throughput diagnosis and TX pipeline fixes (unverified on hardware)

After fixing the 80 MHz ROC deafness (`amtk_awdl_phy=1`, `amtk_awdl_infra_bw=0`)
and premature peer route removal, `diagnostics/airdrop-benchmark-702hhi02`
completed a 64 MiB Mac -> iPhone AirDrop cleanly (879 ms to first flow packet,
1.27 s to upload start, 99.97% TX ACKed at 156 Mbps VHT MCS 8 2x2 20 MHz),
but plateaued at 3.31 MiB/s (`MTKAWDLTXTraceSummary` average submit-to-free
latency 31.98 ms, `MTKAWDLNativeDroppedPackets=1013`, `MTKAWDLTXBackpressure=672`,
`MTKAWDLTXTraceTXS=68856`, `MTKAWDLTxBATorn=14`). Analysis showed five
compounding TX-path bottlenecks:

1. **Missing QoS/LDPC/VHT A-MPDU WTBL flags (`MtkMwx::awdlPeer`).**
   `STA_REC_BASIC.qos` and `WTBL_GENERIC.qos` were left 0 (unlike
   infrastructure STA Wi-Fi), `WTBL_HT.af` was capped at 3 (64 KB) instead of
   VHT's 7 (1 MB), and `WTBL_HT.ldpc` / `WTBL_VHT.ldpc` were 0.
2. **Hardware evidence from `diagnostics/airdrop-benchmark-zez4jv9x` (UUID `5B2B22E2-3B65-3867-8C11-FC9659ECA441`).**
   All 4 benchmark scenarios (`wifi`, `wifi-scan`, `wifi-disconnect`, `direct`)
   completed cleanly (`shared`), `MTKAWDLNativeDroppedPackets` dropped from
   1,013 in 1 trial to 67 across all 4 trials, and average `submit -> txfree`
   latency dropped from 31.98 ms to 16.91 ms. However, clearing
   `MT_TXD3_SN_VALID` while passing `p.seq` instead of `p.ssn` to
   `awdlTxBlockAck` caused repeated 1-3 s iPhone RX-BA reorder freezes
   (33.9 s of >=100 ms stalls in `01-01-wifi`, dropping average speed to
   1.14-1.35 MiB/s). In addition, `MTKAWDLTXTraceTXS` `raw4` hardware TX
   timestamps during full-ring bursts measured 4.67-5.87 ms per 16 frames
   (291-366 us/frame), proving MT7922 still transmitted 1 MPDU per PPDU under
   `MT_TXD2_FIX_RATE=1` despite `MT_TXD3_BA_DISABLE=0`.
   - `handleTxBlockAckFrame` is restored to `p.ssn` and `amtk_awdl_txba_hwsn`
     defaults back to `0` (`MT_TXD3_SN_VALID` kept) to eliminate the RX-BA
     reorder freezes.
   - **Hardware evidence from `diagnostics/airdrop-benchmark-9ldbi18y` (UUID `1E514F60-BED7-3658-929D-D4834E01FE6F`).**
     Firmware RA on the P2P-discovery BSS (`MTKAWDLRAEnabled=Yes`) stayed
     pinned at legacy OFDM 54 Mbps (`MTKAWDLRALastRate=76 / 0x004c`) and
     stalled transfers (`0.14-0.23 MiB/s`). `amtk_awdl_ra` now defaults to `0`
     (opt-in), restoring the 173.3 Mbps VHT MCS 8 2x2 SGI+LDPC ladder.
   - **Hardware evidence from `diagnostics/airdrop-benchmark-ug4wr08p` & `ec717y5h` (UUID `368756C6-EEC8-3DAF-A684-54F90C5F8C89`).**
     TCP/TLS PairVerify completed in ~167 ms on single QoS frames, then QUIC
     Initial sent two back-to-back 1,262-byte UDP packets that `buildAmsdu`
     collapsed into a 2,570-byte A-MSDU (`MTKAWDLAmsduFrames=129`) while the
     rate ladder was still on 6-24 Mbps legacy OFDM (`0x004b..0x0049`); iPhone
     802.11-ACKed them but dropped the payloads, causing `connect_timeout`.
     `amtk_awdl_amsdu` now defaults to `0` (opt-in `amtk_awdl_amsdu=1`), keeping
     the verified zero-stall single-MPDU path alongside all proven clean wins
     (`kDataQueueSize=1024` + lossless Skywalk backpressure, `sc_txq=512`,
     `awdlTickUS=100`, `MT_TXD6_SGI` 173.3 Mbps + `MT_TXD6_LDPC`,
     `basic/generic.qos=1`, `TXS 1-in-16`), and adds low-contention AWDL WMM
     EDCA (`wmm_idx=1`: `aifs=2, cw_min=3, cw_max=7, txop=94` = 3 ms TXOP;
     `amtk_awdl_edca=0` restores AP BE params).
   - **Hardware evidence from `diagnostics/airdrop-benchmark-xls_xrag` (UUID `F0BD459E-27CA-351F-8FC2-A693D7A9B62E`).**
     All 4 benchmark scenarios passed (`shared`: `wifi` 2.99, `wifi-scan` 1.93,
     `wifi-disconnect` 0.93, `direct` 1.63 MiB/s), `MTKAWDLNativeDroppedPackets`
     dropped to 9 across all 4 trials (vs 1,013 in 1 baseline trial), and
     hardware TXS `raw4` burst spacing dropped to `4.25 ms` per 16 MPDUs
     (`265 us/MPDU`, sustained 5.3-5.6 MiB/s 100 ms bursts, 4.39 MiB/s full-s
     peak). Packet-phase analysis proved two exact causes for the remaining
     stalls:
     1. In `01-01-wifi`, all 5 blackouts (5.52 s total, each 1.04-1.18 s) began
        at the exact 1,048.576 ms superframe phase right after the channel 6
        social window (`phase = 343-351 ms`): with up to 512 frames in `sc_txq`
        (`MTKAWDLTXTraceMaxLatencyUS = 207 ms`) plus 8 ms trailing peer-gate
        slack after iPhone hopped to ch 6, frames stranded in MT7922's hardware
        FIFO exhausted all 15 retries in ~2.2 ms, dropping an MPDU and locking
        iPhone's RX-BA reorder buffer for ~1 s. Fix: cap AWDL unicast hardware
        in-flight depth at 48 TXWIs (`~12.7 ms`, `amtk_awdl_hw_inflight`),
        stop submitting unicast data 14 ms before peer channel departure and
        remove trailing post-departure slack (`amtk_awdl_peer_drain_us`), and
        briefly drain in-flight TXWIs (`<= 12 ms`) inside `mwx_awdl_abort`.
     2. In `01-04-direct`, unassociated `airportd` auto-join 41-channel scans
        ran 4 channels x `dwell=120` (~1 s firmware blackout per chunk),
        causing 20.29 s of QUIC PTO / PMTUD blackhole stalls while scan-free
        intervals ran at 3.6-3.93 MiB/s. Fix: during active AWDL data (<3 s),
        chunk scans to 2 channels with firmware auto-dwell (`dwell=0`, keeping
        `timeout_value = nchan * 120`). Hardware verification pending.
3. **Per-MPDU TXS flood and Long GI / BCC on HT/VHT data.**
   Every unicast data frame set `MT_TXD5_TX_STATUS_HOST`, generating 68,856
   extra 32-byte RX events per upload, while HT/VHT fixed rates omitted Short
   GI and LDPC. Once a TX-BA peer settles at its top rung, unicast data now
   samples TXS 1-in-16 (`amtk_awdl_txs_sample`, default 16, 1 = every frame),
   and HT/VHT data enables 400 ns Short GI (`amtk_awdl_sgi=0` off; 156 -> 173.3
   Mbps at VHT MCS 8 2x2 20 MHz) and LDPC (`amtk_awdl_ldpc=0` off).
4. **Silent Skywalk queue drops during QUIC slow-start bursts.**
   `kDataQueueSize` was 256 while QUIC sent 644-packet bursts in 5.4 ms,
   causing `submitNative` to drop 1,013 packets while reporting them consumed.
   `kDataQueueSize` is now 1024 and `submitNative` stops consuming (`break`)
   when full to apply lossless Skywalk backpressure.
5. **2 ms TX-backpressure sleep and half-sized TX DMA ring.**
   `sc_txq` allocated only 256 descriptors despite `MWX_TXWI_MAX=512`, and
   `serviceInGate` slept 2,000 us after every `mwx_tx_full` break. `sc_txq` now
   matches `MWX_TXWI_MAX` (512), `awdlTickUS` defaults to 100 us
   (`amtk_awdl_tick_us`), and TX backpressure caps the re-arm delay to
   `awdlTickUS()`.

Hardware benchmark verification after installing the rebuilt Wi-Fi kext and
rebooting is pending.
