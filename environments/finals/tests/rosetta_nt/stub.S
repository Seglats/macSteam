/* stub.S — milestone-2 emitter-entry trampolines (round 3).
 *
 * Assembled -arch arm64 -c; mkstub.py flattens __TEXT into a word array
 * (stub_blob.h) that ntstub.c copies verbatim onto its anonymous page. The
 * blob is position-independent; only the MAGIC address slots are rewritten
 * at install (found by value).
 *
 * Round history (2026-10-31):
 *  r1  call-site wrappers at 0x1a0ec/0x1b9a0 — never executed (counters 0),
 *      but that run predates the eager-translation discovery, so "cold" was
 *      unproven either way: Rosetta pre-translates the whole binary __TEXT
 *      before main, so any startup-translated code bypasses late patches.
 *  r2  entry patches at 0x2a7fc/0x2abd0 with counter trampolines — with a
 *      fresh-anon-page trigger (translation guaranteed post-install) the
 *      call HUNG in a killable mach wait, PC parked at the trampoline's
 *      first word; the NOHOOK control (same trigger, no patch) is clean.
 *      So executing the trampoline during live translation is what breaks.
 *  r3  bisect ladder, selected by MODE in ntstub.c:
 *        MODE=0 PURE trampoline: the displaced entry word and the return
 *              branch are install-patched into this page; the stub executes
 *              no register-writing instruction and no indirect branch. If
 *              this still hangs, fetching our anon page mid-translation is
 *              the problem (permissions/registration/lock), not registers.
 *        MODE=1 counter trampoline (r2 design) — adr x17 counter / replay
 *              assembled displaced word / ldr x16 ret / br x16. Clobbers
 *              x16+x17; suspected incompatible with the dispatch's live
 *              register state, or with BTI on the indirect br back.
 *
 * Entry contract: reached by the `b` ntstub installs over each emitter's
 * first word; all callee state must be preserved exactly.
 */
.section __TEXT,__text

/* --- MODE=0 pure trampolines ------------------------------------------ */
.globl _stub_popf_pure
_stub_popf_pure:
pdword_popf:
    .word 0xd420b001                 /* install: original word at rb+0x2a7fc */
pback_popf:
    b    .                           /* install: b -> rb+0x2a800 */

.globl _stub_pushf_pure
_stub_pushf_pure:
pdword_pushf:
    .word 0xd420b002                 /* install: original word at rb+0x2abd0 */
pback_pushf:
    b    .                           /* install: b -> rb+0x2abd4 */

/* --- MODE=1 counter trampolines (r2) ----------------------------------- */
.globl _stub_popf_entry
_stub_popf_entry:
    adr x17, ctr_popf
    ldr x16, [x17]
    add x16, x16, #1
    str x16, [x17]
disp_popf:
    stp x24, x23, [sp, #-0x40]!      /* displaced 0x2a7fc prologue */
    ldr x16, ret_popf
    br x16                            /* -> rb+0x2a800 (patched at install) */

.globl _stub_pushf_entry
_stub_pushf_entry:
    adr x17, ctr_pushf
    ldr x16, [x17]
    add x16, x16, #1
    str x16, [x17]
disp_pushf:
    stp x22, x21, [sp, #-0x30]!      /* displaced 0x2abd0 prologue */
    ldr x16, ret_pushf
    br x16                            /* -> rb+0x2abd4 */

/* --- literals (no underscore: assembly symbol names are literal) -------- */
.p2align 3
ctr_popf:  .quad 0
ctr_pushf: .quad 0
ret_popf:  .quad 0x504554505f464f50 /* "POPF_PTER" magic -> rb+0x2a800 */
ret_pushf: .quad 0x504554505f485355 /* "USHP_PTER" magic -> rb+0x2abd4 */
