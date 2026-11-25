#!/usr/bin/env python3
"""Exercise read-only network diagnostics with fake system commands."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
script = ROOT / 'scripts/check-airdrop-network.sh'
with tempfile.TemporaryDirectory(prefix='mtk-network-path-') as tmp:
    work = Path(tmp)
    tools = {
        'sysctl': '''#!/bin/bash
case "$2" in
net.filter_state) printf '%s\\n' "$TEST_FILTER" ;;
net.link.generic.system.enable_netagent)
 [ "$TEST_AGENT" != unavailable ] || exit 1
 printf '%s\\n' "$TEST_AGENT" ;;
*) exit 99 ;;
esac
''',
        'ifconfig': '#!/bin/bash\n[ "$1" = -l ] || exit 99\nprintf "lo0 en0 awdl0 utun1\\n"\n',
        'scutil': '#!/bin/bash\nread -r command\nprintf "%s : LinkQuality 100\\n" "$command"\n',
    }
    for name, text in tools.items():
        path = work / name
        path.write_text(text)
        path.chmod(0o700)
    for agent, filters in [('0', 'pf'), ('0', 'ipf'), ('1', ''), ('unavailable', '')]:
        env = dict(os.environ, PATH=f'{work}:/usr/bin:/bin', TEST_AGENT=agent, TEST_FILTER=filters)
        result = subprocess.run(['/bin/bash', str(script)], env=env, text=True,
                                capture_output=True, check=True)
        assert ('WARNING: userspace network path is disabled' in result.stdout) == (agent == '0')
        assert ('pf is listed in filter state' in result.stdout) == (agent == '0' and filters == 'pf')
        assert ('no conclusion' in result.stdout) == (agent == 'unavailable')
        assert 'show State:/Network/Interface/en0/LinkQuality' in result.stdout
        assert 'show State:/Network/Interface/awdl0/LinkQuality' in result.stdout
        assert 'lo0 LinkQuality' not in result.stdout and 'utun1 LinkQuality' not in result.stdout
        assert not result.stderr
print('PASS: network path enabled/disabled/unavailable, pf warning and identifier-free interface selection')
