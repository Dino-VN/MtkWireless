// Auditable source for the exact bluetoothd helper bytes in Patcher.cpp.
// Offline tests assemble this file and compare its payload with those arrays.
// RVAs, native frames and object offsets are specific to x86_64 25G332.
// These are leaf helpers: no allocation, new frame, IRK access or HCI reply.
.text
.org 0xaa2c00

// LE_HCIEventHandler_AdvertisingReport reuses its descriptor in a loop.
// Its direct-address field is unused for legacy undirected reports. Clear
// exactly seven bytes, preserving the live flag immediately after them.
L_rpa_legacy25G332:
    xorl %eax, %eax
    movl %eax, 0x33(%rdi)
    movl %eax, 0x36(%rdi)
    .byte 0xe9
    .long 0x223424 - 0xaa2c00 - (. - L_rpa_legacy25G332) - 4

// Called only after the native AH resolver returned a verified identity.
// rbp is the scan frame, rbx the descriptor, r12 its primary typed address.
// Retain only chip 0 legacy ADV_IND RPAs; directed/extended reports retain
// their original direct-address meaning. Always do the native identity copy.
L_rpa_retain25G332:
    .byte 0x48, 0x8d, 0x15
    .long 0x20c3f1 - 0xaa2c00 - (. - L_rpa_legacy25G332) - 4
    cmpq %rdx, 8(%rbp)
    jne L_identity
    cmpb $0, (%rbx) // legacy ADV_IND
    jne L_identity
    .byte 0x48, 0x8b, 0x15
    .long 0xb3abb0 - 0xaa2c00 - (. - L_rpa_legacy25G332) - 4
    testq %rdx, %rdx
    je L_identity
    cmpl $0, 0x334(%rdx)
    jne L_identity
    movl (%r12), %eax
    movl 3(%r12), %ecx
    movl %eax, 0x33(%rbx)
    movl %ecx, 0x36(%rbx)
    movb $0x81, 0x33(%rbx) // internal marker; 0x01 is the real random type
L_identity:
    movl -0x48(%rbp), %eax
    movl -0x45(%rbp), %ecx
    movl %ecx, 3(%r12)
    movl %eax, (%r12)
    retq

// Called in the native mark-scanned setter while its recursive mutex is held.
// rax is LeDevice (nullable), r14b the original bool. Only the verified
// LeScanManager caller has the native parent-frame advertisement at -0x480.
// Consume/clear the internal marker before cache merge or listeners, even
// if the device disappeared; other setter callers keep their bool ABI.
L_rpa_device25G332:
    .byte 0x48, 0x8d, 0x0d
    .long 0x73b87c - 0xaa2c00 - (. - L_rpa_legacy25G332) - 4
    cmpq %rcx, 8(%rbp)
    jne L_flag
    movq (%rbp), %rcx
    movq -0x480(%rcx), %rcx
    movq 8(%rcx), %rcx
    cmpb $0x81, 0xd6(%rcx)
    jne L_flag
    movq 0xd0(%rcx), %rdx
    movq $0, 0xd0(%rcx) // original undirected-report value
    testq %rax, %rax
    je L_done
    .byte 0x48, 0x8b, 0x0d
    .long 0xb3abb0 - 0xaa2c00 - (. - L_rpa_legacy25G332) - 4
    testq %rcx, %rcx
    je L_flag
    cmpl $0, 0x334(%rcx)
    jne L_flag
    cmpq $0, 0x18(%rax)
    jne L_rpa
    movq 0x10(%rax), %rcx // native scan still supplied the resolved identity
    movq %rcx, 0x18(%rax)
L_rpa:
    btrq $55, %rdx // marker type 0x81 -> real random address type 0x01
    movq %rdx, 0x10(%rax) // replace last advertised address, keep identity
L_flag:
    testq %rax, %rax
    je L_done
    movb %r14b, 0x61(%rax)
L_done:
    retq

// Only the two initiating/filter-accept-list call sites use this wrapper.
// Use the native getter's advertised-address bool for chip 0. Its existing
// lock, nullable lookup and identity fallback remain the original code.
L_rpa_address25G332:
    xorl %edx, %edx
    .byte 0x48, 0x8b, 0x05
    .long 0xb3abb0 - 0xaa2c00 - (. - L_rpa_legacy25G332) - 4
    testq %rax, %rax
    je L_getter
    cmpl $0, 0x334(%rax)
    jne L_getter
    incl %edx
L_getter:
    .byte 0xe9
    .long 0x7068f0 - 0xaa2c00 - (. - L_rpa_legacy25G332) - 4

// Chip 0 needs the single current advertised address instead of the paired
// identity/extra-address set at these two connection sites. The native paired
// test itself and all security callers remain unchanged. Other chips delegate.
L_rpa_paired25G332:
    .byte 0x48, 0x8b, 0x05
    .long 0xb3abb0 - 0xaa2c00 - (. - L_rpa_legacy25G332) - 4
    testq %rax, %rax
    je L_paired
    cmpl $0, 0x334(%rax)
    jne L_paired
    xorl %eax, %eax
    retq
L_paired:
    .byte 0xe9
    .long 0x714156 - 0xaa2c00 - (. - L_rpa_legacy25G332) - 4

// Remaining end-of-__TEXT padding stays zero.
.org 0xaa2c00 + 296

// Replacement for the original 90-byte direct-address encoder. The pointer
// references a bounded 0x50-byte descriptor; the eighth load byte is its live
// flag, discarded by bswap/shr. Preserve cl/dl for the native following branch.
.section __TEXT,__rpa_decode,regular,pure_instructions
    movq 0x1768(%r14), %rsi
    movzbl (%rsi), %eax
    cmpb $1, %al
    jbe L_encode
    cmpb $0x81, %al
    jne L_invalid
    // The marker is only valid for a legacy connectable undirected report
    // with an RPA-shaped random address. Native direct types 0/1 skip this.
    cmpb $0, 0x1756(%r14)
    jne L_invalid
    cmpb $0, (%r14)
    jne L_invalid
    movb 1(%rsi), %al
    andb $0xc0, %al
    cmpb $0x40, %al
    jne L_invalid
L_encode:
    movq (%rsi), %rax
    bswapq %rax
    shrq $8, %rax
    jmp L_store
L_invalid:
    xorl %eax, %eax
    jmp L_store
.org 90, 0x90
L_store:
