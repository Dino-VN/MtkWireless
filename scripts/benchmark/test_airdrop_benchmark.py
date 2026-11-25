"""Exercise the benchmark using synthetic captures, without changing the network."""
import csv
from datetime import datetime
import importlib.util
import ipaddress
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from airdrop_benchmark_network import NetworkSession, action_state_matches, matches_scenario
from airdrop_benchmark_packets import NS, decode_frame, write_json

HERE = Path(__file__).resolve().parent
LOCAL = 'fe80::1'
PEER = 'fe80::2'
STAMP = 1_700_000_000 * NS
MAC = b'\x02\x00\x00\x00\x00\x01'
REMOTE_MAC = b'\x02\x00\x00\x00\x00\x02'
ASSOCIATED = {'ethernet_up': [], 'wifi_powered': True, 'wifi_associated': True,
              'channel': 149, 'band_ghz': 5, 'width_mhz': 80}


def udp_frame(remote=PEER, reply=False):
    payload = 1200
    src, dst = (remote, LOCAL) if reply else (LOCAL, remote)
    source_port, destination_port = (8770, 53201) if reply else (53201, 8770)
    ethernet = (MAC + REMOTE_MAC if reply else REMOTE_MAC + MAC) + b'\x86\xdd'
    ipv6 = struct.pack('!IHBB16s16s', 6 << 28, 8 + payload, 17, 32,
                       ipaddress.ip_address(src).packed, ipaddress.ip_address(dst).packed)
    udp = struct.pack('!HHHH', source_port, destination_port, 8 + payload, 0)
    return ethernet + ipv6 + udp + bytes(payload)


def add_record(stream, elapsed_ms, frame):
    stamp = STAMP + elapsed_ms * 1_000_000
    sec, nano = divmod(stamp, NS)
    data = frame[:192]
    stream.write(struct.pack('<IIII', sec, nano, len(data), len(frame)))
    stream.write(data)


def make_pcap(path, ambiguous=False):
    with path.open('wb') as stream:
        stream.write(struct.pack('<IHHIIII', 0xa1b23c4d, 2, 4, 0, 0, 192, 1))
        for elapsed in (200, 300, 400, 2200, 2220, 3800):
            add_record(stream, elapsed, udp_frame())
            if ambiguous:
                add_record(stream, elapsed + 1, udp_frame('fe80::3'))
        add_record(stream, 4000, udp_frame(reply=True))


def config_fixture():
    return {
        'status': 'shared', 'scenario': 'wifi-scan', 'file_bytes': 6000,
        'start_ns': STAMP, 'end_ns': STAMP + 6_200_000_000,
        'wifi_before': ASSOCIATED, 'wifi_after': ASSOCIATED,
        'captures': {'awdl0': {'pcap': 'awdl0.pcap', 'addresses': [LOCAL], 'macs': [],
                              'kernel_drops': 0, 'exit_status': 0}},
        'action': {'status': 'ok', 'start_ns': STAMP + NS, 'end_ns': STAMP + 1_500_000_000,
                   'trigger': {'interface': 'awdl0', 'transport': 'UDP',
                               'local_ip': LOCAL, 'local_port': 53201, 'peer_ip': PEER, 'peer_port': 8770}},
    }


def send_log():
    category = b'AirDrop.ABCDEF12'
    def line(milliseconds, category, message):
        stamp = datetime.fromtimestamp(STAMP // NS + milliseconds / 1000)
        return stamp.isoformat(sep=' ', timespec='milliseconds').encode() + (
            b' Df sharingd[2039:21bb8] [com.apple.sharing:' + category + b'] ' + message)

    return (
        line(1000, category, b'Send StateMachine UPLOAD START'),
        line(4998, b'AirDropNW', b'Received UPLOAD response'),
        line(4998, b'AirDropNW', b'Canceling send transfer <private>'),
        line(4999, category, b'Send StateMachine UPLOAD END'),
        line(5000, b'Metrics', b"MetricEvent 'com.apple.sharing.airdrop.send-finish' : "
             b'{"size": 6000, "result": 0, "transferID": "ABCDEF12-E111-4FF4-8224-19726211CC99"}'),
    )


class BenchmarkTest(unittest.TestCase):
    def analyze(self, directory, *args):
        finished = subprocess.run([str(HERE / 'AirDrop-Benchmark.command'), '--analyze', str(directory),
                                   *args], capture_output=True, text=True, check=True)
        return Path(finished.stdout.strip().removeprefix('Report: ')).parent

    def test_detailed_report(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            config = config_fixture()
            config['transfer_start_evidence'] = dict(config['action']['trigger'], seen_ns=STAMP + 400_000_000)
            config['stall'] = {'detected_ns': STAMP + 6 * NS, 'last_progress_ns': STAMP + 3800000000,
                               'last_tx_ns': STAMP + 3800000000, 'last_rx_ns': STAMP + 4 * NS,
                               'last_tx_interface': 'awdl0', 'capture_active': True,
                               'idle_s': 2.2, 'tx_payload_bytes': 7200}
            write_json(path / 'run.json', config)
            make_pcap(path / 'awdl0.pcap')
            start, response, _, end, finish = send_log()
            (path / 'system-live.log').write_bytes(b'\n'.join((start, response, end, finish)) + b'\n')
            (path / 'trial-events.jsonl').touch()
            (path / 'awdl0-routes.log').touch()
            events = [{'event': 'picker_requested', 'wall_ns': STAMP, 'uptime_ns': 0},
                      {'event': 'shared', 'wall_ns': STAMP + 5 * NS, 'uptime_ns': 5 * NS}]
            (path / 'sharing-events.jsonl').write_text(''.join(json.dumps(e) + '\n' for e in events))
            selection = {'event': 'recipient_selected', 'wall_ns': STAMP + 100_000_000,
                         'uptime_ns': 100_000_000}
            (path / 'recipient-events.jsonl').write_text(json.dumps(selection) + '\n')
            analysis = self.analyze(path)
            summary = json.loads((analysis / 'summary.json').read_text())
            self.assertEqual(summary['outcome'], 'shared')
            self.assertIsNotNone(summary['native_upload_mib_s'])
            self.assertTrue(summary['scenario_valid'])
            self.assertEqual(summary['picker_to_terminal_s'], 5)
            self.assertEqual(summary['picker_to_recipient_ms'], 100)
            self.assertEqual(summary['recipient_to_first_flow_ms'], 100)
            self.assertEqual(summary['connect_to_bulk_ms'], 300)
            self.assertEqual(summary['stall']['elapsed_ms'], 6000)
            self.assertEqual(summary['diagnostic_files']['system-live.log'], '../system-live.log')
            self.assertEqual(summary['diagnostic_files']['awdl0-routes.log'], '../awdl0-routes.log')
            self.assertEqual(summary['selected_flow']['tx_payload_bytes'], 7200)
            self.assertEqual(summary['gap_ms']['tx_payload']['min'], 20)
            self.assertEqual(summary['gap_ms']['tx_payload']['mean'], 720)
            self.assertEqual(summary['gap_ms']['tx_payload']['max'], 1800)
            self.assertEqual(summary['action_to_next_tx_payload_ms'], 1200)
            self.assertEqual(summary['action_tx_payload_packets'], {'before': 3, 'after': 3})
            self.assertEqual(summary['ap_channels'][0]['channel'], 149)
            with (analysis / 'packets.csv').open() as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 7)
            self.assertEqual(rows[3]['action_phase'], 'after')
            self.assertEqual(rows[6]['direction'], 'rx')
            with (analysis / 'timeline.csv').open() as stream:
                timeline = list(csv.DictReader(stream))
            self.assertEqual(len(timeline), 7)
            self.assertEqual(timeline[-1]['tx_payload_bytes'], '0')
            self.assertIn('Wi-Fi action started', (analysis / 'timeline.html').read_text())
            self.assertIn('Stall detected', (analysis / 'timeline.html').read_text())

    def test_ambiguous_flow_and_partial_capture(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            write_json(path / 'run.json', config_fixture())
            capture = path / 'awdl0.pcap'
            make_pcap(capture, ambiguous=True)
            with capture.open('ab') as stream:
                stream.write(b'\x12')
            analysis = self.analyze(path)
            result = json.loads((analysis / 'summary.json').read_text())
            self.assertEqual(result['attribution'], 'multiple_bulk_candidates')
            self.assertEqual(result['capture_quality']['awdl0']['error'], 'partial pcap record header')
            analysis = self.analyze(path, '--flow', '1')
            result = json.loads((analysis / 'summary.json').read_text())
            self.assertEqual(result['attribution'], 'manual')
            self.assertEqual(result['selected_flow']['peer_ip'], PEER)

    def test_network_conditions_and_decoder(self):
        direct = dict(ASSOCIATED, wifi_associated=False)
        self.assertTrue(matches_scenario(ASSOCIATED, 'wifi-5'))
        self.assertFalse(matches_scenario(ASSOCIATED, 'wifi-2.4'))
        self.assertFalse(matches_scenario(dict(direct, ethernet_up=['en0']), 'direct'))
        self.assertTrue(action_state_matches(direct, 'wifi-disconnect'))
        self.assertTrue(action_state_matches(dict(direct, wifi_powered=False), 'wifi-radio-off'))
        self.assertIsNone(decode_frame(udp_frame()[:61]))
        self.assertEqual(decode_frame(udp_frame()[:62]).payload, 1200)

    def test_live_trigger_and_overview(self):
        spec = importlib.util.spec_from_file_location('benchmark', HERE / 'airdrop-benchmark.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        args = SimpleNamespace(recipient='exact device', recipient_timeout=15, connect_timeout=30)
        timing = module.TrialTiming(args, 0)
        self.assertIsNone(timing.timeout_status(14))
        self.assertEqual(timing.timeout_status(15), 'recipient_timeout')
        timing.selected(5, 30)
        self.assertEqual(timing.timeout_status(35), 'connect_timeout')
        timing.started(20, 300)
        self.assertIsNone(timing.timeout_status(319))
        self.assertEqual(timing.timeout_status(320), 'transfer_timeout')
        flow = ('awdl0', 'TCP', ipaddress.ip_address(LOCAL).packed, 53201,
                ipaddress.ip_address(PEER).packed, 8770)
        progress = module.TransferProgress(flow, STAMP, 1, 1000, 1000, 20)
        packet = SimpleNamespace(payload=1000, proto='TCP', seq=0xfffffff0)
        progress.observe(flow, 2, STAMP + NS, 'tx', packet, 21)
        for index in range(3, 20):
            progress.observe(flow, index, STAMP + index * NS, 'tx', packet, 21 + index)
        other_peer = (*flow[:4], ipaddress.ip_address('fe80::3').packed, flow[5])
        progress.observe(other_peer, 20, STAMP + 20 * NS, 'tx', packet, 40)
        progress.observe(flow, 21, STAMP + 20 * NS, 'rx', packet, 40)
        sample = progress.snapshot(40)
        self.assertEqual(sample['tx_payload_bytes'], 2000)
        self.assertEqual(sample['last_rx_ns'], STAMP + 20 * NS)
        self.assertTrue(timing.stalled(sample, 15, 6000))
        self.assertFalse(timing.stalled(sample, 0, 6000))
        self.assertFalse(timing.stalled(dict(sample, tx_payload_bytes=421865), 15, 64 * 1024**2))
        self.assertFalse(timing.stalled(dict(sample, tx_payload_bytes=6000), 15, 6000))
        progress.observe(flow, 22, STAMP + 21 * NS, 'tx', SimpleNamespace(payload=1000, proto='TCP', seq=984), 41)
        self.assertFalse(timing.stalled(progress.snapshot(41), 15, 6000))
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            config = config_fixture()
            config['file_bytes'] = 32000
            pcap = path / 'awdl0.pcap'
            pcap.touch()
            activity = module.ActivityTrigger(path, config)
            try:
                for worker in activity.workers:
                    worker.start()
                time.sleep(.1)
                with pcap.open('wb') as stream:
                    stream.write(struct.pack('<IHHIIII', 0xa1b23c4d, 2, 4, 0, 0, 192, 1))
                    for index in range(10):
                        add_record(stream, 20 + index, udp_frame())
                self.assertTrue(activity.stop.wait(2))
                self.assertEqual(activity.match['capture_index'], 7)
                self.assertEqual(activity.match['peer_ip'], PEER)
            finally:
                activity.stop.set()
                for worker in activity.workers:
                    worker.join()
            rows = module.save_overview(path, [
                {'scenario': 'wifi', 'trial': 'first', 'outcome': 'skipped_condition'},
                {'scenario': 'direct', 'trial': 'second', 'outcome': 'timeout',
                 'report': 'second/report.md'}], True, None)
            self.assertEqual([row['outcome'] for row in rows], ['skipped_condition', 'timeout'])
            self.assertIn('[second](second/report.md)', (path / 'overview.md').read_text())

    def test_ethernet_restores_after_partial_setup_failure(self):
        initial = dict(ASSOCIATED, wifi_interface='en1', ethernet_up=['en0', 'en2'])
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            session = NetworkSession(path / 'native', initial, path)
            ok = subprocess.CompletedProcess([], 0)
            failure = subprocess.CalledProcessError(1, 'ifconfig')
            with patch('airdrop_benchmark_network.subprocess.run', side_effect=[ok, ok, failure, ok, ok]) as run, \
                 patch('airdrop_benchmark_network.network_state', return_value=initial):
                with self.assertRaises(subprocess.CalledProcessError):
                    session.prepare()
                self.assertTrue(session.restore())
            commands = [entry.args[0][-2:] for entry in run.call_args_list[1:]]
            self.assertEqual(commands, [['en0', 'down'], ['en2', 'down'], ['en0', 'up'], ['en2', 'up']])
            self.assertFalse(session.recovery.exists())

    def test_native_cancellation_status(self):
        spec = importlib.util.spec_from_file_location('benchmark', HERE / 'airdrop-benchmark.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        helper = type('Picker', (), {'pid': 123, 'poll': lambda self: None,
                                     'wait': lambda self, timeout: 0})()
        for native, ax, final in (('cancelled', 'cancel_pressed', 'cancelled'),
                                  ('shared', 'cancel_pressed', 'native_ended'),
                                  (None, 'cancel_tile_cleared', 'tile_cancelled'),
                                  (None, 'cancel_tile_missing', 'unconfirmed')):
            with self.subTest(native=native, ax=ax), tempfile.TemporaryDirectory() as temp:
                path = Path(temp)

                def cancel(command, stdout, **kwargs):
                    event = {'event': ax, 'wall_ns': STAMP, 'uptime_ns': 0}
                    stdout.write((json.dumps(event) + '\n').encode())
                    if native:
                        event['event'] = native
                        (path / 'sharing-events.jsonl').write_text(json.dumps(event) + '\n')
                    return subprocess.CompletedProcess(command, int(ax != 'cancel_pressed'))

                active = 'Dino' if ax.startswith('cancel_tile_') else None
                with patch.object(module.subprocess, 'run', side_effect=cancel) as command, \
                     (path / 'trial-events.jsonl').open('w') as events:
                    result = module.cancel_pending_share(path / 'native', path, helper, events, active)
                self.assertEqual(command.call_args.args[0][1:], ['--cancel-share', '123'] + ([active] if active else []))
                self.assertEqual(result['status'], final)
                self.assertEqual(result['native_outcome'], native)
                self.assertIn('airdrop_cancel_finished', (path / 'trial-events.jsonl').read_text())

    def test_send_completion_matches_one_upload_and_file_size(self):
        spec = importlib.util.spec_from_file_location('benchmark', HERE / 'airdrop-benchmark.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        start, response, cancel, end, finish = send_log()
        for lines, expected in (
                ([start, response, end, finish], True),
                ([start, end, finish], False),
                ([start, cancel, end, finish], False),
                ([start, response, cancel, end, finish], False),
                ([start, end, response, finish], False),
                ([start, response.replace(b'2039:', b'2040:'), end, finish], False),
                ([start, response, finish], False),
                ([start, response, end, finish.replace(b': 0,', b': 2,')], False),
                ([start, response, end, finish.replace(b'6000', b'7000')], False),
                ([start, response, end, finish.replace(b'ABCDEF12', b'0000ABCD')], False),
                ([start, start.replace(b'ABCDEF12', b'0000ABCD'), response, end, finish], False)):
            with self.subTest(lines=lines), tempfile.TemporaryDirectory() as temp:
                path = Path(temp) / 'system-live.log'
                observer = module.SendCompletion(STAMP, 6000)
                path.write_bytes(b'\n'.join(lines))
                self.assertIsNone(observer.read(path))
                with path.open('ab') as stream:
                    stream.write(b'\n')
                self.assertEqual(bool(observer.read(path)), expected)

    def test_report_needs_delivery_before_cancellation(self):
        start, response, cancel, end, finish = send_log()
        for status, lines, cancel_ns, expected in (
                ('transfer_stalled', (start, cancel, end, finish), 4900, 'transfer_stalled'),
                ('transfer_stalled', (start, response, end, finish), 5000, 'transfer_stalled'),
                ('transfer_stalled', (start, response, end, finish), 5100, 'shared'),
                ('shared', (), None, 'delivery_unconfirmed')):
            with self.subTest(status=status, lines=lines, cancel_ns=cancel_ns), tempfile.TemporaryDirectory() as temp:
                path = Path(temp)
                config = dict(config_fixture(), status=status)
                if cancel_ns is not None:
                    config['cancellation'] = {'start_ns': STAMP + cancel_ns * 1_000_000, 'status': 'tile_cancelled'}
                write_json(path / 'run.json', config)
                make_pcap(path / 'awdl0.pcap')
                (path / 'system-live.log').write_bytes(b'\n'.join(lines) + b'\n')
                events = [
                    {'event': 'picker_requested', 'wall_ns': STAMP, 'uptime_ns': 0},
                    {'event': 'shared', 'wall_ns': STAMP + 5 * NS, 'uptime_ns': 5 * NS},
                ]
                (path / 'sharing-events.jsonl').write_text(''.join(json.dumps(e) + '\n' for e in events))
                result = json.loads((self.analyze(path) / 'summary.json').read_text())
                self.assertEqual(result['outcome'], expected)
                for rate in ('native_upload_mib_s', 'file_mib_s_picker_to_success'):
                    self.assertEqual(result[rate] is not None, expected == 'shared')

    def test_default_transfer_watch_ignores_lan_capture(self):
        spec = importlib.util.spec_from_file_location('benchmark', HERE / 'airdrop-benchmark.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        config = dict(file_bytes=64 * 1024**2, captures={'en1': {}, 'awdl0': {}})
        with patch.object(module, 'ActivityTrigger') as trigger:
            module.watch_transfer(HERE, config, SimpleNamespace(interface=[]), 'wifi-scan', STAMP)
            self.assertEqual(len(trigger.call_args_list), 2)
            for call in trigger.call_args_list:
                self.assertEqual(set(call.args[1]['captures']), {'awdl0'})
                self.assertEqual(call.args[1]['start_ns'], STAMP)


if __name__ == '__main__':
    unittest.main()
