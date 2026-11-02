# Instant Hotspot

## 1.0.7 (dev 1.0.30): Apple vendor IEs in the Association Request (fixed)

A native Mac's Association Request carries two Apple vendor IEs that
MtkWireless never sent (REA of macOS 26.7.2 25G332):

- `dd 0b 00 17 f2 0a 00 01 04 00 00 00 00`: Apple device IE. airportd
  installs it at Wi-Fi power-on (custom-IE request 85); our `setIE`
  returned unsupported ("configure Apple device IE failed").
- `dd 0b 00 17 f2 0b 00 01 04 FLAG 00 00 00`: Instant Hotspot IE, added by
  `AppleBCMWLANJoinAdapter::performJoin` when the WCL candidate flags at
  `+0x1e0` have bit 2 (manual, FLAG 2) or bit 4 (auto, FLAG 1).

`decodeJoin` now keeps the candidate flags and `ieee80211_get_assoc_req`
appends both IEs (0x0b only for hotspot joins). Boot-arg `amtk_apple_ie`:
bit 0 = 0x0a, bit 1 = 0x0b, default 3, 0 = off. Join diagnostics
`AppleJoinFlags` and `AppleIE` (mask | FLAG << 8).

Hardware, 2026-11-02: Instant Hotspot join with `AppleJoinFlags` 2,
`AppleIE` 515, M1 received, port valid, DHCP 172.20.10.2. Without the IEs
the iPhone associated us but never started the 4-way handshake.

## 1.0.29: correct MAC timing by band and ERP slot state

Source review found `mt7921_mac_set_timing()` hard-coded `is_2ghz=1` and
slot time 9 us. Thus a 5 GHz hotspot join programmed SIFS=10 us instead of
16 us. MT76's shared `mt792x_mac_set_timeing()` uses the current channel's
band (10 us at 2 GHz, 16 us otherwise) and its negotiated PHY slot time:
https://github.com/openwrt/mt76/blob/master/mt792x_mac.c (reviewed 2026-11-01).
This is a concrete configuration defect; the available host captures do
not establish that it caused the absent hotspot M1.

1.0.29 uses the same channel selection as the channel-switch command:
the STA BSS channel, otherwise the IBSS channel. SIFS is 10/16 us by band.
Slot time is 9 us for 5/6 GHz and follows net80211's short-slot flag
(9 or 20 us) for 2 GHz. `ic_updateslot` now applies association/ERP changes
without a channel switch. RUN also refreshes timing after successful
firmware BSS/peer setup, including 20 MHz joins that never switch again.
The existing pause/program/resume sequence and timeout values remain.
Firmware ROC scheduling and RX acceptance are unchanged.

Join dictionaries retain `MacTimingBand` (0=2 GHz, 1=5 GHz, 2=6 GHz),
`MacTimingSIFSUS`, `MacTimingSlotUS`, `MacTimingICR0` and
`MacTimingICR0Readback`. On channel 149, expect 16/9 us and register
`0x09100968`; written/readback values should agree. The 1.0.28 TX-free
ACK diagnostics remain. `scripts/test-wifi-timing.py` runs the production
MMIO routine and callback under ASan/UBSan for all bands/slot states,
invalid/fallback channels, callback shutdown and register readback.
Hardware hotspot and AWDL verification of 1.0.29 is pending.

Validation: production timing MMIO/callback test, hotspot, Enterprise EAPOL,
STA RX admission and capability regressions pass. Release build and kernel
checks pass (Wi-Fi 1129 and BT 367 undefined symbols resolve). AWDL shared
layout (213 entries), protocol test and 12 Python tests pass. The unchanged
`test-mi-discovery.py` cannot compile: its stand-in AirportMtkAwdlInterface
has no `inputNativeView` method, called by the existing native-RX optimization
in AirportMtkAwdlInterface.cpp. That test compiles only the AWDL source and
does not include this MAC timing change. The upstream test was left intact;
the full AWDL command consequently exits 1, and is not reported as passing.

## 1.0.27 hardware failure and 1.0.28 TX diagnostics

On 2026-11-01 at 22:14, the machine is running Wi-Fi and BT 1.0.27.
The retained failed join (attempt 5, BSSID `66:f1:67:4d:6f:6b`) receives
successful Open System authentication and Association Response status 0.
RUN commits 18.842 ms after the response. All three delayed EAPOL-Start
frames reach DMA (`EapolStartTx=3`, `EapolTxDMA=3`, errors=0), but raw and
parsed M1 remain zero until macOS aborts about 10 seconds later. There are
32 BSS beacons, no BSS data, no HAL U2M drop, no ROC grants and no infra-PS
handshakes. The three station MAC diagnostics agree. DMA rings are drained,
with no DMA stalls, MCU timeouts or full resets in the later live snapshot.
These host observations do not establish whether the iPhone sent M1.

The old `AssocDMA=4` was a diagnostic bug: the management subtype defaulted
to 0, which is also Association Request, so the three Starts were counted
again as Association transmissions. 1.0.28 classifies locally constructed
TX frames by both type and subtype and retains the join attempt and frame
class with each TXWI token. A Rust-validated TX-free completion records
`{Auth,Assoc,Eapol}TxAcked`, `TxFailed`, `TxLastStatus`, `TxLastCount`,
`TxLastRaw` and `TxCompletedAtUS` in the existing join dictionaries.
Only unicast is counted; unrelated frames and stale completions from an
earlier join cannot overwrite the current attempt. Diagnostic version is 3.

DMA submission remains distinct from firmware's reported acknowledgment.
`EapolTxAcked>0` means firmware reports the AP acknowledged at least one
unicast EAPOL packet; `EapolTxFailed>0` retains its failure status/count.
DMA>0 with no completion narrows investigation to the firmware/TX-free
path. Even an ACK does not prove the AP processed EAPOL or sent M1; an
independent RF capture is still needed to distinguish AP silence from RX
loss. 1.0.28 adds diagnostics without changing packet acceptance, channel
scheduling, RSN negotiation or the existing EAPOL-Start experiment.

Validation: `scripts/test-wifi-hotspot.py` executes the production helpers
with ASan/UBSan, covering data/Association subtype collisions, ACK/failure
results, DMA failures, multicast, stale attempts and token reset placement.
Hotspot, capability and Enterprise EAPOL regressions pass. Release build
and kernel checks pass (Wi-Fi 1129 and BT 367 undefined symbols resolve).
Hardware verification of these new counters is pending. The subsequent
CLI retry reports `Could not find network Dino`; the user cannot currently
enable Personal Hotspot, so no controlled new live test was completed.


## 1.0.15: diagnose and protect the Wi-Fi join

The 1.0.14 hardware attempt at 2026-10-31 22:10 on Tahoe 26.7.2 / 25G332
passes the outgoing BLE path: LE readiness, rapportd PSM 0x0081 and sharingd
`Request complete: success` with `SFRemoteHotspotInfo name: Dino, channel: 149,
hasPassword: yes`. The remaining timeout occurs in the Wi-Fi join.
Saved evidence: `/tmp/rpa-log/wifi.log`, `join.log`, `bt5.log`.

At 22:10:04.094698 WCL submits Open System / WPA2-PSK with a PMK and a valid
private MAC. The phone advertises PSK+SAE, CCMP and MFPC (RSN caps 0x8c), BSSID
52:b8:a8:1f:02:b7, channel 149. WCL association completes at 09.873011 and
LinkStatus/Joined Bss arrives at 10.009938. The native manager then waits for
ConnectComplete until the approximately ten-second timeout at 14.151129 and
leave at 14.320560. The prior home AP lacks the SAE/MFPC combination and works.
The 5.78 seconds includes MAC restart, a new directed scan, authentication and
HAL work; it does not measure Authentication Response latency alone. Neither
these WCL messages nor the timeout prove which EAPOL message was lost.

Three code issues have been fixed, with different levels of evidence:

- **AWDL firmware-wait race (most plausible radio explanation).**
  `mwx_awdl_infra_joining` used only committed `ic_state`. AUTH setup runs
  after `sc_ns_state` is updated, while `ic_state` may remain SCAN during
  synchronous MCU waits that release the gate. A reservation could pass the
  guard then; AUTH also did not abort an existing pending/granted ROC before
  changing to the AP channel. The guard now covers queued AUTH/ASSOC and RUN
  setup, and AUTH aborts the existing ROC first. Existing scan preemption,
  the RUN/invalid-port guard, PM handshakes and escape hatches remain.
  This is a demonstrable code gap, not proof that a ROC caused this capture.
- **RSN OUI lifetime.** The .c file is built as C++; choosing a pointer between
  two C compound-literal OUI macros leaves a dangling pointer after the full
  expression. The extracted production builder produces an ASan
  `stack-use-after-scope` on its first selector copy. It now uses permanent
  three-byte arrays. The optimized kext may have folded those constants;
  this does not independently establish an incorrect over-the-air RSN IE.
- **Key installation acknowledgment and PMF ordering.** Individual key
  commands discarded the firmware status, and PTK+GTK completion opened the
  port before the queued IGTK finished. Only key commands now interpret the
  real firmware status. The queue publishes port-valid after all queued
  keys finish successfully; a pending leave cannot reopen the port. A
  failed initial key is counted and keeps authorization closed until fresh
  auth/key teardown. GTK index 0 is accepted when pairing CCMP+BIP for firmware, as
  net80211 accepts GTK KDE indices 0..3. Software BIP fallback is retained.

The tested PSK+PMF path generates suite **00:0f:ac:2**, not SHA256-PSK (:6)
or SAE (:8), and EAPOL descriptor version **2**. SHA256-PSK still uses 3;
SAE still uses 0. `selectWPA2TransitionMode` remains disabled while PMF is
available. The Enterprise-only BSD `setEAPOLTransport` gate is unchanged:
net80211 PSK M2/M4 enqueue directly to HAL `if_snd` and call `if_start`, so
that gate does not control their transmission. The WCL first-beacon RUN
gate is retained because WCL reads RUN-only extended BSS information there.
MAC initialization still copies WCL's BSD `ac_enaddr` to `ic_myaddr` and
DEV_INFO_UPDATE; PTK derivation and outgoing assoc/EAPOL use `ic_myaddr`.
The original capture cannot confirm the address actually used by the driver.

### Collect after the user installs to EFI disk0s1 and reboots

Both `build/Release/MtkWirelessWifi.kext` and `MtkWirelessBT.kext` must report
1.0.15. Retry Dino, then save the controller properties immediately:

```sh
ioreg -r -c AirportMtkWlan -l -w0 > /tmp/hotspot-1.0.15-ioreg.txt
ioreg -a -r -c AirportMtkWlan > /tmp/hotspot-1.0.15-ioreg.plist
```

`AirportMtkWlan` publishes **MTKWiFiJoinDiagnosticVersion = 1** and dictionaries
**MTKWiFiJoin** (latest attempt/live state) and **MTKWiFiJoinLastFailed**
(last failed/aborted attempt before authorization). Each dictionary contains
keys with the full prefix **MTKWiFiJoin**, for example
`MTKWiFiJoinEapolRxM1`. Counters reset per WCL join, not per reboot. A deferred
MAC restart retains its original attempt/start timestamp. Failed state is
copied before RSN/PMK/BSS cleanup and survives a later successful home-AP
join. A later failed join replaces it, so identify the BSSID and attempt.
Properties refresh on the existing watchdog, independently of AWDL creation.

All timestamps are **microseconds since boot**, not wall-clock time; 0 means
not observed. No password, PMK/PTK/GTK/IGTK bytes, nonces or MIC bytes are
published. RSN IEs contain only public security negotiation data. Fields are
observations, not proof of on-air ACKs: management TxCount counts construction
attempts, DMA counts successful driver submission, and M2/M4 Tx counts
construction through the EAPOL sender before enqueue.

| Exact keys (inside either dictionary) | Expected / interpretation |
|---|---|
| `MTKWiFiJoinAttempt`, `MTKWiFiJoinActive`, `MTKWiFiJoinOutcome` | Attempt >0. Active=1 while tracked; Outcome=0 on successful ConnectComplete. A nonzero IOReturn marks failure/abort. Outcome=0 with no completion timestamp is still pending. |
| `MTKWiFiJoinBSSID` | Hex `52b8a81f02b7` for the saved phone; verify the current BSSID if it changes. |
| `MTKWiFiJoinStartedAtUS`, `MTKWiFiJoinDeferredAtUS`, `MTKWiFiJoinReadyAtUS`, `MTKWiFiJoinScanStartedAtUS`, `MTKWiFiJoinScanDoneAtUS` | Separate private-MAC restart and real directed scan time from auth/assoc. Deferred/Ready are zero if no restart was needed. |
| `MTKWiFiJoinLowerAuth`, `MTKWiFiJoinUpperAuth`, `MTKWiFiJoinKeyCipher`, `MTKWiFiJoinKeyLength` | For the captured request: 1, 8, 6 (PMK), 32. No key content is stored. |
| `MTKWiFiJoinAuthRequestedAtUS`, `MTKWiFiJoinAuthTaskAtUS`, `MTKWiFiJoinAuthCommittedAtUS`, `MTKWiFiJoinAuthTxAtUS`, `MTKWiFiJoinAuthRxAtUS`, `MTKWiFiJoinAuthStatus` | Successful Open System auth has Tx/Rx >0 and status 0; 65535 means no response. Request-to-task is queue latency; task-to-commit is HAL setup; Tx-to-Rx is response latency. |
| `MTKWiFiJoinAuthAlgorithm`, `MTKWiFiJoinAuthSequence`, `MTKWiFiJoinAuthDMA`, `MTKWiFiJoinAuthDMAErrors` | Open System response: algorithm 0, sequence 2, DMA >0, errors 0. SAE has its real algorithm/sequence. |
| `MTKWiFiJoinAssocRequestedAtUS`, `MTKWiFiJoinAssocTxAtUS`, `MTKWiFiJoinAssocRxAtUS`, `MTKWiFiJoinAssocStatus`, `MTKWiFiJoinAssocDMA`, `MTKWiFiJoinAssocDMAErrors` | Assoc status 0 with Tx/Rx/DMA >0 and DMA errors 0. A status without later RUN narrows the failure to setup/response validation. |
| `MTKWiFiJoinRunRequestedAtUS`, `MTKWiFiJoinRunTaskAtUS`, `MTKWiFiJoinRunCommittedAtUS`, `MTKWiFiJoinHalError` | RUN commits after channel/BSS/STA setup; HAL error 0. Large task-to-commit explains a delayed first beacon/link. |
| `MTKWiFiJoinAPAKMs`, `MTKWiFiJoinAPCiphers`, `MTKWiFiJoinAPGroupCipher`, `MTKWiFiJoinAPRSNCaps` | Captured phone: 18 (PSK=2 + SAE=16), 8 (CCMP), 8, 140 (0x8c: MFPC, not MFPR). These are net80211 masks, not Apple's capability masks or AKM suite numbers. |
| `MTKWiFiJoinAssocAKMs`, `MTKWiFiJoinAssocCipher`, `MTKWiFiJoinAssocGroupCipher`, `MTKWiFiJoinAssocGroupMgmtCipher`, `MTKWiFiJoinAssocRSNCaps`, `MTKWiFiJoinPMF` | PSK side with default PMF: 2, 8, 8, 32 (BIP), 140, 1. Group-management cipher omitted by the AP defaults to BIP in net80211. |
| `MTKWiFiJoinAPRSN`, `MTKWiFiJoinAssocRSN` | OSData with actual saved AP RSN IE and generated assoc RSN IE including element ID/length. Assoc must contain exactly AKM 00:0f:ac:2 and CCMP for this WCL PMK request. |
| `MTKWiFiJoinWCLMAC`, `MTKWiFiJoinAssocMAC`, `MTKWiFiJoinFirmwareMAC`, `MTKWiFiJoinPTKMAC`, `MTKWiFiJoinMacRequestedAtUS`, `MTKWiFiJoinMacPlan` | For the saved request all addresses should be `eaab1b1025a2`. WCLMAC is the last **setHardwareAddress** request, with its timestamp; compare it with airportd's logged private MAC. An old/zero WCLMAC is not evidence that this join requested it. FirmwareMAC retains actual DEV_INFO_UPDATE address bytes and the submission timestamp; FirmwareMacError should be 0 (transport result, not hardware readback). PTKMAC becomes nonzero when M1 selects a PMK. MacPlan: 0 invalid, 1 unchanged, 2 busy, 3 change. |
| `MTKWiFiJoinEapolRawM1`, `MTKWiFiJoinEapolRawM3`, `MTKWiFiJoinEapolRawWrongMAC`, `MTKWiFiJoinEapolRawPreRun`, `MTKWiFiJoinEapolRawState` | Plaintext EAPOL observations before STA address/replay filters, scoped to the target BSSID. WrongMAC should be 0; PreRun shows arrival before state 4. Raw M1>parsed M1 indicates a later RX gate/drop. Complete contiguous plaintext nonaggregated data/QoS frames only; encrypted/fragmented/A-MSDU frames are excluded. This is not a monitor-mode RF capture. |
| `MTKWiFiJoinEapolRxM1`, `MTKWiFiJoinEapolTxM2`, `MTKWiFiJoinEapolRxM3`, `MTKWiFiJoinEapolTxM4` | Each >=1 on a successful PSK four-way handshake; retries may increase counts. Zero locates the stage. Counts are before descriptor validation (RX) or before queueing (TX). |
| `MTKWiFiJoinEapolRxDescriptor`, `MTKWiFiJoinEapolTxDescriptor`, `MTKWiFiJoinEapolRxV2`, `MTKWiFiJoinEapolTxV2`, `MTKWiFiJoinEapolDescriptorRejected`, `MTKWiFiJoinEapolRejectedDescriptor` | PSK+CCMP+PMF still uses 2; V2 counts increase and rejected count stays 0. Version rejection retains the actual rejected version. |
| `MTKWiFiJoinEapolTxDMA`, `MTKWiFiJoinEapolTxEnqueueErrors`, `MTKWiFiJoinEapolTxDMAErrors` | At least M2 and M4 submitted on success; both error counters 0. Construction>DMA with no enqueue error points to TX state/queue/encapsulation gating. DMA does not prove an AP ACK. |
| `MTKWiFiJoinMICFailures`, `MTKWiFiJoinReplayDrops`, `MTKWiFiJoinM3Reject`, `MTKWiFiJoinM3KDEs` | Normally 0, 0, 0 and 7 (RSN=1, GTK=2, IGTK=4) for PMF. MIC failures point to PMK/PTK/MAC/nonce mismatch; no key material is captured. See rejection-stage table below. |
| `MTKWiFiJoinPTKQueued`, `MTKWiFiJoinGTKQueued`, `MTKWiFiJoinIGTKQueued`, `MTKWiFiJoinPTKInstalled`, `MTKWiFiJoinGTKInstalled`, `MTKWiFiJoinIGTKInstalled` | Each >=1 for a PMF join carrying IGTK; non-PMF joins normally have IGTK=0. Installed means successful queued completion; IGTK software-only fallback is separately counted. |
| `MTKWiFiJoinPTKFailed`, `MTKWiFiJoinGTKFailed`, `MTKWiFiJoinIGTKFailed`, `MTKWiFiJoinKeyLastError`, `MTKWiFiJoinKeyFirmwareStatus`, `MTKWiFiJoinIGTKSoftware` | Failures/last error 0; real firmware status 0 when a command completed. 4294967295 means unconfirmed/no reply; software-only IGTK fallback normally 0 for CCMP+IGTK. Key failure counts retain failures even if a later command succeeds. |
| `MTKWiFiJoinKeyCompletedAtUS`, `MTKWiFiJoinPortValidAtUS`, `MTKWiFiJoinWCLLinkUpAtUS`, `MTKWiFiJoinFirstBeaconAtUS`, `MTKWiFiJoinConnectCompleteAtUS`, `MTKWiFiJoinPortValid` | Success: key/port/link/first-beacon/completion timestamps nonzero, PortValid=1 while still linked. Port-valid follows successful queued IGTK completion. Port/link valid but no ConnectComplete points to WCL completion/beacon handling. |
| `MTKWiFiJoinROCRefused`, `MTKWiFiJoinROCGrants`, `MTKWiFiJoinROCGrantsDuringHandshake`, `MTKWiFiJoinROCAbortAtUS`, `MTKWiFiJoinROCAbortError` | Refused may increase while AWDL is active; total grants can include scan gaps or activity after authorization. Handshake grants should be 0 after the existing reservation is cancelled. Abort error 0. A stale grant before the AUTH task cancels its token can still be counted. |
| `MTKWiFiJoinDeauthRxAtUS`, `MTKWiFiJoinDeauthRxReason`, `MTKWiFiJoinDisassocRxAtUS`, `MTKWiFiJoinDisassocRxReason`, `MTKWiFiJoinDeauthTxAtUS`, `MTKWiFiJoinDeauthTxReason`, `MTKWiFiJoinDisassocTxAtUS`, `MTKWiFiJoinDisassocTxReason` | Real observed/constructed frame reasons during the tracked attempt. No timestamp means no observation. User/WCL abort Outcome is separate from AP-sent deauth; cleanup after the snapshot freezes does not change it. |

`M3Reject` is a stage marker, set immediately before a validation step and
cleared after M3 passes. Its nonzero value locates the latest incomplete M3:

| Value | Stage |
|---|---|
| 1 | Supplicant state |
| 2 | Replay counter |
| 3 | PMK selected |
| 4 | ANonce match |
| 5 | MIC verification |
| 6 | Key Data decryption |
| 7 | Mandatory RSN IE |
| 8 | GTK encryption |
| 9 | IGTK requires GTK |
| 10 | Pairwise Install bit |
| 11 | AP RSN equality / second RSN validation |
| 12 | M4 construction/transmission enqueue |
| 13 | PTK length/install queue |
| 14 | GTK KDE length/install queue |
| 15 | IGTK KDE length/index/install queue |

Zero parsed M1 with raw M1>0 and no WrongMAC points to state/replay/security
RX filtering. Parsed M1 and M2 DMA but no M3 means the AP has not progressed
past M2 (wrong PMK/PTK, lost TX/RX, or peer rejection; an RF trace discriminates
these). M3 with MIC failures narrows the key/MAC hypothesis. M3 accepted/M4
submitted but key failures pinpoints firmware installation. Key and port/link
success without ConnectComplete instead points to the WCL event path.

### Optional boot-argument comparison (performed by the user)

A single retry with **amtk_pmf=0** is useful if the default build still fails:
it removes PMF negotiation and strips SAE from the transition-mode beacon
presented to WCL, while this particular request still chooses PSK and descriptor
2. If only that run works, the evidence favors the PMF/RSN/IGTK path, but does
not uniquely identify an IGTK firmware bug because WCL's advertised network
view also changed. Check AssocRSNCaps has MFPC/MFPR clear and PMF=0, IGTKQueued=0.
If both fail with no M1 or no M3, compare MAC/timing/ROC diagnostics first.
**amtk_awdl_offchan=0** tests associated off-channel scheduling only; it does
not disable unassociated/pending ROCs and therefore is not a clean test of the
old firmware-wait race. **amtk_awdl=0** is the broader AWDL-off comparison.
No boot arguments, NVRAM, firmware installation or reboot were performed here.

### Offline validation

`scripts/test-wifi-hotspot.py` runs the real RSN builder, EAPOL descriptor
checks, AWDL pending-state predicate, key command status handling and queued
key-completion barrier under ASan/UBSan. It covers PSK+PMF, SHA256-PSK, SAE,
legacy WPA M4 without Secure, PMKID, key failure and a pending leave. Rust
join-parser tests cover every truncated prefix, length inconsistencies,
address preservation, direction, encryption, fragmentation, A-MSDU exclusion
and QoS HT Control. The FFI regression checks C layout and invalid-input
clearing. Existing Wi-Fi, Enterprise and unchanged upstream AWDL suites are
required as well. Final validation passes: Rust workspace (61 WLAN tests),
both FFI feature suites, all six Wi-Fi scripts, the 14 unchanged upstream
AWDL tests plus RX trace, LE readiness, both BT page validators and mSBC
codec/integration tests. Release build exits 0 through check.sh: 1129 Wi-Fi
and 365 BT undefined symbols resolve, no SSE/AVX/x87, and both bundles report
1.0.15. Targeted Rust formatting and git diff whitespace checks pass;
workspace-wide rustfmt reports existing formatting differences in unchanged
AWDL/BT files. Hardware hotspot/DHCP/traffic verification remains pending.

All dictionary keys (exact spelling):

```text
MTKWiFiJoinNodeFlags
MTKWiFiJoinEapolRxV0
MTKWiFiJoinEapolRxV1
MTKWiFiJoinEapolRxV2
MTKWiFiJoinEapolRxV3
MTKWiFiJoinEapolRxVUnknown
MTKWiFiJoinEapolTxV0
MTKWiFiJoinEapolTxV1
MTKWiFiJoinEapolTxV2
MTKWiFiJoinEapolTxV3
MTKWiFiJoinEapolRejectedDescriptor
MTKWiFiJoinWCLLinkUpAtUS
MTKWiFiJoinConnectCompleteAtUS
MTKWiFiJoinFirstBeaconAtUS
MTKWiFiJoinEapolRawM1
MTKWiFiJoinEapolRawM3
MTKWiFiJoinEapolRawOther
MTKWiFiJoinEapolRawAtUS
MTKWiFiJoinEapolRawState
MTKWiFiJoinEapolRawWrongMAC
MTKWiFiJoinEapolRawPreRun
MTKWiFiJoinObservedAtUS
MTKWiFiJoinState
MTKWiFiJoinRequestedState
MTKWiFiJoinPortValid
MTKWiFiJoinIGTKSoftware
MTKWiFiJoinAuthDMA
MTKWiFiJoinAuthDMAErrors
MTKWiFiJoinAssocDMA
MTKWiFiJoinAssocDMAErrors
MTKWiFiJoinAttempt
MTKWiFiJoinActive
MTKWiFiJoinStartedAtUS
MTKWiFiJoinEndedAtUS
MTKWiFiJoinOutcome
MTKWiFiJoinLowerAuth
MTKWiFiJoinUpperAuth
MTKWiFiJoinKeyCipher
MTKWiFiJoinKeyLength
MTKWiFiJoinDeferredAtUS
MTKWiFiJoinReadyAtUS
MTKWiFiJoinScanStartedAtUS
MTKWiFiJoinScanDoneAtUS
MTKWiFiJoinAuthRequestedAtUS
MTKWiFiJoinAuthTaskAtUS
MTKWiFiJoinAuthCommittedAtUS
MTKWiFiJoinAuthTxAtUS
MTKWiFiJoinAuthTxCount
MTKWiFiJoinAuthRxAtUS
MTKWiFiJoinAuthRxCount
MTKWiFiJoinAuthAlgorithm
MTKWiFiJoinAuthSequence
MTKWiFiJoinAuthStatus
MTKWiFiJoinAssocRequestedAtUS
MTKWiFiJoinAssocTxAtUS
MTKWiFiJoinAssocTxCount
MTKWiFiJoinAssocRxAtUS
MTKWiFiJoinAssocRxCount
MTKWiFiJoinAssocStatus
MTKWiFiJoinRunRequestedAtUS
MTKWiFiJoinRunTaskAtUS
MTKWiFiJoinRunCommittedAtUS
MTKWiFiJoinHalError
MTKWiFiJoinAPProtos
MTKWiFiJoinAPAKMs
MTKWiFiJoinAPCiphers
MTKWiFiJoinAPGroupCipher
MTKWiFiJoinAPGroupMgmtCipher
MTKWiFiJoinAPRSNCaps
MTKWiFiJoinAssocAKMs
MTKWiFiJoinAssocCipher
MTKWiFiJoinAssocGroupCipher
MTKWiFiJoinAssocGroupMgmtCipher
MTKWiFiJoinAssocRSNCaps
MTKWiFiJoinPMF
MTKWiFiJoinEapolRxM1
MTKWiFiJoinEapolRxM2
MTKWiFiJoinEapolRxM3
MTKWiFiJoinEapolRxM4
MTKWiFiJoinEapolTxM1
MTKWiFiJoinEapolTxM2
MTKWiFiJoinEapolTxM3
MTKWiFiJoinEapolTxM4
MTKWiFiJoinEapolRxGroup
MTKWiFiJoinEapolTxGroup
MTKWiFiJoinEapolRxAtUS
MTKWiFiJoinEapolTxAtUS
MTKWiFiJoinEapolRxDescriptor
MTKWiFiJoinEapolTxDescriptor
MTKWiFiJoinEapolDescriptorRejected
MTKWiFiJoinEapolTxEnqueueErrors
MTKWiFiJoinEapolTxDMA
MTKWiFiJoinEapolTxDMAErrors
MTKWiFiJoinEapolRxState
MTKWiFiJoinEapolRxSuppState
MTKWiFiJoinEapolTxState
MTKWiFiJoinMICFailures
MTKWiFiJoinReplayDrops
MTKWiFiJoinM3Reject
MTKWiFiJoinM3KDEs
MTKWiFiJoinPTKQueued
MTKWiFiJoinGTKQueued
MTKWiFiJoinIGTKQueued
MTKWiFiJoinPTKInstalled
MTKWiFiJoinGTKInstalled
MTKWiFiJoinIGTKInstalled
MTKWiFiJoinPTKFailed
MTKWiFiJoinGTKFailed
MTKWiFiJoinIGTKFailed
MTKWiFiJoinKeyLastError
MTKWiFiJoinKeyFirmwareStatus
MTKWiFiJoinKeyCompletedAtUS
MTKWiFiJoinPortValidAtUS
MTKWiFiJoinROCRefused
MTKWiFiJoinROCGrants
MTKWiFiJoinROCGrantsDuringHandshake
MTKWiFiJoinROCAbortAtUS
MTKWiFiJoinROCAbortError
MTKWiFiJoinDeauthRxAtUS
MTKWiFiJoinDeauthRxReason
MTKWiFiJoinDeauthTxAtUS
MTKWiFiJoinDeauthTxReason
MTKWiFiJoinDisassocRxAtUS
MTKWiFiJoinDisassocRxReason
MTKWiFiJoinDisassocTxAtUS
MTKWiFiJoinDisassocTxReason
MTKWiFiJoinMacRequestedAtUS
MTKWiFiJoinMacPlan
MTKWiFiJoinFirmwareMacError
MTKWiFiJoinFirmwareMacAtUS
MTKWiFiJoinWCLMAC
MTKWiFiJoinAssocMAC
MTKWiFiJoinFirmwareMAC
MTKWiFiJoinPTKMAC
MTKWiFiJoinBSSID
MTKWiFiJoinAPRSN
MTKWiFiJoinAssocRSN
```


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

## Candidate selection for shared hotspots

WCL association candidates normally include the hotspot BSSID. Same-account
and Family Sharing activation can leave that field zero and let the directed
SSID/channel result select the BSS. The driver accepts the all-zero address as
a wildcard, clears `IEEE80211_F_DESBSSID` for that join, and records the real
BSSID when association completes. Multicast candidate addresses are still
rejected.

## Directed probes during a join

The MT7922 1.0.6 campus report (2026-10-31) shows a hotspot on channel 149
in macOS's network list, followed by join scans with `ssids=0` and no
authentication attempt. WCL's join supplies the SSID, but does not create a
separate `SCAN_REQ`; the firmware encoder previously used SSIDs only from
explicit scan requests.

Join scans now take their directed target from `ic_des_essid` when
`ic_wcl_join_requested` is set and no explicit WCL scan is active. Each
retry includes that target, with active scanning, two probes, and
`ssid_type=0x4 ssids=1 ext_ssids=0 ssid_type_ext=1`. Explicit list and
passive scans retain priority and their original targets. Stale wildcard
metadata from a completed scan does not add wildcard probes to a join.
The existing channel selection and scan/AWDL admission policy are unchanged.

The report demonstrates the missing directed target; it does not establish
whether that particular iPhone was withholding beacons. Hardware verification
must confirm the BSS is found and authentication starts with the fixed build.

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

## 2026-10-31: outgoing BLE fails before Wi-Fi

On macOS 26.7.2 / 25G332 x86_64, MT7922 USB `0489:e0d8`, the loaded 1.0.9
advertising address patches work: both `BluetoothdAdvAddressPatched` and
`BluetoothdAdvAddressRetryPatched` are 1, and the earlier STATUS 718 loop is
gone. Instant Hotspot still fails before any Wi-Fi request. Between 14:59:23
and 15:00:12, rapportd repeatedly opens an on-demand ForceL2CAP connection to
CBDevice `9475561C-E0CA-2E66-F071-B2713B9A7DB8`, PSM `0x0081`, and times out
after about 11 seconds. Incoming iPhone-to-Mac BLE connections already work.

The four connection logs at 14:59:25.897, 14:59:35.899, 14:59:48.428 and
14:59:59.899 show **both** `adv-addr` and `resolved-addr` as public
`10:A2:D3:21:31:FE`. The accept-list add succeeds for that public identity.
`Identities resolve added` and the temporary-IRK lifetime describe host-side
state; they do not establish that an IRK reached the controller.

Historical PacketLogger captures `airdrop-bt-20261024-154805/hci.pklg` and
`airdrop-bt-20261024-155439/hci.pklg` supply the on-air evidence. Matching
manufacturer payloads correlate the same CBDevice with random legacy
ADV_IND addresses `74:AC:69:D1:64:95` (13 reports) and
`6E:42:48:27:59:D8` (61 reports). Both have the RPA `01` upper bits. This
correlation is not an independent cryptographic AH check. In both captures,
`0x2011` adds public `10:A2:D3:21:31:FE`, and `0x200D` initiates with filter
policy 1 (accept list) and a random own address. There is no `0x2027`,
`0x202D` or `0x204E`. The unsuccessful LE Connection Complete status `0x02`
arrives after cancellation. The controller reports LE features byte 0
`0xfd`, including LL Privacy bit 6. These HCI captures are from October 24;
the October 31 attempt has logs, not a fresh complete HCI capture.

Static analysis of the installed 25G332 x86_64 slice confirms the software
address mismatch. The slice UUID is `7ef7ef4d-9b0e-355a-bba8-f34713b2580a`;
the retained extraction SHA-256 is
`55c2233c38f532b5d9a8fc430953daff004bf5bc42662ed385b8c82ce61ee67b`.
The host AH resolver at `0x10022766b` checks the RPA against its IRK cache.
On a successful match, `0x1002234d2` overwrites all seven primary typed-address
bytes with the identity. `LeDevice` is subsequently created/looked up by
that identity. Its ordinary address getter at `0x1007068f0` prefers the
resolved identity at `+0x18`; its advertised address at `+0x10` has also
already become the identity. Merely selecting the advertised field would
therefore leave the observed public-address failure intact.

Controller privacy is more than a feature-bit gate in this binary. The chip
getter at `0x100394472` reads singleton `0x100b3abb0`, whose chip ID is at
`+0x334`. The privacy backend initializer at `0x1001e390c` creates BCM
(IDs 1–1999), ACI (2000–3999/6000), or SRS (4000–5999) backends; it creates
none for THIRD_PARTY_DONGLE / chip 0. The add wrapper at `0x1001e5ead`
returns `0xE11` for a null backend. Chip IRK operations use internal vendor
IDs `0xF1/0xF2`; the reviewed BCM/ACI add paths send `0xFCE9/0xFEFF`, not
standard `0x2027`. The `BTVCLinkAgent` resolving-list method at
`0x100438d52` is virtual bookkeeping. Forcing a support flag would not
initialize a working standard privacy backend. That backend would also
have to coordinate resolving-list updates with scanning, advertising and
initiation. The kernel shim has no peer IRK to provision.

The new exact-byte `rpa*25G332` page patches implement a host RPA fallback:

| Patch site (unslid VA) | Behavior |
|---|---|
| `0x1000026b8` | Install five leaf helpers in 261 bytes of executable alignment padding, preserving load commands and the first native function. |
| `0x10020c3dd` | Clear exactly seven unused direct-address bytes for each legacy report, preserving the following live flag. |
| `0x1002234c3` | After a successful native AH match, retain the raw RPA only for chip 0 legacy ADV_IND; still replace the primary address with its identity. |
| `0x100729834` | Decode the internal `0x81` direct-address marker only on legacy connectable undirected, RPA-shaped reports; native direct types 0/1 keep their meaning. |
| `0x100717002` | Under the native mark-scanned mutex, consume/clear the marker, update `LeDevice+0x10`, and retain identity at `+0x18`. |
| `0x1007140c3` | Initial initiating-list builder selects the current advertised address for chip 0. |
| `0x1006d11da` | Accept-list refresh uses the same chip-0 address selection. |

The device helper checks the exact native caller return address
`0x10073b87c` before accessing that caller's advertisement pointer in its
frame at `-0x480`. It preserves the setter's bool ABI and clears the
temporary marker even if the device disappeared. The public paired-test
function and all its security callers remain native. The two connection
call sites alone use wrappers: chip 0 gets one current advertised address,
other chips tail-call the original paired test and identity-preferring
getter. No structure is expanded, key is copied into the kernel, or HCI
connection success is fabricated.

This change intentionally does **not** send `0x2027/0x202D/0x204E`. With
controller resolution unused, `0x2011` must contain the actual random RPA
that the controller sees. The existing `0x200D` accept-list initiator and
its real completion callbacks then operate normally. A recent verified
legacy ADV_IND is required; extended-only advertisements do not acquire
this fallback. RPA rotation is covered offline, but correct timing across
a live scan/connection attempt remains a hardware check.

The helper anchor includes the complete original zero padding and an
unchanged, build-specific first-function call. The load-command page is
encountered during daemon startup. Child sites stay native until this
25G332 anchor has been installed. `-mtkbtnorpa` disables the fallback;
`-mtkbtnoextadv` also disables it, and the verify-block route is required.
25G241 keeps its existing advertising/HFP patches and receives no RPA
fallback. Rebuilt binaries retain the current numeric VERSION, 1.0.9;
the new ioreg properties distinguish this build from the loaded 1.0.9.

Both validators pass against `/usr/sbin/bluetoothd` on 25G332:

```sh
python3 scripts/test-bt-page-patches.py
python3 scripts/test-bt-msbc-page-patches.py --binary /usr/sbin/bluetoothd
```

They check unique original or live-cache signatures, page boundaries,
helper padding/protection, every new call/global target, caller frames and
mutex placement. Native instruction tests exercise the real resolver
guards with mocked AH results, all address types/statuses, report scope,
marker consumption, existing bool callers, null devices, RPA rotation and
other chips under an ASan/UBSan harness. The page-hook test runs extracted
production code and verifies boot/build gates, counters, idempotence and
APFS restoration of every RPA patch together with advertising and foreign
patches, without range overflow. The installed-binary check also identified
the 25G332 HFP fill call displacement at `0x100337505`; its exact variant
preserves the existing mSBC bridge/controller scope. The 25G241 advertising
validator remains supported. These tests do not execute bluetoothd or
establish a real BLE connection.

After the user loads the rebuilt kext and starts a fresh bluetoothd, verify:

1. In IOResources `MtkWirelessBT`, `BluetoothdRpaEnabled`,
   `BluetoothdRpaHelpersReady` and `BluetoothdVerifyFixRouted` are true.
   All seven `BluetoothdRpa{Helpers,Legacy,Scan,Decode,Device,Connect,Update}Patched`
   counters become nonzero after scanning and attempting Instant Hotspot.
   They count patched pages, not connections. Existing advertising address
   counters remain nonzero; `BluetoothdPatchRangeOverflow` and
   `BluetoothdVerifyRestoreFailed` stay 0. Missing counters mean the loaded
   build or a required patch page has not been confirmed.
2. Run `scripts/Collect-AirDrop-BT.command` and click Dino in the Wi-Fi
   Personal Hotspot menu during the capture. Compare the iPhone's current
   random ADV_IND RPA with the `0x2011` entry (address type 1), and confirm
   `0x200D` filter policy 1. The log should show a random advertised address
   alongside the public resolved identity `10:A2:D3:21:31:FE`.
   `0x2027/0x202D` remain absent by design in this fallback.
3. Require a **real** LE Connection Complete (legacy or enhanced) with
   status 0, role 0 / central, for this iPhone. An incoming peripheral-role
   completion or a status `0x02` after cancel does not verify this fix.
   rapportd's outgoing PSM `0x0081` connection must complete without the
   BLE client timeout, and sharingd must successfully activate the hotspot.
4. Verify Wi-Fi association, DHCP and traffic through Dino. Repeat attempts
   and repeat after the iPhone's RPA rotates. Also check BLE advertising and
   iPhone-to-Mac BLE still work. For an A/B comparison, `-mtkbtnorpa`
   disables only this fallback while advertising remains available.

`scripts/build.sh Release` completed successfully, including `check.sh`:
all 1,129 Wi-Fi and 364 BT undefined symbols resolve, and the SSE/x87 scan
passes. No new kext has been installed or booted in this investigation;
end-to-end hardware verification remains pending.

## 1.0.12: helpers moved off the load-command page

1.0.10 placed the five RPA helpers at 0x1000026c8, the zero padding right
after the load commands (sizeofcmds 9896). The first bluetoothd launch after
boot ran, but once that page was patched in the file cache every later exec
failed: launchd `spawn failed`, `last exit reason = OS_REASON_EXEC`, 27 retries,
Bluetooth dead until reboot (2026-10-31 17:27, collector restart). Child sites
also waited for the helper page and only 2 of 7 were patched in ~6 minutes.

1.0.12 relocates the helpers to the end-of-__TEXT padding after `__eh_frame`
(0x100aa2c00; anchor 0x100aa2b88 = last 16 bytes of `__eh_frame` plus zeros,
all in one page). Child call displacements were adjusted; the assembly source
(`tests/bt-page-patches/rpa-25G332.S`, `.org 0xaa2c00`) reassembles to the same
bytes. Children are patched whenever their page is validated, since the
helper page is only faulted by their calls; page validation installs the
helpers before the first call executes. To keep 25G241 (identical child
signatures, no helpers) safe, the fallback is enabled only when
`kern.osversion` is `25G332`. `-mtkbtnorpa` still disables it.

Hardware checks: restart bluetoothd (`sudo killall bluetoothd`) several times
and confirm it comes back; all seven `BluetoothdRpa*Patched` counters become
nonzero as the paths run (`Helpers` once a child path executes).

## 1.0.13 hardware and 1.0.14 LE event-mask repair

The 2026-10-31 20:25 logs (`/tmp/rpa-log/bt3.log`, `bt4.log`) verify the RPA
fallback on 1.0.13: all seven RPA page counters are 1, the phone's random
address is selected, a real central-role LE connection completes at 37.411,
and encryption succeeds at 37.551. Remote version information completes at
37.553 and remote features at 37.572. The earlier disconnect at 37.308
explicitly reports `LE_READY_WAIT_FOR_PHY`; the next connection again logs
`PHYReady:no` and disconnects at 49.391 with `linkReady:0`. rapportd's outgoing
PSM 0x0081 is never ready. This is a later failure than address resolution.

The 25G332 x86_64 binary establishes the following path (unslid addresses):

| Site | Behavior |
|---|---|
| `0x100171183–0x100171198` | Calls backend getter `0x1001609cb`; generic backend 0 selects mask at `0x100a74481` with `cmove`. That mask is `1f 00 00 00 00 00 00 00`. Nonzero backends select `0x100a74489` (`7f fa 86 3f ff f3 0b 00`). |
| `0x10017119c–0x1001711aa` | Submits internal command ID 0xa2, whose descriptor maps to standard LE Set Event Mask 0x2001, eight parameters. |
| `0x1002163ed`, call at `0x100216747` | Remote-features callback computes DLE in `esi`, 2M in `edx`, and status 0 in `ecx`, then calls `0x100209f45` while PHYReady is false. These arguments were reversed in the initial hypothesis. |
| `0x100209f61`, `0x10020a016` | If 2M is enabled and `conn+0x99` is false, set `conn+0x84 = 4` (wait-for-phy). This helper sends no HCI command. DLE with `conn+0x88 != 1` instead selects state 3 (wait-for-length). |
| `0x10020d318 → 0x10020a45f` | Parses real LE meta 0x0c, sets PHYReady (`+0x99 = 1`), updates TX/RX PHY (`+0x90/+0x94`), and advances state 4 through native scaling/readiness. |
| `0x10020a062`, `0x10020a384` | Parses real LE meta 0x07, updates length/time at `+0x68..+0x6e`, marks length ready (`+0x88 = 1`) even if effective values were unchanged, then advances state 3. This also handles an early autonomous DLE event. |
| `0x100214c10` | Calls native ready callbacks and sets state 7 (idle). The fast-connection `+0x7d/+0x164` state-5 path remains native. |

The explicit LE Set PHY path is `0x10020ad40`, which submits internal ID
0xc8 at `0x10020ae24–0x10020ae34` (descriptor: opcode 0x2032, seven parameters).
The Set Data Length path is `0x100209d71`, internal ID 0xc2 at
`0x100209e36–0x100209e46` (0x2022, six parameters); its error callback
`0x100209ec7` uses cached effective lengths. LE Read PHY is internal ID 0xc6
(0x2030, two parameters), with Command Complete parser at `0x10017df5a`.
The ordinary feature callback simply waits for DLE/PHY observations; it does
not issue any of these three commands after the feature callback. A standard
LE Set PHY has Command Status followed by LE meta 0x0c; Set Data Length has
Command Complete and a later 0x07 only when effective values change; Read PHY
has Command Complete with handle/TX/RX. The saved logs do not constitute a
complete HCI capture proving which explicit or autonomous procedure occurred.

The concrete incompatibility is the generic mask: bits `(subevent - 1)` for
0x07 and 0x0c are both clear, so the controller is instructed to suppress the
events needed for readiness. 1.0.14 ORs exactly `0x840` into every nonzero
0x2001 mask on a firmware-loaded MediaTek USB controller. The legacy request
`0x1f` becomes `0x85f` (decimal 2143). An explicit zero mask remains zero.
Existing bits, command credits, HCI responses, negotiated values, connection
and encryption results all retain native behavior. There is one original
EP0 transfer; the shim writes only its eight mask bytes. The decision and
all diagnostic parsing use Rust `mtk_common::Reader`. No new code-page patch,
padding helper or synthetic event is added; existing RPA and VNOP_VERIFY
handling remains in place. `-mtkbtnolereadymask` restores original masks while
keeping the diagnostics enabled. The repair is on for Tahoe; it does not
depend on the RPA/advertising/verify escape hatches.

### Hardware checks after installing 1.0.14

After the user copies both Release kexts to the EFI on disk0s1 and reboots,
inspect the `MtkWirelessBT` dictionary in IOResources:

```sh
ioreg -r -n IOResources -l -w0 | rg 'MtkWirelessBT|LeReady|LeEventMask|LeSetEventMask|LeSetDataLength|LeSetPhy|LeReadPhy|LeDataLengthChange|LePhyUpdateComplete|LeChannelSelection|LeRemoteFeatures|BluetoothdRpa|BluetoothdVerify'
```

| Exact key | Expected observation |
|---|---|
| `LeReadyDiagnosticsRouted`, `LeReadyMaskEnabled` | Both true. |
| `LeSetEventMaskCommands`, `LeSetEventMaskCompleteEvents` | Nonzero after controller initialization. |
| `LeSetEventMaskLastStatus`, `LeSetEventMaskLastResponseKind` | 0 success and 14 (0x0e Command Complete). A status without a response count is unconfirmed. |
| `LeEventMaskRequested`, `LeEventMaskSent` | Normally 31 (0x1f) and 2143 (0x85f) on the generic backend. A later nonzero mask preserves all its other bits. A later explicit zero remains zero. |
| `LeEventMaskRewritten` | At least 1 for the legacy mask. |
| `LeEventMaskDleEnabled`, `LeEventMaskPhyEnabled` | Both 1 for a running nonzero mask. |
| `LeEventMaskWriteErrors`, `LeSetEventMaskUsbErrors`, `LeReadyMalformedEvents` | 0. |
| `LeRemoteFeaturesEvents`, `LeRemoteFeaturesLastStatus` | Count increases, status 0 for the phone. |
| `LePhyUpdateCompleteEvents`, `LePhyUpdateCompleteLastStatus` | Count increases, status 0 if the real PHY procedure succeeds. `LastHandle` must match the phone's HCI handle. TX/RX are `LastValue0/LastValue1` (1=1M, 2=2M, 3=coded). |
| `LeDataLengthChangeEvents` | Increases if the effective data length changes. `LastValue0..3` are TX octets/time and RX octets/time. |
| `LeSetPhyCommands`, `LeSetDataLengthCommands`, `LeReadPhyCommands` | Show which explicit standard commands actually reached USB. Zero is meaningful: the feature callback itself does not submit them. |
| `LeChannelSelectionEvents` | Observes subevent 0x14 when enabled by the native mask. The repair does not enable this unrelated bit. Zero with legacy mask is expected. |
| `BluetoothdRpa{Helpers,Legacy,Scan,Decode,Device,Connect,Update}Patched` | Existing seven counters remain nonzero as the paths execute. |
| `BluetoothdVerifyFixRouted`, `BluetoothdPatchRangeOverflow`, `BluetoothdVerifyRestoreFailed` | True, 0, 0 respectively. |

For each exact prefix `LeSetEventMask`, `LeSetDataLength`, `LeSetPhy`,
`LeReadPhy`, the suffixes are `Commands`, `UsbErrors`, `LastUsbStatus`,
`StatusEvents`, `CompleteEvents`, `LastStatus`, `LastResponseKind`,
`LastHandle`, `LastArg0`, `LastArg1`, `Serial`, `ResponseSerial`.
`LastResponseKind` is 15 for Command Status or 14 for Command Complete.
`LeSetDataLengthLastArg0/1` are requested TX octets/time;
`LeSetPhyLastArg0` packs all-PHY, TX-PHY and RX-PHY bytes at bits 0, 8 and 16,
and `LastArg1` is PHY options. `LeReadPhyLastArg0/1` become actual TX/RX PHY
from a successful real Command Complete.

For each exact prefix `LeDataLengthChange`, `LePhyUpdateComplete`,
`LeChannelSelection`, `LeRemoteFeatures`, the suffixes are `Events`,
`LastHandle`, `LastStatus`, `LastValue0`, `LastValue1`, `LastValue2`,
`LastValue3`, `Serial`. Status is 4294967295 for subevents without a status
field (0x07/0x14). Remote feature bytes are two little-endian u32 values:
the observed `FD 79 01 F7 F9 00 00 00` gives `0xf70179fd`, `0x000000f9`.
`LeReadySerial` orders relevant command/event observations, so an event with
a smaller serial than remote features demonstrates early negotiation.
Counters are cumulative and last values describe the latest observation
across loaded MediaTek controllers; compare handles, deltas and
`LeReadyControllerID` rather than attributing every event to the phone.

Use `/usr/bin/log`, since zsh shadows `log`. A successful attempt must get
past `PHYReady:no`/wait-for-phy, complete rapportd's PSM 0x0081, activate the
phone's hotspot, and associate/get DHCP/pass traffic through Wi-Fi. Repeat
after RPA rotation and check advertising, AirDrop and HFP. If the mask repair
is acknowledged but PHY events remain absent, the new command/status/event
counts distinguish an omitted procedure from controller rejection or an
event that was seen on USB but ignored by userspace. Do not infer that HCI
encryption alone verifies Hotspot. End-to-end verification remains pending.

Offline checks exercise Rust framing/bit preservation, original event order,
unchanged-value and early DLE observations, the production EP0/interrupt
wrappers with the real Rust ABI under ASan/UBSan, descriptor write/prepare
failure, asynchronous callback lifetime and failures, foreign-pipe isolation,
and existing 0xFD3C compatibility. The existing validators also check the
25G332 mask selection, command descriptors and readiness instruction bytes.

Validation: workspace tests, both FFI feature suites, both Bluetooth page
validators and the mSBC codec/integration tests pass. `scripts/build.sh Release`
exits 0, including `check.sh`: all 1129 Wi-Fi and 365 BT undefined symbols
resolve, with no SSE/AVX/x87 instructions. Both Release bundles report 1.0.14.
