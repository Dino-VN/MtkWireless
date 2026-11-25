#!/usr/bin/env python3
"""Send one file through the native macOS AirDrop picker and benchmark the transfer."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import plistlib
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time
from collections import Counter
from datetime import datetime

from airdrop_benchmark_network import (ACTIONS, DEFAULT_SCENARIOS, SCENARIOS, NetworkSession,
                                       action_state_matches, matches_scenario, network_state, public_state)
from airdrop_benchmark_packets import (MIB, NS, capture_packets, distribution, flow_details,
                                       save_analysis, write_json)

HERE = Path(__file__).resolve().parent
DEFAULT_OUTPUT = HERE.parents[1] / 'diagnostics'
TERMINAL_EVENTS = {'shared', 'cancelled', 'failed', 'unavailable'}
CANCEL_OUTCOMES = {'recipient_timeout', 'connect_timeout', 'transfer_timeout',
                   'transfer_stalled', 'transfer_unobservable'}
LOG_PREDICATE = (
    '(process == "sharingd" AND (category CONTAINS[c] "airdrop" OR '
    'eventMessage CONTAINS[c] "airdrop" OR eventMessage CONTAINS[c] "quic")) OR '
    '((process == "kernel" OR subsystem CONTAINS[c] "skywalk") AND '
    '(eventMessage CONTAINS[c] "failed to allocate slab" OR '
    'eventMessage CONTAINS[c] "ENOMEM" OR eventMessage CONTAINS[c] "flow control"))'
)
LIVE_LOG_PREDICATE = (
    f'({LOG_PREDICATE}) OR (process == "sharingd" AND '
    '(subsystem == "com.apple.network" OR eventMessage CONTAINS[c] "upload")) OR '
    '(subsystem CONTAINS[c] "airdrop" OR processImagePath CONTAINS[c] "AirDrop.appex") OR '
    '(process == "rapportd" AND (eventMessage CONTAINS[c] "com.apple.sharingd.AirDrop" OR '
    'eventMessage CONTAINS[c] "on-demand connection discovery")) OR '
    '((process == "wifid" OR process == "airportd") AND '
    '(eventMessage CONTAINS[c] "AWDL" OR eventMessage CONTAINS[c] "disassoc" OR '
    'eventMessage CONTAINS[c] "roam" OR eventMessage CONTAINS[c] "scan"))'
)
SEND_LOG = re.compile(rb'^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\s+\w+\s+'
                      rb'sharingd\[([0-9]+):([0-9a-f]+)\] '
                      rb'\[com\.apple\.sharing:(AirDrop\.[0-9A-Fa-f]+|AirDropNW|Metrics)\] (.*)$')


def build_picker(directory):
    bundle = directory / 'AirDrop Benchmark.app' / 'Contents'
    macos = bundle / 'MacOS'
    macos.mkdir(parents=True)
    binary = macos / 'AirDropBenchmark'
    (bundle / 'Info.plist').write_bytes(plistlib.dumps({
        'CFBundleName': 'AirDrop Benchmark',
        'CFBundleExecutable': binary.name,
        'CFBundleIdentifier': 'com.dinovn.MtkWireless.AirDropBenchmark',
        'CFBundlePackageType': 'APPL',
        'CFBundleVersion': '1',
        'NSHighResolutionCapable': True,
    }))
    subprocess.run(['/usr/bin/xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra',
                    '-Wno-unused-parameter', '-framework', 'AppKit', '-framework', 'CoreWLAN',
                    '-framework', 'SystemConfiguration', '-framework', 'ApplicationServices',
                    str(HERE / 'airdrop-benchmark-share.m'), '-o', str(binary)], check=True)
    return binary


def interface_state(name):
    result = subprocess.run(['/sbin/ifconfig', name], capture_output=True, text=True, timeout=5)
    if result.returncode:
        raise ValueError(f'interface {name} unavailable: {result.stderr.strip()}')
    return {'addresses': sorted(set(re.findall(r'^\s*inet6?\s+(\S+)', result.stdout, re.M))),
            'macs': sorted(set(re.findall(r'^\s*ether\s+([0-9a-f:]{17})', result.stdout, re.M)))}


class Capture:
    def __init__(self, directory, interface, state):
        self.directory = directory
        self.interface = interface
        self.state = dict(state, pcap=f'{interface}.pcap', log=f'{interface}-tcpdump.log')
        self.child = None

    def start(self):
        command = ['/usr/bin/sudo', '-n', '/usr/sbin/tcpdump', '-i', self.interface,
                   '-p', '-n', '-s', '192', '-B', '4096', '-U', '-w', '-', 'ip or ip6']
        with (self.directory / self.state['pcap']).open('xb') as pcap, self.log_path.open('xb') as log:
            self.child = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                          stdout=pcap, stderr=log)

    @property
    def log_path(self):
        return self.directory / self.state['log']

    def ready(self):
        return 'listening on ' in self.log_path.read_text(errors='replace')

    def stop(self):
        if self.child is None:
            return
        stop_process(self.child, signal.SIGINT)
        self.state['exit_status'] = self.child.poll()
        log = self.log_path.read_text(errors='replace')
        match = re.search(r'(\d+) packets? dropped by kernel', log)
        self.state['kernel_drops'] = int(match[1]) if match else None
        try:
            after = interface_state(self.interface)
        except (OSError, ValueError, subprocess.TimeoutExpired):
            self.state['end_interface_observed'] = False
        else:
            self.state['end_interface_observed'] = True
            for key, values in after.items():
                self.state[key] = sorted(set(self.state[key] + values))


def stop_process(process, initial_signal):
    for signum in (initial_signal, signal.SIGTERM, signal.SIGKILL):
        if process.poll() is not None:
            return
        process.send_signal(signum)
        try:
            process.wait(timeout=4)
        except subprocess.TimeoutExpired:
            continue
        return


def wait_for_capture(captures):
    limit = time.monotonic() + 8
    while time.monotonic() < limit:
        if any(capture.child.poll() is not None for capture in captures):
            break
        if all(capture.ready() for capture in captures):
            return
        time.sleep(.05)
    raise RuntimeError('tcpdump did not start. See the interface tcpdump log in the run folder.')


def read_events(output, filename='sharing-events.jsonl'):
    path = output / filename
    if not path.exists():
        return []
    events = []
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        try:
            entry = json.loads(line)
        except json.JSONDecodeError:
            continue
        if (isinstance(entry, dict) and isinstance(entry.get('event'), str) and
                type(entry.get('wall_ns')) is int and type(entry.get('uptime_ns')) is int):
            events.append(entry)
    return events


def sharing_outcome(output):
    return next((event['event'] for event in reversed(read_events(output))
                 if event['event'] in TERMINAL_EVENTS), None)


def start_live_log(output):
    command = ['/usr/bin/log', 'stream', '--style', 'compact', '--level', 'debug',
               '--color', 'none', '--predicate', LIVE_LOG_PREDICATE]
    with (output / 'system-live.log').open('xb') as stream, (output / 'system-live.stderr').open('xb') as errors:
        return subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=errors)


def start_route_log(output):
    # Give route a terminal so it flushes each event before trial cleanup.
    command = ['/usr/bin/script', '-q', '/dev/null', '/sbin/route', '-n', 'monitor', '-ifindex', 'awdl0']
    with (output / 'awdl0-routes.log').open('xb') as stream:
        return subprocess.Popen(command, stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT)


class SendCompletion:
    """Require a receiver response before treating one native upload as delivered."""
    def __init__(self, start_ns, file_bytes, before_ns=None):
        self.start_ns, self.file_bytes = start_ns, file_bytes
        self.before_ns = before_ns
        self.sessions = set()
        self.upload_start_ns = None
        self.upload_response_ns = None
        self.upload_end = None
        self.cancel_ns = None
        self.position = 0

    def observe(self, line):
        match = SEND_LOG.match(line)
        if not match:
            return None
        date, process, thread, category, message = match.groups()
        observed = datetime.fromisoformat(date.decode())
        stamp = int(observed.timestamp()) * NS + observed.microsecond * 1000
        if stamp < self.start_ns or (self.before_ns is not None and stamp >= self.before_ns):
            return None
        session = (process, category)
        if category.startswith(b'AirDrop.') and message == b'Send StateMachine UPLOAD START':
            self.sessions.add(session)
            if self.upload_start_ns is None:
                self.upload_start_ns = stamp
        if len(self.sessions) != 1:
            return None
        sender_process, upload_category = next(iter(self.sessions))
        if process != sender_process or stamp < self.upload_start_ns:
            return None
        if category == b'AirDropNW' and message.startswith(b'Canceling send transfer'):
            self.cancel_ns = min(self.cancel_ns, stamp) if self.cancel_ns is not None else stamp
        if category == b'AirDropNW' and message == b'Received UPLOAD response' and self.upload_end is None:
            self.upload_response_ns = stamp
        if session in self.sessions and message == b'Send StateMachine UPLOAD END':
            self.upload_end = (thread, stamp)
        prefix = b"MetricEvent 'com.apple.sharing.airdrop.send-finish' : "
        if (category != b'Metrics' or not message.startswith(prefix) or self.upload_end is None or
                self.upload_response_ns is None):
            return None
        sender, end_ns = self.upload_end
        if thread != sender or not self.upload_response_ns <= end_ns <= stamp <= end_ns + NS:
            return None
        fields = message[len(prefix):]
        # Some OS versions include redaction tokens that are not JSON strings.
        transfer = re.search(rb'"transferID"\s*:\s*"([0-9a-fA-F]{8})-', fields)
        size = re.search(rb'"size"\s*:\s*([0-9]+)\s*[,}]', fields)
        if transfer and transfer[1].upper() != upload_category.split(b'.')[1].upper():
            return None
        if not size or int(size[1]) != self.file_bytes or not re.search(rb'"result"\s*:\s*0\s*[,}]', fields):
            return None
        return {'source': 'sharingd_send_finish', 'wall_ns': stamp, 'file_bytes': self.file_bytes,
                'upload_start_ns': self.upload_start_ns, 'upload_response_ns': self.upload_response_ns,
                'upload_end_ns': end_ns}

    def read(self, path):
        completion = None
        with path.open('rb') as stream:
            stream.seek(self.position)
            for line in stream:
                if not line.endswith(b'\n'):
                    break
                self.position += len(line)
                completion = self.observe(line) or completion
        if completion and len(self.sessions) == 1 and (
                self.cancel_ns is None or completion['wall_ns'] < self.cancel_ns):
            return completion
        return None


def trial_event(stream, name, **fields):
    stream.write(json.dumps(dict(fields, event=name, wall_ns=time.time_ns(),
                                 monotonic_ns=time.monotonic_ns())) + '\n')
    stream.flush()


class NetworkSampler:
    def __init__(self, output, binary):
        self.output = output
        self.binary = binary
        self.done = threading.Event()
        self.ethernet_up = threading.Event()
        self.worker = threading.Thread(target=self.run, name='wifi-sampler')

    def run(self):
        with (self.output / 'wifi.jsonl').open('x', encoding='utf-8') as stream:
            while True:
                started = time.monotonic()
                row = {'start_ns': time.time_ns()}
                try:
                    row['state'] = public_state(network_state(self.binary))
                    if row['state'].get('ethernet_up'):
                        self.ethernet_up.set()
                except (OSError, ValueError, subprocess.SubprocessError) as error:
                    row['error'] = str(error)
                row['end_ns'] = time.time_ns()
                stream.write(json.dumps(row) + '\n')
                stream.flush()
                if self.done.is_set():
                    return
                self.done.wait(max(0, 1 - (time.monotonic() - started)))


class TransferProgress:
    def __init__(self, key, stamp, index, tx_bytes, quantum, now):
        self.key = key
        self.quantum = quantum
        self.tx_bytes = self.progress_bytes = tx_bytes
        self.last_progress_at = now
        self.last_progress_ns = self.last_tx_ns = stamp
        self.last_tx_index = index
        self.last_tx_interface = key[0]
        self.last_rx_ns = None
        self.tcp_end = {}

    def observe(self, key, index, stamp, direction, packet, now):
        if key[1] != self.key[1] or key[4] != self.key[4]:
            return
        if direction == 'rx':
            self.last_rx_ns = max(self.last_rx_ns or stamp, stamp)
            return
        if not packet.payload:
            return
        if stamp >= self.last_tx_ns:
            self.last_tx_ns, self.last_tx_index, self.last_tx_interface = stamp, index, key[0]
        count = packet.payload
        if packet.proto == 'TCP':
            end = (packet.seq + count) & 0xffffffff
            if key in self.tcp_end:
                advance = (end - self.tcp_end[key]) & 0xffffffff
                if advance >= 0x80000000:
                    return
                count = min(count, advance)
            self.tcp_end[key] = end
        self.tx_bytes += count
        if self.tx_bytes - self.progress_bytes >= self.quantum:
            self.progress_bytes = self.tx_bytes
            self.last_progress_at = now
            self.last_progress_ns = max(self.last_progress_ns, stamp)

    def snapshot(self, now):
        return dict(flow_details(self.key), tx_payload_bytes=self.tx_bytes, progress_quantum_bytes=self.quantum,
                    idle_s=max(0, now - self.last_progress_at), last_progress_ns=self.last_progress_ns,
                    last_tx_ns=self.last_tx_ns, last_tx_capture_index=self.last_tx_index,
                    last_tx_interface=self.last_tx_interface, last_rx_ns=self.last_rx_ns)


class ActivityTrigger:
    def __init__(self, output, config, threshold=None, track_progress=False):
        self.stop = threading.Event()
        self.match = None
        self.progress = None
        self.track_progress = track_progress
        self.lock = threading.Lock()
        # Wait for a bulk flow to move data, independent of AppKit picker timing.
        self.threshold = threshold or min(256 * 1024, max(8192, config['file_bytes'] // 8))
        self.workers = [
            threading.Thread(target=self.watch, name=f'activity-{name}',
                             args=(output, dict(config, captures={name: capture})))
            for name, capture in config['captures'].items()
        ]

    def watch(self, output, config):
        totals = Counter()
        for key, index, stamp, _, _, direction, packet in capture_packets(output, config, follow=self.stop):
            if self.match is not None:
                if self.track_progress:
                    with self.lock:
                        self.progress.observe(key, index, stamp, direction, packet, time.monotonic())
                continue
            if direction != 'tx':
                continue
            totals[key] += packet.payload
            if totals[key] >= self.threshold:
                with self.lock:
                    if self.match is None:
                        if self.track_progress:
                            self.progress = TransferProgress(key, stamp, index, totals[key], self.threshold,
                                                             time.monotonic())
                            if packet.proto == 'TCP':
                                self.progress.tcp_end[key] = (packet.seq + packet.payload) & 0xffffffff
                        self.match = dict(flow_details(key), capture_index=index,
                                          seen_ns=stamp, threshold_bytes=self.threshold)
                        if not self.track_progress:
                            self.stop.set()
                if not self.track_progress:
                    return

    def progress_sample(self, now):
        with self.lock:
            return self.progress.snapshot(now) if self.progress else None

    def start(self):
        for worker in self.workers:
            worker.start()

    def close(self):
        self.stop.set()
        for worker in self.workers:
            if worker.ident is not None:
                worker.join()


class TrialTiming:
    def __init__(self, args, now):
        self.phase = 'recipient' if args.recipient else 'connect'
        self.deadline = now + (args.recipient_timeout if args.recipient else args.connect_timeout)

    def selected(self, now, timeout):
        self.phase, self.deadline = 'connect', now + timeout

    def started(self, now, timeout):
        self.phase, self.deadline = 'transfer', now + timeout

    def timeout_status(self, now):
        return f'{self.phase}_timeout' if now >= self.deadline else None

    def stalled(self, progress, limit, file_bytes):
        # A small control flow can reach the startup threshold without carrying the file.
        # Once enough TX bytes to contain the file have been seen, wait for the native result.
        sufficient = min(file_bytes // 4, 512 * 1024)
        return self.phase == 'transfer' and bool(limit and progress and progress['idle_s'] >= limit and
                                                 sufficient <= progress['tx_payload_bytes'] < file_bytes)


def watch_transfer(output, config, args, scenario, selected_ns):
    session = dict(config, start_ns=selected_ns)
    if not args.interface and 'awdl0' in session['captures']:
        # LAN packets are useful diagnostics, but bulk TCP/HTTPS on en1 can belong to another app.
        session = dict(session, captures={'awdl0': session['captures']['awdl0']})
    # A small file can finish before it reaches the default startup threshold.
    threshold = min(16 * 1024, max(1, config['file_bytes'] // 4))
    start = ActivityTrigger(output, session, threshold, track_progress=True)
    action = None
    if scenario in ACTIONS:
        action = ActivityTrigger(output, session)
    start.start()
    if action is not None:
        action.start()
    return start, action


def start_action(binary, output, scenario, match):
    print(f'  Triggering {scenario} after observing TX payload.', flush=True)
    with (output / 'action.log').open('xb') as log:
        stamp = time.time_ns()
        worker = subprocess.Popen([str(binary), ACTIONS[scenario]], stdin=subprocess.DEVNULL,
                                  stdout=log, stderr=subprocess.STDOUT)
    return worker, dict(status='running', start_ns=stamp, trigger=match)


def cancel_pending_share(binary, output, helper, event_file, active_recipient=None):
    result = {'start_ns': time.time_ns()}
    trial_event(event_file, 'airdrop_cancel_requested')
    print('  Cancelling this AirDrop trial in macOS…', flush=True)
    terminal = sharing_outcome(output)
    if terminal is None and helper.poll() is None:
        args = [str(binary), '--cancel-share', str(helper.pid)]
        if active_recipient is not None:
            args.append(active_recipient)
        try:
            with (output / 'cancellation-events.jsonl').open('xb') as events, \
                 (output / 'picker.log').open('ab') as log:
                command = subprocess.run(args, stdin=subprocess.DEVNULL, stdout=events, stderr=log, timeout=4)
            result['ax_exit_status'] = command.returncode
        except (OSError, subprocess.TimeoutExpired) as error:
            result['error'] = str(error)
        command_events = read_events(output, 'cancellation-events.jsonl')
        if command_events:
            result['ax_event'] = command_events[-1]['event']
        if result.get('ax_event') in ('cancel_pressed', 'cancel_tile_pressed'):
            try:
                helper.wait(timeout=4)
            except subprocess.TimeoutExpired:
                pass
        terminal = sharing_outcome(output)
    status = 'unconfirmed'
    if terminal == 'cancelled':
        status = 'cancelled'
    elif terminal:
        status = 'native_ended'
    elif result.get('ax_event') == 'cancel_tile_cleared':
        status = 'tile_cancelled'
    result.update(end_ns=time.time_ns(), native_outcome=terminal, status=status)
    trial_event(event_file, 'airdrop_cancel_finished', cancellation=result)
    print(f'  AirDrop cancellation: {result["status"]}.', flush=True)
    return result


def live_run(args, output, binary, file, scenario, initial):
    interfaces = args.interface or ['awdl0']
    if not args.interface and initial['wifi_associated']:
        interfaces.append(initial['wifi_interface'])
    config = {'schema_version': 1, 'scenario': scenario, 'file_bytes': file.stat().st_size,
              'wifi_before': public_state(initial), 'status': 'starting',
              'timeouts_s': {'recipient': args.recipient_timeout if args.recipient else None,
                             'connect': args.connect_timeout, 'transfer': args.timeout,
                             'stall': args.stall_timeout},
              'start_ns': time.time_ns(), 'end_ns': time.time_ns(), 'captures': {}}
    captures, helper, sampler = [], None, None
    event_file = live_log = route_log = None
    activity = transfer = action_process = selector = start_mono = None
    action_deadline = None
    next_progress_event = 0
    next_completion_check = 0
    selected_ns = None
    action = {'status': 'not_triggered'} if scenario in ACTIONS else None
    try:
        for name in interfaces:
            capture = Capture(output, name, interface_state(name))
            captures.append(capture)
            config['captures'][name] = capture.state
        subprocess.run(['/usr/bin/sudo', '-v'], check=True)
        for capture in captures:
            capture.start()
        wait_for_capture(captures)
        sampler = NetworkSampler(output, binary)
        sampler.worker.start()
        config['start_ns'] = time.time_ns()
        start_mono = time.monotonic_ns()
        event_file = (output / 'trial-events.jsonl').open('x', encoding='utf-8')
        trial_event(event_file, 'trial_started', scenario=scenario, timeouts_s=config['timeouts_s'])
        try:
            live_log = start_live_log(output)
        except OSError as error:
            config['live_log'] = {'error': str(error)}
            trial_event(event_file, 'live_log_failed', error=str(error))
        if 'awdl0' in config['captures']:
            try:
                route_log = start_route_log(output)
            except OSError as error:
                config['route_log'] = {'error': str(error)}
                trial_event(event_file, 'route_log_failed', error=str(error))
        if args.recipient:
            print(f'  Searching for {args.recipient!r} ({args.recipient_timeout}s).', flush=True)
        else:
            print(f'  Choose a recipient and accept the transfer ({args.connect_timeout}s to start).', flush=True)
        with (output / 'sharing-events.jsonl').open('xb') as events, (output / 'picker.log').open('xb') as log:
            helper = subprocess.Popen([str(binary), '--share', str(file)], stdin=subprocess.DEVNULL,
                                      stdout=events, stderr=log)
        timing = TrialTiming(args, time.monotonic())
        completion = SendCompletion(config['start_ns'], config['file_bytes'])
        if args.recipient:
            with (output / 'recipient-events.jsonl').open('xb') as events, (output / 'picker.log').open('ab') as log:
                selector = subprocess.Popen([str(binary), '--select-recipient', str(helper.pid), args.recipient],
                                            stdin=subprocess.DEVNULL, stdout=events, stderr=log)
        else:
            selected_ns = config['start_ns']
        while helper.poll() is None:
            now = time.monotonic()
            if sampler.ethernet_up.is_set():
                config['status'] = 'ethernet_reenabled'
                break
            link_change = scenario in ('wifi-disconnect', 'wifi-radio-off') and action_process is not None
            if not link_change and any(capture.child.poll() is not None for capture in captures):
                config['status'] = 'capture_failed'
                break
            if timing.phase == 'recipient' and selector.poll() is not None:
                selection = next(iter(read_events(output, 'recipient-events.jsonl')), None)
                if not selection or selection['event'] != 'recipient_selected':
                    config['status'] = selection['event'] if selection else 'recipient_selector_failed'
                    break
                timing.selected(now, args.connect_timeout)
                trial_event(event_file, 'recipient_selected', selection_ns=selection['wall_ns'])
                print(f'  Recipient selected. Waiting up to {args.connect_timeout}s for transfer activity.', flush=True)
                selected_ns = selection['wall_ns']
                completion.start_ns = selected_ns
            if selected_ns is not None and live_log is not None and now >= next_completion_check:
                delivery = completion.read(output / 'system-live.log')
                next_completion_check = now + .25
                if delivery and delivery['upload_start_ns'] >= selected_ns:
                    config.update(status='shared', delivery_evidence=delivery)
                    break
            native_start = completion.upload_start_ns if len(completion.sessions) == 1 else None
            if native_start is not None and selected_ns is not None and native_start < selected_ns:
                native_start = None
            capture_only = live_log is None or live_log.poll() is not None
            if selected_ns is not None and transfer is None and (native_start is not None or capture_only):
                transfer, activity = watch_transfer(output, config, args, scenario, native_start or selected_ns)
            if timing.phase == 'connect' and (native_start is not None or
                                              (capture_only and transfer is not None and transfer.match is not None)):
                timing.started(now, args.timeout)
                config['native_upload_start_ns'] = native_start
                trial_event(event_file, 'transfer_started', native_upload_start_ns=native_start, flow=transfer.match)
                print(f'  Upload started. Waiting up to {args.timeout}s for completion.', flush=True)
            if timing.phase == 'transfer':
                progress = transfer.progress_sample(now)
                if progress and now >= next_progress_event:
                    trial_event(event_file, 'transfer_progress', **progress)
                    next_progress_event = now + 1
                if timing.stalled(progress, args.stall_timeout, config['file_bytes']):
                    terminal = sharing_outcome(output)
                    if terminal:
                        config['status'] = terminal
                        break
                    observed = any(capture.interface == progress['last_tx_interface'] and capture.child.poll() is None
                                   for capture in captures)
                    config['status'] = 'transfer_stalled' if observed else 'transfer_unobservable'
                    config['stall'] = dict(progress, detected_ns=time.time_ns(), capture_active=observed)
                    trial_event(event_file, config['status'], **config['stall'])
                    print(f'  {config["status"]}: no bulk TX progress for {progress["idle_s"]:.1f}s.', flush=True)
                    break
            timeout = timing.timeout_status(now)
            if timeout is not None:
                config['status'] = timeout
                break
            if activity is not None and activity.match is not None and action_process is None:
                action_process, action = start_action(binary, output, scenario, activity.match)
                trial_event(event_file, 'wifi_action_started', action=action)
                action_deadline = time.monotonic() + 20
            if action_process is not None and action['status'] == 'running':
                exit_status = action_process.poll()
                if exit_status is not None:
                    action.update(status='ok' if exit_status == 0 else 'failed',
                                  end_ns=time.time_ns(), exit_status=exit_status)
                    trial_event(event_file, 'wifi_action_finished', action=action)
                elif time.monotonic() >= action_deadline:
                    stop_process(action_process, signal.SIGTERM)
                    action.update(status='timeout', end_ns=time.time_ns())
                    trial_event(event_file, 'wifi_action_finished', action=action)
            time.sleep(.05)
        else:
            config['status'] = sharing_outcome(output) or 'picker_exited_without_result'
            config['picker_exit_status'] = helper.returncode
            time.sleep(.7)
        if (not config.get('delivery_evidence') and selected_ns is not None and live_log is not None
                and (config['status'] in CANCEL_OUTCOMES or
                     config['status'] in ('shared', 'picker_exited_without_result'))):
            delivery = completion.read(output / 'system-live.log')
            if delivery and delivery['upload_start_ns'] >= selected_ns:
                config.update(status='shared', delivery_evidence=delivery)
        if config['status'] == 'shared':
            if config.get('delivery_evidence'):
                trial_event(event_file, 'airdrop_delivery_confirmed', **config['delivery_evidence'])
                print('  macOS confirmed delivery. Closing the AirDrop window.', flush=True)
            else:
                config['status'] = 'delivery_unconfirmed'
        if config['status'] in CANCEL_OUTCOMES:
            # Stop any auto-picker first, so it cannot click while we cancel.
            if selector is not None:
                stop_process(selector, signal.SIGTERM)
                selector = None
            active_recipient = (args.recipient or '') if timing.phase == 'transfer' else (
                args.recipient if timing.phase == 'connect' else None)
            config['cancellation'] = cancel_pending_share(binary, output, helper, event_file, active_recipient)
    except KeyboardInterrupt:
        config['status'] = 'interrupted'
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        config['status'] = 'setup_failed'
        config['error'] = str(error)
    finally:
        config['end_ns'] = time.time_ns()
        if start_mono is not None:
            config['monotonic_span_ns'] = time.monotonic_ns() - start_mono
        if event_file is not None:
            trial_event(event_file, 'trial_finished', outcome=config['status'], error=config.get('error'))
        if selector is not None:
            stop_process(selector, signal.SIGTERM)
        if helper is not None:
            stop_process(helper, signal.SIGTERM)
        if transfer is not None:
            transfer.close()
            config['transfer_start_evidence'] = transfer.match
        if activity is not None:
            activity.close()
        if action_process is not None and action['status'] == 'running':
            try:
                exit_status = action_process.wait(timeout=10)
                action.update(status='ok' if exit_status == 0 else 'failed', exit_status=exit_status)
            except subprocess.TimeoutExpired:
                stop_process(action_process, signal.SIGTERM)
                action['status'] = 'timeout'
            action['end_ns'] = time.time_ns()
            trial_event(event_file, 'wifi_action_finished', action=action)
        if action is not None:
            config['action'] = action
        for capture in captures:
            capture.stop()
        if sampler is not None:
            sampler.done.set()
            sampler.worker.join()
        try:
            config['wifi_after'] = public_state(network_state(binary))
        except (OSError, ValueError, subprocess.SubprocessError):
            config['wifi_after'] = None
        if live_log is not None:
            ended_early = live_log.poll() is not None
            stop_process(live_log, signal.SIGINT)
            config['live_log'] = {'exit_status': live_log.poll(), 'ended_early': ended_early}
        if route_log is not None:
            ended_early = route_log.poll() is not None
            stop_process(route_log, signal.SIGTERM)
            route_log.stdin.close()
            config['route_log'] = {'exit_status': route_log.poll(), 'ended_early': ended_early}
        if event_file is not None:
            event_file.close()
        write_json(output / 'run.json', config)
    return config


def system_logs(output, config):
    if config['status'] == 'setup_failed' or config['end_ns'] <= config['start_ns']:
        return
    start = datetime.fromtimestamp(config['start_ns'] // NS).strftime('%Y-%m-%d %H:%M:%S')
    end = datetime.fromtimestamp(config['end_ns'] // NS + 1).strftime('%Y-%m-%d %H:%M:%S')
    command = ['/usr/bin/log', 'show', '--style', 'compact', '--info', '--start', start,
               '--end', end, '--predicate', LOG_PREDICATE]
    with (output / 'system.log').open('xb') as stream:
        try:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=15)
            if result.returncode != 0:
                stream.write(f'log show exited with status {result.returncode}.\n'.encode())
        except (OSError, subprocess.TimeoutExpired) as error:
            stream.write(f'log show: {error}\n'.encode())


def wifi_analysis(output, config, result):
    samples_path = output / 'wifi.jsonl'
    samples = [{'start_ns': config['start_ns'], 'end_ns': config['start_ns'],
                'state': config['wifi_before']}] if 'wifi_before' in config else []
    if samples_path.exists():
        samples.extend(json.loads(line) for line in samples_path.read_text(encoding='utf-8').splitlines())
    if config.get('wifi_after'):
        samples.append({'start_ns': config['end_ns'], 'end_ns': config['end_ns'],
                        'state': config['wifi_after']})
    scenario = config.get('scenario', 'wifi')
    action = config.get('action')
    known = [sample for sample in samples if 'state' in sample]
    channels = {(state.get('band_ghz'), state['channel'], state.get('width_mhz'))
                for state in (sample['state'] for sample in known) if state.get('channel')}
    states = [sample['state'] for sample in known]
    checks = [state.get('ethernet_up') == [] for state in states]
    precondition = matches_scenario(config.get('wifi_before', {}), scenario)
    if action and 'start_ns' in action:
        before = [sample['state'] for sample in known if sample['end_ns'] < action['start_ns']]
        after = [sample['state'] for sample in known if sample['start_ns'] >= action['start_ns']]
        checks.append(all(matches_scenario(state, scenario) for state in before))
        if scenario in ('wifi-disconnect', 'wifi-radio-off'):
            action['network_state_observed'] = any(action_state_matches(state, scenario) for state in after)
            if 'wifi_after' in config and config['wifi_after']:
                action['network_state_observed'] |= action_state_matches(config['wifi_after'], scenario)
            checks.append(action['network_state_observed'])
        else:
            checks.append(all(matches_scenario(state, scenario) for state in after))
        flow = result['selected_flow']
        action['trigger_matches_analyzed_flow'] = (
            all(flow.get(key) == value for key, value in action['trigger'].items()
                if key not in ('capture_index', 'seen_ns', 'threshold_bytes')) if flow else None)
        checks.append(action['trigger_matches_analyzed_flow'] is True)
        terminal = next((event for event in reversed(result['sharing_events'])
                         if event['event'] in TERMINAL_EVENTS), None)
        action['started_before_sharing_result'] = not terminal or action['start_ns'] <= terminal['wall_ns']
        checks.append(action['started_before_sharing_result'])
        if result['outcome'] == 'shared':
            checks.append(result['action_to_next_tx_payload_ms'] is not None)
    else:
        checks.append(all(matches_scenario(state, scenario) for state in states))
    if scenario != 'direct':
        checks.append(len(channels) == 1)
        checks.append(all(state.get('channel') for state in states if state.get('wifi_associated')))
    return {
        'scenario_valid': precondition and bool(config.get('wifi_after')) and all(checks)
        and (not action or action['status'] == 'ok'),
        'ap_channels': [{'band_ghz': band, 'channel': channel, 'width_mhz': width}
                        for band, channel, width in sorted(channels, key=lambda row: tuple(x or 0 for x in row))],
        'wifi_sample_count': len(known), 'wifi_sample_errors': len(samples) - len(known),
        'wifi_rssi_dbm': distribution([state['rssi_dbm'] for state in states if 'rssi_dbm' in state]),
        'wifi_noise_dbm': distribution([state['noise_dbm'] for state in states if 'noise_dbm' in state]),
        'wifi_phy_tx_mbps': distribution([state['phy_tx_mbps'] for state in states if 'phy_tx_mbps' in state]),
    }


def build_report(output, analysis, config, manual=None):
    recipient_events = read_events(output, 'recipient-events.jsonl')
    selection = next((event for event in recipient_events if event['event'] == 'recipient_selected'), None)
    if selection:
        config = dict(config, recipient_selected_ns=selection['wall_ns'])
    logs = output / 'system-live.log'
    status = config['status']
    cancel_ns = (config.get('cancellation') or {}).get('start_ns')
    delivery = None
    if logs.is_file() and (
            status in CANCEL_OUTCOMES or status in ('shared', 'delivery_unconfirmed', 'picker_exited_without_result')):
        before_ns = config['end_ns'] + 1
        if cancel_ns is not None:
            before_ns = min(before_ns, cancel_ns)
        start_ns = config.get('recipient_selected_ns', config['start_ns'])
        delivery = SendCompletion(start_ns, config['file_bytes'], before_ns=before_ns).read(logs)
    if delivery and status != 'shared':
        config = dict(config, status='shared', original_outcome=status)
    elif not delivery and status == 'shared':
        fallback = (config.get('original_outcome') or 'cancelled') if cancel_ns is not None else 'delivery_unconfirmed'
        config = dict(config, status=fallback, original_outcome=status)
    result, timeline = save_analysis(output, config, analysis, manual)
    events = read_events(output)
    picker = next((event for event in events if event['event'] == 'picker_requested'), None)
    terminal = next((event for event in reversed(events) if event['event'] in TERMINAL_EVENTS), None)
    if cancel_ns is not None and terminal and terminal['wall_ns'] >= cancel_ns:
        terminal = None
    picker_ms = (picker['wall_ns'] - config['start_ns']) / 1e6 if picker else None
    upload_start_ns = (delivery or {}).get('upload_start_ns', config.get('native_upload_start_ns'))
    upload_end_ns = (delivery or {}).get('upload_end_ns')
    upload_s = ((upload_end_ns - upload_start_ns) / NS
                if upload_start_ns is not None and upload_end_ns is not None and upload_end_ns > upload_start_ns else None)
    terminal_ns = delivery['wall_ns'] if delivery else terminal['wall_ns'] if terminal else None
    terminal_ms = (terminal_ns - config['start_ns']) / 1e6 if terminal_ns is not None else None
    picker_span = (terminal['uptime_ns'] - picker['uptime_ns']) / NS if picker and terminal else None
    if picker_span is not None and picker_span < 0:
        picker_span = None
    clock_error = ((config['end_ns'] - config['start_ns'] - config['monotonic_span_ns']) / 1e6
                   if 'monotonic_span_ns' in config else None)
    cross_clock_ok = clock_error is None or abs(clock_error) < 20
    if delivery and picker and cross_clock_ok and terminal_ns >= picker['wall_ns']:
        picker_span = (terminal_ns - picker['wall_ns']) / NS
    flow = result['selected_flow']
    evidence = config.get('transfer_start_evidence')
    evidence_match = flow and evidence and all(evidence[key] == flow[key] for key in
                                              ('interface', 'transport', 'local_ip', 'local_port', 'peer_ip', 'peer_port'))
    connect_start = selection or picker
    if not cross_clock_ok:
        result['recipient_to_first_flow_ms'] = result['recipient_to_first_tx_payload_ms'] = None
    result.update(
        outcome=config['status'],
        original_outcome=config.get('original_outcome'),
        elapsed_s=(config.get('monotonic_span_ns', config['end_ns'] - config['start_ns'])) / NS,
        error=config.get('error'),
        timeouts_s=config.get('timeouts_s'),
        live_log=config.get('live_log'),
        route_log=config.get('route_log'),
        delivery_evidence=delivery,
        native_upload_seconds=upload_s,
        native_upload_mib_s=config['file_bytes'] / MIB / upload_s if upload_s else None,
        cancellation=config.get('cancellation'),
        diagnostic_files={name: os.path.relpath(output / name, analysis)
                          for name in ('trial-events.jsonl', 'system-live.log', 'system-live.stderr',
                                       'cancellation-events.jsonl', 'awdl0-routes.log')
                          if (output / name).is_file()},
        file_bytes=config['file_bytes'], capture=config['captures'],
        sharing_events=events, picker_open_ms=picker_ms, terminal_ms=terminal_ms,
        recipient_events=recipient_events, transfer_start_evidence=evidence,
        picker_to_recipient_ms=((selection['uptime_ns'] - picker['uptime_ns']) / 1e6
                                if selection and picker else None),
        connect_to_bulk_ms=((evidence['seen_ns'] - connect_start['wall_ns']) / 1e6
                            if evidence_match and connect_start and cross_clock_ok
                            and evidence['seen_ns'] >= connect_start['wall_ns'] else None),
        connect_to_native_upload_ms=((upload_start_ns - connect_start['wall_ns']) / 1e6
                                     if upload_start_ns is not None and connect_start and cross_clock_ok
                                     and upload_start_ns >= connect_start['wall_ns'] else None),
        picker_to_terminal_s=picker_span, wall_minus_monotonic_span_ms=clock_error,
        file_mib_s_picker_to_success=(
            config['file_bytes'] / MIB / picker_span if config['status'] == 'shared' and picker_span else None),
        picker_to_first_flow_ms=(flow['first_ms'] - picker_ms if flow and picker and cross_clock_ok
                                 and flow['first_ms'] >= picker_ms else None),
        first_flow_to_first_tx_payload_ms=(result['first_tx_payload_ms'] - flow['first_ms']
                                           if flow and result['first_tx_payload_ms'] is not None else None),
        last_tx_payload_to_terminal_ms=(terminal_ms - result['last_tx_payload_ms']
                                        if terminal_ms is not None and cross_clock_ok and result['last_tx_payload_ms'] is not None
                                        else None),
    )
    result.update(wifi_analysis(output, config, result))
    result.update(scenario=config.get('scenario', 'wifi'), action=config.get('action'))
    stall = config.get('stall')
    if stall:
        result['stall'] = dict(stall, elapsed_ms=(stall['detected_ns'] - config['start_ns']) / 1e6,
                               last_progress_ms=(stall['last_progress_ns'] - config['start_ns']) / 1e6,
                               last_tx_ms=(stall['last_tx_ns'] - config['start_ns']) / 1e6,
                               last_rx_ms=((stall['last_rx_ns'] - config['start_ns']) / 1e6
                                           if stall['last_rx_ns'] is not None else None))
    if result['action'] and 'start_ns' in result['action']:
        action = result['action']
        action['elapsed_ms'] = (action['start_ns'] - config['start_ns']) / 1e6
        action['duration_ms'] = (action['end_ns'] - action['start_ns']) / 1e6
    write_json(analysis / 'summary.json', result)
    render_report(analysis, result, timeline)
    return result


def render_report(directory, result, timeline):
    def val(number, places=3):
        return f'{number:.{places}f}' if number is not None else '—'

    flow = result['selected_flow']
    lines = ['# AirDrop Benchmark', '',
             f'- Scenario — **{result["scenario"]}**',
             f'- Scenario conditions verified — {result["scenario_valid"]}',
             f'- AP channels — {channel_label(result)}',
             f'- macOS sharing result — **{result["outcome"]}**',
             f'- File size — {result["file_bytes"] / MIB:.2f} MiB',
             f'- Picker to completion — {val(result["picker_to_terminal_s"])} s',
             f'- File MiB/s from picker to successful completion — {val(result["file_mib_s_picker_to_success"])}',
             f'- Flow selection — {result["attribution"]} ({result["flow_count"]} captured flows)']
    if result['error']:
        lines.append(f'- Error — {result["error"]}')
    if result['delivery_evidence']:
        lines.append('- Delivery confirmed by macOS sharingd for this file size.')
    if result['native_upload_seconds'] is not None:
        lines.append(f'- macOS upload — {val(result["native_upload_seconds"])} s, '
                     f'{val(result["native_upload_mib_s"])} MiB/s of file data.')
    if result['original_outcome']:
        lines.append(f'- Original benchmark result — {result["original_outcome"]}.')
    cancellation = result.get('cancellation')
    if cancellation:
        lines.append(f'- AirDrop cancellation — {cancellation["status"]}. '
                     f'Accessibility — {cancellation.get("ax_event") or "—"}. '
                     f'macOS result — {cancellation.get("native_outcome") or "—"}.')
    stall = result.get('stall')
    if stall:
        lines += [f'- No observed bulk TX progress for {val(stall["idle_s"])} s. '
                  f'Detected at {val(stall["elapsed_ms"])} ms into the trial.',
                  f'- Last TX payload / RX packet on the tracked peer — {val(stall["last_tx_ms"])} / '
                  f'{val(stall["last_rx_ms"])} ms into the trial.',
                  f'- Tracked TX payload — {stall["tx_payload_bytes"]:,} bytes. '
                  f'Capture still active on {stall["last_tx_interface"]} — {stall["capture_active"]}.']
    if result['picker_to_recipient_ms'] is not None:
        lines += [f'- Picker to automatic selection — {val(result["picker_to_recipient_ms"])} ms',
                  f'- Selection to first flow packet — {val(result["recipient_to_first_flow_ms"])} ms',
                  f'- Selection to first TX payload — {val(result["recipient_to_first_tx_payload_ms"])} ms']
    connect_label = 'Selection' if result['picker_to_recipient_ms'] is not None else 'Picker'
    lines.append(f'- {connect_label} to macOS upload start — {val(result["connect_to_native_upload_ms"])} ms')
    lines.append(f'- {connect_label} to observed bulk startup — {val(result["connect_to_bulk_ms"])} ms')
    action = result['action']
    if action:
        lines += [f'- Interruption result — {action["status"]}',
                  f'- Action start / duration — {val(action.get("elapsed_ms"))} / {val(action.get("duration_ms"))} ms',
                  f'- Action trigger agrees with analyzed flow — {action.get("trigger_matches_analyzed_flow")}',
                  f'- Action to next observed TX payload — {val(result["action_to_next_tx_payload_ms"])} ms',
                  f'- TX payload gap across action start — {val(result["action_crossing_gap_ms"])} ms']
        if 'network_state_observed' in action:
            lines.append(f'- Expected post-action network state observed — {action["network_state_observed"]}')
    if flow:
        lines += [f'- Flow {flow["flow"]} — {flow["interface"]} {flow["transport"]}, '
                  f'peer `{flow["peer_ip"]}:{flow["peer_port"]}`',
                  f'- Picker to first flow packet — {val(result["picker_to_first_flow_ms"])} ms',
                  f'- First flow packet to first TX payload — {val(result["first_flow_to_first_tx_payload_ms"])} ms',
                  f'- TX payload span — {val(result["tx_payload_span_seconds"])} s',
                  f'- Last TX payload to sharing result — {val(result["last_tx_payload_to_terminal_ms"])} ms',
                  f'- Mean captured TX payload rate over data span — {val(result["tx_payload_mib_s_over_span"])} MiB/s',
                  f'- TCP SYN → SYN/ACK — {val(result["tcp_syn_to_synack_ms"])} ms',
                  '', '| Metric | Samples | Min | Mean | P50 | P95 | P99 | Max |',
                  '|---|---:|---:|---:|---:|---:|---:|---:|']
        distributions = dict(result['gap_ms'],
                             **{'full seconds (TX MiB/s)': result['interior_full_second_tx_payload_mib_s']})
        labels = {'tx': 'TX packet gap (ms)', 'rx': 'RX packet gap (ms)', 'tx_payload': 'TX payload gap (ms)'}
        for name, data in distributions.items():
            if data:
                values = ' | '.join(val(data[key]) for key in ('min', 'mean', 'p50', 'p95', 'p99', 'max'))
                lines.append(f'| {labels.get(name, name)} | {data["count"]} | {values} |')
        lines += ['', '## Longest TX payload gaps', '',
                  'Times are relative to benchmark launch.',
                  '', '| Start (ms) | End (ms) | Gap (ms) | Next capture packet |',
                  '|---:|---:|---:|---:|']
        for gap in result['largest_tx_payload_gaps']:
            lines.append(f'| {val(gap["start_ms"])} | {val(gap["end_ms"])} | {val(gap["gap_ms"])} '
                         f'| {gap["next_capture_index"]} |')
        lines += ['', '[Throughput chart](timeline.html) · [1-second timeline](timeline.csv)']
    else:
        lines += ['', 'Review flows.csv. To analyze a partial or ambiguous transfer, rerun with '
                  '`--analyze RUN_DIR --flow NUMBER`.']
    lines += ['', '## Capture quality', '']
    for interface, capture in result['capture'].items():
        quality = result['capture_quality'].get(interface, {})
        lines.append(f'- {interface} — {quality.get("usable_packets", 0)} usable packets, '
                     f'{capture.get("kernel_drops")} kernel drops, tcpdump exit {capture.get("exit_status")}, '
                     f'{quality.get("timestamp_regressions", 0)} timestamp regressions, '
                     f'pcap read issue: {quality.get("error", "none")}')
    if result['wall_minus_monotonic_span_ms'] is not None and abs(result['wall_minus_monotonic_span_ms']) >= 20:
        lines.append('- Wall and monotonic clocks diverged during the run. Cross-clock phase durations were omitted.')
    lines += ['', 'Packet timestamps and payload rates come from the local capture interface. '
              'Offload can group packets. Packet spacing measures gaps between local observations. '
              'Receiver-side timing, radio airtime and QUIC ACK/RTT require independent evidence. '
              'Picker elapsed time includes human interaction. Bulk flow selection is a candidate based on volume.',
              '', '[Packets](packets.csv) · [Flows](flows.csv) · [Full summary](summary.json)']
    links = result['diagnostic_files']
    if links:
        lines.append(' · '.join(f'[{name}]({path})' for name, path in links.items()))
    live_log = result.get('live_log') or {}
    if live_log.get('ended_early') or live_log.get('error'):
        lines.append('macOS live logging stopped early or could not be started. Check the log diagnostics.')
    route_log = result.get('route_log') or {}
    if route_log.get('ended_early') or route_log.get('error'):
        lines.append('The macOS AWDL route monitor stopped early or could not be started.')
    (directory / 'report.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    if timeline:
        write_chart(directory / 'timeline.html', timeline, action, stall)


def write_chart(path, timeline, action, stall):
    """Build a self-contained chart with one hoverable bar per second."""
    units = 'tx_payload_mib_s'
    peak = max(row[units] for row in timeline) or 1
    count = len(timeline)
    bars = []
    for index, row in enumerate(timeline):
        rate = row[units]
        height = rate / peak * 216
        bars.append(f'<rect x="{57 + index / count * 811:.3f}" y="{248 - height:.3f}" '
                    f'width="{811 / count:.3f}" height="{max(.8, height):.3f}">'
                    f'<title>{index}s · {rate:.3f} MiB/s · {row["tx_packets"]} TX packets</title></rect>')
    ticks = sorted(set(round(index * (count - 1) / 5) for index in range(6)))
    labels = ''.join(f'<text x="{57 + second / count * 811:.3f}" y="273">{second}s</text>' for second in ticks)
    marker = ''
    for event, color, label in ((action, '#ffb866', 'Wi-Fi action started'),
                                (stall, '#ff7b7b', 'Stall detected')):
        if event and event.get('elapsed_ms') is not None:
            x = 57 + event['elapsed_ms'] / 1000 / count * 811
            if 57 <= x <= 868:
                marker += f'<path d="M{x:.1f} 27 V248" stroke="{color}"><title>{label}</title></path>'
    chart = f'''<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>AirDrop benchmark throughput</title>
<style>
body{{max-width:58rem;margin:2rem auto;padding:0 1rem;font:1rem system-ui;background:#111827;color:#e5edf8}}
svg{{width:100%;background:#192338;border-radius:.5rem}}text{{fill:#d6e2f2;font:12px system-ui}}
rect{{fill:#67d3bb}}rect:hover{{fill:#fff}}a{{color:#9ad8fd}}
</style>
<h1>AirDrop TX throughput</h1>
<p>Captured transport payload per second, measured from benchmark launch.</p>
<svg viewBox="0 0 910 294" role="img" aria-label="AirDrop transmit throughput over time">
<path d="M54 27 V248 H881" stroke="#8799af" fill="none"/>
<text x="8" y="33">{peak:.2f}</text><text x="12" y="246">0</text>
{"".join(bars)}{marker}{labels}
</svg>
<p><a href="timeline.csv">Download the time series in MiB/s</a></p></html>
'''
    path.write_text(chart, encoding='utf-8')


def positive(value, lower, upper):
    try:
        number = int(value)
    except ValueError:
        raise argparse.ArgumentTypeError('expected an integer') from None
    if not lower <= number <= upper:
        raise argparse.ArgumentTypeError(f'expected an integer from {lower} to {upper}')
    return number


def channel_label(result):
    channels = result.get('ap_channels', [])
    if not channels:
        return '—'
    return ', '.join(f'{row["band_ghz"] or "?"} GHz / ch {row["channel"]}'
                     + (f' / {row["width_mhz"]} MHz' if row['width_mhz'] else '')
                     for row in channels)


def save_overview(output, attempts, restored, suite_error):
    rows = []
    for attempt in attempts:
        report = attempt.get('result', {})
        action = report.get('action') or {}
        rows.append({
            'trial': attempt['trial'], 'scenario': attempt['scenario'],
            'outcome': report.get('outcome', attempt.get('outcome', 'no_summary')),
            'analysis_error': attempt.get('analysis_error'),
            'conditions_verified': report.get('scenario_valid', False),
            'action': action.get('status'),
            'ap_channels': report.get('ap_channels', []),
            'file_mib_s': report.get('file_mib_s_picker_to_success'),
            'native_upload_mib_s': report.get('native_upload_mib_s'),
            'tx_payload_mib_s': report.get('tx_payload_mib_s_over_span'),
            'total_s': report.get('picker_to_terminal_s') or report.get('elapsed_s'),
            'stall_idle_s': (report.get('stall') or {}).get('idle_s'),
            'cancellation': (report.get('cancellation') or {}).get('status'),
            'connect_first_packet_ms': (report.get('recipient_to_first_flow_ms')
                                        if report.get('picker_to_recipient_ms') is not None
                                        else report.get('picker_to_first_flow_ms')),
            'connect_to_bulk_ms': report.get('connect_to_bulk_ms'),
            'tx_span_s': report.get('tx_payload_span_seconds'),
            'p95_tx_gap_ms': (report.get('gap_ms', {}).get('tx_payload') or {}).get('p95'),
            'max_tx_gap_ms': (report.get('gap_ms', {}).get('tx_payload') or {}).get('max'),
            'report': attempt.get('report'),
        })
    write_json(output / 'overview.json', {'network_restored': restored, 'error': suite_error, 'trials': rows})
    lines = ['# AirDrop Benchmark Overview', '', f'Network restored: {restored}', '']
    if suite_error:
        lines.extend((f'Benchmark interrupted: {suite_error}', ''))
    lines += ['| Trial | Scenario | AirDrop result | Cancel | Wi-Fi action | Conditions verified | AP | Total s | First packet / bulk startup ms | TX span s | File MiB/s | Upload MiB/s | TX payload MiB/s | P95 / max TX gap ms |',
              '|---|---|---|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|']
    def fmt(value):
        return f'{value:.2f}' if value is not None else '—'

    for row in rows:
        trial = f'[{row["trial"]}]({row["report"]})' if row['report'] else row['trial']
        values = (trial, row['scenario'], row['outcome'], row['cancellation'] or '—', row['action'] or '—',
                  'yes' if row['conditions_verified'] else 'no', channel_label(row),
                  fmt(row['total_s']), f'{fmt(row["connect_first_packet_ms"])} / {fmt(row["connect_to_bulk_ms"])}',
                  fmt(row['tx_span_s']), fmt(row['file_mib_s']),
                  fmt(row['native_upload_mib_s']),
                  fmt(row['tx_payload_mib_s']),
                  f'{fmt(row["p95_tx_gap_ms"])} / {fmt(row["max_tx_gap_ms"])}')
        lines.append('| ' + ' | '.join(values) + ' |')
    lines += ['', 'File rate over total picker time includes recipient selection and acceptance. '
              'Connect measurements start at automatic selection or at opening the manual picker. '
              'Captured TX payload rate covers the data span and includes transport overhead and repeats. '
              'Compare runs with verified conditions and the same file size.']
    (output / 'overview.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return rows


def run_suite(args, output):
    attempts = []
    suite_error = None
    restored = None
    with tempfile.TemporaryDirectory(prefix='mtk-airdrop-') as temp:
        temp = Path(temp)
        binary = build_picker(temp)
        if subprocess.run([str(binary), '--accessibility-status']).returncode:
            raise RuntimeError('Automatic AirDrop cancellation needs Accessibility permission for Terminal. '
                               'Enable it in System Settings → Privacy & Security → Accessibility, then retry.')
        if args.file:
            file = args.file.expanduser().resolve(strict=True)
            if not file.is_file() or file.stat().st_size == 0:
                raise ValueError('--file must be a non-empty regular file')
        else:
            file = temp / f'AirDrop-Benchmark-{args.mib}MiB.bin'
        initial = network_state(binary)
        session = NetworkSession(binary, initial, output)
        ethernet = ', '.join(initial['ethernet_up']) or 'none'
        print(f'WARNING: This benchmark temporarily disconnects Ethernet ({ethernet}) '
              'and may disconnect Wi-Fi or turn the Wi-Fi radio off. Internet access and '
              'remote sessions can drop. Network restoration is attempted when it ends.', flush=True)
        if input('Type yes to continue: ').strip().lower() != 'yes':
            print('Benchmark cancelled before changing the network.')
            return 1
        if not args.file:
            print(f'Preparing a {args.mib} MiB test file…', flush=True)
            with file.open('xb') as stream:
                for _ in range(args.mib):
                    stream.write(os.urandom(MIB))
        try:
            session.prepare()
            cases = args.scenario or DEFAULT_SCENARIOS
            for repetition in range(1, args.runs + 1):
                for index, scenario in enumerate(cases, 1):
                    trial = f'{repetition:02d}-{index:02d}-{scenario}'
                    attempt = {'trial': trial, 'scenario': scenario, 'outcome': 'setup_failed'}
                    attempts.append(attempt)
                    print(f'[{trial}]', flush=True)
                    state = session.prepare_scenario(scenario)
                    if state is None:
                        attempt['outcome'] = 'skipped_condition'
                        continue
                    if scenario in ACTIONS and scenario != 'wifi-scan':
                        session.wifi_may_have_changed = True
                    directory = output / trial
                    directory.mkdir()
                    config = live_run(args, directory, binary, file, scenario, state)
                    attempt.update(directory=directory, config=config, outcome=config['status'])
                    print(f'  AirDrop result: {config["status"]}', flush=True)
                    if config.get('error'):
                        print(f'  {config["error"]}', file=sys.stderr, flush=True)
                    if config['status'] in ('interrupted', 'ethernet_reenabled', 'setup_failed'):
                        raise RuntimeError(config.get('error', config['status']))
                    cancellation = config.get('cancellation')
                    if cancellation and cancellation['status'] == 'unconfirmed':
                        if cancellation.get('ax_event') == 'accessibility_unavailable':
                            raise RuntimeError('AirDrop cancellation needs Accessibility permission for Terminal')
                        raise RuntimeError('macOS did not confirm the end of the AirDrop session after cancellation')
        except (KeyboardInterrupt, EOFError, RuntimeError, OSError, ValueError, subprocess.SubprocessError) as error:
            suite_error = str(error) or 'interrupted'
        finally:
            restored = session.restore()
    for attempt in attempts:
        if 'config' not in attempt:
            continue
        directory, config = attempt['directory'], attempt['config']
        try:
            attempt['result'] = build_report(directory, directory, config)
            attempt['report'] = f'{directory.name}/report.md'
            system_logs(directory, config)
        except (OSError, ValueError, KeyError) as error:
            attempt['analysis_error'] = str(error)
            print(f'Could not finish the report for {directory.name}: {error}', file=sys.stderr)
    rows = save_overview(output, attempts, restored, suite_error)
    for row in rows:
        if row['native_upload_mib_s'] is not None:
            rate = f' · upload {row["native_upload_mib_s"]:.2f} MiB/s'
        else:
            rate = f' · TX {row["tx_payload_mib_s"]:.2f} MiB/s' if row['tx_payload_mib_s'] is not None else ''
        action = f' · action {row["action"]}' if row['action'] else ''
        print(f'{row["trial"]}: {row["outcome"]}{action}{rate}')
    print(f'Overview: {output / "overview.md"}')
    return 0 if rows and restored and not suite_error and all(
        row['outcome'] == 'shared' and row['conditions_verified'] for row in rows) else 1


def interrupted(signum, frame):
    raise KeyboardInterrupt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument('--mib', type=lambda value: positive(value, 1, 1024), default=64,
                        help='random test file size in MiB (default: 64)')
    source.add_argument('--file', type=Path, help='share an existing file instead of random data')
    parser.add_argument('--recipient', help='automatically select this exact AirDrop device name using Accessibility')
    parser.add_argument('--recipient-timeout', type=lambda value: positive(value, 1, 300), default=15,
                        help='seconds to find the automatic recipient (default: 15)')
    parser.add_argument('--connect-timeout', type=lambda value: positive(value, 1, 300), default=30,
                        help='seconds to observe transfer start after selection, or after opening the manual picker (default: 30)')
    parser.add_argument('--timeout', type=lambda value: positive(value, 1, 1800), default=300,
                        help='seconds from observed transfer start to completion (default: 300)')
    parser.add_argument('--stall-timeout', type=lambda value: positive(value, 0, 1800), default=15,
                        help='seconds without bulk TX progress after startup (default: 15; 0 disables)')
    parser.add_argument('--scenario', action='append', choices=SCENARIOS,
                        help='repeat to choose scenarios and order (default: wifi, wifi-scan, wifi-disconnect, direct)')
    parser.add_argument('--runs', type=lambda value: positive(value, 1, 10), default=1,
                        help='repeat the scenario matrix')
    parser.add_argument('--interface', action='append', metavar='IFACE',
                        help='override the capture interface(s); by default use awdl0 and associated Wi-Fi')
    parser.add_argument('--analyze', type=Path, metavar='TRIAL_DIR',
                        help='reanalyze a saved trial; skips sudo and AirDrop')
    parser.add_argument('--flow', type=lambda value: positive(value, 1, 10**9),
                        help='select a flow by its number in flows.csv when reanalyzing')
    args = parser.parse_args()
    if args.flow and not args.analyze:
        parser.error('--flow requires --analyze')
    if args.recipient is not None and not args.recipient.strip():
        parser.error('--recipient must be a non-empty device name')
    args.interface = list(dict.fromkeys(args.interface or []))
    if not all(re.fullmatch(r'[A-Za-z][A-Za-z0-9]{0,14}', name) for name in args.interface):
        parser.error('invalid interface name')
    os.umask(0o077)
    try:
        if args.analyze:
            output = args.analyze.expanduser().resolve()
            config = json.loads((output / 'run.json').read_text(encoding='utf-8'))
            if not 0 <= config['end_ns'] - config['start_ns'] <= 2500 * NS:
                raise ValueError('invalid capture time window in run.json')
            analysis = Path(tempfile.mkdtemp(prefix='analysis-', dir=output))
            build_report(output, analysis, config, args.flow)
            print(f'Report: {analysis / "report.md"}')
            return 0
        if sys.platform != 'darwin' or os.geteuid() == 0 or not sys.stdin.isatty():
            parser.error('run in Terminal as the macOS desktop user')
        signal.signal(signal.SIGHUP, interrupted)
        signal.signal(signal.SIGTERM, interrupted)
        # Avoid simultaneous changes to the same host network state.
        lock_path = Path(tempfile.gettempdir()) / f'mtk-airdrop-benchmark-{os.getuid()}.lock'
        with lock_path.open('a') as lock:
            try:
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise RuntimeError('another AirDrop benchmark is already running') from None
            DEFAULT_OUTPUT.mkdir(exist_ok=True)
            output = Path(tempfile.mkdtemp(prefix='airdrop-benchmark-', dir=DEFAULT_OUTPUT))
            print(f'Run folder: {output}', flush=True)
            return run_suite(args, output)
    except (KeyboardInterrupt, EOFError):
        print('Benchmark interrupted.', file=sys.stderr)
        return 130
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f'Benchmark: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
