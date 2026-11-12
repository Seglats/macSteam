# THE FINALS: the rope that was missing on Suspended Structures (2026-11-10)

## Result

The rope or chain that holds up the hanging box on Suspended Structures did not draw on the
Mac. It drew on Proton, and blowing it off worked on both, so only the picture was wrong.

**Cause:** the rope's pixel shader (checksum `064345499da6ecbf`, extract file
`b52cb6bb3f0ad537.dxbc`) decides a dithered fade with one `dx.op.discard`. On the Mac that
condition discards every pixel. Removing the shader's fast-math flags brings the rope back.

**Fix, in the d3d12 layer since runtime `macfinals-65`:** for every shader with a discard, the
operations its condition depends on lose their fast-math flags. Everything else stays fast.
On by default; `MACFINALS_PRECISE_DISCARD=0` turns it off. No shader list. Confirmed in game on
`macfinals-65` with plain `%command%` (2026-11-10): the rope draws.

**Not yet proven:**

- "Apple's compiler reorders the large-coordinate math" is the likeliest explanation, not an
  observation. The compiled result was never read, and that test removed every flag at once, so
  another flag (no-NaN, no-infinity) could be the one that matters. The fix covers all of them.
- Why Proton is fine: as far as known, dxil-spirv does not carry DXIL's `fast` into SPIR-V for
  Vulkan. The Proton translation of this shader was not read.
- The performance cost is unmeasured (see "Open").

## What was ruled out, in order

| Idea | How it was tested | Result |
|---|---|---|
| Our skin-fix rewrite hides it | `MACFINALS_SKIN_FIX=0` | Arms vanished, rope still missing |
| Gameplay or simulation problem | Rasmus: the rope can be blown off, and it draws on Linux | Drawing only |
| Thin geometry lost at low render scale | 100 % render scale in game | Still missing |
| Metal API misuse | `MTL_DEBUG_LAYER=1` | Warnings only, nothing about the rope (below) |
| Draw never issued | Metal capture, draw list | Draw 853 is the rope, in the right place |
| Colour writes off | Pipeline 77 | Write mask All, no blending, rasterization on |
| Depth test | `MTLDepthStencilState 16`, also used by the box | `GreaterEqual`, same state as the box |
| Stencil test | Same state, read mask `0x2`; references 128 and 132 | Both have that bit clear, same test as the box |
| `SV_Position.w` differs (D3D `w`, Metal `1/w`) | 1,325 translated shaders | D3DMetal inverts it in 1,323; not the cause |
| All pixel discards off | `MACFINALS_NO_DISCARD=1` | **Rope appears** |
| This shader's fast-math flags | `MACFINALS_NO_FAST=0643-0643` | **Rope appears** |

## The shader

Four colour outputs (`SV_Target0-3`), three texture samples, one discard, about 890 lines of
DXIL. The condition `%366` is `(value) < 0`, where the value combines an opacity texture, a
temporal dither from pixel position and a noise texture, a fade on pixel depth, and a fade on
world height and distance. World position uses Unreal's large-world tiles: per-primitive data
from a raw buffer at `PRIMITIVE_ID * 44`, with tile values multiplied by 2,097,152 and later
subtracted so they cancel. That cancellation is why the order of the math should matter.

## Capture findings

Capture `capture-20261110-132406.gputrace`, 2 s window: 10 command buffers, 31 render encoders,
211 draws, 42 dispatches. Numbers beside draws are positions in the command list, not draw
counts. The GBuffer pass is `MTL4RenderCommandEncoder 3` under `MTL4CommandBuffer 6`, draws
327-868. Render size 2260x944 for that output.

| Draw | What | Notes |
|---|---|---|
| 853 | The rope | 60 indices, instance count 1, pipeline 77, vertex function 169, pixel function 168 |
| 734 | The box | 1,050 indices |
| 586 | Cables that work | 132 indices, 3 instances, all on screen; pipeline 54, pixel function 173 |
| 2617 | End-of-frame copy | `ScreenPassVS` + `CopyRectPS`; box visible, no rope |

Xcode draws the current draw's outline in green on the attachment thumbnails, which is the
quickest way to find an object. Color 4 stays mostly black, so green shows up best there. Xcode
keeps the last sub-view when you move between draws, so choose Attachments once, not Bound
Resources, which is slow. D3DMetal's shaders have no source, so Xcode cannot show them or debug
a pixel.

**Compile statistics** in a capture's hash-named property-list files can identify pipelines by
instruction count, but they do not map to function numbers. 1,798 of the capture's 2,027
metallibs matched the 2026-11-02 extract byte for byte. Matching compiled shaders to DXIL by
constants or size gave a wrong family of six candidates, so do not trust it; the binary search
is what found the shader.

## The binary search

`MACFINALS_NO_DISCARD=LLLL-HHHH` disables discards for shaders whose first two checksum bytes
fall in the range and logs each one. The checksum is the DXBC header's, not the extract's file
name (`dxil_rewrite` tool or `xxd -s 4 -l 16` maps between them).

| Range | Shaders with a discard | Rope |
|---|---|---|
| 0000-7fff | 2,713 | yes |
| 0000-3fff | 1,362 | yes |
| 0000-1fff | 668 | yes |
| 0000-0fff | 321 | yes |
| 0000-07ff | 155 | yes |
| 0000-03ff | | no |
| 0400-05ff | | no |
| 0600-06ff | 15 | yes |
| 0600-0643 | | yes |
| 0600-0628 | | no |
| 063c | | no |
| 0643 | 1 | **yes** |

## Why the rule has this shape

The 2,097,152 constant is in 6,186 of 6,998 pixel shaders and 1,816 of 3,978 compute shaders,
so filtering on it barely narrows anything, and dropping fast math wholesale risks performance.

The rule walks backward from each discard's condition (`mark_discard_slices` in
`d3d12_layer/dxil_rewrite.c`). In the extract it changes 3,095 of 13,965 shaders, median 121
operations each; in the rope's shader 61 of 252. Considered and not done: limiting it to chains
that contain the 2,097,152 constant, which would miss a shader that gets the offset from a buffer.

Tests run on all 13,965 extracted shaders under ASan and UBSan: with the switches off the output
is byte-identical to the committed rewriter, with no sanitizer reports; 155 sampled shaders with
the rule on pass Microsoft's `dxv` (`~/tools/dxc/bin` on the Linux box), and the rope's discard
condition is an exact `fcmp olt`.

## Tools added

| Tool | Use |
|---|---|
| `STEAMDRIVER_PERF=1 %command%` | Per-process counters from a normal Steam launch, in `<compatdata>/perf/<date>/` |
| `tests/finals_bisect/perf_report.py` | Summarises a session's counters: calls, threads, faults, freezes, timeline |
| `STEAMDRIVER_CAPTURE=1 %command%` | Metal capture; Control-Option-G saves to `<compatdata>/captures/`, beeps when written |
| `WINEMAC_METAL_CAPTURE_MS` | Capture window in ms (default 150) |
| `tests/finals_bisect/capture_stall_profile.py` | Time Profiler triggered when a capture starts stopping (tested on a probe only) |
| `MACFINALS_NO_DISCARD`, `MACFINALS_NO_FAST` | Debug, `1` or `LLLL-HHHH`; the layer logs each shader changed |
| `MACFINALS_PRECISE_DISCARD=0` | Turns the fix off |

### Metal capture lessons

- `MTL_CAPTURE_ENABLED=1` must be set when the process starts, or capture reports it isn't available.
- Stopping on the Mac driver's main thread never finished in the game. The capture has its own
  queue since `macfinals-57` (`wine-winemac-metal-capture-queue.patch`).
- 150 ms captured no draws (4 command buffers). 2,000 ms captured real frames.
- One 2,000 ms capture (13:03) made no progress for over 10 minutes; the next finished in 42 s.
  Cause unknown. **Wait for the beep**, because quitting first leaves an unusable capture.
- Keep Xcode closed while the game runs. A session with Xcode open produced a run of Little
  Snitch prompts from the Denuvo update service. No anti-cheat action was seen, and the link
  was not established. Use the second account for captures.

### Metal API validation

`MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=nslog MTL_DEBUG_LAYER_WARNING_MODE=nslog`, one
7-minute run: no errors. About 8 million redundant-state warnings, 1.92 million "attachment
texture is not added to any residency set", and 10,784 "residency sets do not support
concurrent write operations". It wrote 20 million lines (1.6 GB) and the game hard-froze leaving
the map, so do not use it for long. The residency warnings are inside D3DMetal and unexplained.

## Performance recordings (`STEAMDRIVER_PERF`, 20 minutes in play)

- 295,000 NT calls a second, 68 % `NtQueryPerformanceCounter`; `NtYieldExecution` only in bursts.
- GameThread 59 % user + 4 % system of a core, at 90 % or more in 20 % of intervals. The likely
  main limit is game code under Rosetta. The syscall storm stays fixed (`sigsys` about 3/s).
- 28 freezes, 0.1-1.3 s, the longest around 112-116 s in.
- Not explained: one `NtQuerySystemInformation` took 376 s; `tryfixed-fail` 36,900/s.

## Open

1. Check that the rope fades as it should at a distance.
2. Measure the cost: `MTL_HUD_ENABLED=1`, with and without `MACFINALS_PRECISE_DISCARD=0`, same
   spot, second runs of each so the shaders are cached.
3. Threads killed with "nested exception on signal stack" at the same `usr1_handler` address:
   about 67 threads in one session, during play. Possibly related to the unresponsive menu buttons.
4. Ship: `macfinals-65` is installed by hand. `SteamDriver-2.app` still bundles `macfinals-53`, so
   reinstalling from it undoes this. `runtime-macfinals-53` is kept in the tool's folder.
