#!/usr/bin/env python3
"""Host-only mSBC tests: no driver loading, Bluetooth control or audio capture."""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--no-sanitize", action="store_true", help="run without ASan/UBSan when unavailable on the host")
args = parser.parse_args()
FLAGS = ["-O1", "-g", "-Wall", "-Wextra", "-Werror"]
if not args.no_sanitize:
    FLAGS += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]

with tempfile.TemporaryDirectory(prefix="mtkwireless-msbc-") as temporary:
    build = Path(temporary)
    source = build / "source"
    # Keep the production test includes intact; never rewrite the source under test.
    shutil.copytree(ROOT / "kext/BT/msbc", source)
    shutil.copytree(ROOT / "tests/bt-msbc", source / "tests")
    includes = ["-I", str(source), "-I", str(source / "vendor/google-libsbc/include")]
    codec = [source / "MtkMsbcCodec.cpp", source / "MtkMsbcStream.cpp"]
    for name in ("sbc", "bits"):
        obj = build / (name + ".o")
        subprocess.run(["xcrun", "clang", "-std=c11", *FLAGS, *includes, "-c",
                        str(source / "vendor/google-libsbc/src" / (name + ".c")),
                        "-o", str(obj)], check=True, timeout=120)
        codec.append(obj)
    for name in ("controller-scope-tests", "stream-tests", "transport-tests", "test-usb-msbc", "test-usb-real-stream"):
        flags = ["-std=c++17", *FLAGS, *includes]
        sources = [source / "tests" / (name + ".cpp")]
        if name.startswith("test-usb"):
            flags.append("-DUSB_MSBC_BRIDGE_TEST")
            sources.append(source / "UsbMsbcBridge.cpp")
        if name == "test-usb-real-stream":
            flags.append("-DUSB_MSBC_BRIDGE_REAL_STREAM_TEST")
        if name not in ("test-usb-msbc", "controller-scope-tests"):
            sources.extend(codec)
        binary = build / name
        subprocess.run(["xcrun", "clang++", *flags, *map(str, sources), "-o", str(binary)],
                       check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=120)
