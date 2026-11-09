# R19 — the direct-syscall fast path (design, 2026-11-06)

Goal: THE FINALS issues ~32,000 direct x86 `syscall` instructions/s (rax=0xffffffff and
Windows SSNs — classes 0 and 7). Each takes the full Rosetta exception round trip
(brk #5 → exception server → emulated SIGSYS → Wine's sigsys_handler): ~16-18 µs vs
~2 µs on Proton. Scope extension approved by Rasmus 2026-11-02: route that path straight
into Wine's dispatcher **with exactly the behaviour Wine's sigsys_handler gives today**
(same registers, same resume point). In-process, fail-closed, POPF/PUSHF/IRET + syscall
only. `tests/rosetta_nt/R17_PERFORMANCE.md` has the cost measurements.

## Every fact the design rests on (all measured/disassembled)

### The emitted code around a direct `syscall`
- Per-site emission (byte-stable across pages/processes): `mov w0,<eax>`; then
  **`movz/movk x28,<post-syscall x86 RIP>`** (movz lsl#32 + movk lsl#16 + movk, Rd=28
  fixed); then `bl <page+0x200>` (the strict syscall thunk entry). Verified on 3 sites.
- **x28 = RIP_sig at the trap.** The entry (page+0x200) saves x0-x15 to ctx+0x10..0x8f,
  NZCV→ctx+0x90, x17→ctx+0x98, LR→ctx+0x8, tags x18 bit63, branches to the class
  dispatch at page+0xb10. Nothing on entry→dispatch→trap touches x28.
- Class dispatch (page+0xb10): `and w23,w0,#0x07000000; lsr #24`; 1→Mach (limit 0x7f),
  2→BSD (0x218), 3→machdep (6) — anything else (class 0 and 7) → the single trap.
- **The trap** (page+0xeb8): `brk #0x5; b page+0x400`. `brk` always faults; the `b` is
  the resume-if-the-server-skips path. The only brk #5 in the dumps.
- Emit-time signature for the matcher (from sys-8 dump 0x102e85ea4-0x102e85ebc):
  `mov x0,x7` (aa0703e0); `mov x3,#2` (52800043); `movz x16,#0x8000,lsl#16` (d2a00010);
  `svc #0x80` (d4000002); `b <epilogue>` (0x14......); `brk #5` (d42000a0); `b <back>`.
- The lenient entry (class 7 → Rosetta private table 0..9, used by libc's
  syscall(0x07000009) region registration) is a DIFFERENT entry; the strict one is what
  plain x86 `syscall` sites get.

### The x18 thread context (ctx) — layout we rely on
- ctx+0x10..0x8f: the 16 shadow GPR slots (x86 rax..r15 ↔ arm64 x0..x15 by number:
  RCX=x1→+0x18, R11=x11→+0x60, RSP=x4→+0x30)
- ctx+0x8: arm64 return address; ctx+0x90 NZCV parked; ctx+0x98 x17 (flag shadow) parked
- ctx+0x158: table pointer? (veneer used it once); **ctx+0x0 → the runtime `__const`
  table** (r10a); table+0x0 → runtime+0x5964, table+0x20 → veneer +0x4f88,
  table+0x28 → iret handler +0x13848, table+0x98 → called with (x1=flag, x2=x23)
- ctx+0x188: **self-pointer** ([ctx+0x188] == ctx — makes 0x13848's "PT" reads
  ([..+0x194]/+0x220/+0x444) the same fields the JIT reads off x18 directly)
- ctx+0x194 CS mode word; ctx+0x198 continuation ({entry,mask}); ctx+0x1c8 pending-flags
  word; ctx+0x1d0 recorded arm64 pc; ctx+0x208 in-runtime-call byte;
  ctx+0x220 CS class byte; ctx+0x444 TF byte
- x18 is tagged (bit63) inside the thunk; every exit untags before ret.

### The resume mechanism (how translated code re-enters at an arbitrary x86 RIP)
The thunk's own recovery path does it — page+0x400 block ends `mov x22,x0; mov x28,x1`
(runtime's x1 = next x86 RIP), branches to the page epilogue at page+0x2560:

```
ldr w27,[x18,#0x1c8]; cbz → plain restore+ret      ; nothing pending
tbz w27,#9 else ldr x21,[x18,#0x1a8]
adr x29,#0; str x29,[x18,#0x1d0]                   ; record pc
mov w29,#1; strb w29,[x18,#0x208]                  ; in-runtime byte
mov x1,x30; mov x2,x28                             ; (arm64 lr, x86 RIP)
ldr x29,[x18]; ldr x29,[x29]                       ; table[0] = runtime+0x5964
mov x0,x18; blr x29                                ; CALL table[0](ctx, lr, rip)
strb wzr,[x18,#0x208]
ldr x29,[x18,#0x198]; ldp x19,x29,[x29]            ; continuation {entry,width}
ldr w27,[x18,#0x1c8]; orr w27,w27,w1; tbz #1 → restore
0x102e855dc: ldp x29,x17,[x18,#0x90]; msr NZCV,x29 ; restore parked flags
  ldp x14,x15..ldp x0,x1 (all GPR slots); ldr x30,[x18,#0x8]; and x18,x18,#~bit63; ret
```

`runtime+0x5964` (table[0]): `w=0x5908(ctx,rip)`; if w bit9: read ctx+0x194/0x220/0x444,
`x0=0x59c4(TFbyte, CSmode, CSclass, rip)`; **`str x0,[ctx+0x8]`** — installs the entry —
return (entry, w). 0x59c4 → TF clear: **0x6644 = pure CRC32 hash TB-cache lookup**
(hit → entry; no locks); TF set / other: 0x8ce4 = lock + translate path. So the epilogue's
`ldr x30,[ctx+0x8]; ret` resumes at the TB for the requested x86 RIP. The stub replicates
this exactly — the same call the thunk's own return makes.

### Wine parity (what sigsys_handler does — the spec we copy)
```
frame = TEB+0x378 (thread_data->syscall_frame)
frame->rcx(0x10)   = RIP_sig                 ; = post-syscall RIP = x28
frame->rip(0x70)   = RIP_sig + 0xb
frame->eflags(0x80)= EFL_sig                 ; = compose(parked NZCV, parked x17)
frame->restore_flags(0xb4) = (EFL_sig & 0x100) ? CONTEXT_CONTROL(1) : 0
RCX_sig = frame; R11_sig = EFL_sig           ; context edits
RIP_sig = __wine_syscall_dispatcher_prolog_end_ptr
```
prolog_end (not the dispatcher head) because the gs_load prolog pops the return address
off the live x86 stack, which is wrong for an injected entry. NT/flags parity holds
because compose is the same function the exception server would use (we own the
converter replacement already). `__wine_syscall_dispatcher_prolog_end_ptr` is a real
global pointer (signal_x86_64.c:3819).

### Publishing the target (Wine patch)
`signal_start_thread` already maps 0x7ffe1000 (`user_shared_data + page_size`) RW and
stores `__wine_syscall_dispatcher` there ("sneak in a syscall dispatcher pointer");
`set_process_instrumentation_callback` swaps slot 0. **Patch: also store
`__wine_syscall_dispatcher_prolog_end_ptr` at 0x7ffe1008** (same place, +8). Both slots
are re-stored per thread (each start remaps the page — upstream behaviour, unchanged).
Under Rosetta x86 VA == arm64 VA, so the payload reads 0x7ffe1008 natively.

### GS / TEB
The gs-resolver veneer is `mrs x29,TPIDRRO_EL0; and x29,x29,#~7; ret`. **Two
cases, both handled (r21d):** with gsbase switching, TPIDRRO_EL0 IS the TEB
and the single-hop check `[TPIDRRO+0x30]==TPIDRRO` holds. In OUR exec-region
setup (`mac_gsbase_switching=0`) TPIDRRO stays the native pthread TEB, and
Wine mirrors the Windows-TEB pointer at pthread_teb+0x30 — so the stub
resolves two-hop: candidate = `[TPIDRRO+0x30]`, validated (nonzero,
canonical, page-aligned, `[candidate+0x30]==candidate`) BEFORE it is
trusted; any failure bails. Measured: pthread 0x7ff2063ce0e0 → TEB
0x7ffc0000 (= `%gs:0x30`, confirmed from the Windows side).

## The implementation

### Emit-time insert (the matcher, payload code, r8 append hook)
**r21 v6 geometry (`insert8`) — see NOTES.md "r21 (2026-11-08)" for the full
mechanism and evidence.** The design below (r19) is history: the emitted
chain's shape was assumed, and the real blocker was a Rosetta fixup pass that
patches whatever word sits at stream offset X (the pre-insert cursor) with a
26-bit immediate, top-6 bits preserved — any live word there becomes an
undefined encoding and executing it is the translator assert. v6 never
executes X:

```
X-4   b #+8      14000002   overwrites the last site word (verified first)
X+0   bl #0      94000000   SACRIFICIAL — absorbs the fixup patch, becomes
                             the site's real relocated bl; bail target
X+4   <site word replay>     restores the x28 carrier / guest rax
X+8   movz/movk x26 ×4       stub address, no pc-relative words anywhere
X+24  blr x26    d63f0340    divert; lr = X+28
X+28  b #-28     17fffff9    bail: back to X (the real relocated bl)
```

The anchor is the `movz x28,#,lsl#32` carrier alone (file-backed sites have
no ldrb/tst/b.eq lead-in); the last site-family word is remembered
(`sysc_last`) so insert8 can verify the word at X-4 before touching it —
mismatch = no insert = stock (this is what declines real-class syscall
sites, whose stream shape differs). Bail = `br x30` → the b at X+28 → X:
byte-identical stock slowpath, no brk#5, no exception server.

### The stub (payload, naked arm64)
Entry state: x18 = ctx (untagged — the site path never tagged it), x0 =
syscall number (guest rax), x28 = post-syscall RIP (the carrier), x30 =
X+28 (the bail b). AAPCS64 keeps x28 callee-saved in our C helpers.

1. **Guards (all side-effect-free; any failure → `br x27`):**
   - g_ctl present (payload armed); ctl[35] (published prolog_end) present, canonical
     user VA (bits 63..47 clear) — the driver-side `refresh_fastpath_target` polls
     0x7ffe1008 until Wine maps it
   - `(w0 & 0x07000000) == 0 || == 0x07000000` (classes 0/7 only; 1/2/3 → stock trap)
   - TEB = TPIDRRO_EL0 & ~7; TEB != 0; [TEB+0x30] == TEB
   - frame = [TEB+0x378]; frame != 0
   - **[TEB+0x330] (instrumentation_callback) == 0** — if set, bail (stock handler
     runs; it would OR 0x10000 into restore_flags, which the stub does not model)
2. **Frame writes** (idempotent — the stock handler would write the same values, so a
   later bail is still correct): rcx=x28, rip=x28+0xb, eflags=compose(ctx+0x90,
   ctx+0x98) via `bl _ntfix_compose` (compiler-assembled = verified; the same function
   the exception server's replaced converter uses, so flag parity — NT included — holds
   by construction), restore_flags = CONTEXT_CONTROL(1) if TF else 0, and when TF:
   clear bit 4 (0x100) of the parked shadow at ctx+0x98 (sigsys_handler's
   `EFL_sig &= ~TF`, so the eventual restore does not re-trap).
3. **Context edits:** ctx+0x18 (RCX) = frame; ctx+0x68 (R11) = eflags  (**+0x68**, not
   +0x60 — arm64 x11 slot = 0x10 + 11*8; caught when re-deriving against the C slot map).
4. **Dispatch, replicating the epilogue:** record pc→ctx+0x1d0, set ctx+0x208=1,
   **queue the continuation: 32-bit `str w(0x200),[ctx+0x1c8]` after checking the
   32-bit word is 0 (anything pending → bail; ctx+0x1cc is a SEPARATE payload
   field — never access 64-bit)**; `ldr x29,[x18]; ldr x29,[x29]; x0=x18; x1=lr;
   x2=target; blr x29`; clear 0x208; **guard: ctx+0x8 changed** vs pre-call —
   unchanged means no entry installed → bail (`br x30`). The pending word is
   REQUIRED: table[0]'s 0x5908 is a `swp` that consumes it and returns the old
   value, and 0x5964 installs only under its bit9.
5. **Restore and enter:** `ldp x29,x17,[x9,#0x90]; msr NZCV,x29`; restore all GPR slots;
   `ldr x30,[x9,#0x8]; and x18,x18,#~bit63; ret`.

Bail is safe at ANY point before the `ret`: frame writes are idempotent, the context
register edits are overwritten by the stock sigsys handler, x28 survives our C helpers
(AAPCS), and the untouched `brk #5` still raises the real exception.

### What is deliberately NOT fast-pathed
- Classes 1/2/3 (real macOS syscalls — Wine's own libc): never diverted.
- The lenient class-7 private path (region registration) uses a different entry anyway.
- Instrumentation-callback set (frame->restore_flags |= 0x10000 in Wine): **fail-closed,
  not a gap** — the stub reads [TEB+0x330] (amd64_thread_data.instrumentation_callback,
  pinned by C_ASSERT) and bails to the stock trap if non-null. In-game it never is set.

### Counters (hook log `sysc` line)
Log line: `sysc at exit: thunks T badwrite B nearmiss N hits H disarm D target P entered E bail5 L`.
`thunks/badwrite/nearmiss` are the quarantined r20 scanner's driver counts (0 while it
is off). The payload ctl fields behind the rest: [32]=inserts (matcher, kind-6 events
in the `events` line mirror them), [33]=disarm request, [34]=chain misses,
[35]=published prolog_end target (0 without Wine — every entry then bails),
[36]=engaged hits (dispatcher entry installed), [37]=stub entries (incremented at
stub entry before any guard), [39]=bail counter (macfast does not bump it — a clean
bail is just `br x30`). So: E proves the divert chain executed; with P=0, H=0 and
exact site markers prove the bail path is byte-clean; with Wine running, H>0 is the
first proof the dispatcher was actually entered.

### Why CW Hack 24265 (mxcsr) needs no analogue
That hack makes Wine's signal-return path force the restored MXCSR to the live MXCSR
because Rosetta loses guest XMM control state across signals. The fast path never
captures and never restores any register state — the guest's XMM/MXCSR live in the
translated code and the stub does not touch them — so it gets the hack's outcome by
construction.

### Verification plan and status
1. clang+otool every hand-built word (standing rule). **Done** — v6 insert verified
   against the compiled payload (the sacrificial bl compiles to `mov w11,#-0x6c000000`
   = 0x94000000, the blr/b pair merges into one 64-bit constant store; insert8 block
   and macfast entry byte-verified inside hooklib.dylib before every test).
2. host_payload_test 5/5 PASS; mkpayload gates pass.
3. ntstub: HOOKLIB mode (the shipping path) passes 3/3; standalone lab harness hangs
   pre-r19 too (NOTES.md) — exonerated.
4. NOHOOK control: stock behaviour unchanged. **Done**.
5. **Carrier (r21, 2026-11-08): DONE.** capsite ×8 — exit 0 every run, 12/12 marker
   lines exact per run, `entered 11` (stub entries), 5 inserts, 5 misses (real-class
   sites correctly declined), zero translator asserts. capsite3 scan-0: 36/36 chain
   copies intact in the arenas, each with a valid relocated bl at X (the sacrificial
   word absorbed the fixup patch; two copies target page+0x200, one +0x204).
6. In-Wine: **DONE 2026-11-08 (r21d) — the fast path ENGAGES and resumes:
   `entered 220043 hits 220038 bail5 0`, rax 0xC000001C every call, rdi
   markers intact, 33 ns/call** (stock 16-19 µs, Proton ~2 µs). See NOTES.md
   "r21d" for the ais4→ais12 arc. The three fixes, all inside this design:
   - **Guard 5b, two-hop TEB:** TPIDRRO is the native pthread TEB in our
     exec-region setup (mac_gsbase_switching=0); [pthread+0x30] = the
     Windows TEB (validated: nonzero, canonical, page-aligned,
     self-pointing before trust).
   - **Guard 7, two-level deref:** [TEB+0x330] holds &instrumentation_callback
     (never null under Wine, signal_x86_64.c:3212); the callback is
     `*[TEB+0x330]` (:1943-1946) — bail only when non-null.
   - **Resume precondition (the big one):** table[0]'s install path is gated
     on bit9 of the 32-bit pending word at ctx+0x1c8 — 0x5908 is
     `swp wzr,w,[ctx+0x1c8]` and returns the OLD value; the epilogue's
     callers always have bit9 queued. The stub queues it itself:
     `ldr w13,[x18,#0x1c8]; cbz; str w13(0x200),[x18,#0x1c8]` — 32-bit
     only, ctx+0x1cc is a separate live payload field (64-bit access
     false-busies on read and zeroes state on write).
   - History (r21b/r21c): the "second emitter" affects ANON mappings only;
     PE-image sites execute their chains every call, hot loops included;
     ais3's rdx=0 was stock restore_flags=0 behaviour, not ours.
7. Crab Champions canary + THE FINALS after the in-Wine run passes.
   **Next: runtime macfinals-41 (add_nt_hook), canary, then THE FINALS with
   the R17 PERF setup.**
