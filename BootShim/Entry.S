/* SPDX-License-Identifier: MIT */
.section .text.entry, "ax"
.global _start
.type _start, %function
_start:
    b       shim_entry
    nop
    .quad   0x80000                 /* Linux arm64 Image text_offset */
    .quad   0                       /* image_size patched by tools/pack_boot.py */
    .quad   0                       /* little endian, unspecified page size */
    .quad   0
    .quad   0
    .quad   0
    .long   0x644d5241              /* ARM\x64 */
    .long   0
shim_entry:
    /* ABL's Linux ABI provides x0=DTB, caches/MMU off.
     * The local stack is included in the Linux image, before the gzip payload.
     */
    mrs     x9, CurrentEL
    cmp     x9, #4
    b.eq    2f
    cmp     x9, #8
    b.ne    1f
    mrs     x9, sctlr_el2
    b       3f
2:  mrs     x9, sctlr_el1
3:  tst     x9, #1
    b.ne    1f
    tst     x9, #4                 /* Linux ABI requires D-cache off. */
    b.ne    1f
    msr     daifset, #0xf
    msr     SPSel, #1              /* Use SP_EL1/EL2 even if ABL used ELxt. */
    mov     x19, x0
    adr     x20, _start
    adrp    x9, __stack_top
    add     x9, x9, :lo12:__stack_top
    mov     sp, x9
    mov     x0, x19
    mov     x1, x20
    bl      ShimMain
1:  wfe
    b       1b
.size _start, . - _start

.global ShimInvalidateRange
.type ShimInvalidateRange, %function
ShimInvalidateRange:
    /* Caller has verified the nonempty, nonoverflowing, disjoint FD range.
     * DMinLine gives log2(words per line); IVAC precedes the uncached copy.
     */
    add     x1, x0, x1
    mrs     x2, ctr_el0
    ubfx    x2, x2, #16, #4
    mov     x3, #4
    lsl     x3, x3, x2
    sub     x4, x3, #1
    bic     x0, x0, x4
1:  dc      ivac, x0
    add     x0, x0, x3
    cmp     x0, x1
    b.lo    1b
    dsb     sy
    ret
.size ShimInvalidateRange, . - ShimInvalidateRange

.global ShimEnterFirmware
.type ShimEnterFirmware, %function
ShimEnterFirmware:
    /* x0=original DTB, x1=original kernel source, x2=FD entry. */
    dsb     sy
    ic      iallu
    dsb     sy
    isb
    br      x2
.size ShimEnterFirmware, . - ShimEnterFirmware
