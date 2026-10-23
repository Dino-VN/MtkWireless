#!/usr/bin/env python3
"""Summarize AirDrop HCI evidence without displaying addresses, payloads or keys."""

import argparse
from collections import Counter
from pathlib import Path
import struct


COMMANDS = {
    0x2005: "Set Random Address",
    0x2006: "Set Advertising Parameters",
    0x2007: "Read Advertising Channel TX Power",
    0x2008: "Set Advertising Data",
    0x2009: "Set Scan Response Data",
    0x200A: "Set Advertising Enable",
    0x200B: "Set Scan Parameters",
    0x200C: "Set Scan Enable",
    0x2027: "Add Device To Resolving List",
    0x202D: "Set Address Resolution Enable",
    0x2035: "Set Advertising Set Random Address",
    0x2036: "Set Extended Advertising Parameters",
    0x2037: "Set Extended Advertising Data",
    0x2038: "Set Extended Scan Response Data",
    0x2039: "Set Extended Advertising Enable",
    0xFD3C: "Vendor command",
}


def records(stream):
    """PacketLogger: BE length excludes its own 4 bytes, then time and type."""
    offset = 0
    while True:
        prefix = stream.read(4)
        if not prefix:
            return
        if len(prefix) != 4:
            raise ValueError(f"truncated record length at offset {offset}")
        length = struct.unpack(">I", prefix)[0]
        if length < 9 or length > 1024 * 1024:
            raise ValueError(f"invalid record length {length} at offset {offset}")
        body = stream.read(length)
        if len(body) != length:
            raise ValueError(f"truncated record at offset {offset}")
        yield body[8], body[9:]
        offset += length + 4


def summarize(stream, response_types=None):
    commands, replies, failures, enabled = Counter(), Counter(), Counter(), Counter()
    for kind, data in records(stream):
        if kind == 0:
            if len(data) < 3 or len(data) != data[2] + 3:
                raise ValueError("malformed HCI command")
            opcode = int.from_bytes(data[:2], "little")
            commands[opcode] += 1
            if opcode in (0x200A, 0x2039) and data[2] and data[3] == 1:
                enabled[opcode] += 1
        elif kind == 1:
            if len(data) < 2 or len(data) != data[1] + 2:
                raise ValueError("malformed HCI event")
            if data[0] == 0x0E and len(data) >= 6:
                opcode, status = int.from_bytes(data[3:5], "little"), data[5]
            elif data[0] == 0x0F and len(data) == 6:
                opcode, status = int.from_bytes(data[4:6], "little"), data[2]
            else:
                continue
            replies[opcode] += 1
            if response_types is not None:
                response_types[opcode, data[0]] += 1
            if status:
                failures[opcode, status] += 1
    return commands, replies, failures, enabled


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="PacketLogger .pklg trace")
    args = parser.parse_args()
    try:
        with args.trace.open("rb") as stream:
            response_types = Counter()
            commands, replies, failures, enabled = summarize(stream, response_types)
    except (OSError, ValueError) as error:
        parser.exit(1, f"HCI analysis failed: {error}\n")
    for opcode, name in COMMANDS.items():
        if commands[opcode] or replies[opcode]:
            print(f"0x{opcode:04X} {name}: commands={commands[opcode]}, replies={replies[opcode]}")
    for (opcode, status), count in sorted(failures.items()):
        print(f"0x{opcode:04X} failed with HCI status 0x{status:02X}: {count}")
    if commands[0xFD3C] or replies[0xFD3C]:
        print(f"0xFD3C response types: Command Complete={response_types[0xFD3C, 0x0E]}, "
              f"Command Status={response_types[0xFD3C, 0x0F]}")
    print(f"Advertising enable=1 requests: legacy={enabled[0x200A]}, extended={enabled[0x2039]}")
    if not sum(enabled.values()):
        print("No advertising enable=1 request recorded. The trace does not establish on-air advertising.")
    else:
        print("Enable requests alone do not establish on-air advertising or an AirDrop transfer.")


if __name__ == "__main__":
    main()
