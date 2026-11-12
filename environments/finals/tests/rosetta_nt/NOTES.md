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

**Two separate kernel-level failure classes, both on macOS 27.0 (26A428):**

1. `mprotect(PROT_READ|WRITE|EXEC)` on libRosettaRuntime's file-backed TEXT page,
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

2. **mach_vm_read_overwrite on translated anonymous EXEC mappings can PANIC the
kernel** (2026-11-07, twice, deterministic: ntstub gate runs at 18:20:00 and
18:21:16, both forced reboots). Kernel data abort far=0x14 — a NULL+0x14 deref
inside the read path — panicked task = the hooked process itself (pid 5216
ntstub), faulting user frame `scan_patch_thunks+494` = the `callq
_mach_vm_read_overwrite` reading a 256K chunk of an anon exec region at
dyld-constructor time, with translation live and a second mach client
(converter_poll) querying in parallel. Panic copies kept in
`/tmp/rosetta-re/panics/` (until /tmp cleanup). sysdump's reads never hit it —
but sysdump read 4K at a time, post-startup, as the process's only mach client;
that precedent does NOT generalise. **Treat every mach_vm_* call that touches
translated mappings (read included) as panic-capable until re-proven**; the
safe alternative for reads is direct user-space loads behind a SIGBUS/SIGSEGV
recovery handler (own address space, worst case is a catchable signal).
Details: "r20 (2026-11-07)" section below.

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

Recon before experimenting, from our own dependencies: GPTK's **x86_64** `libd3dshared.dylib`
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

## r20 (2026-11-07) — KERNEL PANIC: the thunk scanner is quarantined

Context: r19's stream-insert never fired (`entered 0`; the ring template does
not exist in finished code). r20 pivoted to patching the FINISHED 16K thunk
regions directly: trampoline into the 12-word dead padding at +0x2ec0, redirect
the single trap-feeding branch at +0x2b30, trap itself left intact so every
bail is stock SIGSYS (fail-closed). All offsets signature-verified against
three byte-identical ground-truth thunks; payload stub `ntfix_trapfast`
otool-verified; `host_payload_test` PASS. The driver (`scan_patch_thunks` in
hooklib.c) walks anon exec regions and reads them in 256K chunks via
`mach_vm_read_overwrite` looking for the trap pair.

**First in-process run of the scanner panicked the kernel. Twice.** ntstub gate
runs at 18:20:00 and 18:21:16; both panics identical modulo ASLR slide:

- `Kernel data abort`, `far: 0x14` (NULL+0x14 kernel deref), macOS 27.0 26A428.
- `Panicked task … pid 5216: ntstub` — the hooked process, from dyld's
  constructor chain (image 464 = dyld).
- Panicked thread user frames: `[463,7646] = scan_patch_thunks+494` and
  `[463,3537] = macfinals_hook_init+2017`; 463 = hooklib x86_64 UUID
  78bf95bd. `scan_patch_thunks+494` is the return address of `callq
  _mach_vm_read_overwrite` (0x1dd9 in the shipped build) — **the READ panicked,
  not the write path; hot_write never executed**.
- Second ntstub thread was blocked in `converter_poll+102` (its own
  mach query) — two mach clients live at once.
- Kernel regs at fault: x0=8, x1=0x8000, x9=0x11, x10=0x8000,
  x12=0x3b9aca00 (=1e9), x2=0x3009xxxx-ish — TXM/pmap-flavoured values, deep
  in the copy path, not trap-argument shaped.
- Panic files: `/tmp/rosetta-re/panics/` (copies of the originals, which the OS
  deletes after ingestion). No hook log existed — /tmp/rosetta-re had been
  cleaned, so the run's own logf output is lost.

Why sysdump never hit it (it read the same kind of regions): 4K reads vs 256K;
ran post-startup as the only mach client vs constructor-time with translation
and a second mach query live. The bug window is somewhere in that difference —
exact trigger unknown, and unknowable without another panic-per-experiment
ladder, which is Rasmus's call to budget, not ours.

**State after quarantine:**
- hooklib.c gates the whole r20 feature behind `MACFINALS_THUNK_PATCH=1`
  (default OFF, logs why). Scanner, trampoline build and thunk_poll all inside
  the gate. Rebuilt 2026-11-07.
- The **write path never ran and is still unproven** — but after a read-only
  API panicked us, mach_vm_write / mach_vm_protect / sys_icache_invalidate on
  translated exec mappings must be presumed at least as dangerous until a
  consented tier ladder says otherwise.
- Residual risk that SHIPS: `converter_poll`'s `refresh_fastpath_target` uses
  mach_vm_region_recurse + mach_vm_read_overwrite at 0x7ffe1008 (macfinals-40).
  It is one 8-byte read at a checked-readable fixed address — every probe and
  game run so far clean — but it is the same API family. Flagged, kept.
- Installed runtime macfinals-40 contains NO r20 code (verified: its hook has
  no scanner strings) — the shipped runtime cannot panic from this bug.
- If r20 is ever revived: reads must go through direct user-space loads with a
  SIGBUS/SIGSEGV recovery handler (no kernel vm call), and each write-side
  primitive needs its own cowtest-style tier with pre-logged steps and
  Rasmus's explicit crash-budget sign-off. Panic risk is now 2 known classes;
  see "⛔ THE CRASH".

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

## thunkmap probe (2026-11-07, later) — thunks are hidden from direct reads

`thunkmap.c` (new): standalone translated probe, zero mach copy calls —
region walk (metadata) + direct loads behind a sigsetjmp SIGSEGV/SIGBUS
recovery handler. Two findings, both durable:

1. **The safe-read design works.** Run 1 died SIGSEGV (process only, machine
   fine) poking PROT_NONE-current regions with direct loads; filtering to
   cur-prot-readable regions, run 2 completed the full walk (325 regions,
   0 faults). Direct-load probing of translated memory is safe and stable.
2. **Direct loads are BLIND to the thunks.** After translating live syscalls
   (trigger verified running), the trap pair exists in NO readable region —
   not the 186 MB JIT arena, not anywhere. Run 1's 16K exec regions whose
   pages fault on every direct load are where finished thunks live:
   **Rosetta maps JIT/thunk pages PROT_NONE against the process's own
   loads.** The only reader that has ever seen them is the kernel copy path
   (sysdump). Consequence: the r20 page-patch needs mach reads (panic-class
   call, sysdump shape vs 256K constructor shape distinction unproven) AND a
   wholly unproven write primitive on PROT_NONE TXM pages. A mach-read probe
   of the hidden regions (sysdump shape, step-logged) was prepared and
   BLOCKED pending Rasmus's explicit approval — correct call, the trigger
   conditions of the 2026-11-07 panic are unknown.

Alternative mechanism now on the table (no page writes at all): intercept the
brk#5 Mach exception in-process (thread-level exception port on translated
threads), recognise the syscall-thunk register shape, and reply with a
redirected PC into ntfix_trapfast (payload execution from translated threads
is already proven by the NT hook). Non-matching faults forward to Rosetta's
original handler untouched — fail-closed by construction. Feasibility probe
(own thread, exception port, observe one brk#5) uses only mach port calls,
outside both known crash classes.

## exctest (2026-11-07, evening) — exception-port interception is DEAD for brk#5

`exctest.c`: hand-rolled EXCEPTION_STATE_IDENTITY|MACH_EXCEPTION_CODES handler
on our own port, mach-port calls only (nothing in either crash class).
Results, in order:

- **The mechanism works under Rosetta**: a null write from translated code
  (`*(volatile int*)0 = 42`) ARRIVES at a thread-level port as EXC_BAD_ACCESS
  (type 1, codes [0, 0x600000000], flavor 4 / 14 words). Registration,
  delivery and reply all function. (Probe bugs found and fixed en route: both
  marker stores wrote (%rdi) so marker1 was invisible; the reply must MOVE the
  kernel's send-once right — COPY_SEND gets MACH_SEND_INVALID_TYPE and leaves
  the faulting thread suspended forever, i.e. a hang, not a crash.)
- **The brk#5 of an invalid-class `syscall` arrives at NEITHER thread- nor
  task-level ports** (both installed, kr SUCCESS; trigger verified running
  through the stock SIGSYS path both times). TXM recognizes Rosetta's syscall
  trap in translated code and vectors it straight to the in-process
  exceptionserver thread outside the user exception chain (the panic stackshot
  showed that server as a thread of our own pid). Exception-port interception
  cannot see it. **Route closed.**
- Side observation: thread_get_exception_ports from a translated caller
  returns garbage fields (mask/behavior/flavor look like addresses in the
  0x30-0x3xxxxxx runtime range) — the MIG round-trip for that call is broken
  under translation; do not trust it for introspection.

Remaining live routes for the syscall fastpath:
1. **r19 revival, capture-first (recommended, safe):** r19 failed matching an
   ASSUMED append-stream template for syscall sites. The ring-capture mode
   (shipped, read-only, ran for the pushf/popf work) can record the TRUE
   stream form while a fresh syscall site translates; an insert at the site
   then goes through Rosetta's own JIT writer — no PROT_NONE reads, no page
   writes, nothing in either crash class. Caveat: thunks themselves may be
   template-memcpy'd (three byte-identical instances) rather than streamed;
   but the SITE code (movz x28 chain + bl thunk, known from finished dumps)
   must be streamed at first translation — that is the insert target.
2. **mach-read + write ladder (fallback, needs Rasmus's explicit per-tier
   sign-off):** sysdump-shaped 4K reads of the hidden regions, then a COW
   write tier. Panic-class calls; blocked since 2026-11-07 afternoon.

## r21 (2026-11-08) — the stream insert WORKS: mechanism found, v6 geometry, clean ×8

The r19 revival ran capture-first as planned. Outcome: **the divert chain now
lands, executes, and bails byte-cleanly in the carrier** — `capsite` ×8:
exit 0 every run, all markers exact, stub entered 11/12, zero translator
asserts. Along the way the r20-era mystery (why inserts corrupted TBs) got a
complete mechanical answer.

### Ground truth at file-backed sites (capsite + ring)

The site emission has NO ldrb/tst/b.eq prefix (that lead-in only appeared in
the Wine-side probe ring): `movn/movz w0` (guest rax) + `movz x28,#A,lsl#32`
+ `movk x28,#B,lsl#16` + `movk x28,#C` + `bl #0`. The matcher re-armed on
the movz-x28 carrier alone (payload `IS_SYSC_MOVZ_X28`; the r19-era prefix
anchor never existed at these sites).

### THE MECHANISM (capsite3 arena dumps — the answer to every mystery)

A Rosetta fixup pass writes a **26-bit immediate at stream offset X = the
cursor when the bl was emitted (the PRE-insert cursor)**, as a
read-modify-write preserving the word's top 6 bits. Proof by two different
words sitting at X in two dumps: `adr` 100000db → **13ffe81e**, `nop`
d503201f → **d7ffe7de** — both undefined encodings, both exactly
"26-bit patch over top-6-preserved original". So:

- Whatever word occupies X becomes garbage when the fixup lands.
- Executing it = EXC_BAD_INSTRUCTION = the r20-era ExceptionServer assert
  (exit 132) — reproduced deterministically. This was NEVER a
  "pc-relative rewrite on replication"; replication only spreads copies.
- r19's `entered=0` was the same bug seen from the other side: the insert's
  word 0 sat at X, died before its own branch.
- Our x26 immediates survived untouched in **24/24** dumped copies. Only
  offset X is poisoned.

### v6 geometry (`insert8` in payload_arm64.c): never execute X

```
X-4   b #+8          14000002  behind-cursor overwrite of the last site word
                               (read-back verified first: *prev == sysc_last,
                               else no insert — fail-closed)
X+0   bl #0          94000000  SACRIFICIAL: absorbs the fixup patch (26-bit
                               RMW over a real bl = a valid relocated bl)
X+4   <replayed site word>     restores the x28 carrier / guest-rax state
X+8   movz x26,#stub           4 constant words, no pc-relative anywhere
X+24  blr x26        d63f0340  divert; lr = X+28
X+28  b #-28         17fffff9  bail: back to X = the REAL relocated bl
X+32  (emitter's own bl lands here: unrelocated, dead)
```

Bail = `br x30` (stub guard exit) → the b at X+28 → X, whose patch made it
the exact relocated bl stock would have had — lr = X+4 exactly as stock,
slowpath entry byte-identical, no brk#5, no exception server. The stub's
park/guard/late-bail edits (x30-based) work verbatim with this entry state.

### Evidence

- `capsite` ×8 (hooklib + `MACFINALS_HOOK_RING=1 DISABLE_AOT=1`): exit 0,
  12/12 marker lines exact per run, `entered 11`, no asserts.
- Per run: 5 inserts (kind-6 events, ring positions 36..504), 5 chain
  misses — the misses are REAL (valid-class) syscall sites whose stream
  shape doesn't match (extra words before the bl); insert8's `*prev`
  verification declines them → stock. Fail-closed working as designed.
- capsite3 scan-0: **36/36 intact chain copies** across arenas. Every copy:
  b at X-4, **a valid relocated bl at X** (per-copy immediates 97ffe7c9 /
  97ffe785 / 97ffe81e — same imm26 low bits as the v2-era undefined stomps,
  proving the top-6-preserving RMW over OUR 0x94000000), replay, x26 chain,
  blr, bail-b, dead 94000000 at X+32. Two copies' bl target page+0x200 (the
  strict thunk entry per r20 ground truth), one +0x204 — Rosetta's own
  per-copy fixup arithmetic, i.e. exactly what a stock bl there would carry.

### The `13ffe000` "stomps" are Rosetta's fixup TABLE (benign)

capsite3's stomp mask `(w & 0xfffff000)==0x13ffe000` also fires ~198× in a
data region (e.g. 0x10c6a0e50) on words shaped `[u64 pointer][u64 imm26]` —
that is the fixup table itself (pointer to the stream slot, value to patch).
Mask coincidence, not corruption.

### Scanner caution (diagnostic-tool only)

capsite3's arena scan is only stable while translation is QUIESCENT. During
active JIT churn (scan gen-1, right after site executions): one run died
exit 77 on an unguarded load (context dump — fixed with a per-page
recovery point), one still exited 77 (a fault escaped the page guard), and
one HUNG with SIGALRM never delivered — the walker was wedged
uninterruptibly, almost certainly in `mach_vm_region_recurse` on a VM being
restructured underneath it. Not a new crash class (no panic, killable), but
don't region-walk during translation storms, and don't ship a scanner that
does. The 2026-11-07 "region walks stay allowed" rule now carries this
caveat: allowed, but not concurrent with heavy JIT activity.

### Build lesson (cost an hour): the stale lipo slice

A failed hooklib x86_64 build + `lipo -create` silently ships the PREVIOUS
slice. `2>&1 | head` ate the error. Since then: builds capture stderr to a
file and check rc, and the finished dylib is byte-verified — the payload's
insert8 code block and the macfast entry bytes must be found in
hooklib.dylib before any test run ("a failed payload build leaves the old
payload_blob.h" — same failure shape, now guarded on both sides).

## r21b (2026-11-08, afternoon) — in-Wine: chains are PLACED but never EXECUTE

Carrier success (r21) did not carry over. In a Wine process the matcher fires,
insert8 places complete, correctly-addressed v6 chains — and `entered` stays 0.
Full record below; nothing crashed at any point in this session.

### Setup (exact, reproducible)

```sh
root="$HOME/Library/Application Support/MacFinals"
env WINEPREFIX="$root/environment" WINEDEBUG=-all \
  DYLD_INSERT_LIBRARIES="$PWD/tests/rosetta_nt/hooklib.dylib" \
  MACFINALS_HOOK_RING=1 MACFINALS_SYSC_DUMP=1 DISABLE_AOT=1 \
  "$root/runtimes/macfinals-40/bin/wine" runtime/build/probe/direct-syscall-probe.exe
```

Probe output unchanged: ntdll QPC 39 ns/call, direct `syscall` 18 809 ns/call
(≈ the known ~16-19 µs stock cost — the divert never engaged, semantics intact).
Kill the leftover wineserver after each run (CLAUDE.md recipe); rc=137 on the
harness timeout is that kill, not a failure in the dump.

### What works in the Wine process

- The hook installs in every Wine process (constructor passes, DISABLE_AOT
  honored), no translator asserts, no crashes, clean atexit dumps ×3 pids.
- **The append hook sees Wine's syscall sites.** Ring/inslog decode (earlier
  today): 30 x28-carrier sites in one window, all carrying the compat prefix
  (`…tst…; d500401f` (hint); `54000001` (b.eq)) immediately before the
  `movz x28` carrier — capsite's file-backed sites LACK that prefix. Matcher
  arms on them and insert8 fires (kind-6 events, `thunks`-line inserts).
- **Complete v6 chains exist in the process at exit.** syscdump (arena scan at
  atexit, page-guarded direct loads — no mach_vm_* on translated mappings):
  - pid 7994 (exe): 144 chains; pid 8001 (wineboot): 212; pid 9810 (probe):
    144; pid 9819: 212; pid 9826: 264.
  - Every chain is word-for-word correct: `…54000261 (b.ne) d2cdfffc
    (carrier) f2bffe7c (movk16) | 14000002 (b #+8 at X-4) 97ffxxxx (patched
    relocated bl at X) <replay> d2825c1a f2a1039a f2c0005a f2e0001a (x26
    quad = stub addr) d63f0340 (blr) 17fffff9 (bail b) 94000000 (dead emitter
    bl at X+32)`.
  - Stub address decodes to payload_base + 0x12e0 = `ntfix_macfast` — the
    divert target is exactly right.
  - The sacrificial bl at X absorbed Rosetta's fixup patch in every copy
    (97ffxxxx, valid bl), same as capsite. Mechanism holds in Wine.

### What does NOT work — the central finding

`entered = 0` in every run (capsite under identical evidence had entered=11).
The chains are placed in a region that never executes:

- **ALL chain copies live in ONE region: `0x200014000`, prot 7 (RWX)** — the
  SAME FIXED ADDRESS in every process (exe, wineboot, probe, different pids).
  A fixed per-process low RWX arena = Rosetta's persistent emission/stream
  buffer, not where executed TBs come from.
- **A stock-site census (added this session: movz-x28 carrier + movk(s) +
  bl, no chain) finds copies elsewhere**: 116 sites (pid 9810) in region
  `0x20869b000`, **prot 3 = RW, NOT currently executable** (max_protection
  includes exec), size 544 768 B. pid 9819: 91 sites at `0x20869a000`;
  pid 9826: 161 at `0x20869b000`. Same picture every process: chains in the
  fixed 0x200014000 RWX arena, unchained stock copies in a ~0x20869xxxx
  RW(max-exec) region.
- **No scanned exec region contains chains OR stock sites.** So where the
  EXECUTED TBs live is unknown: either materialized from metadata (not from
  the staged bytes), in regions the scan filter skips (named regions,
  submaps, >512 MB, non-readable-at-scan), or torn down before atexit.
- Working theory (UNPROVEN, next thing to test): the append hook at
  libRosettaRuntime+0x18a0 covers ONE emission path. Single-threaded capsite
  emits through it and executes what it wrote (chains in exec arenas,
  entered>0). Wine's multi-threaded JIT (8 slots in the events line) uses a
  second path — the 0x20869xxxx copies — and the executed TBs derive from
  that, dropping our inserted words.

### The published target (ctl[35]) — PLAUSIBLE, still unverified

Log prints decimal: 208 812 694 and 208 809 694 across runs = **0xc723a96 /
0xc722ede**. These differ between runs by a base-sized delta — consistent
with the PE-side `ntdll.dll` base being ASLR'd per run plus a fixed
prolog_end offset (sigsys_handler sets the GUEST rip, so the pointer's value
must be a PE VA; PE images on this prefix load low, ~0xc7xxxxx). So the
publish patch is probably working. VERIFY with
`runtime/build/probe/read-dispatch-page.exe` (reads 0x7ffe1008 Windows-side;
compare against the live ntdll base) before ever letting the stub engage on
it — a wrong-but-canonical target jumps to garbage with no guard to catch it.

### Tooling notes / bugs found while measuring

- **STOCK census lines print the header but NO words** (lines end at
  "words:"). The words loop in the same dprintf block produced nothing; HIT
  lines in the same function print words fine. Cause unknown — debug before
  trusting STOCK line content (the region/counter data is still valid).
- **Counts may be inflated by region revisits**: consecutive identical STOCK
  lines at the same address (2–4×) suggest mach_vm_region_recurse revisited
  the region (or genuine multi-copy emission). HIT lines also cluster 4×
  identical word-content. Dedupe before quoting absolute numbers; the
  region SPLIT (chains here / stock there) is unaffected either way.
- Census numbering cosmetic: all lines say "STOCK 0" (the <24 print cap
  tests the region-tallied total, which is 0 during the scan). Fix when
  touching the file.
- syscdump at atexit is reliable in Wine processes (3 pids, no wedge) —
  consistent with the "scan only when quiescent" rule. One pid (9816,
  1392 regions, 0 chains) = a hook-bearing child with no syscall sites.
- inslog ctx-zeros + ring_at misalignment explained (harmless): sites land
  at the START of recycled buffers (cursor < 26 words → zero context window)
  and concurrent JIT threads interleave records (ring_at pollution). Not
  corruption — the ring itself is sound.

### Crash ledger for this work (all of it, one place)

- r20 `mach_vm_read_overwrite` on translated anon exec region: **kernel
  panic ×2** (2026-11-07) — quarantined, forbidden class.
- In-place W+X on Rosetta file-backed pages: freeze (2026-10-31) — forbidden.
- capsite3 mid-JIT-churn scans: exit 77 / uninterruptible walker hang
  (killable, no panic) — tool limitation; scan only when quiescent.
- r21b session: ZERO crashes, zero asserts, clean exits everywhere.
- Never reproduced since v6: the v2-v5 "corrupt word → translator assert
  exit 132" class (sacrificial-bl design eliminated it, 8/8 capsite + all
  Wine runs clean).

### Open questions (in priority order)

1. Where do EXECUTED TBs live in a Wine process? (region-list dump of the
   full arena map; relax the unnamed-only/512 MB filters; find a known
   site's executed copy; compare capsite vs Wine region maps side by side.)
2. Is the 0x20869xxxx prot-3 family the source of executed TBs (second
   emission path), or dead staging? Do its stock copies carry the compat
   prefix (= Wine-site family) or not (= another family)?
3. Is ctl[35] 0xc723a96/0xc722ede really PE-ntdll prolog_end?
   (read-dispatch-page.exe; must be settled before engagement.)
4. If executed TBs bypass the append-hook path entirely: the insert must
   move (different hook point or post-materialization touch) — that is a
   design change; surface to Rasmus before building it.

### Next session's first moves

1. Extend syscdump: dump the region LIST (addr/size/prot/named?) for exec-
   capable regions instead of only scanning; run probe; compare against
   capsite's map (capsite3's scan already logs per-hit regions).
2. Fix the STOCK words-print bug; re-run census; check stock copies for the
   compat prefix.
3. read-dispatch-page.exe under the same env; compare 0x7ffe1008 value with
   the probe-side ntdll base.

## r21c (2026-11-08, evening) — the chain EXECUTES inside Wine; the dispatcher return clobbers rdx

Setup unchanged from r21b (macfinals-40 wine, DYLD_INSERT hooklib,
MACFINALS_HOOK_RING=1, DISABLE_AOT=1, payload v6 + census v2). Four results,
in causal order:

1. **Published target verified correct (read_dispatch_page.exe).** The
   hook publishes `prolog_end` = unix-ntdll.so dispatcher entry + 0x24 at
   0x7ffe1008. In-process probe: dispatcher 0x208809670, prolog_end
   0x208809694, both inside the **unix .so** (in-ntdll:0 — correct; the
   Windows-side ntdll base is 0x6ffffff30000, unrelated). The earlier
   "suspicious 0xc723a96" was a decimal/hex misread of the %llx log.
   R21b open question 3 is CLOSED.

2. **capsite4 / capsite5: the second-emitter switch is neither threads nor
   (outside Wine) anon backing.** capsite4 with 1/4/8 threads → entered
   11/44/88 (perfect scaling). capsite5 (single site memcpy'd to an anon
   RWX page, file-backed warm call first) → ins 1, entered 4: the ANON
   copy's emission went through the hooked append AND its chain executed.
2b. **anon_invalid_site.exe (ais3): the same holds INSIDE a Wine process.**
   Probe pid 80788: 12 polls at ins 38 miss 0 entered 0 (the 38 = startup
   PE-ntdll stub chains), then at the anon call: **ins 40, entered 2 — the
   first chain executions ever recorded inside Wine.** An anon-memory
   syscall site inside Wine uses the hooked emitter and its chain RUNS.
   The switch that hides Wine's PE-image sites from us is therefore
   per-image/per-mapping, NOT per-process — same process, anon site runs
   its chain, PE-image sites' executed TBs still bypass us.

3. **The ais3 crash DECODED (corrected after source work — the first read of
   this run was wrong): the chain never reached the dispatcher.** The exit
   line reads `entered 2 hits 0 bail5 0`; `hits` (ctl[36]) counts stubs that
   actually ENGAGED (table[0] installed a TB entry) — so BOTH stub entries
   took the early-guard `99:` bail, re-ran the stock bl, and everything the
   probe experienced afterwards was the STOCK path. The crash itself:
   `page fault on write to 0x8 at 0x7E0021` = the probe's second marker
   store (`movq %r8, 8(%rdx)`) with rdx=0 — and that is stock Wine-macOS
   behaviour, not ours: sigsys_handler sets `frame->restore_flags = 0`
   (signal_x86_64.c:2953), the dispatcher body zeroes edx in its XState
   prologue (`xorl %edx,%edx`, :3390), and `__wine_syscall_dispatcher_
   return` restores rax/rcx-from-frame/rdx/r8-r11 ONLY when restore_flags
   has CONTEXT_INTEGER (:3592-3612) — with 0 it returns via `ret` leaving
   rdx/r8/r9/r10 clobbered. `bad_syscall` (:3665) returns 0xC000001C in
   rax, no exception. So Wine's emulated raw syscall diverges from real
   Windows (which preserves rdx etc.) — a PRE-EXISTING upstream-Wine
   divergence, orthogonal to the hook, tolerated by the game on the stock
   path today. **No resume-contract redesign is needed**; the blocking
   question is simply: which guard bails? (ais3 probe also relied on rdx
   surviving — real-HW semantics — which the stock path never provided.)

4. **Per-process counter timeline (corrects an earlier misread): every
   Wine process converges to ins 40–58 miss 0 entered 0** (services.exe 58,
   probe exe 38→40). The `ins 0 miss 29–51` values that looked anomalous
   are the ring writer's FIRST 648-byte snapshot (50 ms after install):
   during early startup ~30–50 matchers arm and expire (miss = fail-open,
   stock stream), before the insert stream starts. Tooling: the bin files
   are APPEND-mode — the first snapshot is trustworthy, snapshots written
   during process teardown are recycled-page junk (ins 8732579476 etc.).
   Authoritative counters are the log's `sysc poll` / `sysc at exit` lines.

### r21c conclusions

- Chains placed by insert8 DO execute when the site's mapping uses the
  hooked emitter: Mach-O text (capsite), anon page outside Wine (capsite5),
  anon page inside Wine (ais3 — the stub was entered twice). Only Wine's
  PE-image mappings divert the EXECUTED TBs to the unhooked second path
  (r21b finding, unchanged). **CORRECTED by ais5 (below): PE-image sites DO
  execute their chains — the r21b "second emitter" framing was wrong for
  PE images; it applies to ANON mappings (ais4/ais6: the anon copy never
  enters the stub).**
- ais3's crash is stock-path behaviour (rdx clobber via restore_flags=0);
  our stub bailed at its guards before doing anything. Also recorded:
  Wine's emulated raw syscall clobbers rdx/r8/r9/r10 (pre-existing
  divergence from real Windows; the stock path the game runs on today has
  it too).

### r21d (2026-11-08, night) — THE FAST PATH WORKS IN WINE: hits, resume, 33 ns/call

ais4→ais12 arc, one blocking defect per run, each fixed within the existing
design (no mechanism changes — guard corrections and one missing
precondition):

1. **ais5/ais6: chains from PE-image sites EXECUTE in Wine, every call,
   hot loops included.** site1 (a plain function in the exe, `mov eax,-1;
   syscall` + rdi markers) enters the stub on every spaced AND every hot
   call (20000-loop). No tier-up, no re-emission: the r21b "never execute"
   model is dead for PE images. The ANON copy of the same code never
   enters (entered counts prove it) — second-emitter treatment belongs to
   anon mappings, the opposite of the first reading.
2. **Guard 5b (TEB identity) — TPIDRRO is NOT the TEB in our setup.**
   mac_gsbase_switching=0 (exec regions carry %gs truth), so TPIDRRO_EL0
   stays the native pthread TEB (0x7ff2063ce0e0); [pthread+0x30] = 0x7ffc0000
   is the Windows TEB (confirmed from the Windows side: `%gs:0x30` prints
   7ffc0000, and [TEB+0x378] = live syscall_frame). Fix: two-hop resolution,
   fail-closed (candidate must be nonzero, canonical, page-aligned, and
   self-pointing before it is trusted; single-hop kept for gsbase-switching
   setups). ais6 also exposed that the histogram BYTES wrap at 256 — 5b now
   also counts in a full word, ctl[45].
3. **Guard 7 (instrumentation callback) — the field is never null under
   Wine.** signal_start_thread stores &instrumentation_callback at TEB+0x330
   (signal_x86_64.c:3212) and the dispatcher DEREFERENCES TWICE
   (:1943-1946: r10=[TEB+0x330]; r10=[r10]; test). Fix: same two-level
   deref, canonical-check the slot first, bail only when a real callback is
   installed (the stock handler ORs RESTORE_FLAGS_INSTRUMENTATION into
   restore_flags, which the stub does not model — bail stays correct).
4. **The resume call's missing precondition — table[0] installs ONLY when
   the pending-flags word has bit9.** 0x5908 = `swp wzr,w19,[ctx+0x1c8]`:
   atomically CONSUMES the 32-bit pending word and returns the old value;
   0x5964 takes the install path (0x59c4 → 0x6644 CRC32 TB-cache lookup,
   miss → 0x5c74/translate; entry → `str [ctx+0x8]`) only when bit9 is set
   in it. The epilogue's callers always have bit9 queued; ours had w=0 →
   no install → the clean bail (bail5 = entered, hits 0, ais8/ais9). Fix:
   queue the continuation ourselves — 32-BIT ONLY. **ctx+0x1c8 is a 32-bit
   flags field; ctx+0x1cc is a SEPARATE payload field the emitter keeps
   live state in** (eaa4: `str w21,[x0,#0x1cc]`): a 64-bit read sees the
   payload and false-busies (ais10's c38 diagnostic: flags 0, payload
   0x60482803); a 64-bit store ZEROES the payload (ais8/ais9 bug).
   Correct sequence: `ldr w13,[x18,#0x1c8]; cbz→queue; str w13,#0x200`
   (bit9; bit0 stays clear — that path parks rip at ctx+0x1d8 and calls
   0xebc4, exception-server notify, not our case).
5. **ais11/ais12: ENGAGED and measured.** anon-invalid-site.exe under
   macfinals-40 + local hooklib: `entered 220043 hits 220038 bail5 0` (5
   guard-2 = pre-publish window), every call returns rax=0xC000001C
   (bad_syscall via the real dispatcher — byte-identical semantics), rdi
   markers intact, and the timed hot loop: **33 ns/call** (stock 16-19 µs,
   Proton ~2 µs). The whole chain — v6 insert → stub → guards → frame
   writes → table[0] install → restore → `ret` into the dispatcher TB →
   dispatcher_return — runs on every call.

Tooling notes: c38 (ctl[38]) = last-writer diagnostic (busy-bail stores the
32-bit flags zero-extended; the post-call path stores ctx+0x8); the sysc log
line carries it as `c38`. direct-syscall-probe's own sites still never
engage (entered 0, stock 15821 ns — its emission shape is declined by the
matcher; the site1 shape is the proven one; timing moved into
anon-invalid-site's TIMED loop).

**Open, in order:** (a) the game's own sites are PE-image — but from ais5
PE sites engage, so THE FINALS' 32k/s traffic should engage the same way;
verify in-game counters. (b) Runtime macfinals-41 via add_nt_hook.py, Crab
Champions canary, then THE FINALS with R17's measurement setup. (c) The
5-guard-2 pre-publish window: the first ~100 ms of invalid-class traffic
still takes the stock path (publish lag) — acceptable (today's behaviour).

## r21e (2026-11-09) — the game's first-run crash diagnosed: TF guard added; x23 liveness bug found and fixed

### The crash (2026-11-08 19:20:39, macfinals-41, first run on the new account)

Embark crash reporter; minidump `THEIA_89989837_1424.dmp` (Temp dir, user
`crossover`): exception `0x80000004` (STATUS_SINGLE_STEP), addr
`0x20880b698` = **published fast-path target (prolog_end) + 4**, main thread,
2.3 s after inner Discovery.exe start. +4 is the SECOND dispatcher
instruction — a step trap fires after ONE executed instruction — so guest TF
was live when the fast-path resume entered Wine's dispatcher. Single-stepping
its own startup is classic anti-debug/anti-tamper behaviour (new-account
first-login path; the Nov 2 runs on stock-path runtimes reached the menu, so
the stepping window is state-dependent).

The stock sigsys path is PROVEN TF-correct (source-level reading of
`sigsys_handler` + the game surviving TF windows for weeks on stock runtimes),
so the fix is fail-closed: **decline TF-live sites to stock.** Guard at stub
label 18: live x17 bit 4 (TF) OR the runtime TF byte at ctx+0x444 nonzero →
count ctl[40] byte 7 (0x147) and bail to 99 (stock). Both arming modes
covered — the game's crash is consistent with a step-mode ENTRY TB (table[0]
keys its TB-cache lookup on the ctx TF byte), which no probe arming mode
produces (popfq arming sets only x17 bit 4; the probe never reproduced the
target+4 signature).

### The x23 liveness bug (the lesson of the session)

First build read the TF byte into **w23**. x23 is `_g_ctl` LIVE across label
18 (set at 0x1530 by the guard-5b block; the post-park frame build stores
through it at 0x15f8/0x1600/0x1610/0x1614/0x1634 without reloading). Every
fast-path entry then wrote through a garbage pointer: the ais probe spun at
VirtualAlloc with ~170 re-translations/s (chains re-inserted, executed once,
faulted, invalidated). **capsite passed VACUOUSLY** — all 11 carrier entries
bail at guard 2 (no dispatcher published outside Wine), so label 18 never
ran. Fix: the fall-through check reads into **w24** (dead after guard 7's
last deref, never read on the main path before its next write); the bail
block keeps x23/x24 clobbers like every other guard's bail. tbnz/ldrb/cbz
are flags-neutral, so the entry NZCV park is untouched.

Rules: (1) any insertion into ntfix_macfast needs a register-liveness audit
of the insertion point, not just of the entry; (2) **the regression gate for
stub changes is ais in Wine** (entered≈hits, TIMED ~35-50 ns, done), never
capsite; (3) entered accounting: `entered = hits + g2 + TF-bails(+other)`.

### Verification (all on macfinals-41 wine + local hooklib, ais env)

- ais r21e-2: `entered 220043 hits 220039` — IDENTICAL to the r21d control;
  TIMED 51 ns/call; done. Fast path parity proven.
- TF probe (spaced-warm, VEH stepper, +seh): r21e-2 `entered 33 = hits 29 +
  g2 2 + TF 2 (guards 0200000000000002)` — the armed call BAILS to stock;
  no step rip anywhere in the dispatcher range. r21d control: `entered 32 =
  hits 29 + g2 3` — the same armed call is TAKEN INTO the fast path. The
  guard fires exactly on TF-live entries and nothing else.
- TF-probe artifacts (pre-existing, NOT regressions): the VEH-continue
  spiral (c00000025 → stack overflow at 0x6ffffff9xxxx = Wine unix-side
  trampoline addresses, constant across runs) kills the probe after the
  armed syscall on BOTH stock and hooked runs — since the first VEH version;
  the +seh trace, not the probe's own output, is the evidence that survives.
- ~~Engagement needs the ring writer~~ — WRONG, corrected the same day. The
  ring (`ring_capture_on`) only starts a side thread that dumps counters;
  the ring buffer is allocated regardless and the payload never reads the
  switch. The "entered 0 without the ring" lines were the at-exit dumps of
  Wine's HELPER processes: the probe itself died in the VEH spiral before
  atexit, so without the poll thread its counters were simply never
  printed. Same artifact behind "200 hot calls → entered 0". Rule: a
  process that may die needs the poll thread (MACFINALS_HOOK_RING=1 or
  `touch /tmp/rosetta-re/nt-ring`) or its counters are invisible — absence
  of a line is not entered=0. (ais12's TIMED hot loop engages fine.)

### First game run on macfinals-42 (2026-11-09 07:40, PERF=1 avx msync)

Got in game, no 19:20-class crash — but **the fast path did NOT engage in
Discovery.exe**. perf/wine.<game pid>, in play (t=30-150 s): 26k-56k
SIGSYS/s with rax 0xffffffff counted by Wine's own sigsys_handler (stock
path), Rosetta's exception thread at 20-34 % CPU, GameThread ~75 %. Same
storm as R17. The game process has no hook counters (no poll thread, killed
before atexit), so which step fails is unknown: matcher never fires at the
game's sites (ins 0), chains placed but not executed (ins>0, entered 0 —
the r21b class), or entered>0 and a guard bails (guard 7 = an
instrumentation callback; the game made 24 NtSetInformationProcess calls,
classes unlogged). Next run: `touch /tmp/rosetta-re/nt-ring` first.

### Shipped

Runtime **macfinals-42** (= unhooked 38 + r21e-2 hook; add_nt_hook from base
38; embedded hook sha256 f8a2ae96116335f1… == tree hooklib), installed in
`~/Library/Application Support/MacFinals/runtimes/`. Final ais gate on -42's
own embedded hook: done, TIMED 52 ns/call, counters healthy.

Next: Crab Champions canary on -42, then THE FINALS (`RUNTIME=macfinals-42
PERF=1 tests/finals_bisect/launch_main.sh avx msync`) — the 19:20 crash
should not recur; watch `sysc` counters for in-game engagement and R17
GameThread comparison.

## r21f (2026-11-09) — THE FINALS installs an instrumentation callback; guard 7 now matches sigsys_handler instead of bailing

### The second game run on -42 (07:50, nt-ring switch on)

The game process's own counters (hook-<pid>.log, poll thread via
/tmp/rosetta-re/nt-ring): **entered 11,198,877, hits 137**, guards byte 6
(guard 7, 0x146) wrapping on every poll, byte 7 (TF) = 1. So the matcher
fires at the game's sites and the chains EXECUTE (~28k entries/s = the
storm rate) — the in-game mechanism works — but every entry after early
startup bailed at guard 7: **a ProcessInstrumentationCallback is installed
in the game process.** (Who installs it is not looked into — boundary 3.)
The single TF bail happened before the callback existed (TF guard runs
after guard 7): the r21e guard fired once in the real game and declined, in
the startup window where the 19:20 crash happened.

### The fix (parity, no new mechanism)

sigsys_handler does not decline a callback: `frame->restore_flags = 0; if
(instrumentation_callback) |= RESTORE_FLAGS_INSTRUMENTATION (0x10000)`
(signal_x86_64.c:2953-2954); the dispatcher return path re-reads the
callback via [TEB+0x330] and enters it with r10 = return address
(:3613-3629). The stub now does the same: guard 7 keeps the slot-canonical
bail but carries the callback VALUE in x24 (0 = none); restore_flags =
(x24 ? 0x10000 : 0) | (TF ? CONTEXT_CONTROL : 0). The TF guard's scratch
moved w24 → w22 (dead until the park's mrs x22,NZCV). Liveness audited on
the disassembly: x24 unwritten on the main path from 0x1598 to the cbz at
0x161c, compose touches none of x19-x28, x23 (= syscall FRAME from label
16 — NOT _g_ctl as the r21e comment said; corrected) untouched.
ntfix_trapfast (quarantined r20 path only) unchanged.

### Verification (throwaway prefix in the scratchpad — Steam held the main one)

- New probe `tests/windows_probe/instr_cb_probe.c`: installs a callback
  (NtSetInformationProcess class 40), warms the site, then single calls
  with nothing between the counter reads, then 200k timed.
  - stock (no hook): 1 callback per call, r10 = the instruction after
    `syscall` exactly, rax c000001c, markers ok; **16,464 ns/call**.
  - r21f: byte-identical callback behaviour (1/call, same r10, rax,
    markers); **75 ns/call**; entered 200039 = hits 200035 + 4 guard-2
    startup bails; guard-7 byte 0.
- ais (no callback): entered 220043 hits 220039 — identical to r21d;
  34 ns/call; done.
- Probe pitfall: a naked asm site called from C gets its argument in RCX
  (Win64 ABI), and rdi is callee-saved — the first build wrote through rdi
  and faulted at 0x5f. ais hid this by passing rdi via an asm constraint.

### Shipped

Runtime **macfinals-43** (= unhooked 38 + r21f hook, embedded sha256
4d4abaec4e269250… == tree hooklib), installed. Next: THE FINALS on -43 with
the nt-ring switch on — expect hits ≈ entered, sigsys/s in perf to collapse
from ~30k to ~0, Rosetta exception thread near idle, then FPS.

### r21f in the game — THE STORM IS FIXED (2026-11-09 08:00, macfinals-43, PERF=1 avx msync, nt-ring on)

Game process counters: **hits 10,158,397 / entered 10,159,226 (99.99 %)**,
bail5 827 (late bails, stock fallback), guards byte 7 (TF) = 1.
perf/wine.<game pid>, play window (t=40-130 s), vs the -42 storm run:

| | -42 (storm) | -43 (fast path) |
|---|---|---|
| stock SIGSYS/s | 26k-56k | **1-2** |
| Rosetta exception thread CPU | 20-34 % | **0.0 %** |
| GameThread user / sys | 36 % / 38 % | **81 % / 5 %** |
| context switches/s (process) | 192k | 102k |
| segv/raise/unhandled per s | ~200 | ~200 (the game's own; unchanged) |

GameThread's kernel time (the brk → exception server → SIGSYS detour) is
gone; it now spends ~2.2x as much time running its own code, and is pegged
(~96 % total) — the remaining limit is GameThread's translated x86 work.
Other traffic is cheap: NtQueryPerformanceCounter 232k/s (Wine syscall, not
SIGSYS; absent from the time ranking), MSync event ops ~93k/s at ~3 µs.
Open lever: DISABLE_AOT=1 (the hook requires it) forces JIT translation of
every Mach-O image in the process (Wine's unix side, x86_64 system
libraries, D3DMetal) — its cost has never been measured.

## Freeze hunt (2026-11-09 08:46, macfinals-47, PERF=1 avx msync, warn+macfinals_dx12, +macfinals_hook)

Rasmus: "a few major freezes". Game pid 72957; fast path still engaged (entered 9.43M,
hits 9.43M). Freezes from perf (GameThread ≥ 90 % alone, process quiet): loading +13..+22 s;
in play +94 s (0.4 s), **+109 s (1.7 s)**, **+129 s (2.2 s)**, +138 s (0.7 s).

- **Not pipeline builds.** Only 14 builds ≥ 50 ms all session, all on UE5's
  `PSOPrecompilePool #N` threads in one 7 s burst ending +34 s (~5,260 pipelines; ~35 ms of
  the slow ones was our DXIL rewrite, the rest GPTK). Nothing ≥ 50 ms near any play freeze.
  (Many builds < 50 ms on GameThread are not yet excluded — warn does not show them.)
- **Not Wine syscalls.** During freezes syscalls are cheap (memory calls absent from the
  time ranking), Wine's VM lock is held LESS than in normal play (40 vs 58 ms/s), and
  GameThread is 95 % user / 6 % sys. What rises: NtAllocateVirtualMemory 569/s (normal 85),
  NtFreeVirtualMemory 706 (277), mmap 1320 (522), NtDeviceIoControlFile 615 (207),
  NtDelayExecution 108 (7), NtContinue 417 (270), NtUserCallHwnd 8.7 (0). So: GameThread
  computing in user mode with its allocator churning — the shape of UE5's own hitches
  (garbage collection / streaming), magnified by running translated.
- **Two kinds.** +94 s and +138 s coincide with large translation bursts (2-6 M emitted
  words per 2.4 s interval — new code running for the first time); the two biggest, +109 s
  and +129 s, have translation near idle (2-40 k): pure compute in already-translated code.
- Open: (a) a default-logging run (trace off = no payload ring capture) to see whether the
  translation-burst freezes shrink; (b) what GameThread executes in the compute freezes —
  needs a stack sample of a live translated process (sample/spindump), which touches the
  2026-11-07 panic class and the anti-cheat's view of the process: Rasmus's decision.

## Freeze hunt, continued and PAUSED (2026-11-09 09:20) — NOT DONE

**Status: not finished, parked by Rasmus ("i don't think we need it atm").** The freezes are
still there and their cause is still unknown. Pick it up from here if they become a problem.

What was learned since the section above:

- **Shaders are cached between runs.** D3DMetal keeps its cache in
  `$(getconf DARWIN_USER_CACHE_DIR)d3dm/Discovery.exe/shaders.cache` (about 1.4 GB: the
  DXIL→Metal `bytecode_cache.bin`/`stage_cache.bin` and Metal's compiled `functions.data`).
  It is not wiped between runs. `bytecode_cache.bin` last grew 11-02, `stage_cache.bin`
  08:03 and `functions.data` 08:46 on 11-09; three later runs added nothing, yet they still
  froze. So, together with the pipeline timing above, **the play freezes are not shader
  compilation.** (macOS may purge that cache folder under disk pressure; a purge would bring
  first-run compiles back.) UE5's own `Saved/Discovery_PCD3D_SM6.upipelinecache` is 9 KB.
- **Freeze counts in the two later runs** (perf files, GameThread ≥ 90 % with < 6000 calls per
  0.1 s): 9 and 20 freezes, all 0.3-1.4 s, none of the 2 s kind. Most cluster in loading and
  the first minutes.
- **Apple `sample` is the wrong tool** (`tests/finals_bisect/freeze_sampler.py`). It works on
  translated processes and caused no trouble, but on the game it gets only 6-13 stacks per
  1.5 s (it suspends ~200 threads per stack) and starts 0.7-1.2 s after the freeze began, so
  it mostly samples after a 0.3-0.9 s freeze has ended. Its 8 samples
  (`logs/main-20261109-090403-avx-msync/freeze-samples/`) show GameThread in Wine server
  calls (NtDeviceIoControlFile, NtReadFile/NtWriteFile, waits) and in game code, but they
  are post-freeze and statistically thin — do not draw conclusions from them. One sample had
  frames in the anti-cheat region (only noted, not analysed).
  Also learned: THE FINALS starts a short-lived `Discovery.exe` (it names a GameThread too,
  exits after ~30 s) before the real game; tools must follow the newest process.
- **The better tool is built but has never captured a game freeze:**
  `tests/finals_bisect/freeze_profile.py` (`record`, then `analyze`). It attaches Instruments'
  Time Profiler (`xctrace record --attach`) — kernel sampling at 1 kHz, no thread suspension,
  **no developer mode needed**, stacks of translated processes in x86 terms — and afterwards
  cuts GameThread's samples to the perf-file freeze windows, broken down by module (Windows
  addresses mapped via `+loaddll` bases; the anti-cheat region only counted). Verified on
  probe programs: timing aligns to the millisecond, the hand-over between two processes
  works. Its first game run failed: kperf serves one recording at a time and the first
  process's recording still held it ("could not lock kperf"); fixed (one recording at a
  time, newest process wins, restart on a lock error) and re-tested on probes, **not yet on
  the game**.

To resume: `python3 tests/finals_bisect/freeze_profile.py record`, then
`RUNTIME=macfinals-47 PERF=1 tests/finals_bisect/launch_main.sh avx msync`, play through some
freezes, quit, then `python3 tests/finals_bisect/freeze_profile.py analyze`.
