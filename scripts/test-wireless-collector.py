#!/usr/bin/env python3
"""Run the real collector flow with fake diagnostics; no sudo, live logs or network.

Native bash, file copying, plist extraction, ZIP creation/testing and process
termination are exercised against fixtures in a new temporary directory.
"""
import json
import os
from pathlib import Path
import plistlib
import signal
import stat
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
COLLECTOR = ROOT / "scripts/collect-logs.sh"
SOURCE = COLLECTOR.read_text()
subprocess.run(["/bin/bash", "-n", str(COLLECTOR)], check=True)
assert stat.S_IMODE(COLLECTOR.stat().st_mode) & 0o111

# Rejected arguments must not request credentials or create a Desktop bundle.
for arguments in (
    ["--last", value] for value in
    ("0m", "01h", "8d", "169h", "10081m", "1.5h", "1h;id", "-1h", "abc", "999999m")
):
    result = subprocess.run(["/bin/bash", str(COLLECTOR), *arguments], capture_output=True, text=True)
    assert result.returncode == 2 and "Usage:" in result.stderr, (arguments, result)
for option in ("--last", "--output", "--notes", "--unknown"):
    result = subprocess.run(["/bin/bash", str(COLLECTOR), option], capture_output=True, text=True)
    assert result.returncode == 2, (option, result)
assert subprocess.run(["/bin/bash", str(COLLECTOR), "--help"], capture_output=True).returncode == 0

MOCK = r'''import json
import os
from pathlib import Path
import plistlib
import signal
import subprocess
import sys
import time
name = Path(sys.argv[0]).name
args = sys.argv[1:]
record = (json.dumps({"tool": name, "args": args}) + "\n").encode()
fd = os.open(os.environ["MOCK_CALLS"], os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
os.write(fd, record)
os.close(fd)
if name == os.environ.get("MOCK_HANG") and not Path(os.environ["MOCK_CHILD_PID"]).exists():
    child = subprocess.Popen(["/bin/sleep", "60"])
    Path(os.environ["MOCK_CHILD_PID"]).write_text(str(child.pid))
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    while True:
        child.poll()  # Reap a terminated descendant before the collector kills us.
        time.sleep(0.01)
if name == os.environ.get("MOCK_FAIL"):
    print("simulated tool failure", file=sys.stderr)
    sys.exit(7)
if name == "id":
    print("tester" if args == ["-un"] else os.getuid() if args[0] == "-u" else os.getgid())
elif name == "dscl":
    print("NFSHomeDirectory: " + os.environ["MOCK_HOME"])
elif name == "stat":
    print("tester")
elif name == "chown":
    pass  # Fixtures already belong to the real unprivileged test runner.
elif name == "sudo":
    if os.environ.get("MOCK_DENIED"):
        print("simulated sudo refusal", file=sys.stderr)
        sys.exit(1)
    if "-v" not in args:
        sys.exit(subprocess.run(args[1:]).returncode)
elif name == "ioreg":
    selector = args[args.index("-c") + 1] if "-c" in args else "IOResources"
    sys.stdout.buffer.write(plistlib.dumps([{"class": selector, "RustSelfTestFailed": 0,
        "MTKAWDLInfraPSSleeps": 3, "BluetoothdVerifyHookState": 1}]))
elif name == "system_profiler":
    if "-xml" in args:
        sys.stdout.buffer.write(plistlib.dumps([{"_dataType": "SPBluetoothDataType", "fixture": True}]))
    else:
        print("Mock card: MT7922; State: On")
elif name == "networksetup":
    if args == ["-listallhardwareports"]:
        print("Hardware Port: Wi-Fi\nDevice: en1\nEthernet Address: 00:00:00:00:00:01\n")
        print("Hardware Port: Ethernet\nDevice: en0\n")
    else:
        print("mock network status")
elif name == "kmutil":
    if args[0] == "showloaded":
        print("1 0 com.dinovn.MtkWirelessWifi (1.1.0)\n2 0 com.dinovn.MtkWirelessBT (1.1.0)\n3 0 as.vit9696.Lilu (1.7.0)")
    else:
        print("mock kernel collection / loading state")
elif name == "log":
    if os.environ.get("MOCK_DENIED"):
        print("simulated log permission denial", file=sys.stderr)
        sys.exit(77)
    if args[0] == "collect":
        archive = Path(args[args.index("--output") + 1])
        archive.mkdir(mode=0o700)
        (archive / "Info.plist").write_bytes(plistlib.dumps({"fixture": True}))
        (archive / "mock.tracev3").write_bytes(b"MOCK LOG ARCHIVE DATA")
    print("mock persisted log event")
elif name == "ditto":
    if "-c" in args and os.environ.get("MOCK_ZIP_FAIL"):
        print("simulated ZIP failure", file=sys.stderr)
        sys.exit(9)
    if "-c" in args and os.environ.get("MOCK_ZIP_CORRUPT"):
        Path(args[-1]).write_bytes(b"NOT A ZIP")
    else:
        sys.exit(subprocess.run(["/usr/bin/ditto", *args]).returncode)
else:
    print("mock " + name + " " + " ".join(args))
'''

TOOLS = (
    "/usr/bin/id", "/usr/bin/dscl", "/usr/bin/stat", "/usr/bin/sudo", "/usr/sbin/chown",
    "/usr/bin/sw_vers", "/usr/bin/uptime", "/usr/sbin/sysctl", "/usr/bin/csrutil",
    "/usr/bin/systemextensionsctl", "/usr/sbin/diskutil", "/sbin/mount", "/usr/bin/pmset",
    "/usr/sbin/system_profiler", "/usr/sbin/ioreg", "/usr/sbin/nvram", "/usr/bin/kmutil",
    "/usr/sbin/kextstat", "/usr/sbin/networksetup", "/usr/sbin/netstat", "/usr/sbin/ndp",
    "/usr/sbin/arp", "/usr/sbin/scutil", "/usr/bin/wdutil", "/usr/sbin/ipconfig",
    "/sbin/ifconfig", "/bin/ps", "/bin/launchctl", "/usr/bin/defaults", "/usr/bin/codesign",
    "/usr/bin/log", "/usr/bin/ditto",
)


def fixture(base):
    home = base / "user home"
    replacements = {
        "/Library/StagedExtensions/Library/Extensions": base / "staged",
        "/System/Library/Extensions": base / "system-extensions",
        "/Library/Extensions": base / "extensions",
        "/Library/Logs/DiagnosticReports": base / "system-crashes",
        "/private/var/root/Library/Logs/Bluetooth": base / "root-hci",
        "/private/var/log": base / "system-logs",
        "/usr/sbin/bluetoothd": base / "bluetoothd-fixture",
        "/Volumes": base / "volumes",
    }
    for path in replacements.values():
        if path.name != "bluetoothd-fixture":
            path.mkdir(parents=True)
    (base / "bluetoothd-fixture").write_bytes(b"MOCK bluetoothd binary\x00\x01\xff")
    (base / "system-crashes/bluetoothd-test report.ips").write_text("MOCK crash report")
    (base / "root-hci/trace with spaces.pklg").write_bytes(b"MOCK HCI TRACE")
    (base / "system-logs/Lilu_test.txt").write_text("MOCK Lilu load log")
    for suffix in ("Library/Logs/DiagnosticReports", "Library/Logs/Bluetooth", "Desktop"):
        (home / suffix).mkdir(parents=True)
    oc = base / "volumes/EFI disk/EFI/OC"
    (oc / "Kexts").mkdir(parents=True)
    (oc / "config.plist").write_bytes(plistlib.dumps({
        "Kernel": {"Add": [{"BundlePath": "MtkWirelessWifi.kext", "Enabled": True}]},
        "Misc": {"Security": {"SecureBootModel": "Disabled"}},
        "ACPI": {"Add": []}, "UEFI": {"Drivers": []},
        "PlatformInfo": {"Generic": {"SystemSerialNumber": "DO_NOT_EXPORT_CONFIG_ID"}},
        "NVRAM": {"DO_NOT_EXPORT_NVRAM_SECRET": "secret"},
    }))
    for directory, name in ((base / "extensions", "MtkWirelessBT.kext"), (oc / "Kexts", "MtkWirelessWifi.kext")):
        contents = directory / name / "Contents"
        (contents / "MacOS").mkdir(parents=True)
        (contents / "Info.plist").write_bytes(plistlib.dumps({"CFBundleVersion": "1.1.0"}))
        (contents / "MacOS/fixture").write_bytes(b"MOCK KEXT")
    mocks = base / "mocks"
    mocks.mkdir()
    dispatcher = mocks / "dispatch.py"
    dispatcher.write_text(f"#!{sys.executable} -I\n" + MOCK)
    dispatcher.chmod(0o700)
    text = SOURCE.replace('"$EUID"', '"$MOCK_EUID"').replace("/bin/sleep 0.2", "/bin/sleep 0.01")
    text = text.replace('"$OWNER_HOME/Library/Logs/DiagnosticReports"', '"__USER_CRASH_DIRECTORY__"')
    for tool in TOOLS:
        target = mocks / Path(tool).name
        target.symlink_to(dispatcher.name)
        text = text.replace(tool, str(target))
    for original, target in sorted(replacements.items(), key=lambda item: -len(item[0])):
        text = text.replace(original, str(target))
    text = text.replace('"__USER_CRASH_DIRECTORY__"', '"$OWNER_HOME/Library/Logs/DiagnosticReports"')
    # Keep scripts outside fixture directories, and run Python in isolated mode.
    script = base / "collector-under-test.sh"
    script.write_text(text)
    env = dict(os.environ, HOME=str(home), SUDO_USER="tester", MOCK_HOME=str(home), MOCK_EUID="0",
               MOCK_CALLS=str(base / "calls.jsonl"), MOCK_CHILD_PID=str(base / "child.pid"))
    return home, script, env


def run_case(label, options=(), switches=None, via_command=False, timeout_tool=False):
    with tempfile.TemporaryDirectory(prefix="mtk-collector-test-") as temporary:
        base = Path(temporary)
        home, script, env = fixture(base)
        if switches:
            env.update(switches)
        if timeout_tool:
            text = script.read_text().replace("capture hardware/wifi.txt 75", "capture hardware/wifi.txt 1")
            script.write_text(text)
        command = (["/bin/bash", "-c", script.read_text(), "--"] if via_command else ["/bin/bash", str(script)])
        process = subprocess.Popen(command + list(options), env=env, cwd=base, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
        try:
            stdout, stderr = process.communicate(timeout=90)
        except subprocess.TimeoutExpired:
            # Kill this test's entire isolated group before temporary-file cleanup.
            # A failed production timeout must not leave mock tools running.
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate()
            raise AssertionError(f"{label}: test timeout\n{stdout[-4000:]}\n{stderr[-4000:]}")
        result = subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
        folders = list((home / "Desktop").glob("MtkWireless-Logs-*"))
        folder = next(path for path in folders if path.is_dir())
        archive = Path(str(folder) + ".zip")
        statuses = {fields[3]: int(fields[2]) for line in (folder / "commands.tsv").read_text().splitlines()[1:]
                    if len(fields := line.split("\t")) >= 5}
        calls = [json.loads(line) for line in (base / "calls.jsonl").read_text().splitlines()]
        for call in calls:
            name, args = call["tool"], call["args"]
            if name == "defaults":
                assert args[0] == "read" and len(args) == 3, call
            if name == "networksetup":
                assert args[0].startswith(("-get", "-list")), call
            if name == "kmutil":
                assert args[0] in ("showloaded", "inspect", "dumpstate", "path-for-identifier", "log"), call
            if name == "log":
                assert args[0] in ("show", "collect"), call
        if env.get("MOCK_ZIP_FAIL") or env.get("MOCK_ZIP_CORRUPT"):
            assert result.returncode == 1 and "ERROR: ZIP" in result.stdout, result
            assert "Done. Gui file" not in result.stdout
            assert folder.is_dir()
            assert statuses["zip-result.txt"] == (9 if env.get("MOCK_ZIP_FAIL") else 0)
            assert statuses["zip-check.txt"] != 0
        else:
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert statuses["zip-result.txt"] == statuses["zip-check.txt"] == 0
            assert stat.S_IMODE(archive.stat().st_mode) == 0o600
            assert stat.S_IMODE(folder.stat().st_mode) == 0o700
            with zipfile.ZipFile(archive) as zipped:
                assert zipped.testzip() is None
                prefix = folder.name + "/"
                names = set(zipped.namelist())
                assert zipped.read(prefix + "bluetooth/bluetoothd") == (base / "bluetoothd-fixture").read_bytes()
                assert plistlib.loads(zipped.read(prefix + "ioreg/AirportMtkWlan-before.plist"))
                assert plistlib.loads(zipped.read(prefix + "ioreg/resources-after.plist"))
                assert any(name.endswith("trace with spaces.pklg") for name in names)
                assert any(name.endswith("bluetoothd-test report.ips") for name in names)
                assert any(name.endswith("Lilu_test.txt") for name in names)
                assert any(name.endswith("MtkWirelessWifi.kext/Contents/Info.plist") for name in names)
                assert any(name.endswith("efi-1-Kernel.plist") for name in names)
                assert not any(name.endswith("config.plist") for name in names)
                for name in names:
                    if not name.endswith("/"):
                        data = zipped.read(name)
                        assert b"DO_NOT_EXPORT_CONFIG_ID" not in data and b"DO_NOT_EXPORT_NVRAM_SECRET" not in data, name
                has_archive = any("system.logarchive/" in name for name in names)
                assert has_archive == ("--no-logarchive" not in options and not env.get("MOCK_DENIED"))
            boot_calls = [call for call in calls if call["tool"] == "log" and "boot" in call["args"]]
            assert boot_calls and "KernelManagement" in boot_calls[0]["args"][-1]
            assert any(call["tool"] == "networksetup" and call["args"] == ["-getairportpower", "en1"] for call in calls)
            assert not any(call["tool"] == "networksetup" and call["args"] == ["-getairportpower", "en0"] for call in calls)
            if env.get("MOCK_DENIED"):
                assert "admin_access=0" in (folder / "session.txt").read_text()
                assert statuses["logs/bluetoothd.log"] == 77
            if env.get("MOCK_FAIL"):
                assert statuses["network/wdutil.txt"] == 7
                assert "simulated tool failure" in (folder / "network/wdutil.txt.stderr.txt").read_text()
            if timeout_tool:
                assert statuses["hardware/wifi.txt"] == 124
                assert "Timed out" in (folder / "hardware/wifi.txt.stderr.txt").read_text()
                child = int((base / "child.pid").read_text())
                try:
                    os.kill(child, 0)
                except ProcessLookupError:
                    pass
                else:
                    raise AssertionError("Timed-out tool descendant was left running")
        print("PASS:", label)


if __name__ == "__main__":
    cases = [
        ("full", "sudo/bash -c, invoking user's Desktop, full bundle, native ZIP, private mode and EFI filtering",
         {"options": ("--notes", "AirDrop Mac to iPhone stalls"), "via_command": True}),
        ("reduced", "declined sudo, custom time window, no-logarchive and valid reduced bundle",
         {"options": ("--last", "7d", "--no-logarchive"), "switches": {"MOCK_EUID": "1000", "MOCK_DENIED": "1"}}),
        ("failure", "tool failure is reported without losing the other diagnostics", {"switches": {"MOCK_FAIL": "wdutil"}}),
        ("timeout", "timeout kills the tool tree and records partial output",
         {"switches": {"MOCK_HANG": "system_profiler"}, "timeout_tool": True}),
        ("zip-failure", "ZIP command failure preserves originals and never announces success", {"switches": {"MOCK_ZIP_FAIL": "1"}}),
        ("zip-corrupt", "corrupt ZIP with successful writer is rejected", {"switches": {"MOCK_ZIP_CORRUPT": "1"}}),
    ]
    selected = set(sys.argv[1:])
    assert not selected - {case[0] for case in cases}, selected
    for key, label, arguments in cases:
        if not selected or key in selected:
            run_case(label, **arguments)
    print("PASS: shell syntax, help, rejected options and read-only command contracts")
    print("Not exercised: real sudo/TCC, actual hardware/log availability, live macOS logarchive or EFI discovery.")
