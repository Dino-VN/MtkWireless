#!/usr/bin/env python3
"""Distinguish driver assists from Tahoe's native-host software BSD flags."""
import ast
import json
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
source = (root / 'tools/awdl/check-data.py').read_text()
tree = ast.parse(source)
function = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'offload_state')
namespace = {'re': re}
exec(compile(ast.Module(body=[function], type_ignores=[]), '<offload-check>', 'exec'), namespace)
check = namespace['offload_state']
flags = 'options=6460<TSO4,TSO6,CHANNEL_IO,PARTIAL_CSUM,ZEROINVERT_CSUM>'
driver = {'MTKAWDLBSDOffloadStatus': 0, 'MTKAWDLDriverHardwareAssists': 0,
          'MTKAWDLBSDPreparedOffload': 0, 'MTKAWDLBSDPreparedCapabilities': 0x400,
          'MTKAWDLBSDPreparedEnabled': 0x400}
assert check(driver, flags)['status'] == 'passed'
assert check(driver, 'options=400<CHANNEL_IO>')['status'] == 'passed'
for key, value in [('MTKAWDLDriverHardwareAssists', 0x10),
                   ('MTKAWDLBSDPreparedOffload', 0x200000),
                   ('MTKAWDLBSDPreparedCapabilities', 0x20400),
                   ('MTKAWDLBSDPreparedCapabilities', 0x440),
                   ('MTKAWDLBSDPreparedEnabled', 0x2400)]:
    # A capability outside the checksum/TSO mask remains unrelated.
    expected = 'passed' if value == 0x20400 else 'failed'
    assert check({**driver, key: value}, flags)['status'] == expected
for key in driver:
    incomplete = dict(driver)
    del incomplete[key]
    assert check(incomplete, flags)['status'] == 'unverified'
assert check({**driver, 'MTKAWDLBSDOffloadStatus': 5}, flags)['status'] == 'unverified'

# Actual previous boot has BSD flags and setter success but no driver readback.
previous = root / 'build/awdl-timing-fix/runtime-data.json'
if previous.exists():
    sample = json.loads(previous.read_text())
    result = check(sample['after']['native'], sample['interface'])
    assert result['status'] == 'unverified'
    assert result['bsd_flags']
    assert not any('still enables unsupported' in reason for reason in result['reasons'])
print('PASS offload check: software BSD flags, verified driver/cache snapshot, physical assist failure, missing evidence and previous boot regression.')
