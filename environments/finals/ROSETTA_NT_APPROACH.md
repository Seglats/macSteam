# Rosetta NT-flag correction — approach and theory

Theory only — no code, no offsets, no byte patterns. This document says **how the
correction should be designed and staged** within the boundaries set in
[CLAUDE.md](CLAUDE.md). It does not override CLAUDE.md; where they disagree,
CLAUDE.md wins.

Audience: MacFinals maintainers and future sessions picking this work up.

---

## 1. The defect, precisely

Rosetta does not model the NT (nested task) EFLAGS bit (0x4000). Four observable
consequences, all reproduced with a plain x86-64 macOS binary and no Wine:

| # | Guest does | Real x86-64 CPU | Rosetta |
|---|-----------|-----------------|---------|
| 1 | `POPF` with NT in the pushed flags | NT becomes 1 in EFLAGS | NT silently dropped |
| 2 | `PUSHF` afterwards | NT visible in pushed flags | reads 0 |
| 3 | Reads EFLAGS from a saved exception context | NT present as stored | contexts always carry 0 |
| 4 | `IRETQ` with NT set | #GP(0) — no task switches in long mode | no fault, return proceeds |

There is no truthful shortcut. Ring-3 `POPF` genuinely can set NT on real
hardware, Windows passes all four checks, and the game's protection relies on
them. Anything that faked the checks would be spoofing, which is out of bounds by
principle — the fix must make the environment behave like the hardware it claims
to be.

Already landed and necessary-but-insufficient: `RTL_USER_PROC_IMAGE_KEY_MISSING`
(0x4000) in `ProcessParameters.Flags` —
[patches/wine-ntdll-image-key-missing.patch](patches/wine-ntdll-image-key-missing.patch).

## 2. Why only the translator can fix it

Walk every level the process can observe and ask who sees the flag:

- **`PUSHF`/`POPF`** are translated inline by Rosetta. They are not syscalls,
  not faults, not callbacks. Nobody — not Wine, not a dylib interposer, not a
  signal handler — is invoked. Only the *emitted arm64 code* decides what
  happens. If we do not influence emission, checks 1 and 2 are unreachable.
- **Exception contexts** (check 3) are *synthesized* by Rosetta when a fault
  happens in translated code. Wine receives the result and can, in principle,
  correct it — but only if it knows the true NT state, which only the translated
  stream can have recorded. So even check 3 bottoms out in emission.
- **`IRETQ`** (check 4) executed as guest code is translated like any other
  instruction; the #GP must be *emitted* (a test-and-trap on NT before the
  return). Wine's own `iretq` sites are irrelevant — the game's probe never runs
  through them.

This is the proof by exhaustion behind "Wine cannot fix it": every path to the
flag runs through code Rosetta generates at translation time. A working
correction must interpose on translation itself. Confirmed empirically: a build
that hooks the translator runs the game; everything short of that does not.

## 3. Constraints the design must satisfy

From CLAUDE.md and the recorded incident:

1. **In-process only.** No `task_for_pid` against other processes, no debugger
   entitlement, no `DevToolsSecurity` requirement, no resident loader process.
   The out-of-process debugger architecture is ruled out, full stop.
2. **Never write Rosetta's file-backed pages in-process.** `mprotect(PROT_RWX)`
   on libRosettaRuntime's TEXT from inside a translated process wedged the
   thread unkillably in the kernel and on 2026-10-31 froze the whole machine
   (forced power-off, no panic file). The deadlocked call happens before any
   AMFI verdict, so no entitlement or developer-mode setting helps. The
   known-freeze path — making the *shared* file-backed page writable in place,
   W+X — stays forbidden. Amended by Rasmus later on 2026-10-31: **machine
   crashes are an accepted cost on this machine**, on the single condition
   that nothing is ever written to a file that affects boot. All experiments
   stay in-memory (step logs to /tmp only); "never modify Rosetta on disk"
   (CLAUDE.md) stands. The authorized sequence is the §4 COW ladder,
   heartbeat-logged step by step, with every outcome recorded in the
   experiment ledger (`tests/rosetta_nt/NOTES.md`) so a guideline of what
   works and what freezes accumulates.
3. **Fail-closed.** An unrecognised Rosetta build must deactivate the correction
   and run unpatched (loudly logged), never corrupt translation. No partial
   installs.
4. **Scope: `POPF`/`PUSHF`/`IRET` flag handling only.** Nothing else in the
   translator is touched.
5. Anti-cheat boundaries intact: game files untouched, no analysis of the packed
   PE at `0x700000000000`, no spoofing, GPTK/D3DMetal not modified, no SIP
   changes.

## 4. Principles

The mechanics below are mach/ARM64 semantics, not secrets, and they align with the crash
rule:

- **COW before write.** If a file-backed code page must ever be modified, the
  kernel must privatise it first (`VM_PROT_COPY` on the protect call), so the
  write lands in an anonymous copy, never in the shared page. Note: this is the
  *only* sanctioned sense in which "modifying Rosetta code" can be safe — and
  doing it in-process on this machine is still forbidden by constraint 3.2 until
  proven elsewhere (see §5.4/S3).
- **Never W and X simultaneously.** Sequence is always: drop X → write → flush →
  restore X. RWX is never requested.
- **I-cache coherence.** ARM64 I/D caches are incoherent; every code write must
  be followed by an explicit instruction-cache flush (`sys_icache_invalidate`
  in-process), or another core executes stale bytes.
- **Freeze the target while writing.** Code pages are patched only while no
  thread can execute them. In-process, that means suspending all sibling threads
  of our own task (legal without entitlements) and patching from the one running
  thread.
- **Minimal footprint on Rosetta's image.** At most a handful of tiny branch
  stubs on its code; all real logic lives in a payload we allocate ourselves.
- **Payload in anonymous executable memory.** Allocated with the same mapping
  flag the process's own translated-code buffers use, positioned/relocated by
  our loader code; prebuilt position-independent arm64 (our Wine code is x86-64
  and therefore itself translated — the payload must not be).
- **Disable AOT caching.** Cached ahead-of-time translations bypass live
  translation hooks; the correction is unsound while the AOT path is active.
- **Verify everything before writing anything.** Every entry point is checked
  against precomputed expectations (its initial instructions), the runtime build
  is gated, and a self-integrity magic is required — all *before* the first
  mutation; any mismatch aborts to unpatched.
- **Never strand a half-patched process.** A teardown guard must ensure a failed
  install leaves either a fully-patched or a fully-unpatched process.

## 5. The design, in theory

### 5.1 Placement and gating

The correction lives inside **our Wine build**, initialised from the unix side
during startup — before any guest code beyond Wine itself has run. It ships
disabled and activates only behind an explicit opt-in environment variable, so
every existing user keeps today's behaviour unless they ask. All activity is
logged verbosely; this project's rule is that logging is a feature.

### 5.2 The four correction points

1. **Store** — translated `POPF` must write guest NT into a per-thread home.
2. **Read** — translated `PUSHF` must synthesise guest EFLAGS including NT from
   that home.
3. **Fault** — translated `IRETQ` must raise #GP(0) when NT is set (a
   test-and-trap emitted before the return; no trap when clear, so normal
   returns are unaffected).
4. **Carry** — exception-context pack (fault → guest CONTEXT) must OR NT in from
   the home, and unpack (guest setting a context) must accept it. *This half can
   live in Wine* (§5.5), because Wine sees every context — but only once a home
   exists to read.

### 5.3 Per-thread NT home

The home must be reachable from emitted code at runtime. Candidate designs, in
order of decreasing coupling to Rosetta internals:

- **A spare bit in the runtime's own packed-flags word.** Zero new state, and it
  automatically rides every path the runtime already round-trips — but it
  requires our own structural analysis to find a bit that is genuinely unused
  and stable across the builds we gate on. (Such spare bits do exist;
  which one is *our* finding to make.)
- **A slot in memory whose address emitted code can derive** — e.g. off the
  guest TEB pointer the translator keeps in a reserved register. More
  self-contained, more analysis of the register convention.
- **Our own TLS page** with the address computed at install time and baked into
  the emitted sequences. Most independent, most bytes emitted.

Choosing among these is an output of the structural-analysis task (§9), not a
precondition for it.

### 5.4 Getting control at translation time — the surfaces, ranked

This is the crux. Ranked by safety, with honest verdicts:

**S1 — data-only dispatch pointers in writable `__DATA`.** If any translation or
flag-pack path is reached through a function pointer in writable data, swapping
entries is an ordinary write to an already-writable page — no protection change,
no crash-rule concern, trivially reversible. *Verdict: **disproven, 2026-10-31.** Static
disassembly of both on-disk images (see `tests/rosetta_nt/NOTES.md`):
libRosettaRuntime has **zero indirect calls in its entire `__text`** — 5,938
direct `bl`, and all 23 `br` sites are switch jump tables in read-only
`__TEXT __const` (including the x86 opcode→emitter dispatch itself) except
three compiler-rt interpose thunks in `__data` (0x30 bytes total), nowhere near
flag emission. The runtime image's two `blr` are argument-passed callbacks. No
data-only write in either image can reach the flag paths.*

**S2 — rewriting emitted code in the anonymous JIT buffers after translation.**
The buffers are anonymous RWX — explicitly a sanctioned surface. *Verdict:
unsound as a primary mechanism.* Rosetta translates a block and executes it
immediately in the same thread; a rewriter (even a racing background thread)
cannot guarantee it lands before **first** execution, and a first-execution
failure is exactly the failure being fixed — the protection's very first
`POPF`/`PUSHF` round trip would already be wrong. Keep as a diagnostic tool
(dumping and studying emitted sequences), not as the interposition mechanism.

**S3 — in-process COW-privatised patch of the translator's code pages.**
Protect-with-copy → write stub → flush → restore, executed from our Wine code
with sibling threads suspended (§4). This is the only surface known to reach
translation time completely. *Verdict: the only known-complete in-process
mechanism — and as of 2026-10-31 05:10, **mechanically proven on this
machine**: the cowtest ladder passed all five tiers (own `__LINKEDIT` page,
own private exec mapping, Rosetta `__TEXT` data page, Rosetta `__TEXT` code
page, and a genuinely modified byte held live on the Rosetta image for 5 s
with no code-signing kill). Rasmus's decision the same night waived the
disposable-host requirement: crashes are accepted here, provided nothing ever
writes a boot-affecting file (in-memory only; never modify Rosetta on disk
regardless). The 2026-10-31 freeze was the *non-COW, in-place W+X* variant;
that path stays forbidden, and every run's outcome goes into the experiment
ledger in NOTES.md. What remains unproven is execution *through* a genuinely
patched code page and the anonymous arm64 payload mapping — engineering
milestones, not kernel-survivability questions.*

**S4 — out-of-process debugger.** Complete and demonstrated working, but ruled
out by CLAUDE.md (entitlements, DevToolsSecurity, resident debugger parent,
fragile detach choreography). Recorded here only so nobody re-derives it as if
it were new.

**Headline after the 2026-10-31 survey, decision and cowtest ladder: S1 is
disproven — no data-only dispatch exists in either Rosetta image. S3 is the
only known-complete in-process mechanism, its kernel-survivability is now
**proven on this machine** (ladder 5/5, see the experiment ledger), and what
remains is engineering: the stub/payload design, the native-execution bridge,
and fail-closed install/teardown.

### 5.5 The Wine-side half (shippable independently, inert by design)

Regardless of which surface wins, Wine can own point 4: once a home exists, its
signal/context plumbing ORs NT into every guest CONTEXT it builds and accepts it
on set. Build and land this first, behind the same opt-in gate, with the home
absent: it is then dead code, testable, and the risky half shrinks to emission
alone. This staging also keeps the diff reviewable: truthful-context plumbing in
Wine (our code, our licence) versus a tightly-gated translator interposer.

### 5.6 Payload properties

If/when an interposer is built: prebuilt arm64 machine code (never compiled
guest-side, never translated), position-independent or self-relocated once into
its anonymous page; every call back into original runtime behaviour goes through
pointers captured at install time; buffer growth during emission uses the
runtime's own helpers rather than reimplementing them.

### 5.7 Concurrency, timing, AOT

- Install window: as early as our Wine code runs, with all sibling threads
  suspended for the duration of any code write; single writer; verify-after-write.
- AOT caching must be off for the process before guest code is translated, or
  cached translations will bypass the interposer. Whether disabling it late (at
  our init, after Rosetta's own startup) is early enough is an open question —
  answer with the probes, not by assumption.
- If the home is a runtime-internal bit, its initialisation on new threads must
  be understood (thread creation path) so NT state is not inherited stale.

### 5.8 Fail-closed ladder

1. Gate the whole feature on the opt-in variable; default off.
2. Verify environment: exact Rosetta build(s) we have analysed; anything else →
   log loudly, run unpatched.
3. Verify every interposition point against precomputed structural expectations
   before any write; one mismatch → abort to unpatched. No partial installs.
4. Verify again after writing (read-back), before releasing threads.
5. Teardown guard: any failure between steps 3 and 4 restores or terminates —
   never leaves a half-patched translator.
6. The launcher surfaces all of this in its logs; a deactivated correction is a
   first-class, visible outcome.

## 6. Validation plan

- **Unit of truth: the four checks.** The Windows probe
  ([tests/windows_probe/](tests/windows_probe/)) already encodes them; it must
  pass 4/4 under the correction, 0/4 (today's behaviour) without it — on
  MacFinals. Native repros live in [tests/rosetta_nt/](tests/rosetta_nt/).
- **Differential:** the Proton reference machine passes 4/4 on the same
  wine-11.0 base — use it to prove we changed *only* NT behaviour and nothing
  else regressed.
- **Canary:** Crab Champions must still run (runtime soundness after any
  translator-adjacent change).
- **The game:** THE FINALS launch is the goal, never the test harness — probes
  first, canary second, game last.
- **Disposable host:** any experiment involving protection changes on Rosetta
  pages happens on a VM or sacrificial Mac first, with a documented
  reproduction of the freeze as the negative control. The working machine is
  not a lab for this.

## 7. Experiment safety on this machine

Allowed (no Rosetta page is written or re-protected):

- Static analysis/disassembly of the on-disk Rosetta binaries.
- Dumping and studying the JIT region (see `tests/rosetta_nt/`).
- Allocating our own anonymous memory, including executable-mapped test pages.
- Probes: native x86-64 binaries, the Windows probe, the Proton box.

Forbidden: any `mprotect`/`mach_vm_protect` or write targeting Rosetta
file-backed pages (TEXT, `__DATA_CONST`, the `.aot` mappings,
`/usr/libexec/rosetta/runtime`) from inside this machine, in any process.

## 8. Non-goals

- No broadening beyond THE FINALS; no DX12-title matrix.
- No anti-cheat analysis, no spoofing of CPU vendor or environment, no hiding
  Wine. The correction makes the CPU model *more* truthful, never less.
- No modification of GPTK/D3DMetal, Rosetta on disk, or SIP.

## 9. Open questions, and what answers each

| Question | Answered by |
|---|---|
| Does S1 exist — any data-pointer dispatch on the flag/translate paths? | Our own disassembly of on-disk libRosettaRuntime; zero risk, next concrete task |
| Which per-thread home design is viable (§5.3)? | Same analysis pass; needs the register/state layout of the translator |
| Is a spare packed-flags bit stable across gated builds? | Comparing the builds we gate on, on disk |
| Is in-process COW-patching survivable on this kernel? | **Answered 2026-10-31: yes** — cowtest ladder 5/5 (experiment ledger, NOTES.md) |
| Does disabling AOT at our init time leave cached translations behind? | Probe experiment, safe on this machine |
| Does a self-issued executable mapping from translated code work (payload page)? | A one-line anonymous-mapping experiment, safe on this machine |

## 10. Evidence ledger

- NT semantics missing in Rosetta: reproduced, no Wine involved (see
  [tests/rosetta_nt/](tests/rosetta_nt/)).
- Wine innocent: same wine-11.0 passes 4/4 on real x86-64 under Proton.
- A translator hook runs THE FINALS: confirmed by Rasmus on such a build.
- IFEO `0x4000` fix: landed, necessary, not sufficient.
- `NtWriteFile` status divergence (`c0000024` vs `c0000008`): tolerated;
  Proton shares it; upstreamable, not ours to fix for this goal.
- In-process `mprotect(RWX)` on Rosetta TEXT: machine-freezing kernel deadlock,
  2026-10-31 — the standing prohibition behind §3.2 and §7.
- S1 data-only dispatch: disproven 2026-10-31 by disassembly (zero indirect
  calls in the translator; opcode dispatch is read-only switch tables).
- In-process COW sequence on Rosetta's own pages: **proven survivable
  2026-10-31**, cowtest tiers 1–5, including a live modified byte on the image
  with no asynchronous code-signing kill. Translated processes use 4K pages and
  map the Rosetta JIT region immediately after the main executable — any
  protection window must be region-clamped.
