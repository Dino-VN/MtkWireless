#!/usr/bin/env python3
"""Bounded, read-only iPhone Mirroring Wi-Fi discovery capture."""
import argparse
from datetime import datetime
import json
from pathlib import Path
import plistlib
import subprocess


def snapshot():
    entries = plistlib.loads(subprocess.check_output(
        ['ioreg', '-a', '-r', '-c', 'AirportMtkWlan']))
    selected = {}
    pending = list(entries)
    exact = {
        'MTKWiFiQueryDiagnosticVersion', 'MTKWiFiPowerQueryCount',
        'MTKWiFiStateQueryCount', 'MTKWiFiCapabilitiesQueryCount',
        'MTKWiFiReportedPower', 'MTKWiFiReportedState',
        'MTKWiFiReportedAwdlCapability', 'MTKWiFiPowerPreference',
        'MTKWiFiPMPowerState', 'MTKWiFiInterfaceFlags',
        'MTKWiFiNativeLinkStatus', 'MTKAWDLCreateStage',
        'MTKAWDLCreateStatus', 'MTKAWDLSyncTransition',
        'MTKAWDLDataRxDecoded', 'MTKAWDLDataRxInvalid',
        'MTKAWDLDataTxFailed', 'MTKAWDLROCGrants',
        'MTKAWDLROCResponseTimeouts', 'MTKAWDLNativeDroppedPackets',
    }
    while pending:
        entry = pending.pop()
        pending.extend(entry.get('IORegistryEntryChildren', []))
        keys = (exact & entry.keys()) | {key for key in entry if key.startswith('MTKAWDLCache')}
        for key in keys:
            selected[key] = entry[key]
    interfaces = {}
    for name in ('en0', 'awdl0'):
        result = subprocess.run(['ifconfig', name], capture_output=True, text=True)
        interfaces[name] = {'returncode': result.returncode, 'output': result.stdout}
    return {'time': datetime.now().astimezone().isoformat(),
            'driver': selected, 'interfaces': interfaces}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--duration', type=int, choices=range(5, 61), default=30,
                        metavar='5..60')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    output = args.output or (Path(__file__).resolve().parents[1] / 'build' /
                            'diagnostics' / datetime.now().strftime('mirroring-wifi-%Y%m%d-%H%M%S'))
    output.mkdir(parents=True, exist_ok=True)
    before = snapshot()
    predicate = ('(process CONTAINS "sharingd" OR process == "rapportd" OR process == "wifip2pd" OR '
                 'process == "iPhone Mirroring") AND '
                 '(eventMessage CONTAINS[c] "wifi" OR '
                 'eventMessage CONTAINS[c] "discovery" OR '
                 'eventMessage CONTAINS[c] "BLE device" OR '
                 'eventMessage CONTAINS[c] "peer state" OR '
                 'eventMessage CONTAINS[c] "awdl" OR '
                 'eventMessage CONTAINS[c] "cache" OR '
                 'eventMessage CONTAINS[c] "unlock" OR '
                 'eventMessage CONTAINS[c] "authentication" OR '
                 'eventMessage CONTAINS[c] "pairing does not seem")')
    print(f'Capture running for {args.duration}s. Retry iPhone Mirroring now.', flush=True)
    with (output / 'discovery.log').open('w') as logfile:
        process = subprocess.Popen(['log', 'stream', '--style', 'compact',
                                    '--level', 'debug', '--predicate', predicate],
                                   stdout=logfile, stderr=subprocess.STDOUT)
        try:
            process.wait(timeout=args.duration)
        except subprocess.TimeoutExpired:
            pass
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    result = {'before': before, 'after': snapshot()}
    (output / 'state.json').write_text(json.dumps(result, indent=2))
    print(output)


if __name__ == '__main__':
    main()
