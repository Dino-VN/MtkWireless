#!/usr/bin/env python3
"""Read selected native Tahoe symbols from the disk KC without loading kexts."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import re
import struct
import subprocess

from capstone import Cs, CS_ARCH_X86, CS_MODE_64

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("symbols", nargs="+")
parser.add_argument("--kc", type=Path, default=Path("/System/Library/KernelCollections/BootKernelExtensions.kc"))
parser.add_argument("--symbol-cache", type=Path)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
spec = importlib.util.spec_from_file_location("abi", ROOT / "tests/awdl-abi/check.py")
abi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(abi)
data = args.kc.read_bytes()
segments = []
images = {}
for command, cursor, size in abi.commands(data):
    if command != 0x80000035:
        continue
    address, header, name_offset, reserved = struct.unpack_from("<QQII", data, cursor + 8)
    image = abi.string_at(data, cursor + name_offset)
    for kind, offset, size in abi.commands(data, header):
        if kind == 0x1B:
            images[image] = str(abi.uuid.UUID(bytes=data[offset + 8:offset + 24])).upper()
        if kind == 0x19:
            name, address, length, file_offset, file_size = struct.unpack_from("<16sQQQQ", data, offset + 8)
            if file_size:
                segments.append((address, file_size, file_offset))
raw_symbols = args.symbol_cache.read_text() if args.symbol_cache else subprocess.check_output(["nm", "-n", str(args.kc)], text=True)
symbols = {}
for line in raw_symbols.splitlines():
    match = re.match(r"^([0-9a-fA-F]{16})\s+\S\s+(\S+)$", line)
    if match:
        symbols[int(match[1], 16)] = match[2]
names = {name: address for address, name in symbols.items()}
addresses = sorted(symbols)
decoder = Cs(CS_ARCH_X86, CS_MODE_64)
output = [f"KC: {args.kc}", f"SHA256: {hashlib.sha256(data).hexdigest()}",
          f"IOSkywalkFamily UUID: {images.get('com.apple.iokit.IOSkywalkFamily')}",
          "Static instructions only. Next-symbol bounds can include padding; indirect calls remain unresolved."]
for name in args.symbols:
    address = names[name]
    end = next(value for value in addresses if value > address)
    segment = next(value for value in segments if value[0] <= address < value[0] + value[1])
    offset = segment[2] + address - segment[0]
    output.append(f"\n{name} {address:#x} size={end-address:#x}")
    for instruction in decoder.disasm(data[offset:offset + end - address], address):
        annotation = ""
        if instruction.mnemonic in ("call", "jmp") and instruction.op_str.startswith("0x"):
            annotation = " ; " + symbols.get(int(instruction.op_str, 16), "?")
        output.append(f"{instruction.address:#x} +{instruction.address-address:#x}: {instruction.mnemonic} {instruction.op_str}{annotation}")
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text("\n".join(output) + "\n")
print(args.output)
