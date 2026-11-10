/* fmt_asm.S - front ends for Darwin arm64 variadic libSystem calls.
 *
 * Apple's arm64 ABI passes every anonymous argument on the stack, so at
 * function entry sp points at argument #1 (8-byte slots).  Each stub hands
 * that pointer to the C formatter and tail-branches, keeping the guest's
 * return address in x30.
 */
    .text


.macro ENTRY name
    .globl \name
    .p2align 2
\name:
.endm

ENTRY ds_printf_stub
    mov x1, sp
    b   ds_printf_c
ENTRY ds_fprintf_stub
    mov x2, sp
    b   ds_fprintf_c
ENTRY ds_sprintf_stub
    mov x2, sp
    b   ds_sprintf_c
ENTRY ds_snprintf_stub
    mov x3, sp
    b   ds_snprintf_c
ENTRY ds_asprintf_stub
    mov x2, sp
    b   ds_asprintf_c
ENTRY ds_dprintf_stub
    mov x2, sp
    b   ds_dprintf_c
ENTRY ds___sprintf_chk_stub
    mov x4, sp
    b   ds_sprintf_chk_c
ENTRY ds___snprintf_chk_stub
    mov x5, sp
    b   ds_snprintf_chk_c
ENTRY ds_err_stub
    mov x2, sp
    b   ds_err_cur
ENTRY ds_warn_stub
    mov x1, sp
    b   ds_warn_cur
ENTRY ds_warnx_stub
    mov x1, sp
    b   ds_warn_cur

/* Darwin arm64 jmp_buf is int[48] = 192 bytes (setjmp.h: _JBLEN (14+8+2)*2).
 * musl's jmp_buf is 312 bytes on aarch64 — passing through would write 120
 * bytes past the guest's buffer on every setjmp().  These save exactly
 * 168 bytes: x19-x28, x29, x30, sp, d8-d15. */
ENTRY ds_setjmp
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x9, sp
    str x9, [x0, #96]
    stp d8,  d9,  [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mov w0, #0
    ret

ENTRY ds_longjmp
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x9, [x0, #96]
    mov sp, x9
    ldp d8,  d9,  [x0, #104]
    ldp d10, d11, [x0, #120]
    ldp d12, d13, [x0, #136]
    ldp d14, d15, [x0, #152]
    cmp w1, #0
    csinc w0, w1, wzr, ne      /* longjmp(buf, 0) returns 1 */
    ret

/* sigsetjmp/siglongjmp must tail-call, not wrap: a wrapper's frame is gone
 * by the time longjmp returns. */
ENTRY ds_sigsetjmp
    b   ds_setjmp
ENTRY ds_siglongjmp
    b   ds_longjmp
