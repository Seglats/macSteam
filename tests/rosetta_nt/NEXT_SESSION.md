# MacFinals: where we are (2026-11-09, end of session)

Start here after a context reset. Details live in the linked files.

## Newest: runtime `macfinals-47` — storm fixed in game, debug output follows Wine's model

- **The syscall storm is fixed in THE FINALS** (r21f, runtime -43): 99.99 % fast-path hits,
  stock SIGSYS 30k/s → 1-2/s, Rosetta exception thread 0 %. NOTES.md "r21f in the game".
- **Freezes: investigation PAUSED, not done** (Rasmus, 2026-11-09: not needed right now).
  0.3-2.2 s GameThread freezes, mostly while loading and in the first minutes of play. Ruled
  out: shader compilation (D3DMetal's cache persists between runs and stopped growing; slow
  pipeline builds only at startup) and Wine syscall cost. Cause unknown. Resume with
  `tests/finals_bisect/freeze_profile.py` (Time Profiler, no developer mode; works on probes,
  never yet captured a game freeze). NOTES.md "Freeze hunt, continued and PAUSED".
- **`macfinals-47`** = 46 (assembled with the reworked layer) + the reworked hook. Hook
  payload unchanged since r21f. 44/45 are BROKEN (layer imported libwinpthread-1.dll and did
  not load — never use them; installed 45 was removed). 46/47 verified: d3d12 probe loads the
  layer, channel output correct.
- **Debug output now follows Wine's model** (CLAUDE.md "Debug output"): channels
  `macfinals_hook`, `macfinals_dx12`, `macfinals_scan`; default err+fixme; release builds
  with `--no-debug-msgs` / `DEBUG_CFLAGS`. The nt-ring file / MACFINALS_HOOK_RING /
  MACFINALS_SYSC_DUMP / MACFINALS_D3D12_LAYER_LOG are retired.

## Current build: macfinals-42 (TF guard; the 2026-11-08 game crash fixed at probe level)

- **Newest runtime `macfinals-42`** (= unhooked 38 + r21e-2 hook), installed in
  `~/Library/Application Support/MacFinals/runtimes/`. Tree hooklib and the
  embedded hook are sha256-identical. ais gate on the embedded hook: done,
  ~50 ns/call, counters healthy.
- **r21e-2 = r21d + fail-closed TF guard** in the fast-path stub: live x17
  bit4 or ctx+0x444 → stock, counted at ctl[40] byte 7. This answers THE
  FINALS' first-run crash (2026-11-08 19:20: STATUS_SINGLE_STEP at published
  target+4 — guest TF live at the dispatcher entry; anti-debug stepping on
  the new-account path). Stock sigsys is TF-correct; the guard declines
  TF-live sites instead of fast-pathing them. Verified: ais parity with r21d
  (entered 220043 identical), TF probe accounting (`entered 33 = hits 29 +
  g2 2 + TF 2` vs r21d taking the armed call into the fast path).
- **Rules added this session** (NOTES.md "r21e"): insertions into
  ntfix_macfast need a register-liveness audit at the insertion point (the
  x23 bug — first build destroyed the live `_g_ctl` pointer and Wine spun);
  the stub-change regression gate is **ais in Wine**, capsite is vacuous
  (carrier entries all bail at guard 2); a process that can die needs the
  poll thread (`MACFINALS_HOOK_RING=1` or `touch /tmp/rosetta-re/nt-ring`)
  or its counters never print — a missing line is not entered=0.
- **2026-11-09 07:40 game run on -42: in game, no crash, but the fast path
  did NOT engage in Discovery.exe** — 26k-56k stock SIGSYS/s (rax
  0xffffffff), Rosetta exception thread 20-34 % CPU, low FPS. No hook
  counters for the game process (no poll thread). NOTES.md "First game run
  on macfinals-42".
- **07:50 rerun with the nt-ring switch: the chains DO run in the game**
  (entered 11.2M, ~28k/s) but all bailed at guard 7: THE FINALS has a
  process instrumentation callback installed. **r21f** handles it like
  sigsys_handler (restore_flags |= 0x10000) instead of bailing; probe
  `instr_cb_probe.c` shows identical callback behaviour to stock at 75 ns vs
  16.5 µs. **Newest runtime: `macfinals-43`** (installed). NOTES.md "r21f".
- **Next: THE FINALS on -43** (`RUNTIME=macfinals-43 PERF=1
  tests/finals_bisect/launch_main.sh avx msync`, keep
  /tmp/rosetta-re/nt-ring): expect hits ≈ entered, perf sigsys/s ~0,
  Rosetta exception thread idle — then judge FPS.

## ⛔ Read this first: the r20 kernel panics (2026-11-07)

The syscall-fastpath work ("r20": patch Rosetta's finished 16K syscall thunks
in place) **panicked the kernel twice** — two ntstub gate runs, two forced
reboots (18:20:00, 18:21:16). The fatal call was `mach_vm_read_overwrite` on a
translated anonymous exec region (the scanner's READ; the write path never
executed). Kernel data abort far=0x14, panicked task = the hooked process.
**Everything r20 is quarantined in hooklib.c behind `MACFINALS_THUNK_PATCH=1`
(default OFF).** Treat `mach_vm_*` on translated mappings as panic-capable
(read included); reads must use direct user-space loads behind a SIGBUS/SIGSEGV
handler. Full evidence and rules: NOTES.md sections "⛔ THE CRASH" and
"r20 (2026-11-07)".

**Rasmus's decision, 2026-11-07: fixing the syscall storm is REQUIRED for
success** ("the project may fail, but it can't be a success without fixing
this").

## ✅ r21 (2026-11-08): the stream insert WORKS in the carrier — v6 geometry

The r19 revival succeeded. Ground truth first (capsite carrier + ring), then
the mechanism that killed every earlier insert (a Rosetta fixup pass patches
whatever word sits at the pre-insert cursor X with a 26-bit immediate,
top-6 bits preserved → any live word there becomes undefined → the
translator assert), then the fix: **never execute X** (`insert8` v6 — a
sacrificial `bl #0` at X absorbs the patch and becomes the real relocated
bail target; the live chain starts at X+4 behind a `b #+8` at X-4).

- capsite ×8: exit 0, all markers exact, stub entered 11/12, no asserts.
- capsite3 scan-0: 36/36 intact chain copies; X holds a valid relocated bl
  in every copy (targets page+0x200 ×2, +0x204 ×1).
- Full mechanism, evidence, the fixup-table side-discovery and the scanner
  caution: NOTES.md "r21 (2026-11-08)"; design doc R19_SYSCALL_FASTPATH.md
  (implementation section rewritten for v6).
- hooklib.dylib/payload in the tree carry v6; nothing is in a runtime yet.

**Next steps, in order:**
1. ~~In-Wine test~~ **DONE 2026-11-08 (r21b) — result: chains placed but NOT
   executed.** The insert fires at Wine's syscall sites and complete, correctly-
   addressed v6 chains exist at exit — but ALL of them sit in one fixed RWX
   staging region (`0x200014000`, same address every process) and `entered=0`.
   Uncorrupted stock copies sit in a DIFFERENT region family (~`0x20869xxxx`,
   prot 3 = RW-not-exec). No scanned exec region contains either. Where the
   executed TBs live is now THE open question. Full record: NOTES.md
   "r21b (2026-11-08, afternoon)" — including the crash ledger (zero crashes
   this session) and the suspicious-but-plausible published target
   (0xc723a96/0xc722ede ≈ PE-ntdll base + offset — verify with
   read-dispatch-page.exe before any engagement).
2. Next moves are in NOTES.md r21b "Next session's first moves": region-list
   dump to find executed TBs; fix the STOCK census words-print bug; verify the
   published target. If executed TBs bypass the append-hook path entirely, the
   insert must move — **surface to Rasmus before building a new mechanism.**
3. If engagement is achieved: timing (~16 µs → ~2 µs), runtime `macfinals-41`
   (add_nt_hook as always), Crab Champions canary, then THE FINALS
   (`PERF=1 … avx msync`) — compare against R17_PERFORMANCE.md numbers.

Still-true constraints: no `mach_vm_*` copy/protection call on translated
mappings (the r20 panic class — r21's insert path uses only the emitter's
own buffer writes, nothing in that class); region walks allowed but NOT
concurrent with heavy JIT churn (r21 scanner caution — the walker can wedge
uninterruptibly; that is a tool problem, not a payload one).

## 🎯 r21d (2026-11-08, night): THE FAST PATH WORKS IN WINE — 33 ns/call

The r21c blocker chain is fully decoded and fixed (NOTES.md "r21d"). Every
fix stayed inside the approved design (guard corrections + one missing
precondition; no mechanism changes):

- **PE-image sites execute their chains** (ais5/ais6) — the r21b
  "second emitter" applies to ANON mappings, not PE images. Hot loops
  engage every call; no tier-up.
- **Guard 5b fixed:** TPIDRRO is the pthread TEB (mac_gsbase_switching=0),
  [pthread+0x30] = the Windows TEB → two-hop resolution, fail-closed
  (canonical + page-aligned + self-pointer before trust).
- **Guard 7 fixed:** [TEB+0x330] is never null under Wine (it holds
  &instrumentation_callback); the dispatcher derefs TWICE — so does the
  stub now; bails only when a real callback is installed.
- **Resume precondition found:** table[0]'s install is gated on bit9 of the
  32-bit pending word at ctx+0x1c8 (0x5908 `swp`s it and returns the old
  value). The stub queues bit9 itself — 32-bit ONLY: ctx+0x1cc is a
  separate live payload field (64-bit access = false-busy reads / payload
  zeroing writes; ais8-ais10).
- **ais11/ais12 result:** `entered 220043 hits 220038 bail5 0`, rax
  0xC000001C every call, markers intact, **33 ns/call** (stock 16-19 µs,
  Proton ~2 µs). The syscall storm fix works.

**Next, in order:**
1. Runtime `macfinals-41` via `add_nt_hook.py` (the tree's hooklib now
   carries r21d; assemble as always, never skip the hook step).
2. Crab Champions canary (`APP=774801`) — runtime soundness after the
   hook change.
3. THE FINALS with `PERF=1 … avx msync` (R17 setup): expect the
   exception-server load to collapse; compare GameThread kernel time and
   FPS against R17_PERFORMANCE.md. Watch `sysc` counters for in-game
   engagement (the game's sites are PE-image — they should engage like
   site1 did).
4. The 5-guard-2 pre-publish window (first ~100 ms stock) is accepted
   (matches today's behaviour); revisit only if the game trips on it.

## 2026-11-07 current build (supersedes the 2026-11-02 facts below)

- **Runtime `macfinals-40` installed** in `~/Library/Application Support/MacFinals/runtimes/`
  (= 38 + nt-hook; 38 = 33 + Wine syscall-fastpath publish + SIGSYS diagnostics,
  `R19_SYSCALL_FASTPATH.md` for the Wine side). macfinals-40's hook contains NO
  r20 scanner code (verified) — the shipped runtime cannot trigger the panic.
- Probe iteration uses the local `tests/rosetta_nt/hooklib.dylib` via
  DYLD_INSERT on macfinals-40; no runtime rebuild needed for hook-only changes.
- The 2026-11-02 "Current build" section below (macfinals-33 etc.) is history;
  the "What works" table and d3d12-layer paragraph remain accurate.

## Current build

- **Runtime `macfinals-33`** (2026-11-02, = 31 + AVX signal-context fix, `R18_AVX_CONTEXT.md`;
  in-game check with `avx` pending). Before it: `macfinals-31` (installed in `~/Library/Application Support/MacFinals/runtimes/`).
  Workspace `~/MacFinals-build-gcc/phase-2`. Not yet built into an app bundle; test with
  `tests/finals_bisect/launch_main.sh`.
- Nothing from this session is committed (main is at `81b0293`). Commit only when Rasmus asks.

## What works

| | Since | Where |
|---|---|---|
| Past the startup check (E111000D): PE side built with **MinGW-w64 GCC**, never llvm-mingw | `macfinals-19` | `(removed)` |
| **Skins have arms and neck**: d3d12 layer, Proton-equivalent constant-buffer rewrite | `macfinals-27` (confirmed in game) | `R15_SKIN_CURVE.md` |
| **No Steam TOS assert / in-game minidumps**: Wine fix for macOS `IP_RECVTOS` | `macfinals-29` | `R16_FIRST_LAUNCH_HANG.md` |
| Max logging that the anti-cheat accepts (`DEBUG=max`) | `macfinals-31` | below |

### The d3d12 layer in one paragraph

`d3d12_layer/` is a Wine builtin `d3d12.dll` in front of GPTK's d3d12.dll, which ships
byte-identical as `lib/wine/d3d12_gptk.dll` (outside the arch dir, because Wine matches
builtins by the PE's internal export name; a Wine placeholder reserves the name in system32).
It wraps the device (generic 16-byte forwarding stubs for all 128 vtable slots, so
ID3D12Device11/12 work; the game uses Device12) and rewrites DXIL at pipeline creation:
a local float array filled from consecutive constant-buffer floats and read with a dynamic
index becomes a direct `cbufferLoadLegacy` read, 0 outside the buffer's declared size, which is
what vkd3d-proton does. Structural matching, no shader list. Logs through Wine's debug
output (`pid:tid:macfinals-d3d12:`). `MACFINALS_SKIN_FIX=0` turns the rewrite off. Built by
`scripts/assemble_portable_runtime.py` via `d3d12_layer/build.sh`.

## Rules learned this session (do not relearn)

- **Never log the game with `warn+heap`, `trace+heap`, `relay` or `snoop`.** Plain `warn+all`
  made Wine set debugger-style heap flags and the anti-cheat quit with "disallowed tool". Use
  `warn+all,warn-heap` (that is what `DEBUG=max` does). `+seh` also changed behaviour once.
- `launch_main.sh` clears the environment: only variables it lists are passed on
  (`MACFINALS_SKIN_FIX` is; debug output is chosen with `DEBUG=` or a verbatim `WINEDEBUG=`).
- `launch_main.sh` defaults are the old CrossOver-like bisect baseline: **no MSync, no AVX**
  unless you pass `msync` / `avx`. The app enables both. Measure performance with both.
- The layer's log only shows in the game if it goes through Wine's debug output; a process
  started by Steam has no Windows stderr.
- Probes that start Wine against the real prefix join a running session; make sure no
  MacFinals wineserver is running first.

## Testing

```sh
# quit the launcher, Steam and the game first
RUNTIME=macfinals-31 DEBUG=max tests/finals_bisect/launch_main.sh          # everything safe to log
RUNTIME=macfinals-31 PERF=1 tests/finals_bisect/launch_main.sh avx msync   # performance counters
```

Quit Steam normally at the end: the script then collects Steam logs, new dumps, macOS crash
reports, the D3DMetal cache listing and the macOS log into `logs/<session>/`. `PERF=1` writes
`logs/<session>/perf/wine.<pid>`; find the game's file by its Windows thread ids.

`APP=steam` starts Steam only, `APP=774801` Crab Champions. `tests/finals_bisect/kwine.sh` stops
every Wine process on the Mac, any install (`-n` lists them).

Probes in `tests/windows_probe/`: `d3d12_factory_probe.c` (layer: device paths, Device12,
stubs), `recvtos_probe.c` (TOS control message; Proton gives `IP_TOS 0x88`),
`suspend_probe.c` (suspend/context of new threads), `qpc_probe.c` (QPC cost, RDTSC),
`avx_state_probe.c` (YMM state through exceptions and thread contexts; needs AVX advertised).

## Open problems

1. **Low FPS (CPU-bound).** `R17_PERFORMANCE.md`. First measurement (no MSync, so partly
   unrepresentative): game thread 68% of a core, half in the kernel; 154k
   NtQueryPerformanceCounter syscalls/s, but at 36 ns each that is under 1% (measured);
   Rosetta's exception-server thread 22% of a core while Wine sees only ~280 faults/s, so most
   exceptions are handled inside Rosetta. The NT hook forces `DISABLE_AOT=1` (cost unmeasured).
2. **Main-menu buttons sometimes do not open menus.** Cause unknown. Only anomaly found: a
   thread killed with "nested exception on signal stack" at `usr1_handler`'s first read of its
   signal context (`ntdll.so+0x3b67c`), 3 of 12 runs, also before this session's changes.
   `macfinals-31` logs details when it happens (`wine-nested-exception-diag.patch`).
3. **Crash spawning into the tutorial with AVX** (every AVX run, never without): null read at
   0x8. Cause found: Wine on macOS did not carry YMM state in signal contexts, so continuing
   from an exception did not restore it. Fixed in `macfinals-33` (`R18_AVX_CONTEXT.md`); confirm
   in game.
4. Steam's crash reporter once hung while suspending the game's threads (dbghelp). Its trigger
   is gone with the TOS fix; the hang itself is unexplained.
5. First launch in a prefix shows Denuvo's "issue with starting" dialog; the second goes in.

## Next steps, in order

1. Done 2026-11-02: QPC costs 36 ns on MacFinals (37 with the hook), RDTSC usable. Not the
   FPS problem; the user-mode QPC patch is dropped (`R17_PERFORMANCE.md`).
2. Performance run with the app's settings (`PERF=1 … avx msync`) to get representative numbers.
3. **`HOOK=0` does not work: the anti-cheat quits with "disallowed tool"** (2026-11-02, `macfinals-33`,
   light logging, no heap channels). The NT fix is still required, so `DISABLE_AOT=1` stays.
   Gameplay with the hook (`macfinals-33`, avx+msync): GameThread 309 ms user + 423 ms kernel per
   second, Rosetta's exception server 71 + 294 ms, 426k Mach calls/s, Wine sees ~34 faults/s.
   (`DISABLE_AOT` hardly matters for game code: Rosetta translates Windows PE code at run time
   either way.) Crab Champions (`APP=774801`, ~20 s each): **no exception-server load with or
   without the hook**, so the storm is specific to THE FINALS' process; the hook still costs
   ~30-40% on Crab's spin loop (yield calls 758k/s with, 1,058k/s without). Next: per-second
   counters in the hook (words handled, time spent) to measure its cost inside THE FINALS.
   **Main cost found (2026-11-02):** GameThread issues ~32,000 direct `syscall` instructions a
   second with rax=0xffffffff (+~1,500 real ones), ~16 us each under Rosetta (Proton 2 us),
   ~0.5 s/s. Wine already answers like Windows. Only lever left: Rosetta's `syscall`
   translation, outside the hook scope; waiting on Rasmus's decision. Details `R17_PERFORMANCE.md`.
   Smaller, Wine-fixable: ~35k failed fixed-address maps/s in `try_map_free_area` on workers.
4. Build an app bundle from the newest runtime once these settle; recheck Crab Champions.

## Boundaries that applied this session

Game files never touched (crash reports and logs the game writes are read only); the
anti-cheat's protected code is not analysed: count its effects, do not ask why; GPTK ships
unchanged; Rosetta is not modified on disk; the closed-source package is not copied.
