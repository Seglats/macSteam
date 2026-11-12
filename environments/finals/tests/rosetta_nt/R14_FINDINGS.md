# r14 — what we found out, and decisions (2026-11-02)

2026-11-02. Context: `(removed)` (results at the top).

## Where things stand

- **Crab Champions works** on MacFinals-13 / `macfinals-17` (checked in game; exit 0 at
  01:20). **THE FINALS still stops with `E111000D`**, about 2 s after its real process loads.
- **Tried, no change to the error:** `%gs` and TLS at Proton parity, all Windows code
  registered, the address-search fix, the hook bug fix, and MSync off with AVX not
  advertised.
- **The hook is working in the game:** 8 of 8 PUSHF and 17 of 17 POPF fixups recognised,
  nothing dropped. No `iretq` fixup fired.
- **We still have no direct comparison with a setup where the game runs.** That is the
  biggest gap.

## Still open: run our probes where the game ran

The most useful single missing measurement, and only Rasmus can take it. Run these with
that route's runtime, in its own temporary prefix, and paste the output. All are
in `runtime/build/probe/`:

1. `cpu-behaviour-probe.exe`
2. `nt-patterns-probe.exe`
3. `nt-transitions-probe.exe`
4. `macfinals-probe.exe`
5. `segment-probe.exe`



## Decision: write the Wine exception-fidelity patch (Rasmus, 2026-11-02)

**Yes.** It is a Wine patch, it does not touch Rosetta, and it can be tested in one build.
It makes exception reports match a real CPU:

| Case | Today | Real CPU (Proton) |
|---|---|---|
| RF flag in fault contexts | clear | set |
| `mov` from control or debug registers, `clts` | `c000001d` | `c0000096` |
| `int1` | `c000001d` | `80000004` |

Nothing shows the game depends on these. If the probe comparison above happens soon, do it
first: it may show these do not matter.

## GPTK 3 vs our GPTK 4

- The route that ran the game used **CrossOver 26.3.0.39832 with GPTK 3**, or
  CrossOver Preview 27. We use GPTK 4 (D3DMetal 4.0b3), this fork's stated base.
- **Stock CrossOver 26.3 ships D3DMetal 3.0.** The local install has it as
  `apple_gptk/external.old` (dated Jul 15); the active `external` is 4.0b3 (Oct 8) and
  `external.2` is 4.0b2.
- Evidence it does not matter: when E111000D fires, the game process has loaded only
  `Discovery.exe`, `preloader.dll` and 21 Wine builtins, with no d3d12/dxgi on the Windows
  side (log `game-69E8AA28-…`, pid 06fc).
- Evidence it might: `libd3dshared` is dlopened Unix-side from the first PE load
  (CW HACK 22434) and owns the code-region table all `%gs` semantics depend on, and it
  differs between 3.0 and 4.0b3.
- A one-off test (copy the 3.0 files unchanged into a new runtime, which counts as use) is
  Rasmus's call.

## The Rosetta green light stays POPF/PUSHF/IRET

Today it covers `POPF`/`PUSHF`/`IRET` flag handling only, and the CPU fix in the route
where the game ran has the same scope. Widening is **not**
recommended unless the probe comparison shows a specific reason. What would need it:

- `lar`, `verr`, `verw` (illegal instruction here)
- hardware breakpoints (never fire)
- missing faults: misaligned `movaps`, reserved `ldmxcsr` bit, `mov ss,0`
- hangs: bad segment load, far return to a bad CS, 16-byte instruction

## Decisions (Rasmus, 2026-11-02): checkpoint and branches

- Checkpoint `81b0293` on `main` (not pushed). Branches **`r14_runtime`** (this work) and
  **`test_validation`** (for merging with the other build, for testing) both start there.
- `.gitignore` now excludes `.claude/`, `context.txt` and the r13 build outputs.
  `hooklib.dylib` stays tracked because `add_nt_hook.py` uses it.
- What exactly goes into `test_validation` still needs confirming (see the session).

## Independent next steps

- Look into the first-call `GetSystemMetrics` NT loss and the APC difference (low priority:
  the check was measured as making no system calls).
- Find out why `steam.exe` spins when the wineserver goes away abruptly (not related to
  this error).
