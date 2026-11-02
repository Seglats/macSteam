# Next session — start here (written 2026-11-01)

## Result of this plan (read first; the sections below are the original plan)

Steps 1–4 are done and step 5.1 is built: runtime **macfinals-13**, app
**`runtime/build/MacFinals-12.app`**. Nothing is committed. Step 5 is complete; the verdict is below.

### Verdict, 2026-11-01 14:12: `%gs` was not the only blocker. Stop chasing it.

Run on MacFinals-12 / `macfinals-13` with `gameDebug` on:

- **THE FINALS: E111000D.** Rasmus saw the "Conflicting software detected" dialog at 13:59, and
  the 14:11 debug log has `NtTerminateProcess(ffffffffffffffff,e111000d)`. No `table full` line,
  so regions were not being dropped. This is the outcome §2 said to record.
- **Crab Champions: works.** Rasmus checked in-game that the camera and movement work and that
  it looks correct; the game loads straight in, with no main menu (exit 0, 21 s session at 14:08). Steam launches
  `steamerrorreporter64.exe` for it after about 4 s and writes an `assert_*.dmp`. Every Crab
  session in the log back to 2026-10-29 shows the reporter, so this is not new.
- **Steam: works, and quit cleanly** at 14:10 with 0 `server_register_wait` errors. So the spin
  below is not on every exit; it follows the launcher going away first.
- **Proton three-way of `macfinals-probe`:** every line matches except CPUID. Same two NtWriteFile
  diffs on both.

Keep the patches: they pass the canary, reach Proton parity on `%gs` and TLS, and make the
environment more truthful, whether or not they were needed for this error.

**Remaining measured differences from Proton:** CPUID identity (avx 0, avx2 0, fsgsbase 0) and
CS/SS selectors (see `R13_STATUS.md`). Probes only cover what was thought of, so other CPU
behaviour may differ too.

**Next steps are now in `(removed)`** (AVX toggle → host-collision fix → CPU
probe). The paragraph below is kept as originally written.

**Suggested next step, not started:** a new generic CPU-behaviour differential probe, Rosetta
against the real CPU on `ssh ubuntu`, in a new file. It must not be derived from the anti-cheat
(boundary 3). Any fix outside `POPF`/`PUSHF`/`IRET` is beyond the current Rosetta green light, so
it needs Rasmus's decision first.

### First real run, 2026-11-01 13:58–14:00 (MacFinals-12, `macfinals-13`, debug logging off)

- **Steam: works.** It started, its UI ran and it launched the game. `err:` counts otherwise look
  like the old build's. No `table full` line in the Steam or game log.
- **THE FINALS: no visible change.** PIDs 1544 and 1616 each exited after about 12 s, exit code
  -11 in Steam's `gameprocess_log.txt`. `macfinals-7` at 13:55 did the same (16 s and 11 s, -11).
  -11 is the most common exit code in that log's history and appears before this work too.
- **The launch process's hook counters:** gather 11, scatter 78, iret fixups 0, misses 3, in line
  with the earlier 12 / 82 / 0 / 3.
- Debug logging was off in this run; see the verdict above for the run with it on.

### New problem: `steam.exe` spins after the launcher quits

The launcher exited at 14:00:14 and the wineserver at 14:00:15. `steam.exe` (pid 61887) stayed
alive at about 150% CPU, logging `err:sync:server_register_wait Failed to send server register
wait: 0x10000003` (`MACH_SEND_INVALID_DEST`, dead server port) from `dlls/ntdll/unix/msync.c` in a
tight loop. The log reached 1.6 GB (roughly 14 MB/s or more) before the process exited on its own within 4 minutes; `steamwebhelper_real.exe` (pid 61920) stayed behind at 0% CPU. The errors begin
at server exit, so they are not a failure during play. The `macfinals-7` session quit at 13:56
without it, but that is one sample each, so a regression is **not established**. Its cause is
unknown. Worth checking: does a client spin like this on any abrupt server exit, and how did
each session end?

Plan changes, all from measurements:

- **GPTK's region table holds 1024 entries and silently ignores the rest, repeats included.**
  `register_non_native_code_region` costs a flat ~230 ns and never reports this, so the
  "dedupe optional" note in step 1 was wrong. Registering per `VirtualProtect` (macfinals-10/11,
  superseded) fills the table in moments. Found by `exec_region_probe`: everything after 100k
  flips read DIFF. `capacity` mode: region 1024 reads DIFF.
- **Fix: `wine-macos-exec-region-budget.patch`.** The loader's zerofill segments
  (`0x1000-0x200000000`, `0x7ff000000000-0x7ff001ff0000`) are registered whole at startup, 2
  entries. Outside them, whole views once each, deduped in a Wine-side table, `err:` when full.
  Result: ~132k live regions before the `err:` line.
- **Registration runs after `virtual_mutex` is released**, at the exits of
  `allocate_virtual_memory`, `NtProtectVirtualMemory` and `virtual_map_section`, not in
  `mprotect_range`. Builtin and system views are excluded, matching CW HACK 22434.
- **Assembled runtimes have no hook and are hardened, so dyld strips the insertion.**
  `scripts/add_nt_hook.py` is the missing build step. See the build cycle below.

Build cycle now: `assemble --version macfinals-N` → `add_nt_hook.py --workspace $W
--base-version macfinals-N --version macfinals-N+1` → `build_native_app.py` on N+1. Without the
middle step the app silently ships with no NT fix. Next free: runtime macfinals-14, app
MacFinals-13.

Probe results, macfinals-13 with hook, GS switch off, region variable set:

| Probe | Result |
|---|---|
| gsregion, plain and `d3d` | all same; device created; exit 0 |
| segment | gs scan 0 of 768, both threads |
| tls-alloc | first TlsAlloc 2, 1085 indices: Proton parity (was 65, 1023) |
| macfinals-probe | NT 4/4; only the two known NtWriteFile diffs |
| exec-region (new), plain and `d3d` | 0 failures; same as Proton line by line |

Open:

1. **Unexplained: macfinals-probe gave empty stdout and exit 1 three times** (macfinals-13, hook
   on, `WINEDEBUG=-all`). Not reproduced in 37 further runs on macfinals-13 or 37 on macfinals-11
   (including gaps from 0 to 16 s). Not shown to be caused by the patches, not cleared either.
   If it recurs, keep stderr and compare with `MACFINALS_NO_EXEC_REGIONS=1`.
2. **Freed views outside the reserved areas stay registered** (no unregister). A native x86 dylib
   mapped there later would read the TEB through inline `%gs`. Needs exec memory past the 8 GB,
   then a free, then a dlopen at that address. Accepted as unlikely; not tested. A dedicated
   tombstone does not work: `map_fixed_area` then refuses re-allocation at that address.
3. **The table did not fill** in Steam or THE FINALS runs so far. `non-native code region table
   full` in a log means later regions read macOS `%gs` again.
4. `force_exec_prot` memory is not registered (DEP-off apps only).

Switches: `MACFINALS_NO_EXEC_REGIONS=1` restores old behaviour, including CW HACK 18756.
`MACFINALS_GS_TEB=1` opts back in to the GS-base switch, which still crashes with D3D.


Handoff for a fresh session. Read this file first, then `CLAUDE.md` (hard boundaries),
then `R13_GS_D3D_CRASH.md` (evidence) and `R13_STATUS.md` (NT hook + GS switch design).
Nothing in this file has been committed. Commit only when Rasmus asks.

## 1. Where things stand

### The two problems being chased

1. **NT EFLAGS bit**: Rosetta doesn't model it. It's handled by the in-process Rosetta hook
   (`tests/rosetta_nt/`, shipped in-runtime, see `R13_STATUS.md`). The SIMD-clobber fix
   is in macfinals-8 and later, but **not** in the shipped app (MacFinals-11 /
   macfinals-7).
2. **`%gs` truthfulness**: Windows code reading `gs:` fields (pid, tid, StackBase, TLS
   slots …) gets macOS pthread data instead of the TEB, except for three mirrored fields
   (0x30 Self, 0x58 TLS pointer, 0x60 PEB). This is a candidate cause of E111000D. **It is
   not proven to be the cause.**

### What was tried for (2) and why it is a dead end

- `patches/wine-macos-gsbase-teb-switch.patch` switches the real GS base to the TEB while
  Windows code runs (machdep trap `0x3000003`) at every dispatcher, signal-handler and
  callback transition. Without D3D it works: `segment_probe` 0 diffs, `tls_alloc_probe`
  Proton parity.
- `patches/wine-macos-gsbase-loader-marker.patch` (follow-up): moves the loader marker
  from `TEB->Instrumentation[1]` (D3D12 dereferences it) into
  `LdrSystemDllInitBlock.mac_gsbase_teb`. It works and that crash is gone.
- **Dead end:** GPTK (`libd3dshared.dylib`: `PatchUnixCallTable`,
  `PatchEntrypointCallTable`, `EmitCallTrampoline`) calls macOS code **directly from
  Windows threads**, skipping every Wine switch point. With GS = TEB, its native code
  (`D3DMThreadInit` → libsystem_pthread) reads the TEB as pthread data and crashes. Every
  DX12 process dies: Crab Champions and THE FINALS included. Proven: same probe, exit 0
  with `MACFINALS_NO_GS_TEB=1`, crash without it.

### The breakthrough (late 2026-11-01)

All earlier gs measurements were taken **without `CX_APPLEGPTK_LIBD3DSHARED_PATH`**, which
the app *does* set (`scripts/portable_setup.m:169`). With it set, CW HACK 22434
(`pe_module_loaded` in `dlls/ntdll/unix/loader.c` ~1719) registers every PE image with
GPTK's `register_non_native_code_region`. Result, macfinals-9, **GS switch off**:

```
gs reads from this loader-mapped image     pid same  tid same  StackBase same
VirtualAlloc RWX                           pid DIFF  tid DIFF  StackBase DIFF
VirtualAlloc RW -> VirtualProtect RX       pid DIFF  tid DIFF  StackBase DIFF
section view (FILE_MAP_EXECUTE)            pid DIFF  tid DIFF  StackBase DIFF
reserve then commit RX at 0x710000000000   pid DIFF  tid DIFF  StackBase DIFF
RW -> RX, called on a second thread        pid DIFF  tid WRONG
```

pid/tid/StackBase are **not** mirrored fields, so inside a registered region Rosetta
gives Windows code real TEB semantics through `%gs`. **No GS base switch is needed.**
Native code (GPTK included) is never registered and keeps macOS `%gs`, so there is no
conflict. Only **dynamically created executable memory** is unregistered. That is what
manually mapped code (the anti-cheat at `0x700000000000` via `preloader.dll`), JITs and
packers run from.

The closed-source CrossOver THE FINALS package (`~/Downloads/THE FINALS — CrossOver 26.3 —
only. Do not open its patch bodies or `wine-modified-source.tar.xz`, and do not reproduce
its code.** Only file names, file lists and comment lines were read. Also, its own bottle

## 2. The plan

### Step 1: Wine patch that registers all Windows executable memory

New file `patches/wine-macos-register-exec-regions.patch`, **appended at the end** of
`WINE_PATCHES` in `scripts/build_portable_runtime.py` (with a comment in the same style as
its neighbours). Write it independently from Wine's own structure.

Design notes (from reading our own CrossOver source, `~/MacFinals-build/phase-2/sources/wine/sources/wine/`):

- **GPTK interface:** `libd3dshared.dylib` exports only `register_non_native_code_region(start,
  end)` and `supports_non_native_code_regions()`. There is **no unregister**. The pointers
  are `static` in `dlls/ntdll/unix/loader.c` (CW HACK 22434, ~1662-1730), loaded lazily
  via `pthread_once(&non_native_init_once, init_non_native_support)` only when
  `CX_APPLEGPTK_LIBD3DSHARED_PATH` is set and the host is Sonoma or later. Add a small
  non-static helper there (e.g. `mac_register_exec_region(void *start, void *end)`) and
  declare it in `unix_private.h`.
- **Deadlock hazard:** the lazy init does `dlopen`. One of the v1.0 patches (see the
  `WINE_PATCHES` comment) moved Unix library loads **outside the virtual-memory lock** for
  a reason.
  Do not let the first `dlopen` happen under `virtual_mutex`. Either force the
  `pthread_once` early (process init, before any Windows code) or record ranges and
  register them after the lock is released. Also unknown: whether
  `register_non_native_code_region` itself takes locks or allocates. Calling it under
  `virtual_mutex` needs a reason, or avoid it.
- **Where executable memory appears, `dlls/ntdll/unix/virtual.c`:**
  - `get_unix_prot()` ~1454 maps `VPROT_EXEC` → `PROT_EXEC`. `mprotect_range()` ~2051
    is the host-protection chokepoint for changes. It is hot (write-watch faults etc.), so
    register only on transitions to executable, and consider deduplicating.
  - `set_vprot()` ~2081 / `set_protection()` ~2103. Callers: `allocate_virtual_memory`
    commit path ~5328 and `NtProtectVirtualMemory` ~5711.
  - `create_view()` ~1918 and `map_view()` ~2340: a fresh reserve+commit with exec
    protection may never go through `set_vprot`. Check `allocate_virtual_memory()` ~5241
    (note the `vprot & VPROT_EXEC || force_exec_prot` mprotect at ~5314).
  - Section views: `NtMapViewOfSection` ~6407 → `map_file_into_view()` ~2444 (mmap with
    exec directly). Image sections: `map_image_into_view()` ~3125 (already covered by the
    PE loader callback for images loaded through `LdrLoadDll`. An image section mapped by
    hand through `NtMapViewOfSection` is not, so cover it too).
  - Only register memory in the **Windows** address space. Never register ranges inside
    Wine's own Unix libraries or dylibs.
- **Gate the feature** like the GS switch: macOS x86_64 under Rosetta only, plus an env
  kill switch (e.g. `MACFINALS_NO_EXEC_REGIONS=1`). Fail-soft: if registration is
  unavailable, behaviour equals today.
- **Unknowns to measure, not assume:** cost per call (some games flip protections a lot),
  whether re-registering the same or overlapping range is harmless, what happens when a
  registered range is later freed and reused by native code (no unregister!). Note:
  Wine's Windows-space allocations are not normally handed to native mmap users, but
  check whether anything native (MoltenVK, Metal, GPTK) can land in freed Windows-space
  ranges.

### Step 2: Gate with probes (switch off)

Build and assemble (see §3). Then, **always with the region variable set**:

```sh
R=~/MacFinals-build/phase-2/artifacts/macfinals-10   # next free version
export CX_APPLEGPTK_LIBD3DSHARED_PATH=$R/lib/external/libd3dshared.dylib
export MACFINALS_NO_GS_TEB=1                          # until step 3 lands
WINEPREFIX=/tmp/r13gs/prefix-gsr10 WINEDEBUG=-all $R/bin/wine runtime/build/probe/gsregion-probe.exe       # must be all "same"
WINEPREFIX=/tmp/r13gs/prefix-gsr10 WINEDEBUG=-all $R/bin/wine runtime/build/probe/gsregion-probe.exe d3d   # must exit 0
WINEPREFIX=/tmp/r13gs/prefix-gsr10 WINEDEBUG=-all $R/bin/wine runtime/build/probe/segment-probe.exe
WINEPREFIX=/tmp/r13gs/prefix-gsr10 WINEDEBUG=-all $R/bin/wine runtime/build/probe/tls-alloc-probe.exe
WINEPREFIX=/tmp/r13gs/prefix-gsr10 WINEDEBUG=-all $R/bin/wine runtime/build/probe/macfinals-probe.exe      # NT hook: 4/4
```

(Run from the repo root. There is no `timeout` binary on this Mac.)

Pass criteria: all six gsregion cases "same", d3d exit 0, macfinals-probe 4/4. For
segment-probe and tls-alloc-probe, record the numbers. TLS parity may still differ
because of CW HACK 18756 (step 4).

### Step 3: Turn the GS-base switch off by default

The switch patch is already applied and recorded, so do **not** remove it from
`WINE_PATCHES` (that forces deleting `$W/sources/wine` and a full rebuild). Append a small
patch that changes the gate in `signal_init_process` (`dlls/ntdll/unix/signal_x86_64.c`,
`mac_gsbase_switching = is_rosetta2 && !getenv("MACFINALS_NO_GS_TEB")`) to opt-in, e.g.
`getenv("MACFINALS_GS_TEB")`. With it off, `LdrSystemDllInitBlock.mac_gsbase_teb` is 0 and
CW HACK 18756 runs as in stock CrossOver. Steps 1 and 3 can share one build.

### Step 4 (after step 2 passes): revisit CW HACK 18756

`dlls/ntdll/loader.c` ~5188: TlsExpansionSlots prealloc plus the "burn" of TLS slots, so
the first `TlsAlloc` returns 65 instead of 2 (Windows/Proton: 2). It exists because
gs-relative TLS reads used to hit pthread data. If every Windows code region is
registered, it may be unnecessary. Test by gating it on "regions registered" and
comparing `tls-alloc-probe` with Proton. Keep it if anything regresses.

### Step 5: Real targets, in this order

Rasmus's rule: if Steam or Crab Champions don't run, THE FINALS won't.

1. Build an app with the new runtime (§3). Next free app name: **MacFinals-12**. This also
   ships the SIMD-fixed NT hook for the first time.
2. Quit the running launcher, Steam and the game. Open MacFinals-12. Verify it's the live
   one (`ps -axo pid,lstart,command | grep 'MacFinals-[0-9]*.app'`) and check
   `active_runtime` in `~/Library/Application Support/MacFinals/state.json`. It currently
   says `macfinals-7`. A fresh log in `logs/` proves new code ran.
3. Steam starts and its UI works → Crab Champions plays (DX12 canary) → THE FINALS. Note
   whether E111000D changes. Rasmus launches the game; never scan or touch the game's
   files.
4. Proton three-way of `macfinals-probe.exe` on `ssh ubuntu` (commands in `CLAUDE.md`).

If THE FINALS still shows E111000D with every Windows region registered and the NT hook
active, `%gs` was not the (only) blocker. Record that and stop chasing gs.

## 3. Build cycle (exact commands)

```sh
cd "/Users/rasmus/Documents/Kod/Annat/The finals/MacVolvo"
W=~/MacFinals-build/phase-2
python3 scripts/build_portable_runtime.py --workspace $W --jobs 4          # incremental, ~minutes
python3 scripts/assemble_portable_runtime.py --workspace $W \
  --identity "MacFinals Signing" --version macfinals-10                   # never reuse a version
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  python3 scripts/build_native_app.py --identity "MacFinals Signing" \
  --runtime-archive $W/artifacts/macfinals-10.tar.gz --output runtime/build/MacFinals-12.app
python3 -m unittest tests.test_macfinals_identity tests.test_macvolvo_lifecycle \
  tests.test_steam_launch_options tests.test_macvolvo_session tests.test_steam_window_fix   # 25 must pass
```

Existing: runtimes macfinals-1…9 in `$W/artifacts/` (6 and 7 tarball only), apps
MacFinals-1…11 in `runtime/build/`. An interrupted run leaves a partial directory. Check
it's partial (no `.tar.gz`/`build.json`) before deleting.

**Patch-writing procedure that worked** (keeps `.macfinals-patches.json` consistent):

1. Snapshot the files you will touch: `cp <file> /tmp/r13gs/<name>.v2` (or the scratchpad).
2. Edit the Wine tree with the Edit tool. Read the region first; the tool refuses
   unread files.
3. Make the patch with `diff -u` per file, rewriting the header lines to
   `--- a/<path>` / `+++ b/<path>`, concatenated into one file.
4. Revert the tree with `patch -R -p1 -i <patch>` and verify with `cmp` against the
   snapshots.
5. `git apply --check <patch>` from the Wine source root.
6. Copy into `patches/`, append to `WINE_PATCHES`, run the 25 tests, build.

Editor diagnostics on Wine sources ("ntstatus.h not found", implicit int) are false
positives: the local clang lacks Wine's include paths. Ignore them.

## 4. Boundaries that matter for this work (full list in `CLAUDE.md`)

- Calling `register_non_native_code_region`, copying libd3dshared unchanged and setting
  `CX_APPLEGPTK_LIBD3DSHARED_PATH` are **use** of GPTK, which is allowed. Patching GPTK's
  code or its emitted trampolines in memory is **modification**, which is not allowed
  without asking Rasmus. Reading libd3dshared's exported symbol names was fine.
  Disassembling it was not done and is not needed.
- Never touch, patch or scan THE FINALS' files. Never analyse the anti-cheat image at
  `0x700000000000`. Registering memory regions generically (every executable range, every
  process) is a compatibility fix. Do not special-case the anti-cheat's addresses.
- The closed-source package is reasoning only (see §1).
- Fixes must make the environment more truthful, never hide Wine.
- Ask before editing `tests/windows_probe/macfinals_probe.c`. New probes go in new files.

## 5. Loose ends (lower priority)

- `R13_STATUS.md` §4: "hook installed but never matched" exit log; the stale-NT hazard
  test; the hard-coded iretq fault address (`sub x22,x1,#2`).
- `CLAUDE.md` "Current status" still describes 2026-10-29. Update it once step 2 has a
  result (it is Rasmus's file; propose the edit).
- Temporary files from this session: `/tmp/r13gs/` (prefixes, logs, v1 snapshots). Safe
  to leave. Don't rely on them persisting.
