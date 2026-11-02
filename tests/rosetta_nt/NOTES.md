# Rosetta NT-flag correction — working notes (2026-10-30/31 session)

Goal (CLAUDE.md): make the four NT (nested-task, EFLAGS bit 14) checks behave on
MacFinals exactly as on Proton (the `ssh ubuntu` box) and Windows. The conformance
gate is `tests/windows_probe/macfinals_probe.c` (three-way run); the fast inner
loop is `nt_native.c` in this directory (same four checks, plain x86-64 macOS
binary, no Wine).

## Baseline

macOS 27.0 (26A428), Apple Silicon. All four checks FAIL under Rosetta with no
Wine involved (`nt_native`): POPF→PUSHF NT=0, signal context NT=0, IRETQ+NT does
not fault, context-injected NT does not survive resume.

## Rosetta anatomy (this build)

- Translator: `/Library/Apple/usr/libexec/oah/libRosettaRuntime`, arm64 Mach-O,
  **UUID A1DA2B2A-3D68-33CB-A631-FA772DCDD167** (fail-closed fingerprint for any
  hook). Mapped per translated process, file-backed, `r-x/rwx` (max RWX).
- Loader/runtime: `/usr/libexec/rosetta/runtime`, UUID E933BC84-AE96-325A-A30E-90380DD274EB.
  Its assert strings name: ThreadContextRegisterState.cpp, ThreadContextMachRegisterState.cpp,
  ThreadContextSignals.cpp, MachExceptions.cpp, TranslationCacheAot.cpp, TranslationCacheJit.cpp…
  — the x86-context compose/decompose (checks 2 and 4) lives here.
- AOT cache: `/private/var/db/oah/<hash>/<hash>/<name>.aot`, SIP-protected from
  outside (listing denied), but mapped readable into the process. Contains the
  translations of dyld-registered images **plus embedded copies of runtime thunks**.
- JIT region: 128 MB anonymous `rw-/rwx` ("Rosetta JIT" in vmmap). Lazy
  per-function translations of anything not AOT-cached — **this is where
  Wine-loaded PE modules (game, anti-cheat) get translated**, on first execution.
- Per-thread context: x18 points at it inside translated code. Layout observed:
  x86 GPRs at +0x10..0x8f, NZCV at +0x90, **x17 flags shadow at +0x98**, x30 at +0x8.
  CS at [x18+0x176], SS at [x18+0x178] (16-bit loads).

## The flags model (decoded from actual translations; fixed regs: w22 compose,
## w23/w24 temporaries, x17 shadow, x4 = x86 RSP mirror)

- PUSHF emits inline: `mov w22,#0x202` (bit1 + IF bit9) + gather:
  SF/ZF/OF from cset mi/eq/vs, CF from `cset lo` (x86 CF = !ARM C), **AF from
  NZCV bit 27, PF from NZCV bit 26**, then shadow reads `lsr w23,w17,#n; bfi w22,…`
  for DF(bit10)←x17[1], AC(bit18)←x17[2], ID(bit21)←x17[3], TF(bit8)←x17[4].
- POPF emits inline: decompose into NZCV (`lsr w24,w22,#7→31`, `#6→30`, `#11→28`,
  CF via `mvn w24,w22; bfi #29` — inverted again, `#4→27` AF, `#2→26` PF),
  `msr NZCV,x23`, then shadow writes `lsr w24,w22,#n; bfi x17,x24,#k` for the
  same four pairs. **x86 bit 14 (NT) is extracted nowhere and inserted nowhere.**
  A TF change calls a runtime helper (site: `eor w25,…; and #0x10; cbz; …; bl <helper>`).
- IRETQ emits inline frame pops then `bl` to an AOT-embedded thunk (e.g.
  0x1045e9034 this run) that saves all regs into the x18 context and branches
  into the runtime. The thunk receives the frame's FLAGS in a register (x26 this run).
- Fault convention (from `cli`): pointer to the **original x86 instruction bytes**
  in a register (x22 this run), fault code `x23=#0x91`, then `brk #0x3` raises the
  x86 exception (#GP). The "descriptor" area IS the x86 code, not a struct.
- Translator sources (assert strings): `TranslatorFlags.cpp` (generate_z_flag…),
  `TranslatorFlagsSaveRestore.cpp` contains **translate_popf / translate_pushf**
  (assert sites at file-offsets 0x2a7e8 / 0x2abbc in THIS build; disasm at
  /tmp/rosetta-re/text.asm). translate_popf builds the four shadow extracts as
  helper calls (`bl 0x1e9c`, src bit in w4: 0x12=AC, 0x15=ID, 0x8=TF, DF=0xa)
  — a fifth call with w4=0xe (bit 14, NT) is the natural emitter extension,
  symmetric in translate_pushf, so both directions stay consistent regardless
  of which physical x17 bit the fifth slot lands on.

## ⛔ THE CRASH — never repeat

`mprotect(PROT_READ|WRITE|EXEC)` on libRosettaRuntime's file-backed TEXT page,
called from inside a translated process, **wedges the calling thread unkillably
in the kernel**; on 2026-10-31 ~00:35 the whole machine froze and needed a forced
power-off (unclean restart, log cut mid-activity at 00:35:22, no panic file —
hard freeze, not a recorded panic). Signing the test binary with
get-task-allow + allow-unsigned-executable-memory + disable-library-validation
did NOT change the hang. Conclusion: **no writes to file-backed code mappings,
ever** — not libRosettaRuntime, not the .aot pages, not `runtime`. The warning
comment and the removed test live in jitdump.c history.

Consequence: the planned "patch the emitters in libRosettaRuntime TEXT" route is
dead on this OS build. Developer Mode / dev-tools authorization is NOT the
blocker — the failure is a kernel deadlock before any policy verdict.

## Remaining safe surfaces

Anonymous RWX memory only — the same class Rosetta itself writes:

1. **JIT-region fixups**: the JIT region is `rw-` current — plain stores, no
   mprotect. Rewrite the canonical PUSHF/POPF sequences (they are byte-stable,
   fixed-register patterns — see above) with a branch to a stub that redoes the
   replaced instruction plus the NT bit (one extra `bfxil x17,x22,#14,#1` on
   POPF, `bfi w22,w17,#14,#1` on PUSHF; single 4-byte atomic instruction
   replacement, safe against concurrent execution). IRETQ: branch the frame-flag
   path through a stub that tests bit 14 and, if set, reproduces the fault
   convention (x86-instruction pointer + `brk #3`) — the stub must point at the
   iretq's own x86 bytes, which the emitter normally supplies; deriving them at
   fixup time needs one more look.
   Open problem: JIT translations appear lazily at first execution, after a
   constructor-time scan. A watcher thread loses that race for the very first
   execution of the check code. Mitigations to evaluate: suspend-all-threads
   scan points, scanning on a very short timer during the launch window, or
   finding a translation-completion signal that is observable without touching
   file-backed code.
2. Checks 2 and 4 (context compose/decompose) run in `/usr/libexec/rosetta/runtime`
   — file-backed again, so patching them is off the table the same way. Needs a
   rethink: possibly nothing needed there if the context path sources its rflags
   from the same NZCV+x17 the translated code maintains (verify by tracing the
   signal path against the thread-context layout above).

## S1 survey (2026-10-31) — data-only dispatch: DOES NOT EXIST

ROSETTA_NT_APPROACH.md §5.4-S1 asked whether any flag/translation path is
reached through a function pointer in *writable* data, so the correction could
be a plain data write. Answered by static disassembly of the on-disk binaries
(safe, see §7 there):

- **libRosettaRuntime (A1DA2B2A…)**: **zero `blr` in the entire `__text`**
  (0x4e6f4 bytes, 5938 direct `bl`). All 23 `br` sites are switch jump tables
  (`ldrh`-offset tables in read-only `__TEXT __const` — this includes the x86
  opcode→emitter dispatch itself) **except three** at 0x1870/0x1880/0x1890:
  `adrp 0x6a000; ldr x9,[__data+0/0x10/0x20]; br x9` — the compiler-rt
  udiv/mod interpose slots (`__data` is 0x30 bytes total, addr 0x6a588).
  Off-path: they dispatch 128-bit math helpers, nothing near flag emission.
- **runtime (E933BC84…)**: 2 `blr`, both call argument-passed callbacks, not
  table dispatch; 115 `br`.

So no write to writable data in either image can redirect translate_popf /
translate_pushf / the compose path. S1 is closed; the doc's "honest headline"
hardens to: **S3 (in-process COW code patch) is the only known-complete
mechanism, and it is quarantined** (disposable host + Rasmus's decision).

Related: `DISABLE_AOT` is a documented Rosetta env toggle (CLAUDE.md), so
"AOT off" needs no data write at all — the launcher can set it at spawn, which
also answers §5.7's "is our init late?" question (env is read before anything
translates). Probably unnecessary for the game anyway: Wine manually maps the
PEs, and manually-mapped images are JIT-translated on first execution (after
any install window), not AOT-cached. Confirm with a probe before relying on it.

## Experiment ledger — what works and what freezes (the guideline)

Every experiment on Rosetta pages, newest last. Rule in force: in-memory only,
never a boot-affecting file, in-place W+X on shared file-backed pages forbidden.

| date/time | experiment | outcome |
|---|---|---|
| 2026-10-31 ~00:35 | in-process `mprotect(PROT_RWX)` + write on libRosettaRuntime TEXT (**shared** file-backed page, **no COW**) | **MACHINE FREEZE.** Thread wedged unkillably in the kernel (twice before as a process-hang: unsigned, and get-task-allow signed), forced power-off, log cut 00:35:22, no panic file. This exact path stays forbidden. |
| 2026-10-31 01:13 | cowtest v1 tier 1 (control!) — COW cycle on own `__LINKEDIT`, but with a **16K** window | **SIGBUS, machine fine** (process death only). The window, masked down 16K, overran `__LINKEDIT` into the **adjacent Rosetta JIT region** and stripped execute from translated code → `KERN_PROTECTION_FAILURE` on instruction fetch at the caller's next translated instruction. Lessons: translated processes use **4K pages**; the JIT region maps **immediately after** the main executable; windows must be **region-clamped**; a de-permissioned JIT page is a clean SIGBUS, never a freeze. The `mach_vm_protect(RW|VM_PROT_COPY)` call itself completed. |
| 2026-10-31 01:14 | cowtest v2 tier 1: region-clamped 4K COW cycle, own `__LINKEDIT` r-- page, same-byte write | **PASS** (kr=0 both protects, read-back match) |
| 2026-10-31 01:15 | tier 2: own binary via private `PROT_EXEC MAP_PRIVATE` file mapping | **PASS** |
| 2026-10-31 01:15 | tier 3: libRosettaRuntime `__TEXT` **data** page (+0x60000, `__const`/`__cstring` range) | **PASS** — first in-process protection change on a Rosetta page since the freeze; no wedge. Note: this sub-region's *current* protection is RWX (7). |
| 2026-10-31 05:09 | tier 4: libRosettaRuntime `__TEXT` **code** page (+0x4e000 — `runtime_entry` + compiler-rt tail), same-byte write | **PASS** — real code page COW'd, written, flushed, restored; no wedge, machine healthy. |
| 2026-10-31 05:10 | tier 5: Rosetta data page, **different** byte written, 5 s alive-spin, restore | **PASS** — no asynchronous code-signing kill while a modified byte lived on the Rosetta image; restore clean. |
| 2026-10-31 ~08:00 | ntstub r2: emitter-entry `b` patches + counter trampolines on a plain anon RWX page, fresh-page trigger | **HANG** (killable mach wait, PC parked at trampoline first word; exceptionserver parked; dies on SIGTERM). Machine fine. |
| 2026-10-31 ~08:30 | ntstub r3: MODE=0 *pure* trampoline (no clobber, no write, no indirect branch) on the plain anon page | **STILL HANGS** → fetching instructions from a plain (unflagged) anon page during live translation is what wedges; register clobbers/BTI exonerated. The COW'd Rosetta page itself executed fine (the patched entry `b` demonstrably ran). |
| 2026-10-31 ~09:02 | ntstub r4: inline trampolines in the emitters' dead cold assert blocks on the COW'd page | **SIGSEGV before install — OUR BUG, Rosetta exonerated.** Byte offsets (0x2c/0x3d4/0x400) were used to index a `uint32_t*` span: wrote up to 0xB80 bytes past a 0x480 stack array; smashed stack dereferenced a half-clobbered pointer (fault 0x3_17fffff5 = our branch word) inside the next `getenv`. |
| 2026-10-31 ~11:30 | ntstub r5a: 16-byte abs-jump (`ldr x9,#8; br x9; .quad`) entries + counter stubs on a `MAP_TRANSLATED_ALLOW_EXECUTE` (0x2000) anon page | **Rosetta exception-server assert, exit 132** — our pool-skip `b` was off by one word (+2 instead of +3), execution fell into the literal; `arm_pc` = stub+0xC, `x86_rip=0xffff...` (unmappable). Machine fine. The patched entry demonstrably executed — the hook path was live. |
| 2026-10-31 ~11:45 | ntstub r5b: same with the branch fixed | **PASS — EMITTERS LIVE.** Install/readback/restore all verified; fresh-page trigger drove the pushf-gather emitter 0→3 hits; control run after restore: 0 new hits. **First clean patched-code-page execution through the live translator.** |
| 2026-10-31 ~12:30 | ntstub r6: `translate_insn` @ **0x19d9c** counter-hook (3rd entry) | **PASS.** ti 0→8 hits over the 9-instruction trigger, x0 captured = ThreadContext (0x…50ad0). Buffer object is **not** at ctx+0x10 as a field. One anchor-gate save: a mis-transcribed PRO_TI word (0xa95f… vs 0xa9095ff8) was caught fail-closed by our own gate — the gate works. |
| 2026-10-31 ~13:00 | ntstub r7: append-ring hook at **0x18a0** (every emitted word passes through it as w1, bufobj in x0 = {base,cursor,limit}) | **crash → PASS.** First run: exception-server assert exit 132 during the first pushfq emission (arm_pc=stub, x86_rip=fresh page) — **two hand-assembled words were wrong**: `str w1,[x12,x10]` must be `0xb82a6981` (mine `0xb9a06181` decoded as a *load* into w1, clobbering the emitted word with ring garbage) and `add x10,x10,#4` must be `0x9100114a` (mine encoded `#85`). After fix + adding PRO_APPEND to the anchor gate: **PASS** — 146 emitted words recorded, clean exit 0, control frozen. **Standing rule: every hand-derived arm64 word gets verified against `clang -arch arm64` + otool before use.** r5a and r7 were both encoding slips. |
| 2026-10-31 ~14:00 | ntstub r8: the NT fixups, live — freestanding arm64 C payload (payload_arm64.c → payload.dylib → payload_blob.h) flat-copied into a flagged region (TEXT RX / DATA RW), append stub (appendstub.S, assembler-built) calling `ntfix_append(bufobj, word, ctl, ring)` before the original helper; payload inserts the x17-bit-5 fixups into the emitted stream (see milestone-4) | **one assert → PASS — CHECK 1 PASSES: NT=1.** First run: exception-server assert exit 132, arm_pc = the stub's `pcall` literal, faulting word = the pointer's low half — **`adr` yields the literal's address, not its value**: the `blr` called the literal itself. Adding the missing `ldr x9,[x10]` before `blr`/`br` fixed it (r5b worked because it used `ldr x9,#8` literal *loads*). After fix: `fresh check: NT=1`, 3 gather + 2 scatter fixups, 0 skipped, control NT=0 with counters frozen after restore, exit 0. **Triple-verified**: behavioral (NT round-trip), event log (5 events at exactly the r7 anchor ring-indices 22/40/79/104/122), and physical (the executed 156-word TB at the bufobj base contains the exact predicted words: `53057e37 331202f6 | f81f8c96` before each push, `530e7ed8 b37b0311 | d51b4217` before each msr — /tmp/rosetta-re/tb-fixups.bin). |
| 2026-10-31 ~15:00 | ntstub r9: checks 2/4 — int3 with NT live, SIGTRAP handler reads `uc_mcontext->__ss.__rflags` and injects 0x4000 for the resume; then the real fix: **the context converters in /usr/libexec/rosetta/runtime** (UUID E933BC84…74EB, mapped in-process at 0x7ff7…, +0x30000 __TEXT) replaced whole — `ntfix_compose` @0x198b8 and `ntfix_decompose` @0x1992c, differential-tested against the original machine code on the host first (host_payload_test.c step 4: originals copied into an anon RX page and called) | **PASS — CHECKS 2 AND 4 PASS.** Baseline first: even with r8 fixups live, ctx flags = `00000206` (NT lost — the converter is *native* code, the emitted gather never runs on the exception path) and injected NT died on resume. After the converter patches: `ctx flags 00004206 (NT=1)` and `post-resume NT=1`, both exception checks, both images restored, NOHOOK control clean. Two bugs the gates caught: (a) my decompose took source bits 9/16/18 instead of 10/18/21 — the original uses *position masks* (`(f>>9)&2` = f bit 10), differential test caught all three; (b) RUNTIME_UUID typo byte 14 `7e` for `74` — the walk visited the image and printed matching prefixes while memcmp failed; a first-diff-index print found it. Lesson: print BOTH sides in full when a comparison fails, and never eyeball a 16-byte constant against a dash-form string. |
| 2026-11-01 ~00:30 | ntstub r10 (a: ctx/thunk-chain survey, b: the insert): **check 3** — a 7-stage matcher in the r8 payload recognises the iretq emission (arm on the CS `ldrh w?,[x18,#0x176]`; then CS push `str [x4,#-8]!`, desc(target) built into x1, RIP push, `ldp [x4],#0x10`, `ldp [x4]`, thunk `bl`) and inserts 4 assembler-verified words before the `bl`: `tbz x17,#5,+16; sub x22,x1,#2; movz x23,#0x91; brk #0x3` — NT clear falls through to the bl untouched, NT set raises #GP with desc(iretq) = desc(target) − delta as the x86 RIP | **one raw kill → PASS — CHECK 3 PASSES: 4/4.** SIGSEGV count=1, rip = **exactly the iretq byte** (delta 0), delivered flags `0x4206` (NT=1, via the r9 compose), si_addr=0 (Windows #GP(0) carries no address either), 1 insert 0 misses, clean shim-resume, both images restored, NOHOOK control: no fault, no insert (fail-open). The raw kill was the resume experiment, not the insert: **Rosetta's sigreturn honours a ucontext `__rip` override but IGNORES `__rsp` edits** (measured stage 2), and the #GP arrives with rsp at the **intact frame top** (entry−0x28; slots RIP,CS=0x2b,RFLAGS,RSP,SS — the JIT's register pops never advance the delivered rsp, which is also Windows' shape: a faulting iretq pops nothing). So the resume must be rsp-neutral: jump to a shim that discards the frame itself (`add $0x28,%rsp; ret`). Also learned: desc(target) materialisation has **two shapes in the wild** — `adrp x1; add x1,x1,#imm` (jitdump process) and `movz/movk x1` absolute (ntstub process) — the chain accepts both. Encoding slips the gates caught: ldp-post const is `0xa8c10080` (bit 16 belongs to imm7, a hand mask dropped it), the adrp predicate must free immlo/immhi (`(w&0x9f00001f)==0x90000001`), and the movz/movk family mask is `0xff80001f` (`0xffe0001f` keeps hw and rejects `lsl#32`). |

**Conclusion:** the in-process COW sequence (`mach_vm_protect(RW|VM_PROT_COPY)` →
write → `sys_icache_invalidate` → restore) is **mechanically viable on this
kernel, from translated code, against Rosetta's own image**. The freeze is
specific to the non-COW in-place W+X path. Remaining unproven: executing
*through* a genuinely patched code page (the next milestone), and the anonymous
arm64 payload mapping. Tool: `cowtest.c`; step logs in
`/tmp/rosetta-re/cowtest.log` (fsynced per step; regenerates per boot).

Payload-page note: **resolved 2026-10-31 (r5b).** The flag is
`MAP_TRANSLATED_ALLOW_EXECUTE` = **0x2000** (not in this CLT SDK's
`vm_statistics.h`; value and mechanics from the MIT-licensed
`Lifeisawful/rosettax87_jit`, cleared by Rasmus for study 2026-10-31 — see
milestone-3 notes). An anon page mapped with it executes **native arm64**
inside the translated process (our counter stubs ran; counters incremented).
Plain anon RX/RWX pages wedge the translator (r2/r3): it treats an execution
attempt on them as x86 guest code to translate. Milestone-1's "no public
native-arm64 execution surface" conclusion is **falsified** — the surface
exists, it is just undocumented in this SDK.

## Milestone-1 survey (2026-10-31, exectest) — payload page & the real GPTK bridge

Recon before experimenting (all from our own dependencies, nothing from the
closed package): GPTK's **x86_64** `libd3dshared.dylib`
(`gptk/gptk/redist/lib/external`) exports
`register_non_native_code_region(start, len)` implemented as literally
`syscall(0x7000009, start, len)`, gated by `supports_non_native_code_regions()`
which checks the OID `sysctl.proc_translated` (NOT `kern.proc_translated` —
gone on this macOS) then `rosetta_get_runtime_version` from
`/usr/lib/libRosetta.dylib` (no file on disk, but **dyld-shared-cache resident —
dlopen by path succeeds**). Our own CrossOver-base Wine calls this interface for
every PE module it maps (`dlls/ntdll/unix/loader.c`, CW HACK 22434), and our
launcher sets `CX_APPLEGPTK_LIBD3DSHARED_PATH` — so the syscall is
production-exercised on this machine.

`exectest.c` results (one tier per process, `/tmp/rosetta-re/exectest.log`):

| date/time | experiment | outcome |
|---|---|---|
| 2026-10-31 | t1: anon RWX page + raw indirect call from translated code | **SIGILL (exit 132) exactly as predicted** — the translator treats an unregistered page as x86 guest code; our arm64 `ret` byte 0xd6 is #UD in 64-bit mode. Machine fine. |
| 2026-10-31 | t2: same after `syscall(0x7000009, start, end)` (returns 0) | **still SIGILL** — the syscall registers *translatable guest-code regions* (Wine's PE use), not native arm64 execution. Wrong mechanism class for a payload. |
| 2026-10-31 | t3: `dlopen` of a self-built arm64 dylib from the translated process | **hard refusal**: "incompatible architecture (have 'arm64', need 'x86_64')". No dyld mixed-loading on this macOS. GPTK runs *everything* translated (its x86_64 D3DMetal uses the x86_64 Metal slice) — the hypothesized dyld native bridge does not exist. |

**Milestone-1 conclusion:** there is *no public native-arm64 execution surface*
for a separate payload mapping — which dissolves the question rather than
answering it. The payload/stubs will live on **COW-privatized Rosetta code
pages themselves**: after `VM_PROT_COPY` the page is an anonymous private copy
that keeps its execute permission, and the patched call sites are Rosetta's own
native arm64 code branching natively — no translated dispatch is involved at
any point. (`exectest t1` says nothing against this: it tested translated
dispatch, not native fetch.) Fallback if native fetch from our own anon page is
ever needed: JIT-region space (Rosetta-owned RWX, findable at runtime).

## Milestone-2 design (2026-10-31) — call-site wrappers, emitter internals not needed

Static analysis of this build's `text.asm` (offsets are file offsets in
libRosettaRuntime, UUID-gated at install):

- `translate_popf` hot entry **0x2a7fc** (`stp x24,x23,[sp,#-0x40]!`); its cold
  assert block 0x2a7e0-0x2a7f8 ("unrecognized POPF opcode", line 0x150).
  Called from **0x1a0ec** (`mov x0,x19; mov x1,x22; bl; b 0x1ce3c`).
- `translate_pushf` hot entry **0x2abd0**; cold assert 0x2abb0-0x2abcc (line
  0x1ac). Called from **0x1b9a0**.
- Every emitter funnels raw instruction words through **0x18a0(x0=ctx+0x10,
  w1=encoded_word)** — the buffer object is `{base@+0x0, cursor@+0x8,
  limit@+0x10}` and 0x18a0 appends one 32-bit instruction with growth via
  0x30b4. This is the entire emission ABI our stubs need.
- Plan: patch the two **call-site `bl`s** (pages 0x1a000, 0x1b000) to branch to
  wrapper stubs; the originals run *untouched*; the wrappers then append NT
  instructions via 0x18a0 using the known emitted-code conventions (w22 flags
  temp, x17 shadow — spare bit 5, x4 RSP mirror):
  - popf fixup (runs after the decompose): `ubfx w23,w22,#14,#1; bfi x17,w23,#5,#1`
  - pushf fixup (runs after the push store): `ubfx w23,w17,#5,#1; ldr w24,[x4];
    bfi w24,w23,#14,#1; str w24,[x4]` — patches the just-pushed value.
- Stubs assembled from arm64 source (`stub.S`) with magic literals patched at
  install; all instruction encodings done by the assembler, not by hand.
- First target = nt_native check 1 only (popf→pushf round-trip). Checks 2/4
  (context compose, IRETQ #GP) are later, separate milestones.
- Tools run with `DISABLE_AOT=1`: the AOT cache would otherwise bypass live
  translation for previously-cached blocks.

Next milestones, in order:
1. ~~Payload page~~ — resolved 2026-10-31: no public anon-native-exec surface;
   payload lives on COW'd Rosetta code pages (see milestone-1 survey).
2. **First live stub** (`ntstub.c`): verify-then-install wrappers at 0x1a0ec /
   0x1b9a0, sibling threads suspended, fail-closed on UUID/anchor mismatch;
   trigger with a fresh popf/pushf round-trip; restore after. Target: check 1
   passes with the hook, fails without it.
3. Then: IRETQ #GP emission (check 4), context carry (check 2, Wine-side per
   approach doc §5.5), nt_native 4/4, three-way probe, Crab Champions canary,
   game last.

## Milestone-3 notes (2026-10-31) — r5 results, labels corrected, new plan

`Lifeisawful/rosettax87_jit` (MIT, cleared for study/comparison by Rasmus
2026-10-31; not blanket authorisation to paste) validated three things we had
guessed at, and its architecture is the model for the next round:

1. `MAP_TRANSLATED_ALLOW_EXECUTE` (0x2000) anon pages = the native-arm64
   scratch surface inside a translated process (their `hook.cpp`,
   ROSETTA_RUNTIME path: mmap RW with the flag, write, `vm_protect` to R|X,
   dcache+icache flush).
2. Hook mechanics: 16-byte abs jump (`ldr x9,#8; br x9; .quad target`) over a
   function entry, displaced prologue + jump back in the trampoline page,
   COW-write on the target via `vm_protect(RW|VM_PROT_COPY)`. No sibling
   freezing anywhere in their code.
3. Architecture: ONE hook at **`translate_insn`** (the per-instruction
   choke point), wrap the original, post-process the emitted output. Their
   offset-finder pattern-matches the prologue
   (`sub sp; stp x28…; …; mov x19,x0`) — on our build that prologue is at
   **0x19d9c** (and a twin at 0x38e5c, presumably the AOT path).
   Their other named offsets confirm our map: ctx+0x210 = free-GPR bitmask
   (init 0x3FC00000), rbit/clz temp allocator, jump-table dispatch,
   `decode_opcode` hookable too.

Corrections to the milestone-2 label map, from the r5b counter run
(3 pushfq + 2 popfq in the trigger):

- **0x2a7fc is the pushf *gather* emitter, not popf.** 3 hits = exactly the 3
  pushfq bytes. It contains the `movz #0x202` gather (0x2a86c) and the
  `movk #0xd53b` mrs-literal (0x2ac4). It is LIVE and hookable (proven).
- **0x2abd0 is neither pushf nor popf** — 0 hits with both byte kinds present.
- The **popf scatter** code is in the JIT dumps: the ubfx/bfi chain on the
  popped word terminating in `msr NZCV` (words
  `53077ed8 33010317 53067ed8 33020317 530b7ed8 33040317 2a3603f8 33030317
  53047ed8 33050317 53027ed8 33060317 d51b4217` + x17 side-bit stores) — but
  WHICH translator function emits it is still unidentified (bfi helper 0x1e9c
  has 200+ callers; the bfi clusters don't isolate it). The libc csrctl
  wrapper's `msr NZCV` sites in the dumps are noise, not popf.
- IF is modeled via **DAIF** (shared emitter at ~0x1e6dc emits
  `msr NZCV` + a `#0x3f` DAIF update) — relevant for popf fidelity later.

**Plan (r6+):** stop hunting the popf emitter statically. Hook
`translate_insn` @ 0x19d9c with a wrapper (r5 mechanics, displaced prologue in
a flagged page): count calls per fresh trigger page, and in the wrapper,
after calling the original, inspect the emitted buffer for the popf-scatter /
pushf-gather signatures. That gives runtime ground truth for where the NT bit
must be preserved (popf: store NT to a per-thread slot; pushf: OR it back
into bit 14 of the pushed word) before we write any fixup emission. Checks 2/4
(signal context, IRETQ #GP) remain later milestones.

## Milestone-4 (2026-10-31) — the live emission trace, and the fix that falls out of it

r7's append-ring hook (every emitted word passes through the helper at
**0x18a0** as `w1`, buffer object in `x0` = {base@+0, cursor@+8, limit@+0x10})
captured the trigger's complete emission: 146 words, disassembled from
`/tmp/rosetta-re/append-ring.bin` (`mkw.py` + `otool -tV`). The two `msr NZCV`
words sit at ring 40 and 122 = the two popf scatters. Ground truth on the
flags model, register-generic (w22/w23/w24 are dynamically allocated; masks
must clear Rd/Rn bits 4:5..9):

- **pushf gather** = `movz wD,#0x202` + cset/bfi chain from NZCV
  (SF→bfi#7, ZF→#6, CF→bfxil#0, OF→#11, AF from NZCV bit 27→#4, PF from
  bit 26→#2) + `lsr wT,x17,#k; bfi wD,wT,#m` pairs (k/m = 1/10 DF, 2/18 AC,
  3/21 ID, 4/8 TF) + `str xD,[x4,#-8]!` (push onto x86 RSP mirror x4).
  **Bit 14 is never built. IOPL isn't either.**
- **popf scatter** = `movz wT,#0` + `lsr wA,wS,#7; bfi wT,wA,#31` (SF→NZCV N),
  `#6→#30` (ZF), `#11→#28` (OF), `mvn wA,wS; bfi #29` (CF via a bit-29
  complement trick), `#4→#27` (AF), `#2→#26` (PF) → `msr NZCV,xT`, then
  `lsr/bfi` pairs into **x17** for DF/AC/ID/TF. **Bit 14 of the popped word is
  read by nothing — no `lsr #14` exists in the emitted stream.** NT does not
  exist in Rosetta's flag model; this is now confirmed by construction, in
  vivo, not by absence-of-observation.
- The trigger's own `or [rsp],0x4000` / `and [rsp],~0x4000` appear inline in
  the trace (ring 23–25, 105–107), so block boundaries are exactly locatable.
- Buffer object captured at append entry = ctx+0x10 **by address arithmetic**
  (allocator-adjacent), not a field at ctx+0x10.

**The r8 fix, falling out of the trace:** x17 is Rosetta's per-thread
"side bits" register and only bits 1–4 are used (the TF-change check masks
`#0x10`). Ride **x17 bit 5** as the NT slot:

- gather: insert `lsr wT,x17,#5; bfi wD,wT,#14,#1` immediately before the
  push `str xD,[x4,#-8]!` (insertion happens when that str is the word being
  appended — wT is dead after the #8 bfi, wD is final).
- scatter: insert `lsr wT,wS,#14; bfi x17,xT,#5,#1` immediately before
  `msr NZCV,xT` (wT — the lsr temp — is dead at msr; the msr's own operand
  register is untouched).

Insertion mechanics: the append hook runs BEFORE the original helper stores
the word, so writing two extra words at *cursor (bounds-checked against
limit, else skip+count fail-open) puts our fixup just ahead of the anchor
word in the emitted stream. Matching is a small state machine over the word
stream (skeleton = word with Rd/Rn cleared; anchors: movz#0x202 /
movz#0+lsr#7+bfi#31 head; msr NZCV / push-str as the insert triggers).

**Payload architecture (r8):** dyld refuses arm64 dylibs in a translated
process (exectest t3), so the state machine moves into a **freestanding C
payload** (`payload_arm64.c`, compiled `-arch arm64 -dynamiclib -nostdlib`,
no imports/strings/pointer-constants, PC-relative statics only) whose
segments ntstub **flat-copies into one MAP_TRANSLATED_ALLOW_EXECUTE region**,
layout-preserving so adrp offsets survive, text pages RX / data RW, icache
flush, entry offset from `nm` embedded and verified by first-instruction
readback. The append stub becomes: save x0–x18 (stx pairs), call the payload
(x0=bufobj, w1=word — the payload may clobber everything, the stub restores),
displaced 0x18a0 prologue, jump back. Host-side, the matcher compiles as x86
against the recorded ring and must fire exactly 2 scatter + 3 gather sites
with byte-exact fixup words before any arm64 deployment.

Why this matters for checks 2/4: if Rosetta's exception path rebuilds EFLAGS
through anything resembling the gather, an NT bit living in x17 rides it into
signal contexts for free — that is the first thing to verify after check 1
passes (r9).

**r8 result (2026-10-31 ~14:00): CHECK 1 PASSES.** `popfq` after
`or [rsp],0x4000` → `pushfq` reads bit 14 back — NT=1, live, with the
payload-built fixups; control after restore NT=0. Operational facts learned:

- dyld refuses arm64 dylibs in a translated process (exectest t3), but
  **flat-copying the dylib's segments into a flagged region works**: the
  payload is fixup/reloc-free by construction (mkpayload.py gates on
  `dyld_info -fixups` and `otool -r` at build time), so PC-relative statics
  survive the copy; __TEXT sealed RX, __DATA (pure bss) stays RW.
- Calling the payload needs **no translated→native call at all**: the
  assembler-built append stub loads x2/x3 (ctl, ring) from patched literals
  and the payload self-initialises on the first word. Direct x86→native
  calls remain untested and unnecessary.
- `adr` = address-of-literal; `ldr` through it = value. Two crashes
  (r5a, r8a) now come from stub-control-flow slips — hence appendstub.S is
  real assembly, embedded via mkstub.py, placeholders patched by value and
  fail-closed if absent.
- The executed TB is readable from the x86 side via the bufobj
  ({base, offset-cursor, limit}) allocator-adjacent to the translate ctx —
  cursor semantics are byte-offset-into-base (the helper emits
  `base[cursor]`), which is how the trigger TB (156 words = 146 + 10
  inserted) is dumped before later translation evicts it.

**r9 result (2026-10-31 ~15:00): CHECKS 2 AND 4 PASS.** The r8 emitted-stream
fixups do **not** reach the exception path: with only the append-hook live,
`int3` under NT delivered a context with flags `00000206` — bit 14 gone, and
NT ORed into the frame by the handler died on resume. The path runs through
native code, not translated code: **/usr/libexec/rosetta/runtime** (a second
image, UUID E933BC84-AE96-325A-A30E-90380DD274EB, mapped in-process at
0x7ff7…, +0x30000 __TEXT, no name in the region walk) owns the x86-context
compose/decompose. Two leaf functions there, fully decoded from the
disassembly and differential-executed on the host:

- **compose** @ 0x198b8 — parked representation (NZCV value + x17 shadow)
  → x86 EFLAGS word. SF via `cmn w0,#1; csel 0x202/0x282` (base 0x282 has
  bit 7 set when NZCV.N == 0… i.e. gt), ZF/OF/PF from NZCV 30/28/26, AF from
  parked bit 27, CF = !NZCV bit 29, DF/AC/ID/TF from shadow bits 1–4,
  **bit 14 produced by nothing**.
- **decompose** @ 0x1992c — the inverse: EFLAGS → NZCV (with CF inverted,
  AF/PF parked at 27/26) + shadow bits 1–4. **Shadow bit 5 never written.**

The fix mirrors r8's slot: x17 bit 5 *is* the NT bit, so both functions were
replaced whole with C in the payload (`ntfix_compose`/`ntfix_decompose`),
differential-tested on the host **against the original machine code** (copied
into an anon RX page and called — host_payload_test step 4) across 384
vectors: bit-identical whenever NT/shadow-5 are clear, exactly bit 14 / bit 5
added otherwise. Installed as 16-byte abs jumps at the two entries (same COW
sequence), anchor-verified against the 4-word prologues, restored and verified
at exit. With both hooks live: `ctx flags 00004206 (NT=1)` and
`post-resume NT=1`, on both the pre-dump and the post-exception retry-path
checks; NOHOOK control `00000206`, NT=0 throughout. Both images restored,
counters frozen, exit 0.

Facts that cost debugging cycles (recorded so they don't again):

- **The converters are position-mask shifts, not extract-then-shift.**
  Original decompose DF is `(f>>9)&0x2` — source is EFLAGS **bit 10** (mask
  lands it on shadow bit 1), not "bit 9". I wrote `((f>>9)&1)<<1` and got DF,
  AC and ID wrong in one stroke; the differential test caught all three
  before deployment. Never mirror disassembly by eye — execute the original.
- **A 16-byte UUID transcribed by hand will be wrong.** Byte 14 went in as
  `7e` (real: `74`) and the walk visited the image, printed matching
  prefixes, and SKIPped twice. When a comparison fails impossibly, print both
  sides in full plus a first-diff index, not a prefix.
- The append hook and the converters are **independent mechanisms that
  compose**: emitted-stream fixups for translated pushf/popf (check 1),
  converter replacements for native context traffic (checks 2/4). Fixing one
  does nothing for the other — measured, not assumed.
**r10 result (2026-11-01 ~00:30): CHECK 3 PASSES — 4/4.** r10a measured the
thunk chain cold (ctx [0] → runtime `__const` table; slot +0x20 → the iret
handler veneer runtime+0xf88; our flavor's tail calls it with x0=&saved-gprs,
x1=RFLAGS, x2=SS — see the r10a block in ntstub.c). r10b chose the cheaper
surface: don't patch the handler, patch the *call site* — the r8 append
machinery recognises the iretq emission and slips four words in front of the
thunk `bl`:

```
tbz  x17, #5, bl      ; NT clear -> business as usual (falls through to bl)
sub  x22, x1, #2      ; desc(iretq) = desc(target) - 2   (dead regs after)
movz x23, #0x91       ; Rosetta's own #GP convention (as seen on cli)
brk  #0x3             ; raise; exception server reports the x86 RIP
```

Measured on the live run: SIGSEGV count 1, **rip delta 0 from the iretq
byte**, flags `0x4206` (NT=1 — through the r9 compose), `si_addr = 0`
(a #GP(0) has no faulting address on hardware either), exactly 1 insert,
0 misses, resume via the blob's fault shim, both images restored, NOHOOK
control clean. Three findings that shape everything downstream:

- **sigreturn honours `__rip` but ignores `__rsp`.** A ucontext rsp edit is
  silently dropped (stage-2 measurement: resume rsp unchanged after `+= 8`).
  Any handler that needs a different rsp must land on code that fixes the
  stack itself. Wine's signal machinery sets rsp in places — worth auditing
  at integration time.
- **The #GP arrives with rsp at the intact frame top** (entry−0x28). The
  JIT's inline `ldp` pops feed registers, not the delivered rsp — which is
  also Windows' shape (a faulting iretq pops nothing). Good for truthfulness;
  the handler sees the full frame it expects.
- **desc(target) materialisation is address-form-dependent**: `adrp+add`
  when the desc area lands JIT-relative, `movz/movk` when absolute. The
  matcher accepts both (family masks `0xff80001f` — hw must be free).

The remaining unsolved piece is the **general-case delta** (see Remaining):
the trigger's `sub x22,x1,#2` is correct only because its return target sits
2 bytes after the iretq. Game iretq sites won't be that polite.

- Region enumeration order is unstable between walks (+68000 first visit vs
  +1000 later); the finder retries and re-inventories rather than caching an
  address.

Remaining for the game: **none of the four checks — 4/4 as of 2026-11-01**
(popf/pushf NT round-trip, exception context carries NT, iretq #GP at the
exact iretq byte, injected NT survives resume; NOHOOK control fail-open).
What still stands between this and THE FINALS, in order:

1. **nt_native 4/4** — rerun the 4-check harness (it predates r10; its check
   3 asserts `fault_code == SIGSEGV` only, ours is stricter: exact RIP).
2. **The general-case delta.** The insert's `sub x22,x1,#2` hard-codes
   delta = (lea target − iretq) = 2, true only when the return target
   immediately follows the iretq. The game's iretq sites (if any execute
   with NT set) have arbitrary targets. Rosetta materialises desc(target)
   in x1 — recovering the *iretq's own* descriptor for an arbitrary layout
   needs the desc-area affine map (desc(X)−desc(Y) = X−Y in x86 bytes) plus
   knowledge of the iretq's byte offset within its TB, or a different
   anchor choice. THIS IS THE ONE UNSOLVED PIECE of r10.
3. **The probe three-way** (MacFinals / Proton on `ssh ubuntu` / Windows)
   with `tests/windows_probe/macfinals_probe.c`.
4. **Crab Champions canary** (DX12 regression), then THE FINALS.
5. Integration into the game process via Wine — in-process, per the
   standing constraints; note the sigreturn finding matters there too: any
   Wine-side #GP handler that fixes rsp through the ucontext will silently
   not get that fix applied.

## r11 (2026-11-01) — integration: hooklib.dylib, hardened-runtime, runtime macfinals-6

**Payload hardening (all verified by host_payload_test steps 1–7, PASS):**
- `slot_for` adoption via `cas_u64` (ldaxr/stxr) and per-slot `last` cursor:
  a cursor regression (buffer reuse) marks the slot IDLE so a stale chain is
  never continued. `ctl_inc` makes all 12 counters atomic (ldaxr add stlxr);
  plain `g_ctl[n]++` lost increments across 4 threads (387/800 observed →
  800/800 exact after).
- Ring contract: the payload writes bounded by compiled RING_WORDS (0x20000),
  NOT the caller array — a short ring gets 9600 words stomped into adjacent
  statics. Callers provide 0x20000 words.
- Step 7 (stale-chain kill): 4-word tail → cursor reset → words 4–11 must NOT
  complete or insert; a fresh full tail fires exactly once.

**Injection vehicle — `hooklib.c`** (universal dylib; the r8/r9/r10 machinery
as a DYLD_INSERT_LIBRARIES payload):
- x86_64 slice constructor: translated check via `sysctl.proc_translated`,
  `MACFINALS_NT_HOOK=0` kill switch, `DISABLE_AOT` unset → refuse (fail-open;
  cached AOT bypasses the hook and leaves a process half-fixed), installs
  append hook at load, converter poll thread handles the demand-mapped
  /usr/libexec/rosetta/runtime (first exception), atexit counter dump to
  /tmp/rosetta-re/hook-<pid>.log. Exports macfinals_ctl/ring/installed for
  in-process consumers. arm64 slice: no-op constructor (native children safe).
- `ntstub` HOOKLIB mode passes ALL 4 checks through the dylib (installed=3);
  NO_IRET control still works via the dylib's ctl.

**Launcher plumbing** (scripts/portable_setup.m, wineEnv): `MACFINALS_NT_HOOK`
(pointing at the dylib) sets `DYLD_INSERT_LIBRARIES` + `DISABLE_AOT=1` for the
whole wine session — Steam, children, game. Steam propagates the unix env to
every child it spawns (27/27 session processes hooked, verified 2026-11-01).

**Hardened runtime is the insertion blocker.** All wine-side executables were
signed `--options runtime` (flags 0x10000) → dyld strips DYLD_INSERT silently
(wine runs, no hook log). Ad-hoc re-sign (`codesign --force --sign -`, flags
0x2) permits insertion; no library-validation fallout because ad-hoc without
the runtime flag doesn't enable it. Platform binaries (Apple's) always strip
DYLD_* — harmless, arm64 slice no-ops anyway. Game-mode apps re-sign with
`--options runtime` in makeGameApps — needs the same relaxation to stay
insertable (done manually in macfinals-6; makeGameApps not yet changed).

**verifyRuntime hashes every file on EVERY launch** (portable_setup.m:444) —
re-signing any runtime file requires regenerating runtime.json (version +
sha256/bytes). The manifest walker must treat symlinked DIRECTORIES as entries
(os.walk puts them in dirnames, not filenames — first regeneration silently
dropped D3DMetal.framework/Resources and Versions/Current). Generated game
apps (Discovery.app) are exempt; the source Overwatch.app is not.

**Runtime macfinals-6** (clone of 5): bin/wine, bin/wineserver,
lib/wine/x86_64-unix/wine, Overwatch.app (inner + bundle) re-signed ad-hoc;
manifest regenerated (exactly those 4 entries differ); state.json
active_runtime=macfinals-6; helper `verify` → runtime_verified; direct
`bin/wine --version` under MACFINALS_NT_HOOK → installed=3 with readback.
Canary session (Crab Champions via Steam UI) started 2026-11-01 — outcome
below when known.

**Ship shape (same night): the hook now rides inside the runtime.**
`lib/macfinals-nt-hook.dylib` (universal, min 26.0 — the default SDK target
stamps 27.0 and the app floor is 26.5, so the flag is required) is a manifest
entry, and wineEnv() auto-detects it (env MACFINALS_NT_HOOK still overrides,
`=0` disables). App Play needs no env vars. `makeGameApps` re-signs game apps
ad-hoc WITHOUT `--options runtime` (the hardened flag strips the insertion);
entitlements are preserved — re-signing game-mode bundles with plain
`codesign --force --sign -` DROPS their entitlements (jit/unsigned-mem/
lib-validation/audio-input); restore them from the pristine macfinals-5
copies. App release must embed the SAME runtime version as active_runtime
(AppModel.swift:542 installs the release runtime otherwise — embedding the 5
archive would have silently reverted the hook), so macfinals-6.tar.gz was
built from the manifest itself (runtime.json + listed files only; Discovery.app
and .DS_Store excluded) and `derive_runtime.minimum_field` was taught fat
Mach-O headers (strictest slice) — a universal dylib in the runtime crashed
`archive_minimum` before. Build: **MacFinals-10.app**, runtimeVersion
macfinals-6, bundle verifies.

## r12 (2026-11-01) — Steam's UI never appears with the hook: CEF stalls, bisected

**Symptom.** With MacFinals-10 (hook auto-on), Steam starts but no window
ever appears; Steam's own "steamwebhelper is not responding" dialog follows.
`steamwebhelper_real.exe` (CEF browser, sometimes its renderer/StorageService
too) pins 100–170% CPU for 10+ minutes; cef_log.txt goes silent after
"This is Chrome version …"; `sample` shows Rosetta's
`com.apple.rosetta.exceptionserver` thread in 93% of samples; the process
survives wineserver exiting (needs `kill -9`). Every hook counter in it stays
0 — the matcher never inserts anything in CEF. The game launched without
Steam (gameDebug Play) is unrelated: Discovery.exe ran 4 s under the full hook
and exited -11 on SteamAPI "could not connect to Steam".

**Bisect** (clean slate per arm — app quit by bundle id, helper `stop`,
`kill -9` survivors; `Helpers/ow2-setup launch` with MACFINALS_NT_HOOK set;
pass = a Steam CEF window within 240 s). NB the env must reach the HELPER
(`Contents/Helpers/ow2-setup`), not `Contents/MacOS/MacFinals` — the Swift app
spawns the helper with an explicit environment, so a var set on the app is
dropped (an earlier "hook off" run was invalid for exactly that reason).

| arm | injected | result |
|---|---|---|
| off | `MACFINALS_NT_HOOK=0` | window 11 s |
| null | empty universal dylib + DISABLE_AOT=1 (30 procs) | window 10 s |
| noconv | hooklib with converters compiled out (`-DMACFINALS_NO_CONVERTERS`) | NO WINDOW 242 s |
| full | shipped macfinals-6 hook | NO WINDOW 242 s |
| skip | full hook, CEF processes skipped | window 10 s; full 2056x1200 UI later, CEF CPU normal |

So neither DISABLE_AOT nor the injection is at fault; the **append hook**
stalls CEF. Mechanism not proven: the leading hypothesis is cost — the append
helper runs on every instruction word Rosetta emits, CEF/V8 translate
continually, and the slot table (16 static slots, never released) is full in
such processes so every call probes all 16. A hypothesis that a ring overflow
wrote into the payload (payload is mapped directly after ctl+ring) was
REJECTED by disassembly: the old code loads `g_ring_at` once and stores at the
checked index. But the old non-atomic counter does LOSE updates and can move
backwards (8-thread stress: 111k–129k of 131072 filled) — fixed anyway with
an atomic `ring_claim` (ldaxr/stlxr, saturating); stress test 3/3 exact,
host_payload_test all PASS.

**Fix shipped: hooklib skips any process whose argv contains
"steamwebhelper"** (CEF needs no NT fix — counters always 0). Hooked:
steam.exe, steamservice, steamsysinfo, hardwareupdater, all Wine system
processes, and the game. The hook log header now records ppid and the real
argv (`proc_pidpath` only ever named the Wine loader, hence the old empty
`()`), plus a `skipped: Steam CEF process` line. Runtime **macfinals-7** =
macfinals-6 + this dylib (manifest: dylib entry + version only), staged in
`$W/stage/macfinals-7`, archive `$W/artifacts/macfinals-7.tar.gz`
(check_archive OK, minimum 26.0); **MacFinals-11.app** (runtimeVersion
macfinals-7, verifies).

Open: whether THE FINALS' own process tolerates the append hook's cost — it is
also a heavy translator. Its 4 s gameDebug run was fine.

## Tooling in this directory

- `ntstub.c` — the milestone-2/3/4 hook harness (r8: NT fixups live via the
  payload; r9: converter replacements in /usr/libexec/rosetta/runtime +
  int3/SIGTRAP exception checks; r10: iretq-emission matcher insert + #GP
  probe (variant-3 blob with fault shim, SIGSEGV handler, ring re-dump);
  r5-style counter stubs kept as cross-validation; fresh-page trigger; TB
  dump; restore).
  `clang -arch x86_64 -O1 -Wall -o ntstub ntstub.c && DISABLE_AOT=1 R10B_RESUME=1 ./ntstub`
  Controls: `NOHOOK=1` (no patch), `NO_IRET=1` (everything but the r10
  insert, via ctl[15]), `R10B_RESUME` unset = log the #GP context and exit 70
  (safe default), `EARLYDUMP=1` (startup JIT dump).
- `payload_arm64.c` — the freestanding arm64 payload (matcher + inserter +
  r9 converter replacements + r10 iret chain; ctl slots 13/14/15 = iret
  fixups / misses / disarm). NOTE: `-nostdlib` must NOT be passed —
  `-dynamiclib` refuses it ("must link with libSystem.dylib"); the payload is
  still freestanding by construction and mkpayload.py gates on it.
  `clang -arch arm64 -O2 -mgeneral-regs-only -dynamiclib -o payload.dylib payload_arm64.c &&
  python3 mkpayload.py payload.dylib payload_blob.h`
- `host_payload_test.c` — host-side gate (build/run `-arch arm64`, Apple
  Silicon host): replays the recorded ring through the exact payload source
  (3+2 fixups at the verified anchors), differential-tests the r9
  converter replacements against the ORIGINAL machine code executed from an
  anon RX page, and replays BOTH recorded iretq tails (adrp-path jit-3,
  movz-path ring) requiring exactly one 4-word insert each. Needs
  `/tmp/rosetta-re/runtime.img` —
  `cp /usr/libexec/rosetta/runtime /tmp/rosetta-re/runtime.img`. Run it after
  touching the payload.
- `appendstub.S` — the assembler-built append stub (embedded by mkstub.py
  into append_blob.h; four placeholder quads patched by value in ntstub.c).
  `clang -arch arm64 -c -o appendstub.o appendstub.S && python3 mkstub.py
  appendstub.o append_blob.h _append_stub,pcall,pctl,pring,pback`
- `nt_native.c` — four-check harness. `clang -arch x86_64 -O1 -o nt_native nt_native.c && ./nt_native`
- `jitdump.c` — marker-wrapped pushf/popf/iretq/cli + full RWX/low-region dumper
  to /tmp/rosetta-re (read-only, safe). `MF_PAUSE=1` keeps it alive for vmmap.
- `wrap_raw.py` — wraps a raw dump in a Mach-O (correct vmaddr) for `otool -tV`.
- `mkw.py` (in /tmp/rosetta-re) — wraps a raw word ring (arm64) for `otool -tV`;
  the ring disassembler used for the milestone-4 trace.
- Analysis artifacts (this build only): /tmp/rosetta-re/{text.asm, libRosettaRuntime,
  jit-*.bin/.txt/.asm}. Regenerate with jitdump after a macOS update.
