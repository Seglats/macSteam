# MacFinals

> **Location (2026-11-09):** this is now the `finals` environment inside SteamDriver
> (`environments/finals/`). Paths below are relative to this folder, except `runtime/…`,
> `gptk/` and `config/`, which are at the repository root. Run the build commands from the
> repository root with the `environments/finals/` prefix (see `README.md`). The app side is
> SteamDriver's; the "App"/"Testing a new app bundle" notes below describe the old standalone
> MacFinals app and stay for history until the own-Steam mode launches this environment.

Fork of MacVolvo: a launcher for **THE FINALS** on Apple Silicon using Apple GPTK 4
(D3DMetal 4.0 beta 3) with **DirectX 12**, and later other DX12 games. Breaking DX11,
CS2 and Deadlock is acceptable — MacVolvo's DXMT path and its CS2/Deadlock settings are
not maintained here.

## Goals

- Make THE FINALS launch and play.
- **The Rosetta syscall storm must be fixed for this to count as working** (Rasmus,
  2026-11-07): the game's direct x86 `syscall`s (~32k/s) each cost ~16–22 µs under
  Rosetta's brk#5 → exception-server → SIGSYS detour (≈ 0.5–0.7 of a CPU core; Proton
  ~2 µs). Routing them straight into Wine's dispatcher is a *requirement for success*,
  not an optimization — "the project may fail, but it cannot succeed without this."
  The mechanism is the r20 finished-thunk patch, currently **quarantined** after two
  kernel panics (see `tests/rosetta_nt/NOTES.md`, "r20 (2026-11-07)"). Reviving it is
  mandatory; the *method* is constrained (below).
- Keep the Wine runtime built from source (CrossOver 26.3 Wine plus our patches) so it
  can actually be changed.
- Extensive, useful logging. Play starts the game directly, with game-only debug logging
  when `gameDebug` is set.
- Sign with Rasmus's own "MacFinals Signing" certificate, so Little Snitch rules survive
  rebuilds.
- Rebuilding must never require re-downloading games.

**Scope is THE FINALS only.** Do not broaden to a matrix of DX12 titles or install games to
test. **Crab Champions is the canary**: it runs on DX12 here, so it is the regression check
that the runtime is still sound after a Wine change. Both games are Unreal Engine 5, so the
risk of tuning for the canary is accepted as slim.

## Hard boundaries — do not cross

1. **THE FINALS' game files are never touched.** Not patched, not modified, not scanned
   in order to patch them.
2. **No spoofing and no hiding Wine from the anti-cheat.** Compatibility fixes only. No
   CPU-vendor spoofing. Fixes must make the environment *more* truthful, never less.
3. **Do not analyse or reverse-engineer the anti-cheat's protected code** — the packed PE
   manually mapped at `0x700000000000` (sections `packer0`, `packer1`, `packer30`, …),
   loaded by `preloader.dll`. Working out which of its checks fails is out of scope even
   when the goal is compatibility. Dialog addresses like `02022AF3` are offsets into it.
5. **Do not modify GPTK / D3DMetal.** Copying `D3DMetal.framework` and
   `libd3dshared.dylib` unchanged, calling interfaces they expose
   (`register_non_native_code_region`) and setting `D3DMETAL_FRAMEWORK_PATH` are *use*,
   not modification — those are fine.
6. **Never modify Rosetta on disk, and never ask the user to disable SIP.**
7. **Commit or push only when asked.**

## Allowed

- Wine patches (the main tool), launcher/app/Swift changes, probe and diagnostics work.
- **Rosetta behaviour correction in memory, in-process only.** Rasmus green-lit this on
  2026-10-29 after first ruling it out; the licence judgement is his and is not to be
  relitigated. If built, it must be: in-process (no `task_for_pid` against other
  processes, no debugger entitlement, no loader daemon), guarded fail-closed so an
  unrecognised Rosetta build deactivates the hook instead of corrupting translation, and
  scoped to `POPF`/`PUSHF`/`IRET` flag handling only.
- **Scope extended 2026-11-02 (Rasmus): the x86 `syscall` instruction.** THE FINALS issues ~32,000
  direct syscalls a second (rax=0xffffffff); under Rosetta each goes brk #5 → exception server →
  emulated SIGSYS → Wine (~16 µs; Proton ~2 µs). The hook may route that invalid-class `syscall`
  path straight into Wine's dispatcher instead, with **exactly the behaviour Wine's
  `sigsys_handler` gives today** (same registers, same resume point). Same constraints:
  in-process, fail-closed, nothing else in Rosetta changed. `tests/rosetta_nt/R17_PERFORMANCE.md`.
- **Method constraint, 2026-11-07 (after two kernel panics):** `mach_vm_read_overwrite`
  on a translated anonymous exec region PANICKED the kernel, twice, deterministically
  (NOTES.md "r20 (2026-11-07)"). Until individually re-proven, **no `mach_vm_*` copy or
  protection call may touch translated mappings** — that includes `mach_vm_write`,
  `mach_vm_read_overwrite` and `mach_vm_protect` on JIT/thunk pages. Reading translated
  memory must use direct user-space loads behind a SIGSEGV/SIGBUS recovery handler
  (worst case a catchable signal). Region *metadata* walks
  (`mach_vm_region_recurse`) have a long clean history and stay allowed. Any write
  primitive that changes page protection needs Rasmus's explicit per-tier sign-off
  first — panic risk is real and now twice-paid.

## Current status (2026-11-07)

**Start with `tests/rosetta_nt/NEXT_SESSION.md`**: current build, open problems, next steps and
the rules learned this session. Its top section covers the 2026-11-07 kernel panics (r20
scanner quarantined; runtime `macfinals-40` installed and clean of that code) and the
mandatory-safe revival plan.

- **The S12 skin fix (missing arms/limbs) ships as a d3d12 layer since runtime `macfinals-20` /
  `macfinals-21`.** `d3d12_layer/` sits between the game and GPTK's d3d12.dll (byte-identical,
  under the `d3d12_gptk` name) and rewrites DXIL before D3DMetal translates it: a local array
  filled from the constant buffer and read with a dynamic index becomes a direct constant-buffer
  read, 0 out of range (what vkd3d-proton does). Structural construct matching, no shader list,
  GPTK and game files untouched. Since `macfinals-27` it wraps every device version (the game
  uses ID3D12Device12). Wine resolves builtins by the PE's internal export name, so the real GPTK
  PE must stay outside `lib/wine/x86_64-windows/` (a placeholder there reserves the name in the
  prefix); layout and history in `tests/rosetta_nt/R15_SKIN_CURVE.md`. **Limbs confirmed back in
  game on `macfinals-27`.** `MACFINALS_SKIN_FIX=0` disables the rewrite.
- **Newest runtime `macfinals-31`** (2026-11-02): skin fix (Proton-equivalent constant-buffer
  rewrite; limbs confirmed back on `macfinals-27`), Wine fix for macOS `IP_RECVTOS` control
  messages (Steam's TOS assert and its in-game minidumps are gone; that dump could hang the game
  on first launch), and a diagnostic for threads Wine kills with "nested exception on signal
  stack". Still inconsistent: main-menu buttons sometimes do not open menus. Test with
  `RUNTIME=macfinals-31 DEBUG=max tests/finals_bisect/launch_main.sh`. See
  `tests/rosetta_nt/R16_FIRST_LAUNCH_HANG.md`.
- **THE FINALS gets past its startup checks on runtime `macfinals-19` / `MacFinals-14.app`.** The
  real `Discovery.exe` loads `denuvo-anti-cheat-runtime.dll`, D3D12 and EOS. **The E111000D
  ("Conflicting software detected") cause was the PE toolchain:** Wine's Windows-side modules were
  built with llvm-mingw (clang + lld). CrossOver builds them with **MinGW-w64 GCC + GNU binutils**,
  whose PE layout differs (`.edata`/`.xdata`/`.idata`/`.bss`). Swapping only `ntdll.dll` between
  the two decided pass or fail. Found by bisecting against CrossOver 26.3's own prebuilt Wine in
  a throwaway setup (`/Users/rasmus/Documents/Kod/Annat/finalsTesting`, never copied back); the
  steps are in `tests/rosetta_nt/(removed)`. **Never go back to llvm-mingw for PE
  modules.**
- **The first launch past the check in a prefix** shows Denuvo's "issue with starting Denuvo
  Anti-Cheat" (`2:{launcher,10}`). The second launch goes in. Seen on both routes.
- **Not yet confirmed:** the main menu. The CrossOver-route run reached Embark ID creation and then
  died of a stack overflow, in a run with heavy `+seh` logging. Test with light or no logging.
- Full write-up of the fix and what we learned: `tests/rosetta_nt/(removed)`.
- Earlier status (kept for history):
- **THE FINALS still stops with E111000D on MacFinals-12 / runtime `macfinals-13`**, confirmed by
  the "Conflicting software detected" dialog and `NtTerminateProcess(-1, e111000d)` in the debug
  log. That is with every Windows code region registered, the NT hook active and TLS at Proton
  parity. **So `%gs` was not the only blocker, and possibly not a blocker at all. Do not keep
  chasing it.** Crab Champions, the DX12 canary, still works and Steam still works, so the patches
  did not break the runtime. Details: `tests/rosetta_nt/R13_NEXT_SESSION.md`.
- **Current work and latest results: `tests/rosetta_nt/(removed)`.** Newest build is
  MacFinals-13 / `macfinals-17`, which fixes the hook losing NT after a literal `0` or `0x202` in
  guest code (`nt_patterns_probe`). Steam and Crab Champions work on it; THE FINALS still stops with
  E111000D. Findings and decisions: `tests/rosetta_nt/R14_FINDINGS.md`. Measured differences from
  Proton beyond CPUID identity and CS/SS are tabled there, and fixing any outside
  `POPF`/`PUSHF`/`IRET` is Rasmus's decision.
- **Rebuild the hook with `tests/rosetta_nt/build_hook.sh`** (it stops on failure). A failed
  payload build leaves the old `payload_blob.h`, and `hooklib.dylib` then silently embeds it.
- **NT handled by the in-process Rosetta hook** (`tests/rosetta_nt/`, shipped as
  `lib/macfinals-nt-hook.dylib`). `macfinals-probe` passes 4/4 inside Wine, and pushf/popf are
  fixed inside the game process, yet the game still fails, so NT is not the whole story.
- **`%gs` is now truthful for Windows code by registering regions, not by moving the GS base.**
  GPTK's `register_non_native_code_region` gives a region TEB semantics under Rosetta; it needs
  `CX_APPLEGPTK_LIBD3DSHARED_PATH`, which the app sets, so **always set it when probing**. The
  GS-base switch crashes every D3D12 process because GPTK calls macOS code directly from Windows
  threads, so it is opt-in (`MACFINALS_GS_TEB`). `MACFINALS_NO_EXEC_REGIONS=1` turns registration
  off, which also brings CW HACK 18756 back.
- **GPTK's region table holds 1024 entries and silently ignores the rest, repeats included.** All
  registration must go through `mac_register_exec_region`, which dedupes. The loader's reserved
  areas take 2 entries and cover most memory. Look for `non-native code region table full` in logs.
- **NT background: Rosetta does not model the bit.** `POPF` never sets it, so exception contexts
  carry 0 and `IRETQ` with NT set does not fault. Reproducible with no Wine involved at all by a
  plain x86-64 macOS binary.
- **Wine cannot fix NT, and Wine is not at fault** — tested on both targets, not assumed.
  Proton on a real x86-64 CPU passes all four NT checks with the same wine-11.0 code, so
  Wine carries the flag correctly. Do not repeat the earlier mistaken claim that Wine's
  three `andq $~0x4000` sites in `dlls/ntdll/unix/signal_x86_64.c` suppress NT: they only
  keep NT out of the flags live during the `iretq` itself, which would fault; the restored
  EFLAGS come from the iret frame. The game's path is `POPF`, which Wine never sees at all.
- **Fixed but not sufficient:** `ProcessParameters.Flags` lacked `0x4000`
  (`RTL_USER_PROC_IMAGE_KEY_MISSING`) — `patches/wine-ntdll-image-key-missing.patch`. It
  must patch **both** `dlls/ntdll/unix/env.c` (`load_global_options`) and
  `dlls/ntdll/env.c` (`init_user_process_params` must copy `Flags`); the Windows-side
  ntdll rebuilds the parameter block at startup and otherwise drops the bit silently, so
  a Unix-side-only patch looks right in source and changes nothing at runtime.
- **Known divergence, tolerated:** `NtWriteFile` with a process handle returns
  `c0000024`; Windows returns `c0000008`. Proton has the same divergence and the game runs
  there, so it is not the cause — but it is a real Wine fidelity bug and upstreamable.
- Rosetta env toggles exist, none affect flag fidelity: `ADVERTISE_AVX` (works, verified
  via CPUID), `ALLOW_GUARD_PAGES`, `AOT_ERRORS_ARE_FATAL`, `DEBUGSERVER_PORT`,
  `DISABLE_AOT`, `DISABLE_EXCEPTIONS`, `DISABLE_SIGACTION`, `HARDWARE_TRACING_PATH`,
  `MEMORY_ACCESS_INSTRUMENTATION`, `PRINT_IR`, `PRINT_SEGMENTS`,
  `SCRIBBLE_TRANSLATIONS`. `PRINT_IR` produces no output on this release build.
- Apple is winding Rosetta down, so an upstream fix is not expected.
- A launcher-side preflight that detected the missing flag and reported it instead of
  letting the user reach the game's dialog was built and then **removed at Rasmus's
  request** on 2026-10-29: it was cosmetic, it did not advance the goal, and it only
  covered the Play button while launching from inside Steam bypassed it. Do not rebuild it
  unasked. The goal is THE FINALS running, not a tidier failure.

## Build

The workspace path must contain no spaces (autoconf), and `DEVELOPER_DIR` must point at
Xcode or the Swift build fails on SwiftUI macros. Wine's Windows side needs **MinGW-w64 GCC**
(`brew install mingw-w64`; GCC 16.2.0 / binutils 2.47 when this was written). The build script
refuses to run without it. The GCC workspace is `~/MacFinals-build-gcc/phase-2`. The older
`~/MacFinals-build/phase-2` was built with llvm-mingw; its next build reconfigures and rebuilds the
whole PE side.

```sh
W=~/MacFinals-build-gcc/phase-2
python3 scripts/build_portable_runtime.py --workspace $W --download-only
python3 scripts/build_portable_dependencies.py --workspace $W --jobs 4
python3 scripts/build_portable_runtime.py --workspace $W --jobs 4   # Wine; incremental after the first run
python3 scripts/assemble_portable_runtime.py --workspace $W \
  --identity "MacFinals Signing" --version macfinals-N
python3 scripts/add_nt_hook.py --workspace $W --base-version macfinals-N --version macfinals-N+1
# assemble also builds d3d12_layer/ via build.sh (MinGW-w64 GCC + the workspace winebuild);
# it needs the wine-build tree the source builds produced, so run it after they finish
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  python3 scripts/build_native_app.py --identity "MacFinals Signing" \
  --runtime-archive $W/artifacts/macfinals-N+1.tar.gz --output runtime/build/MacFinals-M.app
```

`N+1` means the next free number, written out, and `M` is the next free app number. **Do not
skip `add_nt_hook.py`.** An assembled runtime has no hook and signs the Wine executables with
the hardened runtime, which makes dyld strip the insertion, so the app would silently ship
without the NT fix. It adds `tests/rosetta_nt/hooklib.dylib` and re-signs those executables ad hoc.
Build that hook with `tests/rosetta_nt/build_hook.sh` (payload → blob → both slices → signed;
stops on the first failure, so a stale `payload_blob.h` can never be embedded).

**Release builds without debug messages** use Wine's own switches (see "Debug output"):
`build_portable_runtime.py --no-debug-msgs` (Wine; needs its own workspace — the mode is pinned
per workspace and a mismatch is refused), `assemble_portable_runtime.py --no-debug-msgs` (the
d3d12 layer), and `DEBUG_CFLAGS="-DWINE_NO_TRACE_MSGS -DWINE_NO_DEBUG_MSGS"
tests/rosetta_nt/build_hook.sh OUT.dylib` then `add_nt_hook.py --hook OUT.dylib` (the hook).
err messages always stay. `d3d12_layer/build.sh` refuses a layer that imports anything but
kernel32/ntdll/api-ms-win-crt (a MinGW runtime DLL there makes d3d12.dll fail to load).

Runtime versions and app output paths must be new each time; existing ones are never
overwritten. An interrupted run leaves a partial artifact directory or `.app` shell that
has to be deleted before retrying — check it is partial (no `.tar.gz`, no `build.json`)
rather than assuming.

Only the assembled runtime bundles the dependency dylibs, so `$W/install/usr/local/bin/wine`
cannot run on its own (`libinotify.dylib` missing). Testing a Wine change needs the
assemble step, but not the app build.

Any **new helper binary** in the bundle must be added to the signing list in
`build_native_app.py` (the `for binary in (...)` tuple), or signing the outer bundle fails
with "code has no resources but signature indicates they must be present".

Wine patches live in `WINE_PATCHES` in `scripts/build_portable_runtime.py`, ordered.
**Append new ones at the end** so the existing source tree patches incrementally; changing
or reordering an applied patch requires deleting `$W/sources/wine`. Applied patches are
recorded in `.macfinals-patches.json` inside the Wine source.

## Debug output — follow Wine's model

All MacFinals code logs the way Wine does (2026-11-09): **debug channels with the classes
err/fixme/warn/trace, selected at run time with `WINEDEBUG`, and compiled out for a release
build with `-DWINE_NO_TRACE_MSGS` / `-DWINE_NO_DEBUG_MSGS`.** Default err+fixme; nothing is
written to files unless trace is on. New code uses this too — no new `MACFINALS_*_LOG`
variables, no switch files.

| Channel | Code | trace adds |
|---|---|---|
| `macfinals_hook` | `tests/rosetta_nt/hooklib.c` (parses WINEDEBUG itself with Wine's rules: it runs before ntdll) | counter dumps every 2.4 s, `/tmp/rosetta-re/hook-<pid>.log`, payload ring capture |
| `macfinals_dx12` | `d3d12_layer/layer.c` (through ntdll's `__wine_dbg_*`) | every rewritten shader and pipeline; **warn** = pipeline creation ≥ 50 ms, with thread name and epoch time |
| `macfinals_scan` | hooklib's arena scan at process exit | trace class only; heavy, own channel like relay |

Only our output: `WINEDEBUG=-all,+macfinals_hook,+macfinals_dx12` (a named channel always beats
`all`, whatever the order); one process only: `Discovery.exe:+macfinals_dx12`. Channel names are
≤ 14 characters or Wine ignores them. `launch_main.sh DEBUG=macfinals` is that, with Wine's
pid/tid/timestamp prefixes and MoltenVK quieted (`MVK_CONFIG_LOG_LEVEL=1`; MoltenVK logs
outside WINEDEBUG). Any `WINEDEBUG=...` given to `launch_main.sh` is passed verbatim.
Retired: `MACFINALS_D3D12_LAYER_LOG`, `MACFINALS_HOOK_RING`, `MACFINALS_SYSC_DUMP`,
`/tmp/rosetta-re/nt-ring`. Functional switches (`MACFINALS_SKIN_FIX`, `MACFINALS_NT_HOOK=0`,
`MACFINALS_NT_OFF`, `MACFINALS_THUNK_PATCH`, `MACFINALS_PRECISE_DISCARD=0`) are not debug output and
stay. So do the search switches `MACFINALS_NO_DISCARD` and `MACFINALS_NO_FAST`
(`tests/rosetta_nt/R22_MISSING_ROPE.md`).

## Testing a new app bundle — read this first

Every `MacFinals-N.app` shares one bundle identifier (`app.macfinals.launcher`) and sets
`LSMultipleInstancesProhibited`, so **opening a new bundle while an older one runs silently
focuses the old app instead.** This produced two false "nothing changed" results in one
session. Before testing a new bundle: quit the running launcher, Steam and the game, then
open the new `.app`. Confirm which one is actually live with

```sh
ps -axo pid,lstart,command | grep 'MacFinals-[0-9].app'
```

and confirm the runtime in use with `grep active_runtime \
"$HOME/Library/Application Support/MacFinals/state.json"`. A fresh log file in
`logs/` is the cheapest proof that new code actually ran. This caught a run on the old build
again on 2026-11-01. Check the `runtimes/macfinals-N` paths in the log too.

**Never log the game with `warn+heap`, `trace+heap`, `relay` or `snoop`** (so never plain
`warn+all`; use `warn+all,warn-heap`). Heap warnings make Wine set heap validation flags
(`dlls/ntdll/heap.c`, `heap_set_debug_flags`) that look exactly like a process started under a
debugger, and the anti-cheat quits with "disallowed tool" (2026-11-02). `+seh` changed the
game's behaviour too. `tests/finals_bisect/launch_main.sh DEBUG=max` is the safe maximum.

To stop a leftover MacFinals wineserver (probes and `launch_main.sh` refuse to start while one
runs), `wineserver -k` needs the prefix, or it targets `~/.wine` instead:

```sh
root="$HOME/Library/Application Support/MacFinals"
WINEPREFIX="$root/environment" "$root/runtimes/macfinals-31/bin/wineserver" -k
pgrep -fl 'MacFinals/runtimes/.*/wineserver' || echo none
```

Steam records each game exit in `drive_c/Program Files (x86)/Steam/logs/gameprocess_log.txt`,
which is useful when game debug logging was off. E111000D only appears in logs when it is on.

## Tests

```sh
python3 -m unittest tests.test_macfinals_identity tests.test_macvolvo_lifecycle \
  tests.test_steam_launch_options tests.test_macvolvo_session tests.test_steam_window_fix
```

Those 25 must pass. The full `unittest discover` run reports ~71 failures and 20 errors on
an **unmodified** tree as well — MacVolvo's DXMT, display, Retina and MetalFX tests, which
this fork does not maintain. Compare against `git stash` HEAD before blaming a change.

## Probe

`tests/windows_probe/macfinals_probe.c` is the Windows conformance probe: the same `.exe`
runs on MacFinals, real Windows and Proton for a three-way comparison.

```sh
x86_64-w64-mingw32-clang -O1 -Wall tests/windows_probe/macfinals_probe.c \
  -o runtime/build/probe/macfinals-probe.exe -lntdll
```

Ask before changing it — Rasmus has rejected probe edits before. Editor diagnostics on it
are the Mac's own clang lacking Windows headers and can be ignored.

`tests/windows_probe/handle_probe.c` maps what status each syscall returns for a
wrong-typed or invalid handle. MacFinals and Proton return identical tables, so Wine's rule
(wrong type → `c0000024`, not a handle → `c0000008`) is upstream Wine's, not ours. Only a
real Windows run can say where Windows draws the line.

`tests/windows_probe/exec_region_probe.c` checks `%gs` and TLS at scale: hand-mapped images,
free and re-allocate, 100k protection flips, 20k regions. `capacity N` finds where regions stop
working. Short probes cannot see the region table limit; this one can.

## The Proton reference machine

`ssh ubuntu` (in `~/.ssh/config`) reaches an Ubuntu box running Proton Experimental on
**wine-11.0 — the same base as our CrossOver 26.3 Wine** — and THE FINALS *runs* there.
That makes it the most useful test target we have: a known-good Wine on a real x86-64 CPU.

```sh
scp runtime/build/probe/<tool>.exe ubuntu:~/
ssh ubuntu 'WINEPREFIX=~/probe-prefix WINEDEBUG=-all \
  ~/.steam/steam/steamapps/common/"Proton - Experimental"/files/bin/wine ~/<tool>.exe'
```

Use it as a differential test: anywhere **MacFinals diverges from Proton**, we or CrossOver
caused it, and it is a candidate cause for a game failing here but not there. Anywhere both
agree but Windows differs, it is upstream Wine's behaviour and not our bug — so Proton
cannot stand in for Windows when the question is "what does Windows do".
