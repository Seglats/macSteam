# THE FINALS environment (MacFinals)

The Wine environment that runs **THE FINALS** on Apple Silicon: CrossOver 26.3 Wine built
from source with MacFinals' patches, Apple's D3DMetal (GPTK 4) for Direct3D 12, the d3d12
skin-fix layer, and the in-process Rosetta hook that routes the game's ~32,000 direct
`syscall`s a second straight into Wine's dispatcher. Merged from the MacFinals repository on
2026-11-09 with its history (the merge commit's second parent).

**Read [`CLAUDE.md`](CLAUDE.md) before changing anything here.** It holds the hard boundaries
(game files, anti-cheat, GPTK, Rosetta) and the method constraints learned from two kernel
panics. They apply to this environment in full.

All paths in this folder's docs are relative to `environments/finals/`, except
`runtime/…`, `gptk/` and `config/`, which are at the repository root.

| What | Where |
|---|---|
| Current state and next steps | [`tests/rosetta_nt/NEXT_SESSION.md`](tests/rosetta_nt/NEXT_SESSION.md) |
| Wine patches (order: `WINE_PATCHES` in `scripts/build_portable_runtime.py`) | [`patches/`](patches/) |
| Build: dependencies, Wine, assemble, hook | [`scripts/`](scripts/), [`tests/rosetta_nt/build_hook.sh`](tests/rosetta_nt/build_hook.sh) |
| d3d12 layer (skin fix, pipeline timing) | [`d3d12_layer/`](d3d12_layer/) |
| Rosetta hook and its history | [`tests/rosetta_nt/`](tests/rosetta_nt/), [`ROSETTA_NT_APPROACH.md`](ROSETTA_NT_APPROACH.md) |
| Running the game from macOS Steam (compatibility tool "SteamDriver: THE FINALS") | [`compat-tool/`](compat-tool/README.md) |
| Running the game outside the app, logs | [`tests/finals_bisect/launch_main.sh`](tests/finals_bisect/launch_main.sh) |
| Windows probes | [`tests/windows_probe/`](tests/windows_probe/) |

## Building

The workspace is outside the repository (`~/MacFinals-build-gcc/phase-2`; the path must contain
no spaces). The Windows side needs **MinGW-w64 GCC**, never llvm-mingw. Local inputs at the
repository root: `runtime/toolchains/` (llvm-mingw-20251216, bison, libarchive) and `gptk/gptk`
(Apple's Game Porting Toolkit, not redistributable). From the repository root:

```sh
W=~/MacFinals-build-gcc/phase-2
F=environments/finals
python3 $F/scripts/build_portable_runtime.py --workspace $W --download-only
python3 $F/scripts/build_portable_dependencies.py --workspace $W --jobs 4
python3 $F/scripts/build_portable_runtime.py --workspace $W --jobs 4
python3 $F/scripts/assemble_portable_runtime.py --workspace $W --identity "MacFinals Signing" --version macfinals-N
$F/tests/rosetta_nt/build_hook.sh
python3 $F/scripts/add_nt_hook.py --workspace $W --base-version macfinals-N --version macfinals-N+1
```

## Test every change

**Standard (set 2026-11-15): test a change the moment it exists.** A bug found minutes after
it is written costs minutes; the same bug found in a game session days later costs an evening
of bisecting runtimes. Every Wine patch, hook change or runtime bump climbs this ladder before
the next game session, and each rung catches what the previous one can't:

1. **`git apply --check` the patch** against `$W/sources/wine/sources/wine` before building.
   Hand-written or hand-fixed hunks fail here in a second, not 40 minutes into a build.
2. **Run the probe that covers what changed** under the new runtime
   ([`tests/windows_probe/`](tests/windows_probe/): `macfinals_probe`, `handle_probe`, and the
   crash-specific ones like `cs_timeout_probe` and `stuck_send_probe`). Probes run in seconds,
   with no Steam and no game, and are where crash diagnostics get proven before trusting them
   in a session.
3. **`python3 tests/finals_bisect/teardown_check.py` in every mode** (~a minute, no Steam, no
   game). This is the C01–C03/C15 reproducer suite; it would have caught C08 (every launch
   dying with "could not load ntdll.so") before `-104` shipped.
4. **Crab Champions** (the DX12 canary) before THE FINALS — the runtime is proven sound without
   the anti-cheat in the loop.
5. **The game with `STEAMDRIVER_PERF=1`, then `tests/finals_bisect/ab_launches.py`** to confirm
   which runtime and `nt_off` each launch actually ran. A "nothing changed" result has been an
   old runtime still live more often than a real finding.

When a new bug shows up at any rung, lock it in immediately: a probe or a `teardown_check`
mode that reproduces it (that is how C01–C03 and C15 went from mysteries to fixed), and a row
in [`crashes/README.md`](crashes/README.md).

## Differences from main

- Its own `ntdll` fixes (image-key flag, exec-region registration for GPTK, AVX signal
  context, socket TOS messages, the syscall fast-path publication for the hook) and the
  MinGW-w64 GCC PE toolchain, which THE FINALS' anti-cheat needs (E111000D otherwise).
- D3DMetal instead of DXMT; `d3d12_layer/` between the game and GPTK's d3d12.dll.
- `lib/macfinals-nt-hook.dylib` inserted into the Wine loader (`add_nt_hook.py`).
- Debug output in Wine's channel model (`macfinals_hook`, `macfinals_dx12`, `macfinals_scan`).
- The Mac driver patches are the shared ones (see [`../README.md`](../README.md)).
