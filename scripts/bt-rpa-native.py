#!/usr/bin/env python3
"""Run relocated helper instructions against mocks, never the original daemon."""
import platform
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def exercise(plan, selection):
    if platform.machine() != 'x86_64' or platform.system() != 'Darwin':
        print('SKIP: native RPA instruction tests require x86_64 macOS')
        return
    helpers = bytes.fromhex(plan['edits'][0]['replacement']) + bytes(35)
    decode = bytes.fromhex(plan['edits'][3]['replacement'])[:90]
    scan = bytes.fromhex(plan['edits'][2]['replacement'])
    assert len(helpers) == 296 and len(scan) == 64 and len(decode) == 90
    references = {9: 'L_scan_sink', 16: '_rpa_legacy_return', 34: '_rpa_chip',
                  90: '_rpa_update_return', 150: '_rpa_chip', 207: '_rpa_chip',
                  228: 'L_getter_mock', 235: '_rpa_chip', 257: 'L_paired_mock'}

    def emit(code, labels=None, fields=None):
        labels, fields = labels or {}, fields or {}
        out, pos = [], 0
        for off in sorted(set(labels) | set(fields)):
            assert off >= pos
            if off > pos:
                out.append('.byte ' + ','.join(str(b) for b in code[pos:off]))
            if off in labels:
                out.append(labels[off] + ':')
            if off in fields:
                out.append('.long ' + fields[off] + ' - . - 4')
                pos = off + 4
            else:
                pos = off
        if pos < len(code):
            out.append('.byte ' + ','.join(str(b) for b in code[pos:]))
        return '\n'.join(out) + '\n'

    entries = {0: '_rpa_clear', 13: 'L_retain_helper', 87: 'L_device_helper',
               202: '_rpa_address_test', 232: '_rpa_paired_test'}
    assembly = '.text\n' + '\n'.join('.globl ' + name for name in entries.values() if name.startswith('_')) + '\n'
    assembly += emit(helpers, entries, references)
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
    assembly += emit(scan, fields={35: 'L_resolve_mock', 50: 'L_retain_helper'})
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
    assembly += emit(decode)
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
    with tempfile.TemporaryDirectory(prefix='mtk-rpa-native-') as directory:
        directory = Path(directory)
        source, binary = directory / 'native.S', directory / 'native'
        source.write_text(assembly)
        subprocess.run(['clang++', '-arch', 'x86_64', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        str(ROOT / 'tests/bt-page-patches/rpa-tests.cpp'), str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('PASS: dynamically relocated native RPA helpers, caller/chip guards, identity preservation, rotation and decoder registers')
