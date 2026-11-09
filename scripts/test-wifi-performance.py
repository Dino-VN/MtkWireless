#!/usr/bin/env python3
"""Offline stdlib tests; every hardware command is mocked or forbidden."""
import contextlib
import csv
import importlib.util
import io
import json
from pathlib import Path
import plistlib
import random
import subprocess
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).with_name('capture-wifi-performance.py')


def load_collector():
    spec = importlib.util.spec_from_file_location('wifi_performance', SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def entry(classname, identity=1, **properties):
    return dict(properties, IOObjectClass=classname, IORegistryEntryID=identity)


def completed(stdout=b'', returncode=0):
    return subprocess.CompletedProcess([], returncode, stdout, b'PRIVATE-STDERR')


def module_row(bundle, version='1.0.12'):
    return f'210 0 0xffffff7f12340000 0xbd000 0xbd000 {bundle} ({version}) PRIVATE-UUID <7>\n'.encode()


class Clock:
    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class PerformanceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with mock.patch('subprocess.run', side_effect=AssertionError('command on import')):
            cls.capture = load_collector()

    def setUp(self):
        # An accidental unmocked command must fail, never consult this machine.
        guard = mock.patch('subprocess.run', side_effect=AssertionError('unmocked hardware command'))
        guard.start()
        self.addCleanup(guard.stop)
        guard = mock.patch('subprocess.Popen', side_effect=AssertionError('no processes/listeners'))
        guard.start()
        self.addCleanup(guard.stop)

    def registry_snapshot(self, infrastructure, awdl):
        outputs = {'AirportMtkWlan': plistlib.dumps(infrastructure),
                   'AirportMtkAwdlInterface': plistlib.dumps(awdl)}
        with mock.patch('subprocess.run', side_effect=lambda command, **kwargs: completed(outputs[command[-1]])) as run:
            result = self.capture.Collector().snapshot()
        self.assertEqual([call.args[0] for call in run.call_args_list], [
            ['/usr/sbin/ioreg', '-a', '-r', '-c', 'AirportMtkWlan'],
            ['/usr/sbin/ioreg', '-a', '-r', '-c', 'AirportMtkAwdlInterface']])
        for call in run.call_args_list:
            self.assertIs(call.kwargs['stderr'], subprocess.DEVNULL)
            self.assertEqual(call.kwargs['timeout'], 5)
        return result

    def sample_series(self, values, other=None):
        samples = []
        for index, value in enumerate(values):
            metrics = {} if value is None else {'MTKAWDLDataRxDecoded': value}
            metrics.update(other or {})
            samples.append({'index': index, 'timestamp_utc': '2026-11-09T00:00:00+00:00',
                            'elapsed_seconds': float(index), 'query_seconds': 0.1,
                            'sources': {'infrastructure': {'status': 'missing', 'nodes': {}},
                                        'awdl': {'status': 'ok', 'nodes': {
                                            '0': {'identity_available': True, 'metrics': metrics}}}}})
        return samples

    def test_import_has_no_side_effects(self):
        with contextlib.ExitStack() as stack:
            actions = [stack.enter_context(mock.patch(name)) for name in (
                'subprocess.run', 'subprocess.Popen', 'pathlib.Path.mkdir',
                'pathlib.Path.open', 'pathlib.Path.write_text',
                'tempfile.mkdtemp', 'time.sleep')]
            output = stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            load_collector()
        self.assertEqual(output.getvalue(), '')
        for action in actions:
            action.assert_not_called()

    def test_explicit_whitelist_and_privacy(self):
        private = {
            'SSID': 'PRIVATE-SSID', 'BSSID': '02:11:22:33:44:55',
            'IOMACAddress': b'PRIVATE-MAC', 'IP': '192.168.1.9',
            'IORegistryEntryName': 'PRIVATE-NODE-NAME',
            'MTKWiFiJoin': {'PMK': b'PRIVATE-KEY', 'MTKWiFiReportedPower': 999},
            'MTKAWDLDataRxTrace': b'PRIVATE-PAYLOAD',
            'MTKAWDLDataRxInvalidTrace': b'PRIVATE-PREFIX',
            'MTKAWDLDataRxLastRejected': b'PRIVATE-REJECTION',
            'MTKAWDLLastPeer': b'PRIVATE-PEER',
            'MTKAWDLCacheUnknownCounter': 987654,
            'MTKAWDLUnknownNumeric': 123456,
        }
        infrastructure = entry('AirportMtkWlan', 8765432109876,
                               MTKWiFiReportedPower=1, MTKAWDLInfraPowerSave=False,
                               **private)
        awdl = entry('AirportMtkAwdlInterface', 8765432109877,
                     MTKAWDLDataRxDecoded=27, MTKAWDLDensePresence=True,
                     MTKAWDLDataRxTooOld=b'PRIVATE-WRONG-TYPE',
                     MTKAWDLDataRxInvalid=True, **private)
        result = self.registry_snapshot([infrastructure], [awdl])
        self.assertEqual(result['infrastructure']['nodes']['0']['metrics'], {
            'MTKWiFiReportedPower': 1, 'MTKAWDLInfraPowerSave': False})
        self.assertEqual(result['awdl']['nodes']['0']['metrics'], {
            'MTKAWDLDataRxDecoded': 27, 'MTKAWDLDensePresence': True})
        encoded = json.dumps(result)
        for marker in ('PRIVATE', '02:11:22:33:44:55', '192.168.1.9',
                       '8765432109876', '8765432109877', '987654', '123456', 'MTKWiFiJoin'):
            self.assertNotIn(marker, encoded)

    def test_only_expected_classes_and_child_entries(self):
        child = entry('AirportMtkWlan', 2, MTKWiFiNativeLinkStatus=3)
        wrapper = entry('UnrelatedDriver', 100, MTKWiFiNativeLinkStatus=999,
                        IORegistryEntryChildren=[{'IORegistryEntryChildren': [child]}, 'invalid-child'])
        result = self.registry_snapshot([wrapper], [])
        self.assertEqual(result['infrastructure']['nodes']['0']['metrics'], {'MTKWiFiNativeLinkStatus': 3})
        self.assertEqual(result['awdl']['status'], 'missing')

    def test_awdl_is_queried_as_a_separate_root(self):
        infrastructure = entry('AirportMtkWlan', 1, MTKWiFiReportedPower=1,
                               IORegistryEntryChildren=[entry('AirportMtkAwdlInterface', 2,
                                                              MTKAWDLDataRxDecoded=999)])
        result = self.registry_snapshot([infrastructure], [entry('AirportMtkAwdlInterface', 2,
                                                                 MTKAWDLDataRxDecoded=17)])
        self.assertNotIn('MTKAWDLDataRxDecoded', result['infrastructure']['nodes']['0']['metrics'])
        self.assertEqual(result['awdl']['nodes']['0']['metrics']['MTKAWDLDataRxDecoded'], 17)

    def test_multiple_nodes_and_reenumeration_keep_counters_separate(self):
        collector = self.capture.Collector()
        snapshots = []
        for identities in ((10, 20), (20, 10), (30, 20)):
            raw = plistlib.dumps([entry('AirportMtkWlan', identity, MTKWiFiRxDeauth=identity)
                                   for identity in identities])
            with mock.patch('subprocess.run', side_effect=lambda command, **kwargs:
                            completed(raw if command[-1] == 'AirportMtkWlan' else plistlib.dumps([]))):
                snapshots.append(collector.snapshot()['infrastructure']['nodes'])
        self.assertEqual(snapshots[0], snapshots[1])
        self.assertEqual(set(snapshots[2]), {'1', '2'})
        self.assertEqual(snapshots[2]['2']['metrics']['MTKWiFiRxDeauth'], 30)

    def test_absent_identity_does_not_link_observations(self):
        collector = self.capture.Collector()
        raw = plistlib.dumps([{'IOObjectClass': 'AirportMtkWlan', 'MTKWiFiRxDeauth': 5}])
        with mock.patch('subprocess.run', side_effect=lambda command, **kwargs:
                        completed(raw if command[-1] == 'AirportMtkWlan' else plistlib.dumps([]))):
            first = collector.snapshot()['infrastructure']['nodes']
            last = collector.snapshot()['infrastructure']['nodes']
        self.assertEqual(set(first), {'0'})
        self.assertEqual(set(last), {'1'})
        self.assertFalse(first['0']['identity_available'])

    def test_numeric_and_boolean_types_are_strict(self):
        invalid = (True, -1, 2**64, 4.5, float('nan'), float('inf'),
                   '123', b'123', [123], {'number': 123}, None)
        for value in invalid:
            with self.subTest(value=value):
                self.assertFalse(self.capture.numeric(value))
        for value in (0, 1, 2**64 - 1):
            self.assertTrue(self.capture.numeric(value))
        raw = plistlib.dumps([entry('AirportMtkAwdlInterface', 1,
                                   MTKAWDLDensePresence=1, MTKAWDLDataTxPending=float('nan'))])
        nodes, status = self.capture.registry_nodes(raw, 'awdl')
        self.assertEqual(status, 'ok')
        self.assertEqual(nodes[0][1], {})

    def test_missing_and_invalid_plists(self):
        for raw in (b'', b'PRIVATE-NOT-A-PLIST', b'<?xml broken',
                    plistlib.dumps('not registry entries'), plistlib.dumps([1, 2])):
            with self.subTest(raw=raw):
                with mock.patch('subprocess.run', return_value=completed(raw)):
                    result = self.capture.Collector().snapshot()
                for source in result.values():
                    self.assertEqual(source, {'status': 'invalid_plist', 'nodes': {}})
        nodes, status = self.capture.registry_nodes(plistlib.dumps([]), 'awdl')
        self.assertEqual((nodes, status), ([], 'missing'))
        # Also accept a single dictionary root, without assuming an array.
        nodes, status = self.capture.registry_nodes(plistlib.dumps(entry('AirportMtkAwdlInterface')), 'awdl')
        self.assertEqual(status, 'ok')
        self.assertEqual(len(nodes), 1)

    def test_plist_limits_and_cycles(self):
        self.assertEqual(self.capture.registry_nodes(b'x' * (8 * 1024 * 1024 + 1), 'awdl'),
                         ([], 'invalid_plist'))
        root = entry('AirportMtkAwdlInterface', MTKAWDLDataRxDecoded=4)
        root['IORegistryEntryChildren'] = [root]
        with mock.patch.object(self.capture.plistlib, 'loads', return_value=[root]):
            nodes, status = self.capture.registry_nodes(b'fake plist', 'awdl')
        self.assertEqual(status, 'ok')
        self.assertEqual(len(nodes), 1)

    def test_query_errors_drop_partial_and_private_output(self):
        cases = ((FileNotFoundError('PRIVATE-PATH'), 'unavailable'),
                 (PermissionError('PRIVATE-PATH'), 'command_failed'),
                 (subprocess.TimeoutExpired('PRIVATE-COMMAND', 5, output=b'PRIVATE'), 'timeout'))
        for error, status in cases:
            with self.subTest(status=status):
                with mock.patch('subprocess.run', side_effect=error):
                    result = self.capture.Collector().snapshot()
                for source in result.values():
                    self.assertEqual(source, {'status': status, 'nodes': {}})
        with mock.patch('subprocess.run', return_value=completed(b'PRIVATE-PARTIAL', 1)):
            self.assertEqual(self.capture.query(['ignored']), (b'', 'command_failed'))

    def test_loaded_versions_are_exact_and_filtered(self):
        wifi, bt = self.capture.BUNDLES
        raw = (module_row(wifi) + module_row(bt, '1.0.9') + module_row(wifi + 'Extra', '9.9.9')
               + b'PRIVATE-COMMAND-OUTPUT\n')
        with mock.patch('subprocess.run', return_value=completed(raw)) as run:
            result = self.capture.loaded_versions()
        self.assertEqual(result, {wifi: {'status': 'observed', 'version': '1.0.12'},
                                  bt: {'status': 'observed', 'version': '1.0.9'}})
        self.assertEqual([call.args[0] for call in run.call_args_list], [
            ['/usr/bin/kmutil', 'showloaded', '--list-only', '--show', 'loaded', '--bundle-identifier', bundle]
            for bundle in (wifi, bt)])
        self.assertNotIn('PRIVATE', json.dumps(result))

    def test_missing_invalid_and_ambiguous_versions(self):
        wifi = self.capture.BUNDLES[0]
        for raw, status in ((b'', 'not_observed'),
                            (module_row(wifi, '1.0.12-PRIVATE'), 'not_observed'),
                            (module_row(wifi, '192.168.1.9'), 'not_observed'),
                            (f'{wifi} (1.0.12)'.encode(), 'not_observed'),
                            (module_row(wifi) + module_row(wifi, '1.0.11'), 'ambiguous')):
            with self.subTest(status=status):
                with mock.patch('subprocess.run', return_value=completed(raw)):
                    result = self.capture.loaded_versions()[wifi]
                self.assertEqual(result, {'status': status, 'version': None})
        with mock.patch('subprocess.run', side_effect=FileNotFoundError()):
            result = self.capture.loaded_versions()
        self.assertTrue(all(value == {'status': 'unavailable', 'version': None} for value in result.values()))

    def test_counter_resets_and_unobserved_intervals(self):
        samples = self.sample_series([100, 120, 5, 8, None, 40, 45])
        result = self.capture.metric_summary(samples, 'awdl', '0', 'MTKAWDLDataRxDecoded', 'counter')
        self.assertEqual((result['first'], result['last'], result['counter_delta']), (100, 45, 28))
        self.assertEqual((result['resets'], result['delta_intervals'], result['unobserved_intervals']), (1, 3, 2))
        self.assertEqual(result['missing_samples'], 1)
        samples = self.sample_series([10, None, 1, 3])
        result = self.capture.metric_summary(samples, 'awdl', '0', 'MTKAWDLDataRxDecoded', 'counter')
        self.assertEqual((result['counter_delta'], result['resets']), (2, 1))

    def test_no_observations_is_not_zero(self):
        for values, expected in (([None, None], None), ([7], None), ([100, 3], None), ([0, 0], 0)):
            with self.subTest(values=values):
                result = self.capture.metric_summary(self.sample_series(values), 'awdl', '0',
                                                     'MTKAWDLDataRxDecoded', 'counter')
                self.assertEqual(result['counter_delta'], expected)
        summary = self.capture.summarize(self.sample_series([None, None]), 1, 1, {})
        self.assertIn('MTKAWDLDataRxDecoded', summary['sources']['awdl']['unobserved_metrics'])
        self.assertIsNone(summary['sources']['awdl']['nodes']['0']['MTKAWDLDataRxDecoded']['first'])

    def test_gauges_flags_and_rates_have_no_counter_arithmetic(self):
        result = self.capture.metric_summary(self.sample_series([5, 1]), 'awdl', '0',
                                             'MTKAWDLDataRxDecoded', 'gauge')
        self.assertEqual((result['first'], result['last']), (5, 1))
        self.assertNotIn('counter_delta', result)
        summary = self.capture.summarize(self.sample_series([1, 2]), 1, 1, {})
        self.assertIsNone(summary['throughput_bytes_per_second'])
        self.assertIn('idle retained AWDL rate is not active link capacity', summary['notes'][0])
        self.assertEqual(self.capture.METRICS['infrastructure']['MTKAWDLTXPeer0RateMbps'], 'gauge')

    def reference_sources(self, samples):
        # Original rescanning implementation; keep independent of aggregation.
        return {
            source: {
                'unobserved_metrics': [name for name in self.capture.METRICS[source]
                                       if not any(name in data['metrics'] for sample in samples
                                                  for data in sample['sources'][source]['nodes'].values())],
                'nodes': {node: {name: self.capture.metric_summary(samples, source, node, name, kind)
                                 for name, kind in self.capture.METRICS[source].items()}
                          for node in sorted({node for sample in samples
                                              for node in sample['sources'][source]['nodes']}, key=int)},
            }
            for source in self.capture.CLASSES
        }

    def assert_sources_match_reference(self, samples):
        expected = self.reference_sources(samples)
        result = self.capture.summarize(samples, 60, 1, {}, interrupted=True)
        self.assertEqual(result['sources'], expected)
        # Equality alone does not check the serialized node/whitelist ordering.
        self.assertEqual(json.dumps(result['sources']), json.dumps(expected))
        self.assertIsNone(result['throughput_bytes_per_second'])
        self.assertTrue(result['interrupted'])
        self.assertEqual(result['sample_count'], len(samples))
        self.assertEqual(result['first_sample'], samples[0] if samples else None)
        self.assertEqual(result['last_sample'], samples[-1] if samples else None)
        intervals = [b['elapsed_seconds'] - a['elapsed_seconds']
                     for a, b in zip(samples, samples[1:])]
        self.assertEqual(result['interval_seconds'], self.capture.range_summary(intervals))
        self.assertEqual(result['query_seconds'], self.capture.range_summary(
            [sample['query_seconds'] for sample in samples]))

    def test_one_pass_summary_matches_reference_for_gaps_and_empty_captures(self):
        for values in ([], [None], [None, None], [7], [0, 0], [100, 3],
                       [10, None, 1, 3], [100, 120, 5, 8, None, 40, 45],
                       [None, 9, None, 2, None], [4, None, 8, 8, None, 1]):
            with self.subTest(values=values):
                self.assert_sources_match_reference(self.sample_series(values))
        samples = self.sample_series([None] * 3)
        for sample in samples:
            sample['sources']['awdl']['nodes'] = {}
        self.assert_sources_match_reference(samples)

    def test_one_pass_summary_matches_reference_for_all_metrics_and_nodes(self):
        rng = random.Random(7921)
        samples = self.sample_series([None] * 19)
        for index, sample in enumerate(samples):
            sample['elapsed_seconds'] = index * 1.125 + (index % 3) * 0.03125
            sample['query_seconds'] = (index % 5) * 0.125
            for source in self.capture.CLASSES:
                # Non-numeric insertion order; node 2 disappears and reappears,
                # node 99 is always empty, fresh anonymous slots are never linked.
                nodes = {'10': {'identity_available': True, 'metrics': {}},
                         '99': {'identity_available': True, 'metrics': {}},
                         str(100 + index): {'identity_available': False, 'metrics': {}}}
                if index % 3 != 1:
                    nodes['2'] = {'identity_available': True, 'metrics': {}}
                for node, data in nodes.items():
                    if node == '99':
                        continue
                    for name, kind in self.capture.METRICS[source].items():
                        if rng.randrange(4) != 0:
                            data['metrics'][name] = bool(rng.randrange(2)) if kind == 'boolean' else rng.randrange(20)
                sample['sources'][source]['nodes'] = nodes
        self.assert_sources_match_reference(samples)

    def test_summary_preserves_collector_identity_assignment_and_reappearance(self):
        collector = self.capture.Collector()
        samples = self.sample_series([None] * 5)
        for index, identities in enumerate(((10, None), (None,), (10, None), (), (10, None))):
            raw = plistlib.dumps([
                dict(IOObjectClass='AirportMtkAwdlInterface', MTKAWDLDataRxDecoded=10 - index,
                     **({} if identity is None else {'IORegistryEntryID': identity}))
                for identity in identities])
            with mock.patch.object(self.capture, 'query', side_effect=lambda command:
                                   (raw if command[-1] == 'AirportMtkAwdlInterface' else plistlib.dumps([]), 'ok')):
                samples[index]['sources'] = collector.snapshot()
        self.assertEqual([list(sample['sources']['awdl']['nodes']) for sample in samples],
                         [['0', '1'], ['2'], ['0', '3'], [], ['0', '4']])
        self.assert_sources_match_reference(samples)
        result = self.capture.summarize(samples, 4, 1, {})['sources']['awdl']['nodes']
        self.assertEqual(result['0']['MTKAWDLDataRxDecoded']['resets'], 2)
        self.assertIsNone(result['0']['MTKAWDLDataRxDecoded']['counter_delta'])

    def test_summary_operation_counts_are_linear_for_fresh_anonymous_nodes(self):
        def measure(count):
            operations = {'sample_visits': 0, 'node_visits': 0, 'node_lookups': 0}

            class Samples(list):
                def __iter__(self):
                    for value in super().__iter__():
                        operations['sample_visits'] += 1
                        yield value

            class Nodes(dict):
                def items(self):
                    for item in super().items():
                        operations['node_visits'] += 1
                        yield item

                def values(self):
                    for value in super().values():
                        operations['node_visits'] += 1
                        yield value

                def get(self, *args):
                    operations['node_lookups'] += 1
                    return super().get(*args)

            samples = self.sample_series(list(range(count)))
            for index, sample in enumerate(samples):
                node = sample['sources']['awdl']['nodes'].pop('0')
                node['identity_available'] = False
                sample['sources']['awdl']['nodes'] = Nodes({str(index): node})
            with mock.patch.object(self.capture, 'metric_summary', side_effect=AssertionError('reference rescan')):
                result = self.capture.summarize(Samples(samples), count, 1, {})
            self.assertEqual(len(result['sources']['awdl']['nodes']), count)
            self.assertEqual(operations['node_visits'], count)
            self.assertEqual(operations['node_lookups'], 0)
            self.assertLessEqual(operations['sample_visits'], 4 * count)
            return operations

        small, large = measure(32), measure(64)
        self.assertEqual(large, {key: value * 2 for key, value in small.items()})

    def test_cli_nominal_sample_budget_boundaries_and_extreme_floats(self):
        self.assertEqual(self.capture.MAX_NOMINAL_SAMPLES, 10000)
        for duration, interval in (('30', '1'), ('60', '1'), ('9999', '1'),
                                   ('2499.75', '0.25'), ('0.25', '5'),
                                   ('1e308', '1e308'), ('1e308', '1e305'),
                                   ('5e-324', '5e-324')):
            with self.subTest(duration=duration, interval=interval):
                args = self.capture.parse_args(['--duration', duration, '--interval', interval])
                self.assertEqual((args.duration, args.interval), (float(duration), float(interval)))
        for duration, interval in (('9999.000000000002', '1'), ('10000', '1'),
                                   ('2499.7500000000005', '0.25'),
                                   ('1e308', '5e-324'), ('1', '1e-308'),
                                   ('1e308', '1e303')):
            with self.subTest(duration=duration, interval=interval), \
                    tempfile.TemporaryDirectory(prefix='mtk-performance-budget-test-') as work:
                output = Path(work) / 'must-not-exist'
                with contextlib.redirect_stderr(io.StringIO()) as stderr, \
                        contextlib.redirect_stdout(io.StringIO()) as stdout, \
                        mock.patch.object(self.capture, 'query') as query, \
                        mock.patch.object(self.capture, 'create_output_directory') as create, \
                        mock.patch('subprocess.run') as run, mock.patch('subprocess.Popen') as popen:
                    with self.assertRaises(SystemExit) as error:
                        self.capture.main(['--duration', duration, '--interval', interval,
                                           '--output', str(output)])
                self.assertEqual(error.exception.code, 2)
                self.assertIn('10000 nominal sample budget', stderr.getvalue())
                self.assertEqual(stdout.getvalue(), '')
                query.assert_not_called()
                create.assert_not_called()
                run.assert_not_called()
                popen.assert_not_called()
                self.assertFalse(output.exists())
                self.assertEqual(list(Path(work).iterdir()), [])

    def test_cli_defaults_and_positive_finite_validation(self):
        args = self.capture.parse_args([])
        self.assertEqual((args.duration, args.interval, args.output), (30.0, 1.0, None))
        args = self.capture.parse_args(['--duration', '0.5', '--interval', '0.25', '--output', 'new'])
        self.assertEqual((args.duration, args.interval, args.output), (0.5, 0.25, Path('new')))
        for flag in ('--duration', '--interval'):
            for value in ('0', '-1', 'nan', 'inf', '-inf', '1e9999', '1e-9999', 'invalid'):
                with self.subTest(flag=flag, value=value):
                    with contextlib.redirect_stderr(io.StringIO()), mock.patch.object(self.capture, 'query') as query:
                        with mock.patch.object(self.capture, 'create_output_directory') as create:
                            with self.assertRaises(SystemExit) as error:
                                self.capture.main([f'{flag}={value}'])
                    self.assertEqual(error.exception.code, 2)
                    query.assert_not_called()
                    create.assert_not_called()

    def test_default_output_is_new_and_unique(self):
        with tempfile.TemporaryDirectory(prefix='mtk-performance-test-') as work:
            base = Path(work) / 'build' / 'diagnostics'
            first = self.capture.create_output_directory(None, base)
            last = self.capture.create_output_directory(None, base)
            self.assertNotEqual(first, last)
            self.assertEqual(first.parent, base)
            self.assertTrue(first.name.startswith('wifi-performance-'))
            self.assertEqual(list(first.iterdir()), [])

    def test_existing_output_is_not_overwritten_or_queried(self):
        with tempfile.TemporaryDirectory(prefix='mtk-performance-test-') as work:
            output = Path(work) / 'existing'
            output.mkdir()
            sentinel = output / 'samples.json'
            sentinel.write_text('keep existing')
            with contextlib.redirect_stdout(io.StringIO()), mock.patch.object(self.capture, 'query') as query:
                self.assertEqual(self.capture.main(['--output', str(output)]), 1)
            query.assert_not_called()
            self.assertEqual(sentinel.read_text(), 'keep existing')

    def test_capture_outputs_and_timing_with_mocked_commands(self):
        clock, counts = Clock(), {'ioreg': 0}

        def run(command, **kwargs):
            self.assertIs(kwargs['stderr'], subprocess.DEVNULL)
            if command[0] == '/usr/bin/kmutil':
                return completed(module_row(command[-1]) + b'PRIVATE-UNRELATED-OUTPUT\n')
            self.assertEqual(command[:4], ['/usr/sbin/ioreg', '-a', '-r', '-c'])
            clock.now += 0.05
            if command[-1] == 'AirportMtkWlan':
                return completed(plistlib.dumps([entry('AirportMtkWlan', 123456789,
                                                       MTKWiFiReportedPower=1, SSID='PRIVATE-SSID')]))
            counts['ioreg'] += 1
            return completed(plistlib.dumps([entry('AirportMtkAwdlInterface', 987654321,
                                                   MTKAWDLDataRxDecoded=counts['ioreg'] * 5,
                                                   MTKAWDLDataRxTrace=b'PRIVATE-PAYLOAD')]))

        with tempfile.TemporaryDirectory(prefix='mtk-performance-test-') as work:
            output = Path(work) / 'new-capture'
            with mock.patch('subprocess.run', side_effect=run), \
                    mock.patch.object(self.capture.time, 'monotonic', side_effect=clock.monotonic), \
                    mock.patch.object(self.capture.time, 'sleep', side_effect=clock.sleep), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(self.capture.main(['--duration', '2', '--interval', '1', '--output', str(output)]), 0)
            self.assertEqual({path.name for path in output.iterdir()}, {'samples.json', 'samples.csv', 'summary.json'})
            samples = json.loads((output / 'samples.json').read_text())['samples']
            summary = json.loads((output / 'summary.json').read_text())
            with (output / 'samples.csv').open(newline='') as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(samples), 3)
            self.assertEqual([sample['elapsed_seconds'] for sample in samples], [0, 1, 2])
            self.assertAlmostEqual(summary['capture_elapsed_seconds'], 2.1)
            self.assertEqual(summary['interval_seconds'], {'min': 1, 'mean': 1, 'max': 1})
            self.assertEqual(summary['first_sample'], samples[0])
            self.assertEqual(summary['last_sample'], samples[-1])
            self.assertEqual(summary['sources']['awdl']['nodes']['0']['MTKAWDLDataRxDecoded']['counter_delta'], 10)
            self.assertIsNone(summary['throughput_bytes_per_second'])
            self.assertTrue(all(row['metric'] in self.capture.METRICS[row['source']] for row in rows))
            for path in output.iterdir():
                text = path.read_text()
                for marker in ('PRIVATE', '123456789', '987654321', 'MTKWiFiJoin'):
                    self.assertNotIn(marker, text)

    def test_long_interval_still_has_a_final_observation(self):
        clock, samples = Clock(), []
        collector = mock.Mock(snapshot=mock.Mock(return_value={
            source: {'status': 'missing', 'nodes': {}} for source in self.capture.CLASSES}))
        with mock.patch.object(self.capture.time, 'monotonic', side_effect=clock.monotonic), \
                mock.patch.object(self.capture.time, 'sleep', side_effect=clock.sleep):
            self.capture.collect_samples(collector, 0.25, 5, samples)
        self.assertEqual([sample['elapsed_seconds'] for sample in samples], [0, 0.25])

    def test_slow_queries_do_not_replay_missed_ticks(self):
        clock, samples = Clock(), []

        def snapshot():
            clock.now += 2.0
            return {source: {'status': 'missing', 'nodes': {}} for source in self.capture.CLASSES}

        with mock.patch.object(self.capture.time, 'monotonic', side_effect=clock.monotonic), \
                mock.patch.object(self.capture.time, 'sleep', side_effect=clock.sleep):
            self.capture.collect_samples(mock.Mock(snapshot=snapshot), 5, 1, samples)
        self.assertEqual([sample['elapsed_seconds'] for sample in samples], [0, 2, 4])
        self.assertEqual(samples[-1]['query_seconds'], 2)

    def test_empty_summary_and_interrupted_capture(self):
        summary = self.capture.summarize([], 30, 1, {}, interrupted=True)
        self.assertTrue(summary['interrupted'])
        self.assertIsNone(summary['first_sample'])
        self.assertIsNone(summary['interval_seconds'])
        self.assertEqual(summary['sources']['awdl']['unobserved_metrics'], list(self.capture.METRICS['awdl']))
        with tempfile.TemporaryDirectory(prefix='mtk-performance-test-') as work:
            output = Path(work) / 'interrupted'
            with mock.patch.object(self.capture, 'loaded_versions', side_effect=KeyboardInterrupt), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(self.capture.main(['--output', str(output)]), 0)
            summary = json.loads((output / 'summary.json').read_text())
            self.assertTrue(summary['interrupted'])
            self.assertEqual(summary['sample_count'], 0)

    def test_whitelist_names_exist_in_the_actual_publishers(self):
        root = SCRIPT.parent.parent
        controller = (root / 'kext/Wifi/vendor/AirportMtkWlan/AirportMtkWlan.cpp').read_text()
        interface = (root / 'kext/Wifi/vendor/AirportMtkWlan/AirportMtkAwdlInterface.cpp').read_text()
        hal = (root / 'kext/Wifi/vendor/SimpleMtkWlan/hal_mwx/MtkMwx.cpp').read_text()
        for source, metrics in self.capture.METRICS.items():
            source_text = controller + hal if source == 'infrastructure' else interface
            for name in metrics:
                with self.subTest(source=source, name=name):
                    published = 'MTKAWDLTXPeer%uRateMbps' if name in (
                        'MTKAWDLTXPeer0RateMbps', 'MTKAWDLTXPeer1RateMbps', 'MTKAWDLTXPeer2RateMbps') else name
                    self.assertIn('"' + published + '"', source_text)


if __name__ == '__main__':
    unittest.main(verbosity=2)
