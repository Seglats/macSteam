# MacFinals

Fork of MacVolvo: a launcher for **THE FINALS** on Apple Silicon using Apple GPTK 4
(D3DMetal 4.0 beta 3) with **DirectX 12**, and later other DX12 games. Breaking DX11,
CS2 and Deadlock is acceptable — MacVolvo's DXMT path and its CS2/Deadlock settings are
not maintained here.

## Goals

- Make THE FINALS launch and play.
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
4. **Do not copy the closed-source CrossOver "notproton"-style package.** Its reasoning is
   fair to learn from; its code is not to be reproduced. Anything shipped here is derived
   independently.
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

## Current status (2026-10-29)

- **The blocker is the NT (nested task) EFLAGS bit.** Rosetta does not model it: `POPF`
  never sets it, so exception contexts carry 0 and `IRETQ` with NT set does not fault.
  Reproducible with no Wine involved at all by a plain x86-64 macOS binary.
- **Wine cannot fix it, and Wine is not at fault** — tested on both targets, not assumed.
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
Xcode or the Swift build fails on SwiftUI macros.

```sh
W=~/MacFinals-build/phase-2
python3 scripts/build_portable_runtime.py --workspace $W --download-only
python3 scripts/build_portable_dependencies.py --workspace $W --jobs 4
python3 scripts/build_portable_runtime.py --workspace $W --jobs 4   # Wine; incremental after the first run
python3 scripts/assemble_portable_runtime.py --workspace $W \
  --identity "MacFinals Signing" --version macfinals-N
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  python3 scripts/build_native_app.py --identity "MacFinals Signing" \
  --runtime-archive $W/artifacts/macfinals-N.tar.gz --output runtime/build/MacFinals-N.app
```

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
`logs/` is the cheapest proof that new code actually ran.

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
