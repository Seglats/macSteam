# AVX state in Wine's macOS signal contexts (2026-11-02)

## Symptom

With `ROSETTA_ADVERTISE_AVX=1`, THE FINALS crashed every time on spawning into the tutorial:
`EXCEPTION_ACCESS_VIOLATION reading address 0x8` in the game's own code, same stack both times
(`Discovery+86ab082`, `Saved/Crashes/UECC-Windows-C233C…` and `…46C7F26F…`). Without AVX it
did not crash.

## Cause: Wine never read the AVX state of macOS signal contexts

`dlls/ntdll/unix/signal_x86_64.c` defines `XState_sig` as `NULL` on macOS, so contexts built
from signals (exceptions, thread suspension) have no `CONTEXT_XSTATE`. Measured with
`tests/windows_probe/avx_state_probe.c` (AVX advertised; same .exe on Proton):

| | MacFinals-31 | Proton | MacFinals-33 |
|---|---|---|---|
| exception handler's context has AVX state | 0/200 | 200/200 | 200/200 |
| handler's change to ymm5 applied on continue | 0/200 | 200/200 | 200/200 |
| **handler uses AVX itself: YMM restored on continue** | **no: all 16 upper halves keep the handler's values** | yes | yes |
| GetThreadContext(XSTATE) of a suspended thread: ymm upper halves | all wrong (stale) | correct | correct |
| SetThreadContext ymm change applied | 0/50 | 50/50 | 50/50 |
| preemption, suspend/resume stress (values kept) | clean | clean | clean |

The bold row is the likely crash: on Windows, continuing from an exception restores the full
register state from the context. Here it did not restore the YMM upper halves, so any handler
(or the dispatcher) that used AVX leaked its values into the interrupted code. With AVX
advertised, vcruntime's memcpy/memset use AVX. Without AVX advertised, no AVX state exists, so
nothing is lost: hence AVX-only.

## Rosetta is not at fault

A native x86-64 macOS test (no Wine) shows Rosetta delivers the AVX mcontext
(`uc_mcsize` 1032 = `_STRUCT_MCONTEXT_AVX64`), the handler sees correct upper halves, and
changes made to them are applied on return. The layout differs from XSAVE: the upper halves are
580 bytes after `fcw` (4 reserved bytes, then a 64-byte reserved block, all zeros, where the
XSAVE header would be), so Wine cannot reference it in place the way it does on Linux.

## Fix: `patches/wine-macos-avx-signal-context.patch` (runtime `macfinals-32`/`-33`)

- `YMMH_sig()`: the upper halves in a macOS AVX / AVX-512 mcontext (by `uc_mcsize`).
- `struct xcontext` gains a 16-aligned XSAVE buffer on macOS; `save_context` copies the upper
  halves there with a header (`Mask` = legacy | AVX, not compacted) and attaches it as
  `CONTEXT_XSTATE`; `restore_context` copies them back (zero when the AVX bit is clear, which is
  how the server reports an all-zero state).
- Exception continue then goes through NtContinue's existing xstate path.

## Left alone

`KUSER_SHARED_DATA.XState.Size` is 992 here (832 on Proton). Upstream Wine's formula for
non-compacting CPUs adds `offsetof(XSAVE_FORMAT, XmmRegisters)`; Rosetta reports no XSAVEC, so it
takes that branch. Larger than needed, so harmless for buffers; not changed.
