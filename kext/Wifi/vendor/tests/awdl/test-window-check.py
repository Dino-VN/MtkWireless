#!/usr/bin/env python3
"""Exercise the runtime checker with native missing-event and identity fixtures."""
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import plistlib
import runpy
import sys
import tempfile
from unittest.mock import patch

root = Path(__file__).resolve().parents[2]
identity = 'com.eiyuumiru.AirportMtkWlan (1.0.0) 01234567-89AB-CDEF-0123-456789ABCDEF'
expected_uuid = identity.split()[-1]

def check(messages=(), starts=1, ends=1, enabled=True, loaded=identity, reset=False):
    before = {'MTKAWDLAWStartNotifications': 10, 'MTKAWDLAWEndNotifications': 10}
    after = {'MTKAWDLAWStartNotifications': 10 + starts, 'MTKAWDLAWEndNotifications': 10 + ends,
             'MTKAWDLAWWindowOpen': starts - ends == 1, 'MTKAWDLSyncEnabled': enabled}
    if reset:
        after['MTKAWDLAWStartNotifications'] = 0
        after['MTKAWDLAWEndNotifications'] = 0
    snapshots = iter([before, after])
    def command(argv, **kwargs):
        if argv[0] == 'kmutil':
            return loaded.encode()
        if argv[0] == 'ioreg':
            return plistlib.dumps([next(snapshots)])
        assert argv[0] == '/usr/bin/log'
        assert 'aw start event missing' in argv[-1]
        assert 'Did not get AW start event' in argv[-1]
        return '\n'.join(json.dumps({'eventMessage': message}) for message in messages).encode()
    with tempfile.TemporaryDirectory(prefix='mtk-awdl-check-') as tmp:
        output = Path(tmp) / 'result.json'
        argv = ['check-windows.py', '--expected-uuid', expected_uuid, '--seconds', '0', '--output', str(output)]
        with patch.object(sys, 'argv', argv), patch('subprocess.check_output', command), redirect_stdout(io.StringIO()):
            try:
                runpy.run_path(str(root / 'tools/awdl/check-windows.py'), run_name='__main__')
            except SystemExit as exc:
                code = exc.code
        result = json.loads(output.read_text())
        assert code == (0 if result['status'] == 'passed' else 1)
        return result

assert check()['status'] == 'passed'
assert check(starts=1, ends=0)['status'] == 'passed'
assert check(starts=0, ends=0)['status'] == 'unverified'
assert check(enabled=False)['status'] == 'unverified'
assert check(loaded='other-uuid')['status'] == 'unverified'
assert check(reset=True)['status'] == 'failed'
assert check(['com.apple.p2p.awdl0: AWDL SANITY FAILURE: Skipped 2 AW start events'])['status'] == 'failed'
missing = check(['com.apple.p2p.awdl0: IO80211AWDLPeerManager::monitorAWEvents ERROR: aw start event missing for too long'])
assert missing['status'] == 'failed' and missing['missing_start_errors'] and not missing['sanity_failures']
assert check(['com.apple.p2p.other0: aw start event missing for too long'])['status'] == 'passed'
warning = check(['com.apple.p2p.awdl0: WARNING: Did not get AW start event'], starts=0, ends=0)
assert warning['status'] == 'failed' and warning['missing_start_warnings'] and not warning['missing_start_errors']
assert check(['com.apple.p2p.other0: Did not get AW start event'])['status'] == 'passed'
assert check(['com.apple.p2p.awdl0: Did not get AW start event'], enabled=False)['status'] == 'failed'
print('PASS runtime AW checker: UUID, active starts, paired events, reset and missing-start errors/warnings without sanity failures.')
