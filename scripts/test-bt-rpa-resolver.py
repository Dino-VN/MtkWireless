#!/usr/bin/env python3
"""Offline dynamic RPA resolver tests: relocation, old bytes and fail-closed ABI."""
import argparse
import hashlib
import json
import runpy
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
fixtures = runpy.run_path(str(ROOT / 'scripts/bt-rpa-fixture.py'))
make_image = fixtures['image']
cpp_array = fixtures['cpp_array']
rust_array = fixtures['rust_array']
native = runpy.run_path(str(ROOT / 'scripts/bt-rpa-native.py'))['exercise']
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, help='also inspect supplied bytes; never execute the target')
args = parser.parse_args()

build = subprocess.run(['cargo', 'build', '-q', '-p', 'mtk-bt', '--example', 'rpa-plan', '--message-format=json'],
                       cwd=ROOT / 'rust', capture_output=True, text=True, check=True)
artifacts = [json.loads(line) for line in build.stdout.splitlines() if line.startswith('{')]
executable = next(Path(a['executable']) for a in artifacts if a.get('executable') and a.get('target', {}).get('name') == 'rpa-plan')


def inspect(path, success=True):
    before = hashlib.sha256(path.read_bytes()).digest()
    result = subprocess.run([str(executable), str(path)], cwd=ROOT, capture_output=True, text=True)
    assert hashlib.sha256(path.read_bytes()).digest() == before, 'resolver modified input'
    if success:
        assert result.returncode == 0, result.stderr
        return json.loads(result.stdout)
    assert result.returncode != 0 and not result.stdout, 'unsupported image was accepted'
    assert 'RPA plan refused:' in result.stderr, result.stderr
    return result.stderr.strip()


with tempfile.TemporaryDirectory(prefix='mtk-rpa-input-') as directory:
    directory = Path(directory)
    for historical, vm, move, fat in ((True, 0x100000000, 0, 0),
                                       (False, 0x100000000, 0, 0),
                                       (False, 0x700000000, 0x600, 0x4000)):
        data, positions = make_image(historical, vm, move, fat)
        path = directory / 'image.bin'
        path.write_bytes(data)
        plan = inspect(path)
        assert len(plan['edits']) == 7 and plan['slice_offset'] == fat
        helper = bytes.fromhex(plan['edits'][0]['replacement'])
        def target(field):
            return plan['helper_address'] + field + 4 + struct.unpack_from('<i', helper, field)[0]
        for field, expected in ((9, positions['Sink']), (16, positions['Legacy']+20),
                                (34, positions['Global']), (90, positions['Parent']+38),
                                (150, positions['Global']), (207, positions['Global']),
                                (228, positions['Address']), (235, positions['Global']),
                                (257, positions['Paired'])):
            assert target(field) == vm + expected, (field, hex(target(field)), hex(vm+expected))
        assert [e['file_offset'] for e in plan['edits'][1:]] == [fat+positions[n] for n in
            ('Legacy', 'Scan', 'Decode', 'Device', 'Connect', 'Update')]
        if historical:
            expected = [cpp_array('rpaHelpersReplace25G332')[120:381],
                        cpp_array('rpaLegacyReplace25G332'),
                        rust_array('SCAN')[:34] + cpp_array('rpaScanReplace25G332'),
                        cpp_array('rpaDecodeReplace25G332'), cpp_array('rpaDeviceReplace25G332'),
                        cpp_array('rpaConnectReplace25G332'), cpp_array('rpaUpdateReplace25G332')]
            assert [bytes.fromhex(e['replacement']) for e in plan['edits']] == expected, 'historical 25G332 patch changed'
            print('PASS: dynamic output reproduces all seven historical 25G332 patches exactly')
            native(plan, rust_array('ADDRESS_SELECTION'))
        else:
            print(f'PASS: independent code/data relocation, VM base {vm:#x}, FAT slice {fat:#x}; page apply and idempotence')
    pristine, p = make_image()
    cases = {}
    missing = bytearray(pristine); missing[p['Legacy']] ^= 1; cases['missing signature'] = missing
    duplicate = bytearray(pristine); duplicate[0x5200:0x5200+29] = pristine[p['Legacy']:p['Legacy']+29]
    cases['ambiguous signature'] = duplicate
    padding = bytearray(pristine); padding[0x8c00+17] = 1; cases['nonzero helper padding'] = padding
    abi = bytearray(pristine); abi[p['Address']+149] ^= 1; cases['changed getter ABI'] = abi
    frame = bytearray(pristine); frame[p['Parent']+3] ^= 1; cases['changed native frame'] = frame
    disagree = bytearray(pristine)
    struct.pack_into('<i', disagree, p['Update']+15, p['Address']-(p['Update']+19))
    cases['initiating paths disagree'] = disagree
    gate = bytearray(pristine); gate[p['Gate']] = 0; cases['address gate not default true'] = gate
    setter = bytearray(pristine); setter[p['Setter']+5] ^= 1; cases['changed address setter ABI'] = setter
    crossed = bytearray(pristine); old = bytes(crossed[p['Legacy']:p['Legacy']+29]); crossed[p['Legacy']:p['Legacy']+29] = bytes(29)
    crossed[0x2ff0:0x2ff0+29] = old
    struct.pack_into('<i', crossed, 0x2ff0+16, p['Sink']-(0x2ff0+20))
    cases['edit crosses validation page'] = crossed
    header = bytearray(pristine); struct.pack_into('<I', header, 20, 0xffffffff); cases['malformed load commands'] = header
    exhausted = bytearray(pristine); section = 32+72+80
    struct.pack_into('<Q', exhausted, section+32, 0x100008ff0)
    struct.pack_into('<Q', exhausted, section+40, 16)
    struct.pack_into('<I', exhausted, section+48, 0x8ff0)
    cases['no padding after all sections'] = exhausted
    for name, data in cases.items():
        path = directory / 'invalid.bin'; path.write_bytes(data)
        inspect(path, False)
        print('PASS: refused ' + name)
if args.binary:
    plan = inspect(args.binary.resolve())
    print(f'PASS: supplied binary UUID {plan["uuid"]}, {len(plan["edits"])} dynamically relocated edits; input unchanged')
    native(plan, rust_array('ADDRESS_SELECTION'))
print('Dynamic RPA resolver regression passed')
