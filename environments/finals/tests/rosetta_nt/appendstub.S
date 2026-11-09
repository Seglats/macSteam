/* appendstub.S — the 0x18a0 append-hook stub (r8), assembled for real:
 * every encoding here is the assembler's, not hand-derived (r7 lesson).
 *
 *   clang -arch arm64 -c -o appendstub.o appendstub.S && \
 *   python3 mkstub.py appendstub.o append_blob.h _append_stub,pcall,pback
 *
 * ntstub.c copies this to the flagged page, patches the two placeholder
 * quads by value (pcall = payload ntfix_append, pback = 0x18a0 entry+0x10)
 * and installs the 16-byte abs jump at 0x18a0.
 *
 * The payload is plain C and may clobber x0-x18 and NZCV; the translator's
 * fixed assignments (x4 = x86 RSP, x17 = side flags, ...) ride in the saved
 * frame. NZCV is dead here: the original helper body does a cmp of its own.
 */
.globl _append_stub
_append_stub:
    stp x0, x1, [sp, #-0xa0]!
    stp x2, x3, [sp, #0x10]
    stp x4, x5, [sp, #0x20]
    stp x6, x7, [sp, #0x30]
    stp x8, x9, [sp, #0x40]
    stp x10, x11, [sp, #0x50]
    stp x12, x13, [sp, #0x60]
    stp x14, x15, [sp, #0x70]
    stp x16, x17, [sp, #0x80]
    stp x18, x30, [sp, #0x90]
    adr x10, pctl               /* ctl block (counters/events) */
    ldr x2, [x10]
    str x18, [x2, #0x60]        /* r10a: capture the thread ctx pointer */
    adr x10, pring              /* record ring */
    ldr x3, [x10]
    adr x10, pcall
    ldr x9, [x10]               /* adr gives the slot's address, not the value */
    blr x9                      /* (x0 = bufobj, w1 = word — already in place) */
    ldp x18, x30, [sp, #0x90]
    ldp x16, x17, [sp, #0x80]
    ldp x14, x15, [sp, #0x70]
    ldp x12, x13, [sp, #0x60]
    ldp x10, x11, [sp, #0x50]
    ldp x8, x9, [sp, #0x40]
    ldp x6, x7, [sp, #0x30]
    ldp x4, x5, [sp, #0x20]
    ldp x2, x3, [sp, #0x10]
    ldp x0, x1, [sp], #0xa0
    /* displaced 0x18a0 prologue — anchor-gated against the image at install */
    stp x20, x19, [sp, #-0x20]!
    stp x29, x30, [sp, #0x10]
    add x29, sp, #0x10
    mov x20, x1
    adr x9, pback
    ldr x9, [x9]
    br x9
pcall:
    .quad 0xDEAD000000000001    /* -> payload ntfix_append */
pctl:
    .quad 0xDEAD000000000003    /* -> ctl block (RW page) */
pring:
    .quad 0xDEAD000000000004    /* -> record ring */
pback:
    .quad 0xDEAD000000000002    /* -> 0x18a0 entry + 0x10 */
