#!/usr/bin/env python3
"""Synthetic executable images for offline RPA tests; never a runnable OS daemon."""
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def cpp_array(name):
    source = (ROOT / 'kext/BT/Patcher.cpp').read_text()
    body = re.search(r'const uint8_t ' + name + r'\[\] \{(.*?)\};', source, re.S)[1]
    return bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]+)', body))


def rust_array(name):
    source = (ROOT / 'rust/mtk-bt/src/rpa.rs').read_text()
    body = re.search(r'(?:pub )?const ' + name + r': &\[u8\] = &\[(.*?)\];', source, re.S)[1]
    return bytes(int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\b[0-9]+\b', body))


def image(historical=False, vm=0x100000000, move=0, fat=0):
    """Move code/data independently; the old image is an exact byte oracle."""
    if historical:
        positions = dict(Legacy=0x20c3dd, Scan=0x2234a1, Decode=0x729834,
                         Device=0x717002, Connect=0x7140c3, Update=0x6d11da,
                         Parent=0x73b856, Chip=0x511239, ChipGetter=0x394472,
                         Sink=0x223424, Address=0x7068f0, Paired=0x714156,
                         Entry=0x716fca, Resolver=0x22766b, Lookup=0x7000ee,
                         Lock=0x2bc5b8, Unlock=0x2bc610, Global=0xb3abb0)
        text_size, code_end, tail = 0xaa3000, 0x880000, 0xaa2b98
        positions.update(Ext=0x33abec, Wbs=0x374738, Fill=0x337505)
        positions['Pcm'] = positions['Fill'] + 12 + struct.unpack_from('<i', cpp_array('hfpFillFind'),8)[0]
        positions['Retry'] = 0x1fa68d
        positions['Setter'] = positions['Chip'] + 21 + struct.unpack_from('<i', cpp_array('advAddressFind'),17)[0]
        positions['Gate'] = 0xb1cd00
        data_va, data_size = 0xb00000, 0x20000
    else:
        positions = dict(Legacy=0x2100, Scan=0x2280, Decode=0x4100,
                         Device=0x2738, Connect=0x3100, Update=0x3300,
                         Parent=0x2600, Chip=0x2400, ChipGetter=0x2500,
                         Sink=0x2200, Address=0x2900, Paired=0x2800,
                         Entry=0x2700, Resolver=0x4800, Lookup=0x4810,
                         Lock=0x4820, Unlock=0x4830, Global=0x9200,
                         Setter=0x4900, Retry=0x5100, Gate=0x9100,
                         Ext=0x5300, Wbs=0x5400, Fill=0x5500, Pcm=0x4840)
        positions = {k: v + (move if k != 'Global' else move * 2) for k, v in positions.items()}
        text_size, code_end, tail = 0x9000, 0x7000, 0x8b98
        data_va, data_size = 0x9000, 0x1000
    out = bytearray(text_size + data_size)
    def put(at, value):
        out[at:at+len(value)] = value
    def rel(at, field, target, immediate=0):
        struct.pack_into('<i', out, at+field, target-at-field-4-immediate)
    def section(name, at, size):
        return struct.pack('<16s16sQQIIIIIIII', name.encode(), b'__TEXT', vm+at, size, at, 4, 0, 0, 0, 0, 0, 0)
    sections = section('__text', 0x2000, code_end-0x2000) + section('__eh_frame', tail-0x58, 0x58)
    text = struct.pack('<II16sQQQQiiII', 0x19, 72+len(sections), b'__TEXT', vm, text_size, 0, text_size, 5, 5, 2, 0) + sections
    data = struct.pack('<II16sQQQQiiII', 0x19, 72, b'__DATA', vm+data_va,
                       max(positions['Global']-data_va+0x1000,data_size), text_size, data_size, 3, 3, 0, 0)
    commands = text + data + struct.pack('<II16s', 0x1b, 24, bytes.fromhex('7ef7ef4d9b0e355abba8f34713b2580a'))
    put(0, struct.pack('<IIIIIIII', 0xfeedfacf, 0x01000007, 3, 2, 3, len(commands), 0, 0) + commands)
    for name in ('Legacy', 'Decode', 'Device', 'Connect', 'Update'):
        put(positions[name], cpp_array('rpa'+name+'Find25G332'))
    put(positions['Scan'], rust_array('SCAN')[:34] + cpp_array('rpaScanFind25G332'))
    put(positions['Parent'], rust_array('PARENT'))
    put(positions['Chip'], cpp_array('advAddressFind'))
    put(positions['ChipGetter'], rust_array('CHIP_GETTER'))
    for name, prefix in (('Ext','extAdv'),('Wbs','wbs'),('Fill','hfpFill')):
        put(positions[name],cpp_array(prefix+'Find'))
    put(positions['Retry'], cpp_array('advAddressRetryFind'))
    put(positions['Setter'], rust_array('ADDRESS_SETTER'))
    put(text_size+positions['Gate']-data_va,b'\x01')
    put(positions['Entry'], rust_array('DEVICE_ENTRY'))
    put(positions['Sink'], bytes.fromhex('554889e54157415641554154534883ec38'))
    put(positions['Address'], rust_array('ADDRESS_ENTRY'))
    put(positions['Address']+149, rust_array('ADDRESS_SELECTION'))
    put(positions['Paired'], rust_array('PAIRED_ENTRY'))
    for name in ('Resolver', 'Lookup', 'Lock', 'Unlock', 'Pcm'):
        put(positions[name], b'\xc3')
    rel(positions['Chip'], 1, positions['ChipGetter'])
    rel(positions['Chip'], 17, positions['Setter'])
    rel(positions['Wbs'], 5, positions['ChipGetter'])
    rel(positions['Fill'], 8, positions['Pcm'])
    rel(positions['Setter'], 7, positions['Gate'])
    rel(positions['Retry'], 34, positions['Gate'], 1)
    rel(positions['ChipGetter'], 3, positions['Global']+8, 1)
    rel(positions['ChipGetter'], 13, positions['Global'])
    rel(positions['Legacy'], 16, positions['Sink'])
    rel(positions['Scan'], 35, positions['Resolver'])
    rel(positions['Parent'], 34, positions['Entry'])
    rel(positions['Entry'], 46, positions['Lock'])
    rel(positions['Device'], 1, positions['Lookup'])
    rel(positions['Device'], 19, positions['Unlock'])
    for name, fields in (('Connect', (7,45)), ('Update', (15,192))):
        rel(positions[name], fields[0], positions['Paired'])
        rel(positions[name], fields[1], positions['Address'])
    if not historical:
        for name, fields in (('Decode', (112,)), ('Device', (28,)),
                             ('Update', (3,30,44,164,178))):
            for field in fields:
                rel(positions[name], field, positions['Global'])
    if fat:
        assert fat >= 4096 and fat % 4096 == 0
        header = struct.pack('>IIIIIII', 0xcafebabe, 1, 0x01000007, 3, fat, len(out), 12)
        out = bytearray(header) + bytearray(fat-len(header)) + out
    return bytes(out), positions
