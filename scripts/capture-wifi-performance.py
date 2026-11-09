#!/usr/bin/env python3
"""Read-only, key-free Wi-Fi/AirDrop telemetry; run transfers separately."""
import argparse
import csv
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import plistlib
import re
import subprocess
import tempfile
import time
from xml.parsers.expat import ExpatError


CLASSES = {'infrastructure': 'AirportMtkWlan', 'awdl': 'AirportMtkAwdlInterface'}
BUNDLES = ('com.dinovn.MtkWirelessWifi', 'com.dinovn.MtkWirelessBT')
# Operational sample budget, not a byte-memory guarantee. Conservatively allow
# ceil(duration / interval) + 1 observations, including initial and final samples.
# Reject oversized requests; never clamp, drop, or truncate collected samples.
MAX_NOMINAL_SAMPLES = 10000
# Exact properties verified against the controller, AWDL interface and HAL
# publishers. Never select by prefix, traverse diagnostic dictionaries, or
# accept strings/data as numbers. There is no published delivered-byte counter.
COUNTERS = {
    'infrastructure': (
        'MTKWiFiPowerQueryCount', 'MTKWiFiStateQueryCount',
        'MTKWiFiCapabilitiesQueryCount', 'MTKWiFiRxWrongBSS',
        'MTKWiFiRxDeauth', 'MTKWiFiRxDisassoc',
        'MTKAWDLInfraPSSleeps', 'MTKAWDLInfraPSWakes', 'MTKAWDLInfraPSFailed',
        'MTKAWDLROCEvents', 'MTKAWDLROCGrants', 'MTKAWDLROCRejected',
        'MTKAWDLROCResponseTimeouts', 'MTKAWDLROCSubmitted',
        'MTKAWDLROCSendFailed', 'MTKAWDLROCScanBlocked',
        'MTKAWDLROCJoinBlocked', 'MTKAWDLROCBusyBlocked',
        'MTKAWDLScanPreempted', 'MTKAWDLTXSubmitted',
        'MTKAWDLTXBackpressure', 'MTKAWDLTXDMAFailed',
        'MTKAWDLRxBAStarted', 'MTKAWDLRxBAStopped', 'MTKAWDLRxBAFailed',
        'MTKAWDLTXDataAcked', 'MTKAWDLTXDataFailed',
        'MTKAWDLTXMulticastCompleted', 'MTKAWDLTXMulticastFailed',
        'MTKAWDLUMITimeouts', 'MTKAWDLUMICanceled', 'MTKAWDLUMIStaleTXS',
        'MTKAWDLUMIExhausted', 'MTKAWDLUMIBackpressure',
    ),
    'awdl': (
        'MTKAWDLNativeTxPackets', 'MTKAWDLNativeRxPackets',
        'MTKAWDLNativeDroppedPackets', 'MTKAWDLDataTxSubmitted',
        'MTKAWDLDataTxFailed', 'MTKAWDLDataTxDeferred', 'MTKAWDLDataTxExpired',
        'MTKAWDLDataRxFrames', 'MTKAWDLDataRxInvalid',
        'MTKAWDLDataRxUnknownPeer', 'MTKAWDLDataRxWrongDestination',
        'MTKAWDLDataRxReplay', 'MTKAWDLDataRxDecoded',
        'MTKAWDLDataRxDuplicate', 'MTKAWDLDataRxTooOld',
        'MTKAWDLNativeIPv6Decoded', 'MTKAWDLNativeIPv6Duplicate',
        'MTKAWDLADDBARequests', 'MTKAWDLADDBAAccepted',
        'MTKAWDLADDBAFailed', 'MTKAWDLADDBADropped', 'MTKAWDLDELBAReceived',
        'MTKAWDLDELBADropped',
        'MTKAWDLDensePresenceTransitions', 'MTKAWDLServiceTicks',
        'MTKAWDLReserveAttempts', 'MTKAWDLGrantObservations',
        'MTKAWDLLateWindowObservations', 'MTKAWDLShortGrantObservations',
        'MTKAWDLAWStartNotifications', 'MTKAWDLAWEndNotifications',
    ),
}
GAUGES = {
    'infrastructure': (
        'MTKWiFiQueryDiagnosticVersion', 'MTKWiFiReportedPower',
        'MTKWiFiReportedState', 'MTKWiFiReportedAwdlCapability',
        'MTKWiFiPowerPreference', 'MTKWiFiPMPowerState',
        'MTKWiFiInterfaceFlags', 'MTKWiFiNativeLinkStatus',
        'MTKAWDLDiagnosticVersion', 'MTKAWDLROCStateObservedAtUS',
        'MTKAWDLROCGrantDelayUS', 'MTKAWDLROCMaxGrantDelayUS',
        'MTKAWDLROCQueueDelayUS', 'MTKAWDLROCMaxQueueDelayUS',
        'MTKAWDLROCChannel', 'MTKAWDLBSSBaseChannel',
        'MTKAWDLTXPeer0RateMbps', 'MTKAWDLTXPeer1RateMbps',
        'MTKAWDLTXPeer2RateMbps', 'MTKAWDLUMIActive', 'MTKAWDLUMITerminal',
        'MTKAWDLUMILastReason',
    ),
    'awdl': (
        'MTKAWDLNativeLinkState', 'MTKAWDLDataTxPending',
        'MTKAWDLDataTxOldestAgeUS', 'MTKAWDLDataRx12BitStreams',
        'MTKAWDLDataRx16BitStreams', 'MTKAWDLHostChannel',
        'MTKAWDLHostPeerCount', 'MTKAWDLDataPeerCount',
        'MTKAWDLReserveWaitUS', 'MTKAWDLReserveMaxWaitUS',
        'MTKAWDLReserveLeadUS', 'MTKAWDLReserveDurationMS',
        'MTKAWDLDriverHardwareAssists', 'MTKAWDLBSDCurrentOffload',
        'MTKAWDLBSDCurrentCapabilities', 'MTKAWDLBSDCurrentEnabled',
    ),
}
BOOLEANS = {
    'infrastructure': (
        'MTKAWDLEnabled', 'MTKAWDLInterfaceReady',
        'MTKAWDLNativeDataPathAvailable', 'MTKAWDLInfraPowerSave',
        'MTKAWDLROCPending', 'MTKAWDLROCGranted', 'MTKAWDLScanSuspended',
        'MTKWiFiPMF',
    ),
    'awdl': (
        'MTKAWDLDataPathEnabled', 'MTKAWDLSyncEnabled',
        'MTKAWDLNativeQueuesRegistered', 'MTKAWDLTrafficActive',
        'MTKAWDLDensePresence', 'MTKAWDLAWWindowOpen',
    ),
}
METRICS = {
    source: {name: kind for kind, table in (
        ('counter', COUNTERS), ('gauge', GAUGES), ('boolean', BOOLEANS))
        for name in table[source]}
    for source in CLASSES
}
THROUGHPUT_NOTE = (
    'No cumulative delivered-byte counter is published by these nodes. '
    'Measure throughput using transfer byte size and elapsed time or your LAN '
    'iperf result, not PHY ladder rates or packet counters. An idle retained '
    'AWDL rate is not active link capacity.'
)


def positive_seconds(value):
    try:
        number = float(value)
    except ValueError:
        raise argparse.ArgumentTypeError('must be a positive finite number') from None
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError('must be a positive finite number')
    return number


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--duration', type=positive_seconds, default=30.0,
                        metavar='SECONDS')
    parser.add_argument('--interval', type=positive_seconds, default=1.0,
                        metavar='SECONDS')
    parser.add_argument('--output', type=Path, metavar='NEW_DIRECTORY',
                        help='must not exist; default: unique build/diagnostics/wifi-performance-*')
    args = parser.parse_args(argv)
    # Multiplication avoids overflowing duration / interval for extreme finite
    # floats. An overflowing product is safe: every finite duration fits then.
    if args.duration > args.interval * (MAX_NOMINAL_SAMPLES - 1):
        parser.error(f'duration/interval exceeds the {MAX_NOMINAL_SAMPLES} nominal sample budget')
    return args


def query(command):
    """Never retain/print stderr, failures, or partial command output."""
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, timeout=5, check=False)
    except FileNotFoundError:
        return b'', 'unavailable'
    except subprocess.TimeoutExpired:
        return b'', 'timeout'
    except OSError:
        return b'', 'command_failed'
    return (result.stdout, 'ok') if result.returncode == 0 else (b'', 'command_failed')


def numeric(value):
    return type(value) is int and 0 <= value <= 0xffffffffffffffff


def registry_nodes(raw, source):
    """Extract only exact typed scalars; metadata IDs stay transient in memory."""
    if not raw or len(raw) > 8 * 1024 * 1024:
        return [], 'invalid_plist'
    try:
        roots = plistlib.loads(raw)
    except (ValueError, TypeError, OverflowError, RecursionError, ExpatError):
        return [], 'invalid_plist'
    if isinstance(roots, dict):
        roots = [roots]
    if not isinstance(roots, list) or any(not isinstance(root, dict) for root in roots):
        return [], 'invalid_plist'
    pending, seen, nodes = list(reversed(roots)), set(), []
    while pending:
        entry = pending.pop()
        if not isinstance(entry, dict) or id(entry) in seen:
            continue
        seen.add(id(entry))
        if len(seen) > 4096:
            return [], 'invalid_plist'
        children = entry.get('IORegistryEntryChildren')
        if isinstance(children, list):
            pending.extend(reversed(children))
        if entry.get('IOObjectClass') != CLASSES[source]:
            continue
        selected = {}
        for name, kind in METRICS[source].items():
            value = entry.get(name)
            valid = type(value) is bool if kind == 'boolean' else numeric(value)
            if valid:
                selected[name] = value
        identity = entry.get('IORegistryEntryID')
        nodes.append((identity if numeric(identity) else None, selected))
    return nodes, 'ok' if nodes else 'missing'


class Collector:
    def __init__(self):
        self.identities = {source: {} for source in CLASSES}
        self.next_node = {source: 0 for source in CLASSES}

    def snapshot(self):
        sources = {}
        for source, classname in CLASSES.items():
            raw, status = query(['/usr/sbin/ioreg', '-a', '-r', '-c', classname])
            nodes = []
            if status == 'ok':
                nodes, status = registry_nodes(raw, source)
            selected = {}
            for identity, metrics in nodes:
                # Anonymous stable slots prevent merging controllers or continuing
                # counters across re-enumeration. No registry ID/name is saved.
                slot = self.identities[source].get(identity) if identity is not None else None
                if slot is None:
                    slot = self.next_node[source]
                    self.next_node[source] += 1
                    if identity is not None:
                        self.identities[source][identity] = slot
                selected[str(slot)] = {'identity_available': identity is not None,
                                       'metrics': metrics}
            sources[source] = {'status': status, 'nodes': selected}
        return sources


def loaded_versions():
    versions = {}
    for bundle in BUNDLES:
        raw, status = query(['/usr/bin/kmutil', 'showloaded', '--list-only',
                             '--show', 'loaded', '--bundle-identifier', bundle])
        version = None
        if status == 'ok':
            # Match actual showloaded rows, not echoed identifiers, UUIDs,
            # kernel addresses, other modules, or arbitrary version strings.
            pattern = (r'^\s*\d+\s+\d+(?:\s+0x[0-9a-fA-F]+){3}\s+'
                       + re.escape(bundle) + r'\s+\(([0-9]+(?:\.[0-9]+){0,2})\)(?:\s|$)')
            found = {match.group(1) for line in raw.decode('utf-8', errors='replace').splitlines()
                     for match in [re.match(pattern, line)]
                     if match and len(match.group(1)) <= 32}
            status = 'observed' if len(found) == 1 else ('ambiguous' if found else 'not_observed')
            if status == 'observed':
                version = found.pop()
        versions[bundle] = {'status': status, 'version': version}
    return versions


def collect_samples(collector, duration, interval, samples):
    origin, due = time.monotonic(), 0.0
    while True:
        wait = due - (time.monotonic() - origin)
        while wait > 0:
            time.sleep(min(wait, 60))
            wait = due - (time.monotonic() - origin)
        started = time.monotonic()
        sample = {'index': len(samples),
                  'timestamp_utc': datetime.now(timezone.utc).isoformat(),
                  'elapsed_seconds': started - origin}
        sample['sources'] = collector.snapshot()
        ended = time.monotonic()
        sample['query_seconds'] = ended - started
        samples.append(sample)
        elapsed = ended - origin
        if elapsed >= duration:
            return
        # Aim start-to-start, do not burst through a backlog of missed ticks.
        # The final observation is due at duration even if interval is longer.
        due = min(duration, max(sample['elapsed_seconds'] + interval, elapsed))


def metric_summary(samples, source, node, name, kind):
    observations = []
    for index, sample in enumerate(samples):
        metrics = sample['sources'][source]['nodes'].get(node, {}).get('metrics', {})
        if name in metrics:
            observations.append((index, sample['elapsed_seconds'], metrics[name]))
    delta, comparable, observed_intervals, resets = 0, 0, 0, 0
    if kind == 'counter':
        for previous, current in zip(observations, observations[1:]):
            decreased = current[2] < previous[2]
            resets += int(decreased)
            if current[0] == previous[0] + 1:
                observed_intervals += 1
                if not decreased:
                    delta += current[2] - previous[2]
                    comparable += 1
    result = {'kind': kind,
              'first': observations[0][2] if observations else None,
              'last': observations[-1][2] if observations else None,
              'first_elapsed_seconds': observations[0][1] if observations else None,
              'last_elapsed_seconds': observations[-1][1] if observations else None,
              'observed_samples': len(observations),
              'missing_samples': len(samples) - len(observations)}
    if kind == 'counter':
        result.update(counter_delta=delta if comparable else None,
                      delta_intervals=comparable, resets=resets,
                      unobserved_intervals=max(0, len(samples) - 1) - observed_intervals)
    return result


def range_summary(values):
    return {'min': min(values), 'mean': sum(values) / len(values), 'max': max(values)} if values else None


def summarize(samples, duration, interval, versions, interrupted=False):
    # Visit each existing observation once, not every sample for each metric
    # and historical node (anonymous observations can each introduce a node).
    aggregates = {source: {} for source in CLASSES}
    observed = {source: set() for source in CLASSES}
    for index, sample in enumerate(samples):
        elapsed = sample['elapsed_seconds']
        for source in CLASSES:
            for node, data in sample['sources'][source]['nodes'].items():
                metrics = aggregates[source].setdefault(node, {})  # retain empty nodes
                for name, value in data['metrics'].items():
                    kind = METRICS[source].get(name)
                    if kind is None:
                        continue
                    observed[source].add(name)
                    state = metrics.get(name)
                    if state is None:
                        state = metrics[name] = {
                            'first': value, 'first_elapsed_seconds': elapsed,
                            'observed_samples': 0, 'delta': 0, 'comparable': 0,
                            'observed_intervals': 0, 'resets': 0,
                        }
                    elif kind == 'counter':
                        decreased = value < state['last']
                        state['resets'] += int(decreased)
                        if index == state['last_index'] + 1:
                            state['observed_intervals'] += 1
                            if not decreased:
                                state['delta'] += value - state['last']
                                state['comparable'] += 1
                    state['last'] = value
                    state['last_elapsed_seconds'] = elapsed
                    state['last_index'] = index
                    state['observed_samples'] += 1

    sources = {}
    for source in CLASSES:
        nodes = {}
        for node in sorted(aggregates[source], key=int):
            nodes[node] = {}
            for name, kind in METRICS[source].items():
                state = aggregates[source][node].get(name, {})
                count = state.get('observed_samples', 0)
                result = {'kind': kind, 'first': state.get('first'),
                          'last': state.get('last'),
                          'first_elapsed_seconds': state.get('first_elapsed_seconds'),
                          'last_elapsed_seconds': state.get('last_elapsed_seconds'),
                          'observed_samples': count, 'missing_samples': len(samples) - count}
                if kind == 'counter':
                    comparable = state.get('comparable', 0)
                    result.update(counter_delta=state['delta'] if comparable else None,
                                  delta_intervals=comparable, resets=state.get('resets', 0),
                                  unobserved_intervals=max(0, len(samples) - 1)
                                  - state.get('observed_intervals', 0))
                nodes[node][name] = result
        sources[source] = {
            'unobserved_metrics': [name for name in METRICS[source] if name not in observed[source]],
            'nodes': nodes,
        }
    intervals = [b['elapsed_seconds'] - a['elapsed_seconds'] for a, b in zip(samples, samples[1:])]
    return {
        'schema_version': 1, 'requested_duration_seconds': duration,
        'requested_interval_seconds': interval, 'sample_count': len(samples),
        'sample_span_seconds': samples[-1]['elapsed_seconds'] - samples[0]['elapsed_seconds'] if samples else None,
        'capture_elapsed_seconds': samples[-1]['elapsed_seconds'] + samples[-1]['query_seconds'] if samples else None,
        'interval_seconds': range_summary(intervals),
        'query_seconds': range_summary([sample['query_seconds'] for sample in samples]),
        'interrupted': interrupted, 'loaded_versions': versions,
        'first_sample': samples[0] if samples else None,
        'last_sample': samples[-1] if samples else None,
        'sources': sources, 'throughput_bytes_per_second': None,
        'notes': [THROUGHPUT_NOTE,
                  'Counter deltas sum only adjacent nondecreasing observations of the same node. '
                  'Decreases (reset/wrap) and gaps are excluded; no comparable interval means null, not zero. '
                  'Resets counts observed decreases, including across gaps; hidden resets cannot be detected.',
                  'Missing/invalid metrics are absent from samples and null in per-node summaries, never zero-filled.',
                  'Versions are queried only at the beginning and end, outside the sample timing window. '
                  'not_observed does not prove unloaded; query failures are reported without their output.',
                  'Class queries are sequential, not atomic. Properties can remain stale while their publisher is idle. '
                  'Nodes without registry identity get fresh anonymous slots, so their counters are not linked.'],
    }


def create_output_directory(requested, base=None):
    if requested is not None:
        requested.mkdir(mode=0o700, parents=True)
        return requested
    base = base or (Path(__file__).resolve().parents[1] / 'build' / 'diagnostics')
    base.mkdir(parents=True, exist_ok=True)
    prefix = datetime.now(timezone.utc).strftime('wifi-performance-%Y%m%dT%H%M%SZ-')
    return Path(tempfile.mkdtemp(prefix=prefix, dir=base))


def save_capture(output, samples, summary):
    for name, value in (('samples.json', {'schema_version': 1, 'samples': samples}),
                        ('summary.json', summary)):
        with (output / name).open('x', encoding='utf-8') as stream:
            json.dump(value, stream, indent=2, allow_nan=False)
            stream.write('\n')
    fields = ('index', 'timestamp_utc', 'elapsed_seconds', 'query_seconds',
              'source', 'status', 'node', 'identity_available', 'metric', 'kind', 'value')
    with (output / 'samples.csv').open('x', encoding='utf-8', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for sample in samples:
            stamp = {key: sample[key] for key in fields[:4]}
            for source, data in sample['sources'].items():
                row = dict(stamp, source=source, status=data['status'])
                if not data['nodes']:
                    writer.writerow(row)
                for node, node_data in data['nodes'].items():
                    base = dict(row, node=node, identity_available=node_data['identity_available'])
                    if not node_data['metrics']:
                        writer.writerow(base)
                    for name, value in node_data['metrics'].items():
                        writer.writerow(dict(base, metric=name, kind=METRICS[source][name], value=value))


def main(argv=None):
    args = parse_args(argv)
    try:
        output = create_output_directory(args.output)
    except OSError:
        print('Cannot create output directory; --output must name a new writable directory.')
        return 1
    samples, interrupted = [], False
    versions = {endpoint: {bundle: {'status': 'not_queried', 'version': None} for bundle in BUNDLES}
                for endpoint in ('first', 'last')}
    print(f'Read-only capture for {args.duration:g}s; run your own transfer separately.', flush=True)
    try:
        versions['first'] = loaded_versions()
        collect_samples(Collector(), args.duration, args.interval, samples)
        versions['last'] = loaded_versions()
    except KeyboardInterrupt:
        interrupted = True
    summary = summarize(samples, args.duration, args.interval, versions, interrupted)
    try:
        save_capture(output, samples, summary)
    except OSError:
        print('Cannot write capture files; incomplete output may remain in the new directory.')
        return 1
    print(f'Saved {len(samples)} samples: {output}')
    print(THROUGHPUT_NOTE)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
