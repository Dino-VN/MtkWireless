#!/usr/bin/env python3
"""Observe native/BSD AWDL data counters for one explicitly selected kext UUID."""
import argparse
from datetime import datetime
import json
from pathlib import Path
import plistlib
import re
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--expected-uuid', required=True)
parser.add_argument('--seconds', type=int, default=10)
parser.add_argument('--probe', action='store_true', help='Send three IPv6 all-nodes pings on awdl0')
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
if not 0 <= args.seconds <= 60:
    parser.error('--seconds must be between 0 and 60')

def run(command):
    return subprocess.check_output(command, stderr=subprocess.PIPE, timeout=45)

def snapshot():
    registry = run(['ioreg', '-a', '-r', '-c', 'AirportMtkAwdlInterface'])
    if not registry.strip():
        raise RuntimeError('No native AWDL interface is present on this boot.')
    interfaces = plistlib.loads(registry)
    if len(interfaces) != 1:
        raise RuntimeError('Expected one AirportMtkAwdlInterface')
    native = {k: v for k, v in interfaces[0].items() if k.startswith('MTKAWDL')
              and isinstance(v, (int, bool, str))}
    transport = plistlib.loads(run(['ioreg', '-a', '-r', '-c', 'AirportMtkWlan']))
    hal = {k: v for row in transport for k, v in row.items()
           if k.startswith('MTKAWDLTX') and isinstance(v, (int, bool, str))}
    table = run(['netstat', '-ibn', '-I', 'awdl0']).decode(errors='replace')
    row = next((line.split() for line in table.splitlines() if '<Link#' in line), None)
    if not row or len(row) < 11:
        raise RuntimeError('Missing awdl0 BSD link counters')
    bsd = dict(zip(('rx_packets', 'rx_errors', 'rx_bytes', 'tx_packets', 'tx_errors', 'tx_bytes'),
                   map(int, row[4:10])))
    return {'native': native, 'bsd': bsd, 'hal': hal}

def offload_state(native, interface):
    # Tahoe native-host activation deliberately advertises software TSO and
    # partial checksumming after snapshotting the driver's physical assists.
    # ifconfig options alone cannot establish unsupported hardware offload.
    flags = re.findall(r'\b(?:TSO4|TSO6|PARTIAL_CSUM|ZEROINVERT_CSUM)\b', interface)
    keys = ('MTKAWDLDriverHardwareAssists', 'MTKAWDLBSDPreparedOffload',
            'MTKAWDLBSDPreparedCapabilities', 'MTKAWDLBSDPreparedEnabled')
    result = {'bsd_flags': flags, 'status': 'unverified', 'reasons': []}
    if native.get('MTKAWDLBSDOffloadStatus') != 0:
        result['reasons'].append('BSD driver offload preparation failed or its diagnostic is absent.')
    elif any(key not in native for key in keys):
        result['reasons'].append('The loaded build lacks the verified driver offload snapshot; BSD flags may be Skywalk software assists.')
    elif native[keys[0]] or native[keys[1]] or (native[keys[2]] | native[keys[3]]) & 0xe0e3:
        result['status'] = 'failed'
        result['reasons'].append('The driver advertises checksum/TSO/LRO offload it cannot perform.')
    else:
        result['status'] = 'passed'
        result['interpretation'] = ('BSD flags are consistent with Tahoe native-host software GSO/copy checksumming; '
                                    'the driver advertises no physical offload. Packet correctness still requires traffic validation.')
    return result

def check():
    loaded = run(['kmutil', 'showloaded', '--list-only']).decode(errors='replace')
    identity = next((line for line in loaded.splitlines()
                     if 'com.eiyuumiru.AirportMtkWlan (' in line), '')
    result = {'time': datetime.now().astimezone().isoformat(),
              'expected_uuid': args.expected_uuid.upper(), 'loaded': identity,
              'status': 'unverified', 'reasons': []}
    if not re.search(r'\b' + re.escape(args.expected_uuid) + r'\b', identity, re.I):
        result['reasons'].append('The intended kext UUID is not loaded.')
        return result
    started = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    before = snapshot()
    if args.probe:
        probe = subprocess.run(['ping6', '-c', '3', '-i', '0.5', 'ff02::1%awdl0'],
                               capture_output=True, text=True, timeout=10)
        result['probe'] = {'exit_code': probe.returncode, 'stdout': probe.stdout, 'stderr': probe.stderr}
    time.sleep(args.seconds)
    after = snapshot()
    result.update(before=before, after=after)
    keys = ('MTKAWDLNativeTxPackets', 'MTKAWDLNativeRxPackets', 'MTKAWDLNativeDroppedPackets')
    missing = [key for key in keys if key not in before['native'] or key not in after['native']]
    delta = {key: after['native'].get(key, 0) - before['native'].get(key, 0) for key in keys}
    bsd_delta = {key: after['bsd'][key] - before['bsd'][key] for key in before['bsd']}
    result.update(native_delta=delta, bsd_delta=bsd_delta, missing_counters=missing)
    stages = ('MTKAWDLDataTxSubmitted', 'MTKAWDLDataTxFailed', 'MTKAWDLDataTxExpired', 'MTKAWDLDataTxDeferred',
              'MTKAWDLDataRxFrames', 'MTKAWDLDataRxInvalid', 'MTKAWDLDataRxUnknownPeer',
              'MTKAWDLDataRxWrongDestination', 'MTKAWDLDataRxReplay', 'MTKAWDLDataRxDecoded')
    transport_delta = {key: after['native'][key] - before['native'][key] for key in stages
                       if key in before['native'] and key in after['native']}
    result['transport_delta'] = transport_delta
    result['hal_delta'] = {key: after['hal'][key] - before['hal'][key]
                           for key in after['hal'] if key in before['hal'] and not key.startswith('MTKAWDLTXLast')}
    result['firmware_tx_status_available'] = 'MTKAWDLTXDataAcked' in after['hal']
    if any(value < 0 for value in result['hal_delta'].values()):
        result['reasons'].append('HAL counters reset during observation.')
    result['interface'] = run(['ifconfig', 'awdl0']).decode(errors='replace')
    result['transport_findings'] = []
    if any(value < 0 for value in transport_delta.values()):
        result['reasons'].append('Transport counters reset during observation.')
    else:
        for key, finding in (
            ('MTKAWDLDataTxFailed', 'HAL rejected data TX; inspect MTKAWDLDataTxStatus.'),
            ('MTKAWDLDataTxExpired', 'Queued data expired while waiting for a usable radio/peer slot.'),
            ('MTKAWDLDataRxInvalid', 'Raw AWDL data failed wire/header validation.'),
            ('MTKAWDLDataRxUnknownPeer', 'Raw AWDL data came from a peer absent from the authorized data cache.'),
            ('MTKAWDLDataRxWrongDestination', 'Raw AWDL data addressed another interface.'),
            ('MTKAWDLDataRxReplay', 'Duplicate/old AWDL data was rejected by the replay window.')):
            if transport_delta.get(key, 0) > 0:
                result['transport_findings'].append(finding)
        if transport_delta.get('MTKAWDLDataTxDeferred', 0) > 0:
            result['transport_findings'].append('Data TX was deferred with its original packet retained; inspect HAL blocker counters.')
        for key, finding in (
            ('MTKAWDLTXNoGrant', 'TX waited for a matching live radio grant.'),
            ('MTKAWDLTXBusy', 'TX waited for firmware configuration or transport readiness.'),
            ('MTKAWDLTXNoPeer', 'Unicast TX waited for a firmware station.'),
            ('MTKAWDLTXBackpressure', 'Shared TXWI/DMA ring had no capacity.'),
            ('MTKAWDLTXCopyFailed', 'TX attempt allocation failed; original data was retained.'),
            ('MTKAWDLTXDMAFailed', 'TX DMA submission failed; inspect MTKAWDLTXLastErrno.'),
            ('MTKAWDLTXBadFrame', 'HAL permanently rejected a malformed frame or wrong local identity.')):
            if result['hal_delta'].get(key, 0) > 0:
                result['transport_findings'].append(finding)
        for key, finding in (
            ('MTKAWDLTXDataFailed', 'Firmware reported an AWDL unicast queue, RTS or ACK timeout.'),
            ('MTKAWDLTXMulticastFailed', 'Firmware reported an AWDL multicast transmission failure.'),
            ('MTKAWDLTXActionFailed', 'Firmware reported an AWDL discovery-action transmission failure.')):
            if result['hal_delta'].get(key, 0) > 0:
                result['transport_findings'].append(finding)
                result['reasons'].append(finding)
        if transport_delta.get('MTKAWDLDataRxFrames') == 0:
            result['transport_findings'].append('No raw AWDL data reached the receiver during this interval.')
    if not after['native'].get('MTKAWDLDataPathEnabled', False):
        result['reasons'].append('The native datapath is disabled or its diagnostic is absent.')
    result['offload'] = offload_state(after['native'], result['interface'])
    result['reasons'].extend(result['offload']['reasons'])
    try:
        sanity = run(['/usr/bin/log', 'show', '--start', started, '--style', 'compact',
                      '--predicate', 'process == "kernel" AND eventMessage CONTAINS "AWDL SANITY FAILURE"']).decode(errors='replace')
        failures = [line for line in sanity.splitlines() if 'AWDL SANITY FAILURE' in line]
        result['aw_sanity_failures'] = failures
        if failures:
            result['reasons'].append('Native AWDL sanity failures occurred during observation.')
    except (OSError, subprocess.SubprocessError) as exc:
        result['reasons'].append('Could not verify native AWDL sanity logs: ' + str(exc))
    if missing:
        result['reasons'].append('This loaded build does not expose all native data counters.')
    if any(value < 0 for value in (*delta.values(), *bsd_delta.values())):
        result['reasons'].append('Counters reset during observation.')
    if delta['MTKAWDLNativeTxPackets'] <= 0 or bsd_delta['tx_packets'] <= 0:
        result['reasons'].append('No new native/BSD TX was observed.')
    if delta['MTKAWDLNativeRxPackets'] <= 0 or bsd_delta['rx_packets'] <= 0:
        result['reasons'].append('No new native/BSD RX was observed.')
    result['limitations'] = ['Local multicast ping replies do not prove RF traffic.',
                            'Native TX counts accepted protocol frames, not firmware ACKs.',
                            'DataTxSubmitted counts successful HAL/DMA submissions, not firmware ACKs.',
                            'TXDataAcked counts firmware-reported unicast ACKs; multicast/action completion does not prove peer reception.',
                            'Transport properties publish once per second; short intervals can miss updates.',
                            'Data counters do not validate an AirDrop file transfer.']
    if not result['reasons']:
        result['status'] = 'passed'
    return result

try:
    result = check()
except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, plistlib.InvalidFileException) as exc:
    result = {'status': 'unverified', 'reasons': [str(exc)]}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
raise SystemExit(0 if result['status'] == 'passed' else 1)
