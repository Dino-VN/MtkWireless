#!/usr/bin/env python3
"""Validate bluetoothd page patches against the installed x86_64 Mach-O."""
import argparse
import re
import struct
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, default=Path('/usr/sbin/bluetoothd'))
args = parser.parse_args()
binary = args.binary.read_bytes()
if binary[:4] == bytes.fromhex('cafebabe'):
    count = struct.unpack_from('>I', binary, 4)[0]
    for i in range(count):
        cpu, _, offset, size, _ = struct.unpack_from('>IIIII', binary, 8 + i * 20)
        if cpu == 0x01000007:
            binary = binary[offset:offset + size]
            break
    else:
        raise SystemExit('No x86_64 slice')
assert binary[:4] == bytes.fromhex('cffaedfe'), 'Expected little-endian Mach-O 64'
segments = []
sections = {}
uuid = None
text_protection = None
offset = 32
for _ in range(struct.unpack_from('<I', binary, 16)[0]):
    cmd, size = struct.unpack_from('<II', binary, offset)
    assert size >= 8 and offset + size <= len(binary)
    if cmd == 0x19:
        name, va, _, fileoff, filesize = struct.unpack_from('<16sQQQQ', binary, offset + 8)
        segments.append((name.rstrip(b'\0'), va, fileoff, filesize))
        if name.rstrip(b'\0') == b'__TEXT':
            text_protection = struct.unpack_from('<ii', binary, offset + 56)
        for i in range(struct.unpack_from('<I', binary, offset + 64)[0]):
            section = offset + 72 + i * 80
            section_name = binary[section:section + 16].rstrip(b'\0')
            address, length, section_off = struct.unpack_from('<QQI', binary, section + 32)
            sections[(name.rstrip(b'\0'), section_name)] = (address, length, section_off)
    if cmd == 0x1b:
        uuid = binary[offset + 8:offset + 24]
    offset += size
load_commands_end = offset

def virtual(fileoff):
    for _, va, off, size in segments:
        if off <= fileoff < off + size:
            return va + fileoff - off
    raise AssertionError('Patch outside mapped file')

def read_virtual(address, size):
    for _, va, off, length in segments:
        if va <= address and address + size <= va + length:
            return binary[off + address - va:off + address - va + size]
    raise AssertionError('Address outside file-backed segment')

source = (root / 'kext/BT/Patcher.cpp').read_text()
def array(name):
    body = re.search(r'const uint8_t ' + name + r'\[\] \{(.*?)\};', source, re.S)[1]
    return bytes(int(n, 16) for n in re.findall(r'0x([0-9a-fA-F]+)', body))

patched = bytearray(binary)
locations = {}
# Per-build variants: name suffix '' is 25G241, '25G332' is 26.7.2.
def variant(prefix):
    for suffix in ('', '25G332'):
        if 'const uint8_t ' + prefix + 'Find' + suffix + '[]' not in source:
            continue
        find = array(prefix + 'Find' + suffix)
        try:
            replacement = array(prefix + 'Replace' + suffix)
        except TypeError:
            replacement = array(prefix + 'Replace')
        if binary.count(find) + binary.count(replacement) == 1:
            return suffix, find, replacement
    return '', array(prefix + 'Find'), array(prefix + 'Replace')
suffixes = {}
for prefix in ('extAdv', 'advAddress', 'advAddressRetry'):
    suffix, find, replacement = variant(prefix)
    suffixes[prefix] = suffix
    assert len(find) == len(replacement)
    originals, replacements = binary.count(find), binary.count(replacement)
    # cs_validate_page patches can be visible through the live vnode cache.
    assert originals + replacements == 1, prefix + ': target must match exactly once'
    off = binary.index(find if originals else replacement)
    if replacements:
        print(prefix + ': live file cache already contains this patch')
    va = virtual(off)
    assert va // 4096 == (va + len(find) - 1) // 4096, 'Pattern crosses page'
    patched[off:off + len(find)] = replacement
    locations[prefix] = va
    print(f'{prefix}: unique pattern at 0x{va:x}, fits validation page')

# Extended-advertising check returns true, with balanced stack, exactly.
assert array('extAdvReplace')[:8] == bytes.fromhex('55 48 89 e5 b0 01 5d c3')
# Remove only the false setter call. Keep chip lookup, comparison and branch.
a, b = variant('advAddress')[1:]
assert a[:14] == b[:14] and b[14:] == b'\x90' * 7
assert a[14:17] == bytes.fromhex('31 ff e8')
call_end = locations['advAddress'] + len(a)
setter = call_end + struct.unpack_from('<i', a, 17)[0]
setter_code = read_virtual(setter, 13)
assert setter_code[:7] == bytes.fromhex('55 48 89 e5 40 88 3d')
assert setter_code[11:] == bytes.fromhex('5d c3')
gate = setter + 11 + struct.unpack_from('<i', setter_code, 7)[0]
assert read_virtual(gate, 1) == b'\x01', 'Address gate must default true'
# Applying again must be a no-op; a page cannot repeatedly match.
for prefix in ('extAdv','advAddress','advAddressRetry'):
    assert variant(prefix)[1] not in patched
print(f'Per-set address gate defaults true at 0x{gate:x}; false setter alone removed')
# Execute the actual replacement guard instructions on the x86_64 host.
# Its fall-through address is +18; unchanged public addresses jump to +137.
a, b = variant('advAddressRetry')[1:]
assert a[:23] == b[:23] and a[41:] == b[41:]
guard = b[23:41]
assert guard == bytes.fromhex('f6 84 08 0e 0d 00 00 01 75 08 44 09 c2 74 7a 90 90 90')
assert 10 + struct.unpack('b', guard[9:10])[0] == 18
assert 15 + struct.unpack('b', guard[14:15])[0] == 137
assembly = '\n'.join([
    '.text', '.globl _address_guard', '_address_guard:',
    'movq %rdi, %rax', 'xorq %rcx, %rcx', 'movl %esi, %r8d',
    'guard_start:', '.byte ' + ','.join(str(n) for n in guard),
    'movl $1, %eax', 'retq', '.org guard_start + 137',
    'xorl %eax, %eax', 'retq', ''])
harness = r'''
#include <cassert>
#include <cstdint>
#include <vector>
extern "C" int address_guard(const uint8_t*, unsigned, unsigned);
int main() {
    std::vector<uint8_t> params(0xd28);
    // Run the exact machine instructions for cached/changed addresses.
    // Reproduce handle 0: cached random address matches after failed early
    // 0x2035. It must retry after creation, rather than go straight to enable.
    for (unsigned type : {0u,1u}) {
        params[0xd0e] = type;
        for (unsigned lo : {0u,1u,0x80000000u})
            for (unsigned hi : {0u,1u,0x80000000u})
                assert(address_guard(params.data(),lo,hi) ==
                       (type == 1 || lo != 0 || hi != 0));
    }
}
'''
with tempfile.TemporaryDirectory(prefix='mtk-bt-address-') as directory:
    directory = Path(directory)
    (directory / 'guard.S').write_text(assembly)
    (directory / 'test.cpp').write_text(harness)
    subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(directory/'test.cpp'),
                    str(directory/'guard.S'),'-o',str(directory/'test')],check=True)
    subprocess.run([str(directory/'test')],check=True)
print('Random address retry/public address guard: native instruction tests passed')


def validate_rpa():
    # The padding anchor includes a build-specific native first-function call.
    # Some child signatures are identical on 25G241; the page hook must keep
    # them untouched until the 25G332 helper anchor was installed.
    if uuid != bytes.fromhex('7ef7ef4d9b0e355abba8f34713b2580a'):
        print('RPA fallback: this build is unsupported (25G332 only)')
        return
    targets = {
        'rpaHelpers': 0x100aa2b88, 'rpaLegacy': 0x10020c3dd,
        'rpaScan': 0x1002234c3, 'rpaDecode': 0x100729834,
        'rpaDevice': 0x100717002, 'rpaConnect': 0x1007140c3,
        'rpaUpdate': 0x1006d11da,
    }
    pairs = {}
    for prefix, expected in targets.items():
        find, replacement = (array(prefix + name + '25G332') for name in ('Find', 'Replace'))
        assert len(find) == len(replacement) and find != replacement
        originals, replacements = binary.count(find), binary.count(replacement)
        assert originals + replacements == 1, prefix + ': target must match exactly once'
        off = binary.index(find if originals else replacement)
        va = virtual(off)
        assert va == expected, prefix + ': unexpected patch address'
        assert off // 4096 == (off + len(find) - 1) // 4096, 'Pattern crosses validation page'
        patched[off:off + len(find)] = replacement
        assert find not in patched, prefix + ': patch must be idempotent'
        pairs[prefix] = find, replacement
        print(f'{prefix}: unique 25G332 pattern at {va:#x}, fits validation page' +
              (' (live cache already patched)' if replacements else ''))

    original, replacement = pairs['rpaHelpers']
    base = 0x100aa2c00
    helpers = replacement[base - targets['rpaHelpers']:][:261] + bytes(35)
    eh_addr, eh_size, _ = sections[(b'__TEXT', b'__eh_frame')]
    assert targets['rpaHelpers'] + 16 == eh_addr + eh_size, 'Anchor must start at the __eh_frame tail'
    assert base // 4096 != virtual(load_commands_end) // 4096, 'Helpers must stay off the load-command pages'
    assert (targets['rpaHelpers'] + len(original) - 1) // 4096 == base // 4096 < 0x100aa3000 // 4096
    assert text_protection == (5, 5), 'Helpers must reside in executable read-only __TEXT'
    assert original[16:] == bytes(len(original) - 16), 'Only zero end-of-__TEXT padding may be reclaimed'
    assert original[:16] == replacement[:16] and replacement[base - targets['rpaHelpers'] + 261:] == bytes(11)
    assert helpers[261:] == bytes(35)

    # Validate every new RIP/global/branch target before rebinding it for a
    # native instruction test. No original bluetoothd executable is run.
    references = {
        0xaa2c09: (0x223424, 'L_scan_sink'),
        0xaa2c10: (0x20c3f1, '_rpa_legacy_return'),
        0xaa2c22: (0xb3abb0, '_rpa_chip'),
        0xaa2c5a: (0x73b87c, '_rpa_update_return'),
        0xaa2c96: (0xb3abb0, '_rpa_chip'),
        0xaa2ccf: (0xb3abb0, '_rpa_chip'),
        0xaa2ce4: (0x7068f0, 'L_getter_mock'),
        0xaa2ceb: (0xb3abb0, '_rpa_chip'),
        0xaa2d01: (0x714156, 'L_paired_mock'),
    }
    for rva, (target, _) in references.items():
        off = rva - 0xaa2c00
        assert rva + 4 + struct.unpack_from('<i', helpers, off)[0] == target
        assert helpers[off - 1] == 0xe9 or helpers[off - 3:off] in (
            bytes.fromhex('48 8b 05'), bytes.fromhex('48 8b 0d'), bytes.fromhex('48 8b 15'),
            bytes.fromhex('48 8d 0d'), bytes.fromhex('48 8d 15'))

    calls = {
        'rpaLegacy': ((15, 0x223424, 0xaa2c00),),
        'rpaScan': ((0, 0x22766b, 0x22766b), (15, None, 0xaa2c0d)),
        'rpaDevice': ((0, 0x7000ee, 0x7000ee), (5, None, 0xaa2c57)),
        'rpaConnect': ((6, 0x714156, 0xaa2ce8), (44, 0x7068f0, 0xaa2cca)),
        'rpaUpdate': ((14, 0x714156, 0xaa2ce8), (191, 0x7068f0, 0xaa2cca)),
    }
    for prefix, fields in calls.items():
        a, b = pairs[prefix]
        for off, before, after in fields:
            for code, expected in ((a, before), (b, after)):
                if expected is None:
                    continue
                assert code[off] == 0xe8
                assert targets[prefix] + off + 5 + struct.unpack_from('<i', code, off + 1)[0] == 0x100000000 + expected
    assert pairs['rpaScan'][0][15:] == bytes.fromhex('8b45b88b4dbb41894c240341890424')
    assert pairs['rpaScan'][1][20:] == b'\x90' * 10
    assert pairs['rpaDevice'][0][5:14] == bytes.fromhex('4885c0740444887061')
    assert pairs['rpaDevice'][1][10:14] == b'\x90' * 4
    assert pairs['rpaDecode'][0][90:] == pairs['rpaDecode'][1][90:]
    assert pairs['rpaDecode'][0][101:109] == bytes.fromhex('08ca0f8488000000'), 'Native cl/dl branch must stay intact'
    # Only two call displacements change in each accept-list path; bool ABI
    # and the native public paired/identity/security routines remain unchanged.
    for prefix in ('rpaConnect', 'rpaUpdate'):
        a, b = pairs[prefix]
        allowed = {i for off, _, _ in calls[prefix] for i in range(off + 1, off + 5)}
        assert all(x == y or i in allowed for i, (x, y) in enumerate(zip(a, b)))

    getter = read_virtual(0x10039447c, 8)
    assert getter[:3] == bytes.fromhex('488b05') and getter[-1] == 0xc3
    assert 0x100394483 + struct.unpack_from('<i', getter, 3)[0] == 0x100b3abb0
    assert read_virtual(0x100716fca, 16) == bytes.fromhex('554889e541574156534883ec184189d6')
    for address, target in ((0x100716ff7, 0x1002bc5b8), (0x100717014, 0x1002bc610)):
        code = read_virtual(address, 5)
        assert code[0] == 0xe8 and address + 5 + struct.unpack_from('<i', code, 1)[0] == target
    assert read_virtual(0x10073b856, 38) == bytes.fromhex(
        '488b8580fbffff488b400831d283b838010000000f95c24889df4889b570fbffffe84eb7fdff')
    selection = read_virtual(0x100706985, 20)
    assert selection == bytes.fromhex('4584f675094d8b77184d85f6752f4d8b7710eb29')
    scan_guard = read_virtual(0x1002234a1, 34) + pairs['rpaScan'][1]
    assert len(scan_guard) == 64
    assert scan_guard[:13] == bytes.fromhex('3c01753c8a430224c03c407533')

    def emit(code, labels=None, fields=None):
        labels, fields = labels or {}, fields or {}
        out, pos = [], 0
        for off in sorted(set(labels) | set(fields)):
            assert off >= pos
            if off > pos:
                out.append('.byte ' + ','.join(str(x) for x in code[pos:off]))
            if off in labels:
                out.append(labels[off] + ':')
            if off in fields:
                out.append('.long ' + fields[off] + ' - . - 4')
                pos = off + 4
            else:
                pos = off
        if pos < len(code):
            out.append('.byte ' + ','.join(str(x) for x in code[pos:]))
        return '\n'.join(out) + '\n'

    assembly = '.text\n'
    entries = {0: '_rpa_clear', 13: 'L_retain_helper', 87: 'L_device_helper',
               202: '_rpa_address_test', 232: '_rpa_paired_test'}
    assembly += '\n'.join('.globl ' + name for name in entries.values() if name.startswith('_')) + '\n'
    assembly += emit(helpers, entries, {rva - 0xaa2c00: name for rva, (_, name) in references.items()})
    assembly += '''
.globl _rpa_scan_test
_rpa_scan_test:
pushq %rbp
pushq %rbx
pushq %r12
movq %rdi, %rbx
leaq 1(%rbx), %r12
movq %rsi, %rbp
movb (%r12), %al
'''
    assembly += emit(scan_guard, fields={35: 'L_resolve_mock', 50: 'L_retain_helper'})
    assembly += '''
popq %r12
popq %rbx
popq %rbp
retq
.globl _rpa_device_test
_rpa_device_test:
pushq %rbp
pushq %r14
pushq %rbx
movq %rdi, %rax
movq %rsi, %rbp
movl %edx, %r14d
callq L_device_helper
popq %rbx
popq %r14
popq %rbp
retq
.globl _rpa_original_selection
_rpa_original_selection:
pushq %r14
pushq %r15
movq %rdi, %r15
movl %esi, %r14d
L_selection:
'''
    assembly += emit(selection)
    assembly += '''
.org L_selection + 61, 0x90
movq %r14, %rax
popq %r15
popq %r14
retq
.globl _rpa_decode_test
_rpa_decode_test:
pushq %r14
movq %rdi, %r14
movl $0x5a, %ecx
movl $0xa5, %edx
'''
    assembly += emit(pairs['rpaDecode'][1][:90])
    assembly += '''
cmpb $0x5a, %cl
jne L_bad_register
cmpb $0xa5, %dl
jne L_bad_register
popq %r14
retq
L_bad_register:
ud2
.globl _rpa_legacy_return, _rpa_update_return
_rpa_legacy_return:
nop
_rpa_update_return:
nop
nop
L_scan_sink:
jmp _rpa_scan_sink
L_getter_mock:
jmp _rpa_getter_mock
L_paired_mock:
jmp _rpa_paired_mock
L_resolve_mock:
jmp _rpa_resolve_mock
.data
.globl _rpa_chip
_rpa_chip:
.quad 0
'''
    with tempfile.TemporaryDirectory(prefix='mtk-bt-rpa-') as directory:
        directory = Path(directory)
        reference = directory / 'reference.o'
        subprocess.run(['clang', '-arch', 'x86_64', '-c',
                        str(root / 'tests/bt-page-patches/rpa-25G332.S'), '-o', str(reference)], check=True)
        obj, object_sections = reference.read_bytes(), {}
        off = 32
        for _ in range(struct.unpack_from('<I', obj, 16)[0]):
            cmd, size = struct.unpack_from('<II', obj, off)
            if cmd == 0x19:
                for i in range(struct.unpack_from('<I', obj, off + 64)[0]):
                    section = off + 72 + i * 80
                    name = obj[section:section + 16].rstrip(b'\0')
                    _, length, fileoff = struct.unpack_from('<QQI', obj, section + 32)
                    assert struct.unpack_from('<I', obj, section + 60)[0] == 0, 'Unresolved helper relocation'
                    object_sections[name] = obj[fileoff:fileoff + length]
            off += size
        assert object_sections[b'__text'][0xaa2c00:0xaa2c00 + 296] == helpers
        assert object_sections[b'__rpa_decode'] == pairs['rpaDecode'][1][:90]
        (directory / 'native.S').write_text(assembly)
        subprocess.run(['clang++', '-arch', 'x86_64', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        str(root / 'tests/bt-page-patches/rpa-tests.cpp'), str(directory / 'native.S'),
                        '-o', str(directory / 'native')], check=True)
        subprocess.run([str(directory / 'native')], check=True)
    print('RPA fallback: native resolver guards, retention, type decoder, locked device update, rotation and chip scope passed')


validate_rpa()

def validate_le_ready_mask_contract():
    if uuid != bytes.fromhex('7ef7ef4d9b0e355abba8f34713b2580a'):
        print('SKIP: LE readiness binary evidence is specific to 25G332')
        return
    # The native backend getter (0 = generic) selects the legacy mask with
    # cmove; nonzero selects the modern mask. No new page patch is needed.
    selection = read_virtual(0x100171183, 25)
    assert selection == bytes.fromhex('e843f8feff85c0488d05f0329000488d15f1329000480f44d0')
    assert 0x100171188 + struct.unpack_from('<i', selection, 1)[0] == 0x1001609cb
    legacy = 0x100171191 + struct.unpack_from('<i', selection, 10)[0]
    modern = 0x100171198 + struct.unpack_from('<i', selection, 17)[0]
    old_mask = int.from_bytes(read_virtual(legacy, 8), 'little')
    modern_mask = int.from_bytes(read_virtual(modern, 8), 'little')
    assert old_mask == 0x1f and modern_mask & 0x840 == 0x840
    # Internal command IDs map to the actual standard opcodes; readiness
    # setters are not command submitters. Record this independently of names.
    for command_id, opcode, length in ((0xa2, 0x2001, 8), (0xc2, 0x2022, 6),
                                       (0xc6, 0x2030, 2), (0xc8, 0x2032, 7)):
        descriptor = read_virtual(0x100b1b230 + command_id * 24, 8)
        assert struct.unpack_from('<H', descriptor, 2)[0] == opcode
        assert descriptor[6] == length
    getter = read_virtual(0x1001609cb, 12)
    assert getter == bytes.fromhex('554889e58b0523179f005dc3')
    # Remote-features callback passes DLE in esi, 2M in edx, status in ecx.
    assert read_virtual(0x10021673c, 16) == bytes.fromhex('0fb6f20fb6d14889df31c9e8f937ffff')
    assert read_virtual(0x100209f61, 17) == bytes.fromhex('85d2740d80bb99000000000f84a4000000')
    assert read_virtual(0x10020a016, 12) == bytes.fromhex('c7838400000004000000eb0a')
    # Data-length readiness is a separate flag, set to 1 by the handler.
    assert read_virtual(0x10020a437, 10) == bytes.fromhex('c7838800000001000000')
    assert read_virtual(0x10020a2ec, 18) == bytes.fromhex('83bb8400000003755e83bb88000000017555')
    assert read_virtual(0x10020a6b2, 13) == bytes.fromhex('83bb84000000040f8515010000')
    print(f'PASS: 25G332 generic mask {old_mask:#x} disables DLE/PHY; HCI repair is {old_mask | 0x840:#x}; native readiness evidence checked')

validate_le_ready_mask_contract()
print('Bluetooth page patch validation passed')
