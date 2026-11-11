#!/usr/bin/env python3
"""Offline checks for the bulk-stall collector; never run sudo or capture traffic."""
import json
import os
import pathlib
import stat
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
COLLECTOR = ROOT / "scripts/Collect-AirDrop-AWDL.command"
source = COLLECTOR.read_text()
subprocess.run(["/bin/zsh", "-n", str(COLLECTOR)], check=True)

# Invalid durations exit before directory creation, password requests or tools.
with tempfile.TemporaryDirectory(prefix="mtk-awdl-collector-test-") as temporary:
    forbidden = pathlib.Path(temporary) / "sudo"
    forbidden.write_text("#!/bin/sh\nexit 97\n")
    forbidden.chmod(0o700)
    for argument in ("0", "29", "601", "-1", "abc", "1;exit 0", "30.5"):
        result = subprocess.run(
            ["/bin/zsh", str(COLLECTOR), argument],
            env={"PATH": str(pathlib.Path(temporary)) + ":/usr/bin:/bin"},
            capture_output=True, text=True,
        )
        assert result.returncode == 2, (argument, result.returncode, result.stderr)
        assert "Usage:" in result.stderr, (argument, result.stderr)

# Execute only the standalone clock recorder, not the shell capture body.
marker = "<<'PY'\n"
assert source.count(marker) == 1
clock = source.split(marker, 1)[1].split("\nPY\n", 1)[0]
result = subprocess.run(
    [sys.executable, "-I", "-c", clock, "sample-0007-before"],
    check=True, capture_output=True, text=True,
)
record = json.loads(result.stdout)
assert record["sample"] == "sample-0007-before"
assert record["wall_before_ns"] <= record["wall_after_ns"]
assert record["monotonic_ns"] > 0 and record["monotonic_resolution_s"] > 0
assert record["monotonic_implementation"]

# Source contracts: one attempt long enough for the observed 121-second timeout,
# both radio/frontend snapshots, bounded duration and a private unique output.
for required in (
    'duration="${1:-180}"', 'sleep "$2"', '"$out" "$duration"',
    'umask 077', 'out=$(mktemp -d ',
    'clock_sample "$label-before"', 'clock_sample "$label-after"',
    '"$out/$label.plist"', '"$out/wlan-$label.plist"',
    'Send ONE large file', '--start "$started" --end "$ended"',
    'python3 -I - "$1"',
):
    assert required in source, required
assert source.count('/usr/sbin/tcpdump -i awdl0') == 1
assert '"ip6"' in source
# Run the actual privileged-wrapper body as this unprivileged user with a fake
# capture executable. Nothing invokes sudo or opens an interface. Capture exit
# failure must remain visible even when the wrapper's duration wait succeeded.
root_body = source.split("sudo -n /bin/sh -c '\n", 1)[1].split("\n' awdl-capture ", 1)[0]
with tempfile.TemporaryDirectory(prefix="mtk-awdl-capture-wrapper-") as temporary:
    base = pathlib.Path(temporary)
    mock = base / "mock-capture.py"
    mock.write_text(f"#!{sys.executable} -I\n" + '''import os
import pathlib
import signal
import sys
signal.signal(signal.SIGINT, lambda *_: sys.exit(0))
path = pathlib.Path(sys.argv[sys.argv.index("-w") + 1])
if os.environ.get("MTK_MOCK_CAPTURE_FAILURE"):
    sys.exit(7)
path.write_bytes(b"MOCK capture; not a pcap")
path.with_suffix(".pid").write_text(str(os.getpid()))
while True:
    signal.pause()
''')
    mock.chmod(0o700)
    body = root_body.replace("/usr/sbin/tcpdump", str(mock))
    for fail in (False, True):
        output = base / ("failed" if fail else "success")
        output.mkdir(mode=0o700)
        for filename in ("awdl.pcap", "tcpdump.log", "capture-exit-status.txt"):
            assert f': > "$out/{filename}"' in source
            assert source.index(f': > "$out/{filename}"') < source.index("sudo -n /bin/sh -c")
            (output / filename).touch(mode=0o600)
        env = dict(os.environ)
        env.pop("MTK_MOCK_CAPTURE_FAILURE", None)
        if fail:
            env["MTK_MOCK_CAPTURE_FAILURE"] = "1"
        result = subprocess.run(
            ["/bin/sh", "-c", body, "awdl-capture", str(output), "1"],
            env=env, capture_output=True, text=True, timeout=5,
        )
        assert result.returncode == 0, result.stderr
        actual = (output / "capture-exit-status.txt").read_text().strip()
        assert actual == ("7" if fail else "0"), actual
        for filename in ("awdl.pcap", "tcpdump.log", "capture-exit-status.txt"):
            metadata = (output / filename).stat()
            assert metadata.st_uid == os.getuid()
            assert stat.S_IMODE(metadata.st_mode) == 0o600
        if not fail:
            pid = int((output / "awdl.pid").read_text())
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                pass
            else:
                raise AssertionError("capture child was not reaped")
print("PASS: shell syntax, seven rejected durations, bracketed clocks and capture contracts")
print("PASS: mocked capture success/failure, child reaping and private file ownership")
print("Not exercised: sudo/root ownership, actual packet capture, interruption or live log collection")
