# r13 — GS-base switch crashes every D3D12 process (GPTK direct calls)

Status brief, 2026-11-01. Follows `R13_STATUS.md` (the GS-base switch design is there).
Runtime **macfinals-9** (`~/MacFinals-build/phase-2/artifacts/macfinals-9`), not installed in
any app. No commits.

## Summary

- With GS switching on, any process that loads D3D12 crashes. With `MACFINALS_NO_GS_TEB=1`
  the same probe exits 0. The switch is the cause.
- GPTK (`libd3dshared.dylib`) calls macOS code directly from Windows threads, skipping
  Wine's dispatchers and therefore every switch point. That code reads its pthread data
  through `%gs` and gets the TEB.
- GPTK assumes `%gs` is always macOS thread data, which is how CrossOver works today.
  With the switch on for every process, **every DX12 game crashes, including Crab
  Champions (the canary) and THE FINALS.** Steam's web helper does not load D3DMetal and
  may be fine. Not tested.

## 1. Fixed: the loader marker in `Instrumentation[1]`

- v1 (`wine-macos-gsbase-teb-switch.patch`) marked threads for `loader.c` by writing magic
  `0x47425357` into `TEB->Instrumentation[1]`. With D3D12 loaded, something dereferences
  that field: `page fault on read access to 0x4742536F` = magic + 0x18.
- Fix: `patches/wine-macos-gsbase-loader-marker.patch`, appended after v1 because v1 is
  already applied and recorded. The marker moves into `SYSTEM_DLL_INIT_BLOCK`, Wine's
  existing Unix→PE channel:
  - `include/winternl.h`: new last field `ULONG64 mac_gsbase_teb`.
  - `dlls/ntdll/unix/loader.c`: `load_ntdll_functions` sets it from `mac_gsbase_switching`.
    `signal_init_process` has already run by then.
  - `dlls/ntdll/loader.c`: CW HACK 18756 gated on `!LdrSystemDllInitBlock.mac_gsbase_teb`.
  - `signal_x86_64.c`: the magic and the `Instrumentation[1]` write are removed.
- Verified: the patch round-trips (`patch -R` cmp-clean, `git apply --check`), it builds
  incrementally, macfinals-9 assembles, the 25 unit tests pass, and the `0x4742536F` crash
  is gone.

## 2. Open: GPTK's native code runs with GS = TEB

Command:

```sh
WINEPREFIX=/tmp/r13gs/prefix-gsr9 WINEDEBUG=-all \
  ~/MacFinals-build/phase-2/artifacts/macfinals-9/bin/wine \
  runtime/build/probe/gsregion-probe.exe d3d
```

Result (switch on):

```
Unhandled exception: page fault on read access to 0x0000000000000000 in 64-bit code (0x007ff80944f1e3).
 rip:00007ff80944f1e3 rsp:00000000014dfdc0 rbp:00000000014dfde0
 rbx:0000000000000000 rax:e1ca4ab533697525
=>0 0x007ff80944f1e3 in libsystem_pthread.dylib (+0x31e3)
   libsystem_pthread.dylib+0x31e3: xorq (%rbx), %rax
```

- Return addresses symbolised with `atos` against the shipped (unmodified)
  `lib/external/libd3dshared.dylib`, loaded at `0x208c2c000`:
  - `0x208c2e570` = `D3DMThreadInit(void*) + 32`
  - `0x208c40240` = `D3DRMDispatch_Init(void*)` block `+ 96`
- `rsp 0x14dfdc0` is a **Windows thread stack**, not Wine's kernel stack. Native code is
  running without passing through `__wine_syscall_dispatcher` or
  `__wine_unix_call_dispatcher`.
- `libd3dshared` exports `PatchUnixCallTable`, `PatchEntrypointCallTable`,
  `PatchWin32DispatchFunctions` and `EmitCallTrampoline`. GPTK builds its own PE→native
  trampolines and rewrites call tables, so some calls never reach Wine's switch points.
  Only the exported names were read. The code was not disassembled.
- Same run with `MACFINALS_NO_GS_TEB=1`: exit 0, and the probe prints its usual
  without-switch DIFF table. The switch is the cause.

### Correction to the earlier diagnosis

The earlier first d3d crash (`page fault on write access to 0x110000 in
libsystem_kernel.dylib`) was set aside after confirming that GPTK reaches Unix through
`__wine_unix_call_exported` (CW HACK 22435). That check was incomplete: *some* GPTK calls
go through Wine, but the trampolines above do not. `0x110000` is the TEB's `StackBase`,
read as pthread data, so that crash was most likely this same problem, not the marker.

## 3. Options

| Option | Problem |
|---|---|
| Switch GS back to macOS inside GPTK's trampolines | Means changing GPTK's generated code in memory. Probably over hard boundary 5 ("Do not modify GPTK / D3DMetal"). Rasmus's call. |
| Find out which Wine tables `PatchUnixCallTable` / `PatchEntrypointCallTable` rewrite, and see whether a switch fits on Wine's side | Stays within "use, not modification", but means studying what GPTK does to Wine's memory. Not designed yet, may not pan out. |
| Turn the switch off in processes that load D3DMetal | Safe, but THE FINALS is DX12 and would never get the fix. |
| Ship with the switch off by default, keep the patch for later | Nothing lost: switch off, macfinals-9 behaves like macfinals-8. |

**Recommendation:** option 2. It is the only one that could get THE FINALS a truthful `%gs`
without touching GPTK. It examines GPTK's behaviour, so it waits for Rasmus's go-ahead.

## 3b. Finding: registered code regions already get true GS (2026-11-01, later)

All earlier probe runs were done **without** `CX_APPLEGPTK_LIBD3DSHARED_PATH`, so CW HACK
22434 (`pe_module_loaded` → `register_non_native_code_region`) never ran. The app does set
it (`scripts/portable_setup.m:169`). With it set, macfinals-9:

- **A: switch OFF, no d3d:** `gs reads from this loader-mapped image: pid same, tid same,
  StackBase same`. Those fields are not among the three mirrors, so in a region registered
  with GPTK, Rosetta gives Windows code real TEB semantics through `%gs` **without any GS
  base switch**. Unregistered dynamic code still differs: VirtualAlloc RWX, RW→RX, section
  view, reserve+commit RX at `0x710000000000`, second thread: all DIFF/WRONG.
- **B: switch ON, d3d:** crashes as in §2 (also `write access to 0x110000` in
  `libsystem_kernel`, confirming the correction above).

So the GS semantics follow **code regions**, not the thread's GS base. Native code (GPTK
included) is not registered and keeps macOS `%gs`, so there is no conflict. libd3dshared
exports only `register_non_native_code_region` and `supports_non_native_code_regions`
(no unregister).

The other CrossOver THE FINALS package (`~/Downloads/THE FINALS — CrossOver 26.3 —
lists and comment lines were read.

**New plan (supersedes §3):** a Wine patch in `dlls/ntdll/unix/virtual.c` that registers
every Windows executable range with `register_non_native_code_region`: executable
allocations, protection changes to executable, and executable section views, on top of
the PE images CW HACK 22434 already covers. Gate: `gsregion_probe` all "same" with the
switch off. The GS-base switch then defaults off (or is dropped). This is use of a GPTK
interface, which boundary 5 allows.

## 4. Still open

- `macfinals-probe.exe` 4/4 with the NT hook and the GS patch together: not run.
- Steam, Crab Champions, THE FINALS on macfinals-9: not run (Crab Champions and THE FINALS
  are expected to crash as in §2 while the switch is on).
- Proton three-way of `macfinals-probe.exe`: not run.
- Earlier gates that passed on macfinals-8 (switch on, no D3D): `segment_probe` gs_scan 0 of
  768 qwords differ (was 36), `tls_alloc_probe` first TlsAlloc = 2, 1085 slots, FlsAlloc 2,3
  (Proton parity; was 65/1023), `gsregion_probe` without `d3d` all six cases "same". Not
  re-run on macfinals-9.
