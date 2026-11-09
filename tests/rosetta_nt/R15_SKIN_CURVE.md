# THE FINALS skins missing arms and body parts: root cause (2026-11-02)

Symptom on MacFinals (`macfinals-19`): nearly all skins render without arms and parts of the body.
The ARC package ships an in-memory patch of D3DMetal's Metal output for this ("S12 skin
correction"). Rasmus wants a Wine-side fix instead, so that the shader is translated correctly
before GPTK, without touching Rosetta, injecting anything or modifying GPTK.

## Data

| What | Where |
|---|---|
| Mac: 13,965 DXIL shaders the game gave D3DMetal, from D3DMetal's cache `bytecode_cache.bin` | `~/MacFinals-build-testing/shaders/dxbc/` |
| Mac: 14,719 D3DMetal translations (metallibs) from `stage_cache.bin`, disassembled with `metal-opt -S` | `~/MacFinals-build-testing/shaders/{metallib,air}/` |
| Mac DXIL disassembled with Microsoft `dxc -dumpbin` (v1.9.2609) | Ubuntu `/mnt/data/SteamLibrary/mac-shaders/dis/` |
| Proton reference: 8,663 DXIL + vkd3d-proton SPIR-V (`VKD3D_SHADER_DUMP_PATH`) | Ubuntu `/mnt/data/SteamLibrary/finals-shaders/`, disassembled in `/tmp/fs-dis/` |
| Tools | `~/MacFinals-build-testing/shaders/tools/` (`dxil2bc.py`, `metallib2ll.sh`, `curve_fill.py`), `metaltest/curve33.swift` |

The DXBC cache files are D3DMetal's own cache under `$(getconf DARWIN_USER_CACHE_DIR)d3dm/Discovery.exe`.
No game file on disk was read.

## The construct (39 DXIL shaders, 7 fill variants; 44 translated modules)

```llvm
%20 = alloca [33 x float]                                   ; local curve
; 33 stores, slot i <- consecutive cbuffer floats (cbufferLoadLegacy rows, 4 per row)
;   e.g. byte offsets 136, 140, ... 264 (7 distinct base offsets across the shaders)
%319 = fmul fast float %x, 3.200000e+01
%320 = call float @dx.op.unary.f32(i32 26, float %319)      ; Round_ne
%321 = fptosi float %320 to i32                             ; no clamp to [0, 32]
%322 = getelementptr inbounds [33 x float], [33 x float]* %20, i32 0, i32 %321
%323 = load float, float* %322
```

## What each translator does

| | Result |
|---|---|
| **D3DMetal / Apple's converter (AIR)** | Keeps the private array. **Fill identical to DXIL** in all 44 modules (`curve_fill.py`: slot-by-slot byte offsets equal). Index identical: `x*32`, `air.fast_rint`, `fptosi`, no clamp. A correct translation of the DXIL. |
| **Apple's Metal compiler** | A plain Metal kernel with the same pattern (`metaltest/curve33.swift`, M5 Max): **0 of 4,096 wrong** for in-range `x`, fast and safe math, float[33] and float[32]. Not miscompiled. |
| **vkd3d-proton / dxil-spirv (Proton, correct)** | **Removes the array.** Reads the cbuffer directly: `RoundEven(x*32)` → `ConvertFToS` → `IAdd base` → `AccessChain cbuffer` → `Load`. |

**Conclusion:** the only behaviour difference is for **indices outside 0–32**, which DXIL leaves
undefined. Proton then reads neighbouring constant-buffer floats (bounds-checked buffer). The Mac
reads past the private array, an undefined value, so the visibility threshold fails and pixels are
discarded: missing limbs. The game must produce such indices for some parts.

## Fix plan (Wine side, no Rosetta, no injection, GPTK unchanged)

A Wine `d3d12` layer between the game and GPTK's `d3d12.dll` that rewrites this construct the way
dxil-spirv does, before D3DMetal sees the DXIL:

- Match: an `alloca [N x float]` whose every slot is stored once from consecutive cbuffer floats of
  one handle, then read with a dynamic index (no other uses).
- Rewrite: the dynamic read becomes `cbufferLoadLegacy(handle, (base + idx) >> 2)` plus a component
  select by `(base + idx) & 3`. The array and its stores become dead.
- In range: identical. Out of range: the same as Proton.
- Generic, not a shader list. Unmatched shaders pass through untouched.

Open engineering questions:
- DXIL is LLVM 3.7 bitcode. The rewrite needs a bitcode reader/writer that understands it
  (Homebrew LLVM 23 cannot read it). Options: a minimal purpose-built bitcode editor, or linking
  DXC's open-source libraries.
- Whether D3DMetal checks the DXIL container hash or signature after a rewrite (recompute it if so).
- Where the layer sits: a Wine-built `d3d12.dll` forwarding to GPTK's (renamed), wrapping
  `ID3D12Device::Create*PipelineState` and pipeline-state streams.
- Verify that indices really leave 0–32, by a debug mode of the layer that clamps instead (if the
  limbs come back, confirmed).

## Status 2026-11-02 (late): the layer ships and loads

The clamp variant is implemented and shipped (the Proton-style array-elimination rewrite remains
future work in the same engine). Components:

- **`d3d12_layer/`** — `dxil_rewrite.c` (bitstream parser/writer, construct matching, clamp
  insertion, DXBC container rebuild with recomputed checksums), `layer.c` (the forwarding device
  layer; every export except `D3D12CreateDevice` is a def-file forwarder), `gen_device.py`
  (generates the 79 forwarding thunks from MinGW's vtable), `build.sh` (MinGW-w64 GCC, the same
  toolchain as the rest of the Windows side, then `winebuild --builtin`).
  Detection is structural — any dynamic read of a stack float array with no bounds check —
  not a shader list: over the 13,965 dumped shaders it rewrote 94 (39 curve lookups + 55 other
  unchecked reads), all `dxv`-valid, re-encode byte-identical to `dxc`, ASan/UBSan clean.
- **Runtime layout** (first shipped in `macfinals-20`): the layer is installed as
  `lib/wine/x86_64-windows/d3d12.dll`; Apple's own d3d12.dll stays byte-identical at
  `lib/wine/d3d12_gptk.dll` with `lib/wine/d3d12_gptk.so → ../external/libd3dshared.dylib` as its
  unixlib; `lib/wine/x86_64-windows/d3d12_gptk.dll` is a Wine *placeholder* (fake dll).
  Assembled by `scripts/assemble_portable_runtime.py` after the unmodified-Apple check; manifest
  records it under `d3d12_layer`.

### Why the odd layout: Wine resolves builtins by the PE's internal export name

Apple's `d3d12.dll` says `LIBRARY d3d12.dll` inside (`.edata`), and Wine's loader keys builtin
modules on that name, not the filename (`find_builtin_dll`, `dlls/ntdll/unix/loader.c`; the
substitution happens for every image mapping via `virtual_map_section → load_builtin`). So:

- Two files answering to `d3d12.dll` cannot coexist: whichever builtin holds the name absorbs
  every load of the other. A real GPTK PE placed in the prefix (e.g. copied by prefix bootstrap
  from the arch dir) makes the layer's `LoadLibraryW(L"d3d12_gptk.dll")` resolve to the layer
  itself → `D3D12CreateDevice` recurses → EXCEPTION_STACK_OVERFLOW (seen 2026-11-02 04:10).
- Therefore the real GPTK PE lives **outside the arch dir** (`lib/wine/d3d12_gptk.dll`), where
  prefix bootstrap never copies it; only Wine's builtin search reaches it (its third candidate,
  `<dll_path>/<name>.dll`). The placeholder in the arch dir reserves the `d3d12_gptk` name in
  `system32` (bootstrap copies it there; a placeholder has no exports, so the builtin lookup
  keys on the filename and finds the real module).
- Verified on `macfinals-21`: the placeholder survives `wineboot --init`, the layer loads at
  `…F410000` with GPTK at `…F3C0000` behind it, `gsregion-probe.exe d3d` creates a device.

### Testing THE FINALS

```sh
# quit the app, Steam and the game first
RUNTIME=macfinals-21 MACFINALS_D3D12_LAYER_LOG=1 tests/finals_bisect/launch_main.sh
# (2026-11-09: MACFINALS_D3D12_LAYER_LOG is retired; use WINEDEBUG=-all,+macfinals_dx12)
```

`MACFINALS_SKIN_FIX=0` turns the layer into a pure forwarder (A/B test). The layer logs one
`macfinals-d3d12:` init line per process and, with `MACFINALS_D3D12_LAYER_LOG=1`, every rewritten
shader. Known gap: pipelines loaded from a pipeline *library* are not rewritten (the layer logs
that once if the game uses one).


### 2026-11-02 (later): the first two in-game runs never reached the rewrite

`macfinals-21` and `macfinals-23` loaded the layer in the game, but **no shader went through
it**:

- The layer logged to the Windows stderr handle, which a game started by Steam does not have,
  so `macfinals-21` showed no layer lines at all. Since `macfinals-23` the layer writes through
  Wine's `__wine_dbg_output` with a `pid:tid:macfinals-d3d12:` prefix, so its lines land in the
  same log as `WINEDEBUG` traces even with `WINEDEBUG=-all`. Nothing reaches the game.
- `macfinals-23` then showed the real gap: THE FINALS creates its device with
  `D3D12CreateDevice` (GPTK has no `ID3D12DeviceFactory`: `D3D12GetInterface` returns
  `E_NOINTERFACE`) and then asks it for **ID3D12Device11** (`5405c344-…`) and **ID3D12Device12**
  (`5af5c532-…`). The layer wrapped only up to Device10, so those requests got GPTK's device and
  every pipeline was created past the layer (no "first … through the layer" line). Two more
  IIDs it asks for, `a7aa6116-9c8d-4bba-9083-b4d816b71b78` and
  `ccffef16-7b69-468f-bce3-cd953369a39a`, are in neither MinGW's nor Wine's headers.
- Since `macfinals-24`/`-25` every slot of the wrapper's vtable is a 16-byte generic stub
  (`device_thunks` in `layer.c`: `this` becomes GPTK's device, jump to the same slot), so the
  wrapper is correct for any device version without hand-written vtables; C overrides only the
  pipeline methods. GPTK supports up to ID3D12Device12 and the wrapper now answers for it.
  `tests/windows_probe/d3d12_factory_probe.c` checks that QI(Device12) returns the wrapper and
  that stubbed methods, including struct-returning `GetAdapterLuid`, reach GPTK correctly.
- Remaining blind spot: objects GPTK creates return GPTK's device from `GetDevice`, so a
  pipeline created on a device obtained that way would still bypass the layer.

### 2026-11-02: the clamp reached D3DMetal and did not bring the limbs back

`macfinals-25` in game: pipelines go through the layer (`first CreatePipelineState (stream)`,
`first CreateComputePipelineState`), 75+ shaders rewritten. D3DMetal's `bytecode_cache.bin`
holds the clamped versions of **all 39 curve shaders** (and all 94 rewritten ones), matched by
DXBC checksum against the offline C output. Arms and neck are still missing.

So clamping the index does not fix it. Two readings remain, and they predict different things:

1. The game does read the curve out of range and needs what Proton returns there: the
   constant-buffer floats that follow the array, not slot 0 or 32. Then the Proton-style
   rewrite (read the cbuffer directly) is the fix.
2. The indices never leave 0–32 (or it does not matter), and the curve construct is not
   the cause at all.

Untested assumption underneath both: that these shaders draw the missing limbs.
(`launch_main.sh` cleared `MACFINALS_*` before this run, so the per-shader log was off;
fixed afterwards.)

### 2026-11-02: the Proton-style rewrite (runtime `macfinals-26` / `-27`)

How dxil-spirv translates the curve read (vkd3d-proton dump, Ubuntu `/tmp/fs-dis`, found with
a pattern search for `FMul x, 32.0 → RoundEven → ConvertFToS → IAdd base → AccessChain`): the
constant buffer is declared as a plain float array (`OpTypeArray float 115`, the buffer's size)
and indexed directly with `RoundEven(x*32) + base`. The private array does not exist in the
translation. An index past the curve reads the buffer's following floats; outside the buffer,
vkd3d-proton's robust buffer access gives 0.

`dxil_rewrite.c` now does the same (`SITE_CBREAD`):

- Match: an `alloca [N x float]` whose every slot k is stored exactly once from float B+k of
  one constant-buffer handle (`extractvalue` of `cbufferLoadLegacy(59, h, row)`), and whose
  only other users are GEPs that are loaded from. Arrays that do not match keep only the clamp.
- At each dynamic read: `off = idx + B`; `ok = off <u size`; `row = off >> 2`,
  `comp = off & 3` (from `ok ? off : 0`); `cbufferLoadLegacy(59, h, row)`; select the
  component; `ok ? value : 0.0`. The size is the declared one from the handle's
  `annotateHandle` ResourceProperties (`{ i32 13 /* CBuffer */, i32 bytes }`; these are SM 6.6
  shaders), 64 KiB if the handle has none. The old array read stays, clamped and unused.
- Constants the rewrite needs that the module lacks are appended to the function's constant
  block (raw copy of the block, then new records), numbered before the instructions.

Checked: all 39 curve shaders match (base for `0164d04af25b17bd`: float 34 = byte 136, bound
119 floats = 476 bytes; dxc disassembly shows the sequence above and the old load unused).
Over all 13,965 shaders under ASan/UBSan: 98 rewritten (4 more than before, because missing
constants no longer abort the rewrite), 72 of them with direct buffer reads, no sanitizer
reports; `dxv` accepts all 98.

### 2026-11-02: result — the limbs are back (`macfinals-27`)

Rasmus confirmed arms and neck render on `macfinals-27`. So the game does read the curve out of
0–32 and needs the values Proton gives there (the constant buffer's following floats), which
the clamp could not provide. The Proton-equivalent rewrite is the fix.
