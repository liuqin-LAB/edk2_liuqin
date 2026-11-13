/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Entry runs before the original transfer routine switches stacks. The
 * injector patches KepPayloadTail to B to that original transfer routine. */
.section .text.entry,"ax"
.p2align 4
.global KepPayloadStart, KepPayloadTail
.global KepSgiThunkStart, KepSgiThunkEnd, KepSgiThunkOperand, KepSgiThunkReturn
.hidden KepPatchLoadedModules
KepPayloadStart:
    stp x29, x30, [sp, #-32]!
    stp x0, x1, [sp, #16]
    mov x29, sp
    bl KepPatchLoadedModules
    ldp x0, x1, [sp, #16]
    ldp x29, x30, [sp], #32
    b KepPayloadTail

/* Installed with B, not BL: LR and all registers live across the original
 * inline MSR are preserved. The final B is patched to site+4. Aff1=0..7,
 * Aff0=0 is Liuqin's ACPI/SM8450-family DT topology. IRM=1 broadcasts to
 * every CPU except the caller; IRM=0 keeps the original unicast value.
 * This thunk uses only EL1-accessible registers and a normal kernel stack. */
.section .text.thunk,"ax"
.p2align 4
KepSgiThunkStart:
    stp x0, x1, [sp, #-64]!
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    mrs x3, nzcv
    str x3, [sp, #48]
KepSgiThunkOperand:
    mov x0, x8
    dsb ishst
    tbz x0, #40, 3f
    and x0, x0, #0xf000000
    orr x0, x0, #1
    mrs x2, mpidr_el1
    ubfx x2, x2, #8, #8
    mov x1, #0
1:
    cmp x1, x2
    b.eq 2f
    orr x4, x0, x1, lsl #16
    msr icc_sgi1r_el1, x4
    dsb sy
2:
    add x1, x1, #1
    cmp x1, #8
    b.ne 1b
    b 4f
3:
    msr icc_sgi1r_el1, x0
    dsb sy
4:
    ldr x3, [sp, #48]
    msr nzcv, x3
    ldp x4, x5, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp], #64
KepSgiThunkReturn:
    b .
KepSgiThunkEnd:

.section .text.tail,"ax"
KepPayloadTail:
    b .
