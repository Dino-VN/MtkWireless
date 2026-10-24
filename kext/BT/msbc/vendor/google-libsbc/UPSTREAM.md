Source: https://github.com/google/libsbc
Pinned commit: 6e505650145c9973d08a0bdd5e5f5e1914305e40
License: Apache-2.0 (LICENSE retained).
Vendored files: include/sbc.h, src/sbc.c, src/bits.c, src/bits.h.
No optional ARM assembly or tool programs are included.

Local portability changes (2026-10-20):
- include/sbc.h: move two alignas declarations before their types, preserving
  their alignment and layout while allowing the public header in C++. Use a
  local SBC_ALIGNAS macro mapping to C++ alignas / C11 _Alignas so kernel builds
  do not require stdalign.h. Use that macro for local sample buffers as well.
- src/bits.h: select machine/limits.h under KERNEL because the pinned
  MacKernelSDK supplies architecture limits rather than a top-level limits.h.
- src/sbc.c: replace six signed left-shift-by-13 expressions with multiplication
  by 8192. Their bounded fixed-point operands fit in int; negative left shifts
  were undefined in C and triggered UBSan. No codec parameter or arithmetic
  scale changes.

The vendored codec remains Apache-2.0. All modified files carry a change notice.
