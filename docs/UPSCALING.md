# MetalFX upscaling (DLSS) in SteamDriver

How SteamDriver's compatibility tool (the finals environment, `environments/finals/`) runs a
game's NVIDIA DLSS on Apple's MetalFX, what each piece does, and what is known not to work.
State of 2026-11-25, runtime `macfinals-191`.

## In short

- **Turn it on** in macSteam: Profiles → a DX12 or DX11 profile (Global or one game) →
  Settings… → **MetalFX upscaling (DLSS)**. Launch option `STEAMDRIVER_UPSCALE=1` or `0`
  overrides the profile.
- **DX12 games** use Apple's DLSS stand-in on D3DMetal. Works in ARC Raiders and THE FINALS.
- **DX11 games** stay on DXMT and use DXMT's DLSS stand-in.
- With it on, the game sees an **NVIDIA GPU** (D3DMetal: RTX 4090 identity; DXMT: "NVIDIA
  GeForce RTX 4090"), because games only offer DLSS on NVIDIA. The Mac's GPU does all the work.
- THE FINALS gets it like any game (Rasmus, 2026-11-25; `environments/finals/CLAUDE.md`, hard
  boundary 2).
- **Deadlock doesn't offer DLSS** on any route: its DLSS loader accepts only an NVIDIA-signed
  DLSS file (below). Use its FSR 2 instead.

## Which profile applies

The tool's run script (`environments/finals/compat-tool/run`) decides at launch:

1. `STEAMDRIVER_UPSCALE=1|0` in the launch options wins.
2. Else the game's own profile (`profiles/app-<id>/`): `upscaling` in `d3dmetal.conf` (DX12) or
   `steamdriver.upscaling` in `dxmt.conf` (DX11, a key DXMT itself ignores). A game profile
   without the key means off.
3. Else Global DX11 for a game last detected as DX11, else Global DX12. A DX11 game's very first
   launch has nothing detected yet and reads Global DX12.

Switching a game profile between DX11 and DX12 keeps the other kind's file aside
(`dxmt.conf.dx11`, `d3dmetal.conf.dx12`). Details: [`PROFILES.md`](PROFILES.md).

The run log (`compatdata/<id>/steamdriver-run.log`) says which route ran:
`upscaling: DLSS runs on MetalFX (D3DM_ENABLE_METALFX=1, …)` or
`upscaling: DLSS runs on MetalFX through DXMT's stand-in (DXMT_ENABLE_NVEXT=1, …)`.

## DX12: Apple's DLSS on D3DMetal

Apple's Game Porting Toolkit ships `nvngx-on-metalfx` (a DLSS stand-in that calls MetalFX) and
`nvapi64`. Its Read Me ("experimental MetalFX integration") says: use them under the name
`nvngx`, put `nvngx.dll` and `nvapi64.dll` into the prefix's system32, set
`D3DM_ENABLE_METALFX=1` (macOS 26 or later).

| Piece | Where | Why |
|---|---|---|
| `nvngx.dll` / `nvngx.so` links to Apple's `nvngx-on-metalfx` | runtime, `scripts/add_nvngx.py` | Nothing loads it under Apple's file name |
| Apple's real files copied into system32 | run script, each upscaling launch | NVIDIA's loader in the game checks the signature of the file on disk; Wine's placeholder there is unsigned (`800b0100`), Apple's is signed by Apple Inc. |
| `D3DM_ENABLE_METALFX=1`, `nvapi64,nvngx=b` | run script | D3DMetal then switches to its NVIDIA identity and writes NVIDIA's `NGXCore` registry key itself |
| NVAPI layer (`nvapi_layer/`) in front of Apple's NVAPI | runtime, `scripts/add_nvapi_layer.py` | Apple's NVAPI reports driver 576.02 while D3DMetal registers 616.56 (32.0.16.1656); the layer answers the two driver-version queries with the registered version and passes everything else through. Laid out like the d3d12 layer (Apple's file as `lib/wine/nvapi64_gptk.dll` + `.so`); the run script puts its `nvapi64_gptk.dll` placeholder into system32 |
| `trace+macfinals_nv` | run script, upscaling launches without a user `WINEDEBUG` | Logs which NVAPI functions the game asks for and any Apple's NVAPI lacks |

Apple's module also answers Direct3D 11, but only on D3DMetal's own device. That is why DX11
games use DXMT's stand-in instead; a runtime without it falls back to D3DMetal for that launch
(with Vulkan off, as Source 2 games crash on MoltenVK).

## DX11: DXMT's DLSS on DXMT

The main environment's DXMT has a DLSS stand-in (`patches/dxmt-metalfx-upscaling.patch`,
[`METALFX.md`](METALFX.md)) and an NVAPI stand-in. No DX11 game has been confirmed with it on
SteamDriver yet.

| Piece | Where | Why |
|---|---|---|
| DXMT's `nvngx.dll` and `nvapi64.dll` in `lib/dxmt/x86_64-windows/` | runtime, `scripts/add_dxmt_dlss.py` | The names `nvngx`/`nvapi64` belong to Apple's builtins in `lib/wine`, so these load as native: Wine's builtin marker is zeroed, as for DXMT's d3d11 (`add_dxmt.py`) |
| `nvngx.dll` signed again | same script, `scripts/sign_pe.py` | Zeroing the marker breaks the signature. It is signed with SteamDriver's own certificate (`sign_pe.py create-certificate DIR --name SteamDriver`, keys outside git in `~/SteamDriver-build/finals/signing/steamdriver-nvngx`). Wine accepts an intact signature from any root |
| Both copied into system32, `nvapi64,nvngx=n` | run script | |
| `DXMT_ENABLE_NVEXT=1` | run script | DXMT presents an NVIDIA GeForce RTX 4090 (DXGI driver 32.0.19.9999, NVAPI 999.99) |
| `NGXCore` `FullPath` = `C:\Windows\System32` | run script, only if missing | D3DMetal writes it on its route; DXMT doesn't |
| `WINE_D3D_CONFIG=VideoDriverVersion=32.0.19.9999` | run script | Direct3D 9 reports DXMT's version (below) |

The game keeps its DX11 profile's DXMT settings (frame latency, late input sampling, shader
threads) while upscaling.

## Driver versions games see

Games compare the driver version with their minimum and turn features off below it. Deadlock
needs 32.0.15.8108 (NVIDIA 581.08) and reads it through **Direct3D 9**, which is Wine's wined3d.
wined3d's table only knows old cards and gave every new NVIDIA card a GeForce 8 driver,
31.0.13.4052: Deadlock showed "Graphics driver is out of date" and turned asynchronous shader
creation off.

| Source | D3DMetal route | DXMT route |
|---|---|---|
| Registry (display class key) | 32.0.16.1656 (D3DMetal writes it) | none |
| DXGI `CheckInterfaceSupport` | 65535.65535.65535.65535 (D3DMetal's) | 32.0.19.9999 |
| Direct3D 9 (wined3d) | 32.0.16.1656 | 32.0.19.9999 |
| NVAPI | 616.56 (layer) | 999.99 |

The two Wine patches behind the Direct3D 9 row:

- `wine-wined3d-registry-driver-version.patch` (+ `-types.patch`): wined3d takes the
  `DriverVersion` from the display class key when exactly one adapter is registered and its
  provider matches the vendor wined3d sees; otherwise the table stays.
- `wine-wined3d-config-driver-version.patch`: a `VideoDriverVersion` setting
  (`WINE_D3D_CONFIG`, then `Software\Wine\Direct3D`) that wins over both, for the DXMT route.

## Deadlock: why there is no DLSS

Deadlock's DX11 renderer has DLSS (`CDxDlssUpscaler`, NVIDIA's D3D11 NGX code), and all the
checks we can see now pass: driver version above its minimum on every source, no "out of date"
message, NVAPI functions all present, the DLSS module reports itself available
(`SuperSampling.Available` 1, `NeedsUpdatedDriver` 0). Still the console says
`Upscaler UPSCALER_NVIDIA_DLSS is not available : backing down to UPSCALER_AMD_FSR3`, and
**`nvngx.dll` is never loaded**: the game gives up before opening the file.

Its NGX loader (strings in `rendersystemdx11.dll`, in order) looks for the NVIDIA driver through
Windows' service and device APIs (`nvlddmkm`, the driver store, `D3DKMTQueryAdapterInfo`), then
the `NGXCore` registry keys, and **checks the file's signer** before loading it
(`WinVerifyTrust`, `WTHelperGetProvSignerFromChain`, `CertGetNameStringA`, next to the string
`"NVIDIA "`). Apple's stand-in is signed by Apple Inc., DXMT's by SteamDriver (by Recall on the
main environment); neither name starts with NVIDIA, so the file is rejected. Older loaders
only checked that the signature was valid, which is what DXMT's stand-in was built against.
The signer check is inferred from the loader's code, not from a log line, but nothing else fits.

**Not done, on purpose:** signing a stand-in with a certificate in NVIDIA's name. That would
forge another company's code signature and ship it.

**For Deadlock:** leave MetalFX upscaling off in its profile (plain DXMT, its tuned settings, no
NVIDIA identity) and choose **FSR 2** in its video settings; it works on DX11 under DXMT. The
"PIX/NSIGHT WAS detected" line in its console comes from none of the three standard checks
(`ID3DUserDefinedAnnotation::GetStatus`, `D3DPERF_GetStatus`, `DXGIGetDebugInterface1`) and
doesn't seem to matter.

## Games

| Game | API | Result |
|---|---|---|
| Deadlock | DX11 | No DLSS: loader requires an NVIDIA signer. FSR 2 works |
| ARC Raiders | DX12 | DLSS on MetalFX works (D3DMetal route; confirmed by Rasmus 2026-11-25) |
| THE FINALS | DX12 | DLSS on MetalFX works (D3DMetal route; confirmed by Rasmus 2026-11-25) |

## Testing

`environments/finals/tests/windows_probe/dlss_probe.c` checks everything a game's DLSS start-up
sees, without a game:

```sh
x86_64-w64-mingw32-gcc -O1 -Wall environments/finals/tests/windows_probe/dlss_probe.c \
  -o runtime/build/probe/dlss-probe.exe -ld3d12 -ld3d11 -ld3d9 -ldxgi -luuid -ladvapi32 -lwintrust
```

It prints the DXGI adapter (name, vendor, driver version), the Direct3D 9 driver version, NVAPI
start and driver version, the `NGXCore` path, `WinVerifyTrust` on `nvngx.dll` there, and NGX
`Init` / `GetCapabilityParameters` with `SuperSampling.Available`, `NeedsUpdatedDriver` and the
minimum driver, plus D3D11 threading caps and the three capture-tool checks. Run it with
`d3d11` as its argument on the DXMT route (DXMT's DXGI with Apple's d3d12 is not a pair a game
uses). It can't see a game's own signer check.

For a game, `-condebug` after `%command%` makes Source 2 games write their console to
`game/<mod>/console.log`.

## Runtime chain

`macfinals-191` = assemble, `add_nt_hook.py`, `unbundle_gptk.py`, `add_dxmt.py`,
`add_nvngx.py`, `add_nvapi_layer.py`, `add_dxmt_dlss.py`
([`environments/finals/README.md`](../environments/finals/README.md), Building).
