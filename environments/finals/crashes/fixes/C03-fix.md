# C03 fix: no fast-path diverts at macOS shared-cache syscall sites

Fixed in **`macfinals-114`** (hook only). Details: `tests/rosetta_nt/R25_SIGNAL_IN_STUB.md`
("Follow-up").

## Cause

The fast path diverts every translated `syscall` site into the stub, including sites in the macOS
shared cache (libsystem_kernel's own wrappers) that get translated after the hook arms. Such a
site shows up only in some processes, which is why the hang was intermittent. A/B on one build:
skip disabled, 5 / 20 `waitexit` rounds hung; skip enabled, 0 / 54.

Most likely mechanism (not confirmed per site): one such site is a signal-return or mask syscall
(`__sigreturn`, `__pthread_sigmask`) that every Wine signal handler runs. A deferral in the stub
there parks the mask from inside the handler (SIGUSR1 blocked). The guest's sigreturn then
unblocks it, and a later exit routine restores the parked, blocked mask. From then on the thread
never sees SIGUSR1 again: its suspensions are lost, and at exit the threads holding locks can't
be stopped cleanly.

Ruled out on the way (with counters, not guesses): the stub's guard (`deferred-guard` stayed 0),
the converters (relocated or not, same as stock), the stub's resume (a process with no stub
entries still hung). The first theory, a deferral in the guard of a blocking syscall, was wrong.

## Fix (`tests/rosetta_nt/payload_arm64.c`)

The append matcher reads each site's x86 RIP from its `movz/movk x28` carrier
(`sysc_rip_word`) and leaves sites at `≥ 0x7ff000000000` stock (`sysc_skip`; `ctl[98]` counts
skips). The fast path is meant for the game's syscalls in Windows code, so this is also the right
scope. Trace builds also dump each diverted site's RIP (`sites poll`) and every thread's pending
word (`ctx poll`).

## Verified

`teardown_check.py` on `-114`: `waitexit` 0 / 30 (installed runtime 0 / 10), `exit` 0 / 20,
`suspendonly` 0 / 10, `suspend` 0 / 10, `mixed` 0 / 10. Fast path 61 ns per call.
**Not yet confirmed in game:** the next quit hang on `-114` (16:58) was a different one (C05).
