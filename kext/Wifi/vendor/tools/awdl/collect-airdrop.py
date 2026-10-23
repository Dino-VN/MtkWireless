#!/usr/bin/env python3
"""Collect one manual AirDrop attempt without changing driver/network settings."""
import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--expected-uuid', required=True)
parser.add_argument('--seconds', type=int, default=30)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--capture', action='store_true',
                    help='Also capture mDNS/IPv6 neighbor discovery on awdl0; requires root')
args = parser.parse_args()
if not 1 <= args.seconds <= 60:
    parser.error('--seconds must be between 1 and 60')
if args.capture and os.geteuid() != 0:
    parser.error('--capture requires root for /dev/bpf; run this command with sudo')
args.output.mkdir(parents=True, exist_ok=False)
errors = []

def save(name, command):
    try:
        result = subprocess.run(command, capture_output=True, timeout=45)
        (args.output / name).write_bytes(result.stdout)
        if result.stderr:
            (args.output / (name + '.stderr')).write_bytes(result.stderr)
        if result.returncode:
            errors.append({'file': name, 'exit_code': result.returncode})
        return result.stdout
    except (OSError, subprocess.SubprocessError) as error:
        errors.append({'file': name, 'error': str(error)})
        return b''

loaded = save('loaded.txt', ['kmutil', 'showloaded', '--list-only']).decode(errors='replace')
identity = next((line for line in loaded.splitlines()
                 if 'com.eiyuumiru.AirportMtkWlan (' in line), '')
if not re.search(r'\b' + re.escape(args.expected_uuid) + r'\b', identity, re.I):
    (args.output / 'summary.json').write_text(json.dumps({
        'status': 'wrong-build', 'expected_uuid': args.expected_uuid,
        'loaded': identity, 'errors': errors}, indent=2) + '\n')
    raise SystemExit('Expected kext UUID is not loaded. See ' + str(args.output))

def snapshot(label):
    for cls, name in [('AirportMtkWlan', 'controller'),
                      ('AirportMtkAwdlInterface', 'awdl')]:
        save(name + '-' + label + '.plist', ['ioreg', '-a', '-r', '-c', cls])
    save('interfaces-' + label + '.txt', ['ifconfig'])
    save('neighbors-' + label + '.txt', ['ndp', '-an'])
    save('counters-' + label + '.txt', ['netstat', '-ibn', '-I', 'awdl0'])

snapshot('before')
started = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
capture = None
capture_log = None
try:
    if args.capture:
        capture_log = (args.output / 'tcpdump.log').open('wb')
        capture = subprocess.Popen([
            '/usr/sbin/tcpdump', '-i', 'awdl0', '-n', '-s', '0', '-U',
            '-w', str(args.output / 'bonjour-ndp.pcap'), 'udp port 5353 or icmp6'],
            stdout=capture_log, stderr=capture_log)
    print('Attempt a small AirDrop transfer now. Observing for', args.seconds,
          'seconds. Confirm separately whether the file arrives.', flush=True)
    time.sleep(args.seconds)
finally:
    if capture:
        if capture.poll() is None:
            capture.send_signal(signal.SIGINT)
        try:
            capture.wait(timeout=5)
        except subprocess.TimeoutExpired:
            capture.kill()
            capture.wait()
        if capture.returncode != 0:
            errors.append({'file': 'tcpdump.log', 'exit_code': capture.returncode})
    if capture_log:
        capture_log.close()
ended = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
snapshot('after')
predicate = ('process == "rapportd" OR process == "sharingd" OR '
             '(process == "kernel" AND (eventMessage CONTAINS[c] "awdl0" OR '
             'eventMessage CONTAINS "SimpleMtkWlan" OR '
             'eventMessage CONTAINS "AirportMtkWlan"))')
logs = save('session.log', ['/usr/bin/log', 'show', '--start', started, '--end', ended,
                            '--style', 'compact', '--info', '--debug',
                            '--predicate', predicate]).decode(errors='replace')
findings = [line for line in logs.splitlines() if any(term in line for term in (
    'AWDL SANITY FAILURE', 'Not Routable', 'On-demand discovery timeout',
    'resolve-failure', 'Session start send failed', 'RESOLVE: Failed'))]
(args.output / 'findings.txt').write_text('\n'.join(findings) + '\n')
(args.output / 'summary.json').write_text(json.dumps({
    'status': 'collected-with-errors' if errors else 'collected',
    'started': started, 'ended': ended, 'expected_uuid': args.expected_uuid,
    'loaded': identity, 'capture_requested': args.capture, 'errors': errors,
    'findings': findings,
    'limitations': ['Collection does not establish file delivery.',
                    'Native/controller snapshots are separate reads, not atomic radio evidence.',
                    'awdl0 capture observes decoded network traffic, not rejected raw radio frames.']
}, indent=2) + '\n')
print('Saved:', args.output.resolve())
raise SystemExit(1 if errors else 0)
