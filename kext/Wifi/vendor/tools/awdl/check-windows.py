#!/usr/bin/env python3
"""Read-only native AW lifecycle check; require the intended loaded kext UUID."""
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
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
if not 0 <= args.seconds <= 60:
    parser.error('--seconds must be between 0 and 60')

def run(command):
    return subprocess.check_output(command, stderr=subprocess.PIPE, timeout=45)

def snapshot():
    records = plistlib.loads(run(['ioreg','-a','-r','-c','AirportMtkAwdlInterface']))
    if len(records) != 1:
        raise RuntimeError('Expected one registered AirportMtkAwdlInterface')
    return {k: v for k, v in records[0].items()
            if k.startswith('MTKAWDL') and isinstance(v, (int, str, bool))}

def check():
    loaded = run(['kmutil','showloaded','--list-only']).decode(errors='replace')
    identity = next((line for line in loaded.splitlines()
                     if 'com.eiyuumiru.AirportMtkWlan (' in line), '')
    result = {'expected_uuid': args.expected_uuid.upper(), 'loaded': identity,
              'status': 'unverified', 'reasons': []}
    if not re.search(r'\b' + re.escape(args.expected_uuid) + r'\b', identity, re.I):
        result['reasons'].append('The intended kext UUID is not loaded; runtime changes are unverified.')
        return result
    started = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    before = snapshot()
    time.sleep(args.seconds)
    after = snapshot()
    logs = run(['/usr/bin/log','show','--start',started,'--style','ndjson',
                '--predicate','process == "kernel" AND (eventMessage CONTAINS "AWDL SANITY FAILURE" OR eventMessage CONTAINS[c] "aw start event missing" OR eventMessage CONTAINS[c] "Did not get AW start event")']).decode(errors='replace')
    messages, missing, warnings = [], [], []
    for line in logs.splitlines():
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue
        message = record.get('eventMessage', '')
        if 'awdl0' in message and 'AWDL SANITY FAILURE' in message:
            messages.append(message)
        if 'awdl0' in message and 'aw start event missing' in message.lower():
            missing.append(message)
        if 'awdl0' in message and 'did not get aw start event' in message.lower():
            warnings.append(message)
    result.update(before=before, after=after, sanity_failures=messages,
                  missing_start_errors=missing, missing_start_warnings=warnings)
    # Monotonic notification counters belong to one interface instance.
    starts = after.get('MTKAWDLAWStartNotifications', 0) - before.get('MTKAWDLAWStartNotifications', 0)
    ends = after.get('MTKAWDLAWEndNotifications', 0) - before.get('MTKAWDLAWEndNotifications', 0)
    balance = after.get('MTKAWDLAWStartNotifications', 0) - after.get('MTKAWDLAWEndNotifications', 0)
    result.update(start_delta=starts, end_delta=ends, balance=balance)
    if messages:
        result['reasons'].append('Native AWDL sanity failures were recorded.')
    if missing or warnings:
        result['reasons'].append('Native AWDL reported missing AW-start events.')
    if starts < 0 or ends < 0:
        result['reasons'].append('Interface counters reset during observation.')
    if balance != int(bool(after.get('MTKAWDLAWWindowOpen', False))):
        result['reasons'].append('Start/end balance differs from the open-window state.')
    if starts == 0:
        result['reasons'].append('No new AW-start was observed; an empty error log cannot validate synchronization.')
    if not after.get('MTKAWDLSyncEnabled', False):
        result['reasons'].append('AWDL synchronization is disabled.')
    if not result['reasons']:
        result['status'] = 'passed'
    elif messages or missing or warnings or starts < 0 or ends < 0 or balance != int(bool(after.get('MTKAWDLAWWindowOpen', False))):
        result['status'] = 'failed'
    return result

try:
    result = check()
except (OSError, RuntimeError, subprocess.SubprocessError, plistlib.InvalidFileException) as exc:
    result = {'status': 'unverified', 'reasons': [str(exc)]}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
raise SystemExit(0 if result['status'] == 'passed' else 1)
