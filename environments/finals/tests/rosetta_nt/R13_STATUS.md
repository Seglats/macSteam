# r13 — slot table and SIMD clobber fixed; probe 4/4, game still E111000D

## Current state (summary, kept up to date)

- **Fixed:** matcher slot table (§1) and SIMD register clobber (§2, "Root cause"). With
  both fixes, Steam starts under the full hook and `macfinals-probe.exe` passes all four NT
  checks inside Wine, matching Windows.
- **Not fixed:** THE FINALS still exits with `E111000D`.
- **Step 1 done (2026-11-01, two runs, macfinals-7 Wine + test hook):** inside the real
  game process (`Binaries\Win64\Discovery.exe`), every real pushf (8) and popf (17) was
  fixed. The "misses" are false heads: the iret one is a *read* of CS (`ldrh
  w22,[x18,#0x176]` then `mov w2,w22`, no `iretq` translated at all), and the two gather
  timeouts are `movz w1,#0x202` call arguments. It still exits `E111000D`, so by the
  step-1 decision tree this goes to **step 4: NT is not the whole story.** (Miss shapes are now
  logged to inslog: kind 4 = iret disarm, kind 5 = gather timeout.)
- **Step 4 started: segment/TLS differential** (`tests/windows_probe/segment_probe.c`, new
  tool; `macfinals_probe.c` untouched). Built with
  `~/MacFinals-build/toolchains/llvm-mingw-20251216-ucrt-macos-universal/bin`.
  - gs→TEB, gs:[0x60]→PEB, gs:[0x58]→TLS pointer, stack bounds: **identical** on MacFinals
    and Proton, main thread and second thread. The plain "gs is the TEB" reading of the
    GS/TLS suspect is cleared.
  - **CS/SS differ, MacFinals only:** live `mov` and every context (exception,
    `RtlCaptureContext`, `GetThreadContext`) give cs=2b ss=23 (macOS's selectors). Proton
    gives 33/2b, like Windows. Proton's other differences from Windows (fs=0, live ds/es/gs=0)
    are harmless, because the game runs there.
  - It cannot be fixed truthfully within scope. An `iretq` to cs=0x33/ss=0x2b kills the
    process: `rosetta error: invalid gdt selector index 6`. Rosetta models macOS's GDT.
    Reporting 0x33 from Wine while the CPU runs on 0x2b would be inconsistent faking
    (boundary 2). Changing Rosetta's GDT model is outside the POPF/PUSHF/IRET green light.
- **Step 4 finding: on MacFinals, `gs:` is not the TEB** (2026-11-01, probes vs Proton):
  - `segment_probe.exe` scan: 36 of 768 qwords differ between `gs:[off]` and `TEB+off`
    (Proton: 0). Among them StackBase/StackLimit (0x08/0x10), process/thread id
    (0x40/0x48) and LastErrorValue (0x68), which read macOS's thread data instead. Only the
    fields Wine copies into macOS's TSD (0x30, 0x58, 0x60) are right, which is why earlier
    checks passed.
  - `gsregion_probe.exe`: just as wrong in loader-mapped code, RWX, RW→RX, section views,
    a high fixed address and a second thread, **with or without D3DMetal loaded**. So
    registering more code regions with GPTK would not fix it.
  - `tls_alloc_probe.exe`: first `TlsAlloc` = 65 (Proton 2), 1023 slots (Proton 1085).
    CrossOver's "CW HACK 18756" (`dlls/ntdll/loader.c` ~5187) fills all 64 fast TLS slots on
    macOS, *because* gs is macOS's TSD and `gs:[0x1480+8*i]` would land in macOS data.
    Allocations at/near `0x700000000000` behave the same as Proton (collisions cleared).
  - Cause in source: `signal_x86_64.c` never switches GS on macOS. The handler and
    dispatcher GS switching is `#ifdef __linux__` only; macOS gets the three-field copy
    (~line 3082) and nothing else.
  - **Feasible** (`/tmp/claude-r13/gsswitch.c`): under Rosetta, macOS's own trap
    (`syscall` 0x3000003, thread_fast_set_cthread_self) points the real GS base at the
    TEB. Reads are then correct, switching back restores macOS, the process survives, and
    it costs ~160 ns per switch.
  - **Proposed fix (own design, new patch at the end of `WINE_PATCHES`):** GS base = TEB
    while Windows code runs, macOS TSD while Unix code runs. Switch in the syscall and
    unix-call dispatchers (entry: to macOS TSD, `pthread_teb` is already recorded at TEB
    0x320 / Instrumentation[0]; return: to TEB). Switch in signal handler entry (to macOS)
    and when resuming Windows code from a handler (to TEB). Also on user callbacks
    (KiUserCallbackDispatcher, both directions) and at thread start. Once that holds, drop
    the CW HACK 18756 TLS block. Opt-in per process first (env/launcher, Discovery.exe), like
    `tls_alloc_probe` must match Proton, then Crab Champions, then THE FINALS.
  - Main risk: macOS code running while GS = TEB (a signal arriving in Windows code before
    the handler switches back, `_sigtramp`, or anything libsystem does before
    `init_handler`). That code would read TEB fields as pthread data.
  - **Implemented 2026-11-01: `patches/wine-macos-gsbase-teb-switch.patch`** (appended at
    the end of `WINE_PATCHES`), own design, informed only by Wine's own structure:
    - **ON for every process** (Rasmus's call: behave like Linux Wine everywhere, Steam
      included), not opt-in per game. `MACFINALS_NO_GS_TEB=1` disables; `mac_gsbase_switching`
      is also cleared off Rosetta (fail closed, the trap is only known good there).
    - Mechanism: machdep trap `syscall 0x3000003` (thread_fast_set_cthread_self64), what
      pthread itself uses; clobbers only rax/rcx/r11, ~160 ns per switch (both verified in
      `/tmp/claude-r13/gsswitch.c`).
    - Switch points, all in `dlls/ntdll/unix/signal_x86_64.c`: dispatcher + unix-call
      dispatcher entry (→ macOS base, after the kernel-stack switch, mirroring the Linux
      FS block; rcx rebuilt from rbp), shared `__wine_syscall_dispatcher_return` label
      (→ TEB, saves rax+rcx across the trap) and the unix dispatcher's fallthrough return,
      `call_user_mode_callback` before `jmpq *%rcx` (saves rcx = callback), `init_handler`
      (→ macOS) and `leave_handler` (→ whichever the resumed context runs with, decided by
      `is_inside_signal_stack(RSP) || is_inside_syscall(RSP)`), which covers every handler
      exit including `restore_context`, `setup_raise_exception` and the sigsys path.
      Thread start needs nothing: `signal_start_thread` jumps to the return label.
    - `NtCurrentTeb()` in Unix code is `pthread_getspecific`, so the new C helpers use a
      `gs:0x30` read instead — the field is mirrored into the macOS TSD at thread start
      (init_syscall_frame keeps doing that), so it yields the TEB with either base. The
      mirrors stay: they are the fallback for any transition we missed.
    - **Fail-soft:** any Windows↔Unix transition not switched leaves that code exactly as
      today (macOS base + three mirrors), which everything tolerates — the patch can only
      make gs reads more truthful where it fires.
    - Second file, `dlls/ntdll/loader.c`: CW HACK 18756 (TlsExpansionSlots prealloc +
      TlsBitmap burn) is skipped when the switch is on. The marker was first a magic in
      `Instrumentation[1]`, which D3D12-loaded code dereferences; it now lives in
      `LdrSystemDllInitBlock.mac_gsbase_teb` (`wine-macos-gsbase-loader-marker.patch`).
      With the switch off the hack runs, so disabling the switch also restores today's
      TLS layout.
    - **Blocker found 2026-11-01, see `R13_GS_D3D_CRASH.md`:** with the switch on, every
      process that loads D3D12 crashes, because GPTK calls macOS code directly from
      Windows threads, skipping every switch point.
    - Gates: `segment_probe` gs_scan 0 diffs, `tls_alloc_probe` first TlsAlloc = 2 (Proton
      parity), `gsregion_probe` all same, `macfinals-probe` 4/4 (hook + patch together),
      then Steam, Crab Champions, THE FINALS.
    - Residual risks accepted: a hypothetical SIGSYS on our traps would confuse
      `sigsys_handler`'s +0xb skip (the trap is a real pthread syscall, verified working);
      unix C running between leave_handler's switch and sigreturn is only the handler
      epilogue (Wine builds with `-fno-stack-protector`, so no gs-reading canary).
- **Shipped runtime at risk:** macfinals-7 (MacFinals-11) was built before the SIMD fix. Its
  converters may clobber guest XMM registers on every exception. This is unverified for the
  shipped dylib, so assume it does. Ship macfinals-8 before more game testing through the
  app, or set the hook off in 11.
- **The §2 bisect is misleading.** It blamed the scatter insert; the real cause was SIMD
  clobbering. Anything below that tells you to fix "the bad scatter match" is out of date.

Status brief, 2026-11-01. Follows `E111000D_DEBUG.md`. Runtime macfinals-7 / MacFinals-11.app
are unchanged; everything below was tested with `tests/rosetta_nt/hooklib.dylib` through
`MACFINALS_NT_HOOK` on `Contents/Helpers/ow2-setup`. No commits.

## 1. Root cause of "the hook never fixes anything": the slot table (cause A, confirmed)

The matcher had 16 slots, each claimed for life by the first buffer object it saw. A Wine
process uses all 16 before any program code runs, and every word after that was dropped.

Measured with the new diagnostic counters, `macfinals-probe.exe` under the hook:

| | old table | new table |
|---|---|---|
| words dropped for lack of a slot (`nosl`) | 151350 of 155046 | 0 |
| pushf heads seen / gathers fixed | 13 / 3 | 13 / 12 |
| msr NZCV seen / scatters fixed | 13 / 2 | 13 / 8 |
| iret anchors seen / iret fixed | 2 / 1 | 2 / 2 |

Correction to `E111000D_DEBUG.md`: the counters count **translations, not executions**.
Wine's dispatcher is translated once, so small counts are normal and "should show
thousands" was wrong. The table being full is still the real cause.

### The fix (`payload_arm64.c`)

- 64 slots, each with a lock held for the whole step, so a takeover can never hand one
  stream's half-built chain (and its registers) to another stream.
- A slot is claimed only on a chain-head word (`movz #0x202`, `movz #0`, or the iret CS load).
  Any IDLE slot can be taken over.
- The rule that a backwards cursor means a recycled buffer, and the chain resets, is kept.
- The ring now wraps (masked index) instead of saturating, so it holds the most recent
  words. The r12 guarantee that nothing is written past the end still holds.

### Probe result with the fix (`macfinals-probe.exe`, scratch prefix)

| Check | no hook | old hook | new hook | Windows |
|---|---|---|---|---|
| POPF sets NT, PUSHF reads it | NT=0 | NT=0 | **NT=1** | NT=1 |
| exception context keeps NT | NT=0 | NT=0 | **NT=1** | NT=1 |
| IRETQ with NT faults (#GP) | 0 | c0000005 | **c0000005** | c0000005 |
| NT injected via context survives | NT=0 | NT=0 | **NT=1** | NT=1 |

Only the two known NtWriteFile differences remain (tolerated, see CLAUDE.md). The
three-way comparison against Proton on `ssh ubuntu` has **not** been rerun yet.

## 2. New problem: Steam crashes with the fixed hook

With the full fixed hook, `steam.exe` dies about 1 s after start:

- `EXCEPTION_ACCESS_VIOLATION` at `0x14004C48C` in steam.exe, writing near address `0x41`.
- Then crashhandler64 and steamerrorreporter64 run (dump
  `Steam\dumps\crash_steam.exe_20261101070316_1.dmp`).
- No window appears, so neither the user nor the gameDebug Play path ever reaches the game.

The old hook did not crash Steam, because it hardly ever inserted anything.

### Bisect (Steam start only; pass = steamwebhelper is up)

| fixups disabled | result |
|---|---|
| none | CRASH |
| gather + scatter + iret | OK, 4 s |
| gather | CRASH |
| iret | CRASH |
| **scatter** | **OK, 4 s** |

> **Superseded.** This bisect pointed at the scatter insert, but the real cause was SIMD
> clobbering (see "Root cause" below). Disabling scatter only stopped the crash because it
> stopped the scatter chain from arming, and arming was the vectorised code. The three
> mechanisms listed here were all disproven. They are kept as a record.

**The scatter (popf) insert causes the crash.** Gather and iret are clear. Real Windows has
NT and Steam runs there, so the crash comes from the insert, not from NT. The likely
mechanisms are:

- **False match:** the 6-word scatter head followed by an `msr NZCV` within 16 words matches
  something that is not a popf. 26 `msr NZCV` were seen in steam.exe against 8 scatter heads.
- **Live temp:** the `lsr wA,wS,#14` writes a register (wA, taken from the head's first
  `lsr`) that is still live at the `msr` in some shapes.
- **Wrong source:** wS is not the popped flags at the `msr` in some shapes.

### Bisect controls (diagnostics only)

The launcher builds Wine's environment from an allow-list and drops unknown variables, so
the controls come from a file. Delete it after use.

```sh
printf 'scatter' > /tmp/rosetta-re/nt-off   # any of gather, scatter, iret, comma-separated
rm /tmp/rosetta-re/nt-off
```

The payload reads ctl[28] (gather off), ctl[29] (scatter off) and ctl[15] (iret off) when a
chain arms. `MACFINALS_NT_OFF` works too, but only when calling `bin/wine` directly.
Arm script: `scratchpad/arm.sh <label>`. It runs a clean slate, launches the helper and
reports CRASH or OK within 120 s.

### Root cause (found 2026-11-01): the payload clobbered SIMD registers

Not the insert at all. Further arms, all crashing: NT always 0, second word a
NOP, no insert (log only), and finally match-only with no side effects at all,
even with gather off. Burning 1000 iterations per word with every chain off did
not crash, so timing is not the cause either.

The append stub saves x0–x18/x30 only, but `-O2` had vectorised parts of
`ntfix_append` (slot init on chain arming, the `note_event` shift: `q0`/`q1`/`d0`)
and of `ntfix_compose` (`v0`–`v4`). Arming the scatter chain clobbered q-registers
that Rosetta holds live across the 0x18a0 call. The converters could also clobber
guest XMM state.

Fix: build the payload with `-mgeneral-regs-only`. `mkpayload.py` now refuses
any payload whose disassembly touches FP/SIMD registers (verified: refuses
the old build, passes the new one).

| gate | result |
|---|---|
| host_payload_test | 4× PASS |
| Steam, full hook | **starts, webhelper 6 s**; steam.exe 8 scatter + 3 gather fixups |
| macfinals-probe.exe | **4/4 NT checks OK**, only the two known NtWriteFile diffs |
| THE FINALS | still **E111000D**: the real game process (`Binaries\Win64\Discovery.exe`) gets the hook installed, shows the dialog and exits with `NtTerminateProcess(-1, 0xE111000D)`; the parent launcher saw 82 scatter + 12 gather fixups, 0 dropped, 0 iret fixups (3 misses) |

Open: the game process exits via NtTerminateProcess, so its exit counters are
never written. The next arm needs `/tmp/rosetta-re/nt-ring` set so the 50 ms
ctl snapshots (`ctl-<pid>.bin`) show what fired inside that process: whether
the matcher misses the game's code (iret chain especially), or NT is not the
whole story.

## 3. Other changes in the working tree

- `hooklib.c`:
  - prints a `diag` line at install and at exit (ctl[16..27], layout in `payload_arm64.c`)
  - `MACFINALS_HOOK_RING=1` dumps the ring oldest-first to `/tmp/rosetta-re/ring-<pid>.bin`
    at exit (direct `bin/wine` only, for the same allow-list reason)
  - reads the bisect controls described above
  - is built with `-mmacosx-version-min=26.0`
- `host_payload_test.c`:
  - ctl is now 32 slots
  - new step 8: 1000 separate buffers, one iret tail each, all must fire. It fails on the old
    payload (10 of 1000) and passes on the new one. Steps 1–7 still pass.

## 4. Next steps (rewritten after the SIMD fix; replaces the scatter-hunt plan)

The probe already matches Windows, so more probe work will not move the game. The
question is what happens **inside the game process**. Do these in order. Each step has a
result that decides the next one.

1. **See inside the game process.** One launch with `/tmp/rosetta-re/nt-ring` set, so the
   50 ms ctl snapshots (`ctl-<pid>.bin`) record what fired before `NtTerminateProcess`.
   Read the last snapshot for the `Binaries\Win64\Discovery.exe` pid:
   - iret anchors seen but **missed** → step 2.
   - scatter/gather/iret all fixing with **0 misses** and still E111000D → step 4.
   - heads seen but chains **armed-out** or broken → step 3.
2. **Make the iret chain work on real code (cause E + C).** The game launcher had 3 iret
   anchors, 0 fixed, 3 missed. Two parts:
   - Capture the missed shapes (ring dump around each miss) and add them to
     `host_payload_test` as failing cases *before* changing the matcher.
   - Replace the hard-coded `sub x22,x1,#2` with the iretq's real descriptor. Until then,
     any fixed iret reports #GP at the wrong address, which the check can still see as wrong.
   - The probe's iret check only passes because its return target sits 2 bytes after the
     `iretq`. Add a probe variant with a far-away target (needs Rasmus's OK; he has
     rejected probe edits before). Otherwise put the variant in ntstub.
3. **Fix gather/scatter misses** the same way: ring capture, a failing host test, then the
   matcher change. The launcher showed 4 of 16 gathers armed but never completed.
5. **Ship macfinals-8 / MacFinals-12** as soon as step 1 is done (the SIMD fix alone makes
   it safer than macfinals-7): the fixed `lib/macfinals-nt-hook.dylib`, a regenerated
   manifest, a new app. Never overwrite macfinals-7.

Gate after **every** payload change, in this order. Do not skip ahead to the game:
`host_payload_test` → `mkpayload.py` (refuses SIMD) → probe 4/4 → Steam starts (webhelper
up) → Crab Champions (canary) → THE FINALS. Run the Proton three-way once before shipping.

Smaller items still open:
- Log `hook installed but never matched` at exit when a process translated heavily and
  matched nothing. The `diag` line has the data; nobody reads it unless asked.
- Stale-NT hazard: if a scatter misses the `popfq` just before Wine's own `iretq` but the
  iret insert fires, Wine's own `iretq` would #GP. Once iret fixups fire, cover this case
  in `host_payload_test`.

## Lessons from r12/r13 (avoid repeating)

- A clean standalone run (ntstub, host tests) proves the mechanism, not the integration.
  Every hook bug so far (deaf slot table, SIMD clobber, CEF stall) appeared only in real
  Wine processes.
- A bisect shows which switch stops a crash, not why. Disabling a chain also disables the
  code that runs when it arms. Confirm a cause with a run that changes only that cause.
- Payload code runs inside Rosetta's translator with only x0–x18/x30 saved. Never let the
  compiler use FP/SIMD or callee-saved registers there.
- Test a new app only after quitting the old one. All bundles share one bundle ID (see
  CLAUDE.md).

## Do not

- Analyse the anti-cheat image or scan THE FINALS' files (CLAUDE.md boundaries 1 and 3).
  The ctl snapshots show our hook's behaviour, not the anti-cheat's, which is why step 1 is
  allowed.
- Upstream rosettax87_jit is for study only.
- Write the disable-AOT byte or anything else outside POPF/PUSHF/IRET flag handling without
  asking Rasmus.
- Ship or test through the app with macfinals-7 while its hook may still clobber SIMD.
- Leave `/tmp/rosetta-re/nt-off` lying around: it changes every hooked process.
