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
offset = 32
for _ in range(struct.unpack_from('<I', binary, 16)[0]):
    cmd, size = struct.unpack_from('<II', binary, offset)
    assert size >= 8 and offset + size <= len(binary)
    if cmd == 0x19:
        name, va, _, fileoff, filesize = struct.unpack_from('<16sQQQQ', binary, offset + 8)
        segments.append((name.rstrip(b'\0'), va, fileoff, filesize))
    offset += size

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
for prefix in ('extAdv', 'advAddress', 'advAddressRetry'):
    find, replacement = array(prefix + 'Find'), array(prefix + 'Replace')
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
a, b = array('advAddressFind'), array('advAddressReplace')
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
    assert array(prefix + 'Find') not in patched
print(f'Per-set address gate defaults true at 0x{gate:x}; false setter alone removed')
# Execute the actual replacement guard instructions on the x86_64 host.
# Its fall-through address is +18; unchanged public addresses jump to +137.
a, b = array('advAddressRetryFind'), array('advAddressRetryReplace')
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
print('Bluetooth page patch validation passed')
