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
| Running the game from macOS Steam (compatibility tool "SteamDriver", formerly "SteamDriver: THE FINALS") | [`compat-tool/`](compat-tool/README.md) |
| Running the game outside the app, logs | [`tests/finals_bisect/launch_main.sh`](tests/finals_bisect/launch_main.sh) |
| Windows probes | [`tests/windows_probe/`](tests/windows_probe/) |

## First launch: it may take two

The first launch in a new prefix (and sometimes the first after a runtime change) can fail or
stop at start-up, and the second one goes in. That is expected, not a regression: on the first
run the game's anti-cheat sets itself up in the prefix (its service and runtime files), and
the launch that does the setup often doesn't make it into the game. For THE FINALS this shows
as Denuvo's "issue with starting Denuvo Anti-Cheat" (`2:{launcher,10}`); just launch again.
Other games with an anti-cheat can behave the same way. Only treat it as a bug when the
second and later launches fail too.

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
python3 $F/scripts/add_dxmt.py --workspace $W --base-version macfinals-N+1 --version macfinals-N+2
python3 $F/scripts/add_nvngx.py --workspace $W --base-version macfinals-N+2 --version macfinals-N+3
python3 $F/scripts/add_nvapi_layer.py --workspace $W --base-version macfinals-N+3 --version macfinals-N+4
python3 $F/scripts/add_dxmt_dlss.py --workspace $W --base-version macfinals-N+4 --version macfinals-N+5
```

(The shipped chain since `macfinals-191`, with the wined3d VideoDriverVersion patch: assemble, `add_nt_hook.py`, `unbundle_gptk.py`,
`add_dxmt.py`, `add_nvngx.py`, `add_nvapi_layer.py`, `add_dxmt_dlss.py`.)

`add_dxmt_dlss.py` (since `macfinals-184`) adds DXMT's own DLSS and NVAPI stand-ins (from the
main environment's DXMT, `docs/METALFX.md`) to `lib/dxmt`, with Wine's builtin marker zeroed
like DXMT's d3d11, and signs `nvngx.dll` again with SteamDriver's own certificate (NVIDIA's
loader in the game loads only a signed one; `scripts/sign_pe.py create-certificate DIR --name
SteamDriver`, keys outside git in `~/SteamDriver-build/finals/signing/`). With upscaling on, a
DX11 game on DXMT now stays on DXMT: the tool puts both stand-ins into system32 as native, sets
`DXMT_ENABLE_NVEXT=1` (DXMT presents an NVIDIA GeForce RTX 4090) and makes sure NVIDIA's
`NGXCore` key points at system32. Apple's DLSS (D3DMetal) stays the route for DX12 games, and
for DX11 games only on a runtime without these files. `dlss_probe.exe d3d11` proves it: DLSS
starts on DXMT's own D3D11 device.

`add_nvapi_layer.py` (since `macfinals-182`) puts SteamDriver's NVAPI layer (`nvapi_layer/`) in
front of Apple's NVAPI, laid out like the d3d12 layer (Apple's file as `lib/wine/nvapi64_gptk.dll`
with `nvapi64_gptk.so`). It passes every call through except the two driver-version queries,
which it answers with the version D3DMetal registers (616.56) instead of Apple's 576.02, which
was below Deadlock's minimum and left DLSS hidden and asynchronous shader creation off. It only
loads in upscaling launches; the tool then also puts its `nvapi64_gptk.dll` placeholder into
system32.

MetalFX upscaling as a whole (both routes, driver versions, Deadlock): [`docs/UPSCALING.md`](../../docs/UPSCALING.md).

`add_nvngx.py` (since `macfinals-176`, 2026-11-25) makes Apple's DLSS-to-MetalFX module answer
to the name `nvngx`, as the toolkit's Read Me describes ("experimental MetalFX integration").
It does this with two links to Apple's unchanged `nvngx-on-metalfx.dll` / `.so`. With the launch
option `STEAMDRIVER_UPSCALE=1 %command%`, the tool copies Apple's `nvngx.dll` and `nvapi64.dll` into
the prefix's system32, lets them load and sets `D3DM_ENABLE_METALFX=1`. The copies matter: a
game's NGX loader checks the signature of `nvngx.dll` on disk, and only Apple's file is signed
(Apple Inc.). The unsigned placeholder from `wine.inf` fails with `800b0100`, and the game then
hides DLSS. D3DMetal then writes NVIDIA's `NGXCore` key itself, and a game's DLSS
runs on MetalFX. With it on, **D3DMetal reports an NVIDIA GPU** (vendor `10de`; its default
identity is "AMD Compatibility Mode", `1002`), because games offer DLSS only on NVIDIA. It is
off by default. THE FINALS gets it like any game (Rasmus, 2026-11-25: Apple's own switch, for
performance; see CLAUDE.md, hard boundary 2). Apple's module answers
Direct3D 11 as well as 12, but only on D3DMetal's own device, so a DX11 game with upscaling
renders with D3DMetal for that launch even when `STEAMDRIVER_DX11` asks for DXMT (Vulkan stays
off then, as in DXMT mode). macSteam's profiles set it (`docs/PROFILES.md`): `upscaling` in a
DX12 profile's `d3dmetal.conf`, `steamdriver.upscaling` in a DX11 profile's `dxmt.conf`.
Proven with `tests/windows_probe/dlss_probe.c`: off, the probe gets no NVAPI and no NGX path;
on, NVAPI initialises, `nvngx.dll` at `NGXCore\FullPath` passes `WinVerifyTrust`, and NGX starts
(`Init`, `GetCapabilityParameters`) on a D3D12 and a D3D11 device from D3DMetal.

`add_dxmt.py` (since `macfinals-175`, 2026-11-25) adds MacVolvo's DXMT from the main runtime
beside D3DMetal, so DX11 games can use the main environment's renderer on this Wine:
`winemetal` in `lib/wine`, and DXMT's d3d11/dxgi/d3d10core in `lib/dxmt`. The tool copies
those into the prefix and loads them as native, with Wine's builtin marker zeroed in the copies,
because Wine never loads a builtin-marked file as native. The default, `auto` (since
2026-11-25), needs no launch option: DXMT for a game whose macSteam game profile is DX11, a
known DX11 game (CS2, Deadlock) or one detected as DX11 at an earlier launch, else D3DMetal.
Deadlock needs it: on D3DMetal, Source 2 starts Vulkan and crashes in MoltenVK. The launch
option `STEAMDRIVER_DX11=dxmt` or `d3dmetal` overrides it. DX12 is always D3DMetal. Proven with
`tests/windows_probe/d3d11_device_probe.c` (device, swap chain, 30 presents through DXMT).
This widens the environment past THE FINALS at Rasmus's request.

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
- D3DMetal instead of DXMT; `d3d12_layer/` between the game and GPTK's d3d12.dll. DXMT is
  available for DX11 games on request (`STEAMDRIVER_DX11`, see Building).
- `lib/macfinals-nt-hook.dylib` inserted into the Wine loader (`add_nt_hook.py`).
- Debug output in Wine's channel model (`macfinals_hook`, `macfinals_dx12`, `macfinals_scan`).
- The Mac driver patches are the shared ones (see [`../README.md`](../README.md)).
