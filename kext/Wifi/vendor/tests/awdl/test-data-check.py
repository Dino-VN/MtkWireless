#!/usr/bin/env python3
"""Validate the data checker against observed transfer/status combinations."""
import ast
from copy import deepcopy
from datetime import datetime
from pathlib import Path
import re
import subprocess
from types import SimpleNamespace

root = Path(__file__).resolve().parents[2]
tree = ast.parse((root / 'tools/awdl/check-data.py').read_text())
functions = [node for node in tree.body if isinstance(node, ast.FunctionDef)
             and node.name in ('check', 'offload_state')]
uuid = '4E711087-6AC2-387E-B0D3-E838BB36FFF1'
native = {'MTKAWDLNativeTxPackets': 10, 'MTKAWDLNativeRxPackets': 20,
          'MTKAWDLNativeDroppedPackets': 0, 'MTKAWDLDataRxFrames': 20,
          'MTKAWDLDataPathEnabled': True, 'MTKAWDLBSDOffloadStatus': 0,
          'MTKAWDLDriverHardwareAssists': 0, 'MTKAWDLBSDPreparedOffload': 0,
          'MTKAWDLBSDPreparedCapabilities': 0x400, 'MTKAWDLBSDPreparedEnabled': 0x400}
before = {'native': native, 'bsd': {'tx_packets': 10, 'rx_packets': 20},
          'hal': {'MTKAWDLTXDataAcked': 10, 'MTKAWDLTXDataFailed': 0,
                  'MTKAWDLTXMulticastFailed': 0, 'MTKAWDLTXActionFailed': 0,
                  'MTKAWDLTXLastPid': 127, 'MTKAWDLTXLastSequence': 4095}}
after = deepcopy(before)
for key in ('MTKAWDLNativeTxPackets', 'MTKAWDLNativeRxPackets', 'MTKAWDLDataRxFrames'):
    after['native'][key] += 3
after['bsd'] = {'tx_packets': 13, 'rx_packets': 23}
after['hal'].update(MTKAWDLTXDataAcked=13, MTKAWDLTXLastPid=125, MTKAWDLTXLastSequence=2)

def observe(end):
    samples = iter((deepcopy(before), end))
    def run(command):
        if command[0] == 'kmutil':
            return ('com.eiyuumiru.AirportMtkWlan (1.0) ' + uuid).encode()
        if command[0] == 'ifconfig':
            return b'awdl0: flags=UP,RUNNING options=400<CHANNEL_IO>'
        if command[0] == '/usr/bin/log':
            return b''
        raise AssertionError(command)
    namespace = {'args': SimpleNamespace(expected_uuid=uuid, probe=False, seconds=0),
                 'snapshot': lambda: next(samples), 'run': run, 'datetime': datetime,
                 'time': SimpleNamespace(sleep=lambda _: None), 're': re, 'subprocess': subprocess}
    exec(compile(ast.Module(body=functions, type_ignores=[]), '<data-check>', 'exec'), namespace)
    return namespace['check']()

result = observe(after)
assert result['status'] == 'passed' and result['firmware_tx_status_available']
assert result['hal_delta']['MTKAWDLTXDataAcked'] == 3
assert not any(key.startswith('MTKAWDLTXLast') for key in result['hal_delta'])
for key in ('MTKAWDLTXDataFailed', 'MTKAWDLTXMulticastFailed', 'MTKAWDLTXActionFailed'):
    failed = deepcopy(after)
    failed['hal'][key] = 1
    result = observe(failed)
    assert result['status'] != 'passed'
    assert any('Firmware reported' in reason for reason in result['reasons'])
reset = deepcopy(after)
reset['hal']['MTKAWDLTXDataAcked'] = 1
assert 'HAL counters reset during observation.' in observe(reset)['reasons']
idle = deepcopy(after)
idle['native']['MTKAWDLNativeRxPackets'] = native['MTKAWDLNativeRxPackets']
idle['bsd']['rx_packets'] = before['bsd']['rx_packets']
assert observe(idle)['status'] != 'passed'
legacy = deepcopy(after)
del legacy['hal']['MTKAWDLTXDataAcked']
assert not observe(legacy)['firmware_tx_status_available']
print('PASS data checker: transfer counters, firmware failures, HAL reset, decreasing gauges, no RX and missing TX-status diagnostics.')
