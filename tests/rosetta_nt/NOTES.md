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

**Conclusion:** the in-process COW sequence (`mach_vm_protect(RW|VM_PROT_COPY)` →
write → `sys_icache_invalidate` → restore) is **mechanically viable on this
kernel, from translated code, against Rosetta's own image**. The freeze is
specific to the non-COW in-place W+X path. Remaining unproven: executing
*through* a genuinely patched code page (the next milestone), and the anonymous
arm64 payload mapping. Tool: `cowtest.c`; step logs in
`/tmp/rosetta-re/cowtest.log` (fsynced per step; regenerates per boot).

Payload-page note: the mapping flag the closed package uses for its anonymous
arm64 exec region is **not in the public SDK headers** (checked
`vm_statistics.h`, 2026-10-31) — our payload mapping must be derived
independently: try anonymous `PROT_EXEC`, `MAP_JIT`, and the pattern by which
GPTK's arm64 D3DMetal code runs inside the translated game process (sanctioned
"use" per CLAUDE.md). One-line experiments, safe on this machine.

Next milestones, in order:
1. Payload page — anonymous arm64-executable mapping, public API only (see note
   above); prove native arm64 code can execute from it in-process.
2. The stub — a 4-byte branch at translate_popf (~0x2a7e8) / translate_pushf
   (~0x2abbc) into the payload, adding NT-bit store/read via the x17 spare bit;
   verify-then-install, sibling threads suspended, fail-closed on UUID/build
   mismatch (CLAUDE.md's conditions for the correction).
3. Validation ladder — nt_native 4/4, then the three-way probe (MacFinals /
   Proton on `ssh ubuntu` / Windows), Crab Champions canary, game last.

## Tooling in this directory

- `nt_native.c` — four-check harness. `clang -arch x86_64 -O1 -o nt_native nt_native.c && ./nt_native`
- `jitdump.c` — marker-wrapped pushf/popf/iretq/cli + full RWX/low-region dumper
  to /tmp/rosetta-re (read-only, safe). `MF_PAUSE=1` keeps it alive for vmmap.
- `wrap_raw.py` — wraps a raw dump in a Mach-O (correct vmaddr) for `otool -tV`.
- Analysis artifacts (this build only): /tmp/rosetta-re/{text.asm, libRosettaRuntime,
  jit-*.bin/.txt/.asm}. Regenerate with jitdump after a macOS update.
