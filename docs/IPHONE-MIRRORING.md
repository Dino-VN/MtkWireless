# iPhone Mirroring discovery diagnostics

The 2026-11-04 capture on macOS 26.7.2 (25G332), with Dino on iOS 27.3,
finds the phone as `RPRemoteDisplayDevice`, then rejects it:

```
status flags: RPStatusFlags(rawValue: 25690116)
not a superset of: RPStatusFlags(rawValue: 512), ignoring device
```

After five seconds the app reports `deviceNotFound`. This is a discovered
device being filtered, rather than proof that USB or all Wi-Fi discovery
failed. Rapport names bit `0x200` **WiFiP2P**. This remote-device flag is
distinct from the driver's local CARD_CAPABILITIES AWDL bit.

Both en0 and awdl0 were active. A CoreWLAN probe reported power on and an
active interface. The Nearby advertiser logged Wi-Fi state `Unknown` on
startup. A later CUSystemMonitor probe, with its Wi-Fi event handler installed,
reported state 20 (`NotConnected`), rather than Unknown. Static sharingd
analysis shows that its Nearby advertiser reads CUSystemMonitor, and permits
the WiFiP2P advertisement only with a known/on state, an active NearbyAction
scanner and Manatee availability (also subject to the HostAP check).
These observations do not establish a Wi-Fi packet-processing defect.

The separate experimental AKS helper has an incomplete escrow pairing.
Converting the iOS16 four-field peer-state reply to the newer Mac schema
removed the decode failure, but preserved the real
`escrow_record_not_found` state. A Wi-Fi build does not resolve that gate.

Evidence is retained outside the repository in
`~/Library/Application Support/iPhoneDiagnostics/mac-wifi-discovery-evidence.json`
and `native-bridge-focused.log` (23:27:22 discovery/rejection). REA evidence
`ev_b4a3025155a125ed6e310ede12bc5858a42f6a3a975607fd711fe76245df3635`
covers `-[SDNearbyAgent _bleNearbyInfoAdvertiserUpdate]` in the verified
25G332 sharingd binary. Static analysis does not observe runtime execution.

## 1.0.10 diagnostic build

This release adds observations of successful POWER, STATE and
CARD_CAPABILITIES queries. It does not change radio scheduling, RX/TX,
capability bits or authentication. Getter instrumentation performs only
atomic integer stores/increments; the existing controller watchdog publishes
the values under its command gate once per second.

| ioreg property | Meaning |
| --- | --- |
| `MTKWiFiQueryDiagnosticVersion` | 1 identifies this diagnostic schema |
| `MTKWiFiPowerQueryCount` | Successful POWER queries |
| `MTKWiFiStateQueryCount` | Successful STATE queries |
| `MTKWiFiCapabilitiesQueryCount` | Successful CARD_CAPABILITIES queries |
| `MTKWiFiReportedPower` | Last POWER reply's first radio value |
| `MTKWiFiReportedState` | Last STATE reply, in net80211 state numbering |
| `MTKWiFiReportedAwdlCapability` | Last reply's local AWDL capability, 0 or 1 |
| `MTKWiFiPowerPreference` | Current user radio preference |
| `MTKWiFiPMPowerState` | Current power-management state |
| `MTKWiFiInterfaceFlags` | Current infrastructure if_flags |
| `MTKWiFiNativeLinkStatus` | Current controller link-status flags |

Last-reply fields start at `4294967295` until the first successful query.
Counters are independent observations, not an atomic snapshot of all fields.
STATE values are not CUSystemMonitor's 0/10/20/30/40 state numbering.

After installing the Wi-Fi kext and rebooting, run:

```sh
python3 scripts/capture-mirroring-wifi.py --duration 30
```

Retry Mirroring during the capture. It writes discovery.log and before/after
driver/interface state.json to a timestamped build/diagnostics directory.
It does not restart services or change network settings. A zero counter means
that this driver getter was not observed, not that the device has no Wi-Fi.

## Validation

Release build and `scripts/check.sh` passed: all 1130 Wi-Fi and 373 Bluetooth
undefined symbols resolve, with no prohibited SSE/x87 instructions. The Wi-Fi
capability regression passed and the collector completed a five-second capture.
The AWDL layout/protocol and other host cases passed; `test-mi-discovery.py`
has a pre-existing fixture compile error (missing `inputNativeView`), reproduced
against the unchanged HEAD source. Full-suite success is not claimed.
Hardware verification of the new observations is recorded below; usable Mirroring remains unverified.

## Post-reboot capture, 2026-11-05

`kmutil showloaded` confirms Wi-Fi 1.0.10 and Bluetooth 1.0.9.
In `build/diagnostics/mirroring-wifi-20261105-000211`, the POWER counter
advances 440 to 474 with reply 1. The capability counter is 68 with AWDL
reply 1, native link status is 3, and both en0 and awdl0 remain active.
STATE was not queried (counter 0); ioreg serializes its unobserved sentinel
as -1 in the JSON capture. This is not an observed disconnected-state reply.

At 00:02:32, discovery reports remote flags 0x1880386 including WiFiP2P.
The original daemon then establishes an AWDL session. In the later
`build/diagnostics/mirroring-session-20261105-002001/mac.log`, the server
sends `readyForUnlock` at 00:20:45.711. Local Mac authorization is valid,
but authentication fails with Code 11, `Key device has not been registered
by Lock yet`, and the app returns `unlockRequired`. The same trial reports
an AKS session failure and invalid AKS pairing. Repeated physical unlocks
alone have not completed that registration.

The restarted helper bridge successfully reads operation 0x39 (68-byte
normalized peer-state response), but creation of a registration session
(operation 0x2c, flags 0x81) returns 0xe00002e2 (`NotPermitted`).
Whether unlocking the iOS16 helper resolves this current creation failure
is pending a new live check. The separate wrapped escrow-record gate remains
unproven. This evidence does not establish an authenticated screen/control
session or justify changing driver capability bits.

## Native AWDL traffic-registration finding (2026-11-05)

The native service now discovers Dino as a ReplicatorServices phone, but the
first native control attempt reaches AWDL traffic registration and fails. The
runtime trace shows a successful reservation, then
`APPLE80211_IOC_AWDL_PEER_TRAFFIC_REGISTRATION` on `awdl0` with the verified
184-byte request returning `0xe00002ca` after exactly 3000 ms. macOS then
reports `awdlDegraded` and tears down the control stream. Static inspection of
macOS 26.7.2's original BootKernelExtensions.kc contains a three-second
native peer-cache wait consistent with this delay; a kernel runtime trace
has not yet identified the exact failed branch. The native implementation uses a 0x1d0
cache-control transaction and signals completion only after the firmware cache
indication. Our interface already submits the matching 0x1d0 completion
message, so changing request size or fabricating success would be unsafe.

A later retry also hit macOS's independent `Resource busy` guard because the
failed registration left the service session active briefly. This is a cleanup
symptom, not proof that the peer registration succeeded.

The separate authentication gate remains: the native session reaches
`readyForUnlock`, then sharing authentication returns Code 11 (`Key device has
not been registered by Lock yet`) and `unlockRequired`. The Mac is an AMD
Ryzen/VirtualSMC system with no AppleSEPManager/AppleT2Controller instance,
so these hardware services are not present in IORegistry. This is a relevant
authentication limitation, but has not been established as the sole cause
of this session's failure.
Discovery and AWDL traffic are therefore not equivalent to an authenticated,
controllable Mirroring session.

Validation after this investigation: `scripts/test-awdl.sh` passes layout,
protocol, native controls, start, window, radio and sync tests; its known
`test-mi-discovery.py` fixture still fails to compile because the fixture calls
missing `inputNativeView`. Rust workspace tests pass (105 tests).

## 1.0.11 cache-path observations

The original BCM driver submits event `0x33` through Controller::postMessage.
PostOffice delivers it to VirtualInterface::postMessage, which calls
SkywalkInterface::reportDataPathEvents. PeerManager then passes the cache
indication to P2PDataPathManager unless its separate `+0x558` state is set.
An AWDL high-priority add indication signals peer-manager event 8, which the
traffic-registration path waits for. These are static code paths, not proof
that our completion currently traverses them at runtime.

This build observes cache requests, firmware programming results/durations,
completion submissions and event delivery at reportDataPathEvents. The new
override forwards every original argument to the superclass and preserves
its return value. The cache handler retains its previous return behavior,
including when a post submission fails; that failure now has a counter.
It does not change request/completion bytes, native gating, RX/TX, cache
ownership, scheduling or authentication. The experimental AKS bridge stays off.

`MTKAWDLCacheDiagnosticVersion=1` identifies the properties on
AirportMtkAwdlInterface. `Requests`/`Failures`, `FirmwareCalls`/`FirmwareFailures`,
`Posts`/`PostFailures` and `Events`/`InvalidEvents` count successive stages.
An invalid event has a null buffer or a length other than 0x1d0; it is still
forwarded unchanged. `Result`, `FirmwareStatus`, `PostStatus` and `EventStatus`
are unsigned IOReturns, initially NotReady. Firmware calls exclude a delete
whose station is retained by traffic ownership and the existing rollback
removal after a cache commit failure.

`RequestTimeUS`, `PostTimeUS` and `EventTimeUS` are monotonic microseconds;
`DurationUS`, `MaxDurationUS` and `FirmwareDurationUS` measure handler and
firmware time. `RequestVersionOp`/`EventVersionOp` contain the little-endian
first eight prefix bytes (version in low 32 bits, operation in high 32 bits).
`RequestAddressTag`/`EventAddressTag` contain the following eight bytes (MAC
then the still-uninterpreted word at +0xe). No keys or request bodies are
captured. Fields use independent atomic observations, published once per
second by the existing controller watchdog gate, including after AWDL sync
is disabled; they are not a coherent transaction
snapshot and do not prove that a native waiter woke. A zero Event count
does not establish which downstream branch failed.

The capture script now includes these properties and wifip2pd/AWDL/cache/
authentication messages. Hardware testing requires installing this build
and rebooting. Native screen display and working input remain unverified.

Offline verification: Release build/kernel symbol and SSE/x87 checks pass,
and the new production cache/event test passes under ASan/UBSan. The upstream
layout/protocol/native-control/window/sync tests pass. The already broken
MI discovery extraction fixture now also lacks the new diagnostic fields,
helper method and IOReturn post stub; its upstream file remains unchanged.
The dedicated cache test executes those paths with real Rust cache ownership.
