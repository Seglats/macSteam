# The start-up Rosetta crash: a signal inside the syscall stub (2026-11-14)

## Cause and reproducer

THE FINALS sometimes dies during start-up with
`rosetta error: no code fragment associated with the given arm pc` (about 1 launch in 6–9). The
cause is a signal that arrives while a thread is running the hook's syscall stub
(`ntfix_macfast` in `payload_arm64.c`), which is our own native ARM code on our own page. Rosetta
looks for the x86 code belonging to that pc, finds none, and aborts the process.

In the game the signal is mostly SIGQUIT: when a process ends (the launcher handing over at
start-up), wineserver sends it to every remaining thread (the `nested exception ... 0xa8` lines,
which also appear in good launches). The H.264 and AAC decoders load at that same moment in every
launch, so they showed up next to the crash without being involved.

`tests/finals_bisect/teardown_check.py` runs `tests/windows_probe/teardown_probe.c` in a prefix of
its own, with no Steam and no game. A child has 16 threads making the game's syscall shape back to
back and then exits under them. On `macfinals-112`:

| Run | Rounds that crashed |
|---|---|
| Hook, `--mode exit` | 20 / 20 |
| Hook, `--nt-off syscall` (the stub is still entered and bails at once) | 0 / 20 (1 Rosetta error) |
| `--no-hook` | 0 / 20 |
| Hook, `--mode suspendonly` (SuspendThread/GetThreadContext, i.e. SIGUSR1; workers stopped before exit) | 10 / 10 |

So suspension hits it too, and crash reporters, the anti-cheat and Wine itself suspend threads.
It is not limited to start-up.

## How Rosetta handles a signal (rosetta/runtime, this UUID)

- `signal_handler` (ThreadContextSignals.cpp, `0x16aa0`) classifies the interrupted pc with
  `classify_arm_pc` (TranslationCacheAbi.cpp, `0x73a0`): first the runtime image and
  libRosettaRuntime (`0x6de4`), then the interval tree of translation regions. A pc in neither is
  kind 0, which gives `no code fragment associated with the given arm pc`. Our payload page is in
  neither.
- What happens per kind (jump table at `0x16e3c`): translated code (3) is delivered at once; a
  runtime entry (4) is delivered as if at LR − 4; Rosetta's own runtime code (2, 6, 10) is
  **deferred** (`0x1d7e0`): the thread's mask is parked at `ctx+0x1cc`, bit 1 is set in the
  pending word `ctx+0x1c8`, the signal is re-queued with `__pthread_kill`, and the thread resumes
  with every signal blocked.
- In libRosettaRuntime, `abi_for_address` (`0x5da4`) returns 1 for its `runtime_entry` section,
  2 for `runtime_savdnzcv`, and 0 (runtime code, so deferred) for everything else in the image.
- Every exit routine back into translated code (`runtime_exit_ret`, `_exitcall`, `_exitljmp`,
  `_exit_br`, `_exit_sys`, `_exitinst`, `_thread`) calls `table[0]`, ORs the flags it returns
  (`w1`) with the pending word, and on bit 1 restores the parked mask with
  `__pthread_sigmask(SIG_SETMASK, ctx+0x1cc)`. The signal fires right there, at a pc the handler
  can deliver from (kinds 11–16, by position in the routine).

## The fix (hook only; no Wine or Rosetta change on disk)

1. **Our code runs where Rosetta defers signals.** hooklib copies the payload code (8 KB) and the
   append stub into the unused tail of libRosettaRuntime's `__TEXT` (`0x65000`–`0x68000`; the
   image's last section ends at `0x64e34`). The data page stays where it was; the 35 `adrp`s that
   reach it are rebased, and branches must stay inside. The gap must be all zeros, the write is
   the same COW write the append hook has always used on this mapping (a native mapping, not a
   translated one), and the copy is read back. Any mismatch keeps the old placement and warns.
   `MACFINALS_NT_OFF=reloc` turns it off.
2. **The syscall fast path resumes through Rosetta's own `runtime_exitljmp`** (`runtime+0x22560`,
   the routine the thunk's page epilogue at `page+0x2560` is a copy of) instead of its own copy
   of that epilogue. The copy left out the mask step, so a deferred SIGUSR1 was never delivered
   and the suspension hung (`--mode suspendonly`, 10/10 rounds hung after step 1 alone). hooklib
   checks 7 of the routine's words before publishing its address in `ctl[96]`. Unpublished means
   the stub bails to the stock path.
3. **The pending word is queued with a compare-and-swap** (0 → `0x200`). A deferral can set
   bit 1 at any instruction now, and the old `ldr`/`str` would have erased it.

4. **A deferred signal found at entry goes through `runtime_exitljmp` too.** If the CAS finds
   exactly bit 1, the stub jumps there with nothing queued: no install, the mask is restored where
   the signal can be delivered, and the `ret` lands on the bail word (the stock path). Before
   this, that case took a plain bail into the stock invalid-syscall path (brk #5, exception
   server), which doesn't restore the mask. The worker then ran on with every signal blocked and
   the very first `GetThreadContext` never returned (seen with `TEARDOWN_TRACE=1`). The counters
   in the hung child showed it: 169.8 million resumes, one busy bail, `c38 2`. Other flags still
   mean a plain bail.

## Result

`teardown_check.py` with the fixed hook (`macfinals-113`):

| Run | Rosetta crashes | Abnormal exits |
|---|---|---|
| `--mode exit`, 20 rounds | 0 | 0 (was 20/20 crashed) |
| `--mode suspendonly`, 10 rounds | 0 | 0 (was 10/10 crashed, then 10/10 hung before step 4) |
| `--mode suspend`, 10 rounds | 0 | 0 |

The fast path still works: `anon_invalid_site` timed 57 ns per call (33 ns before; the stock
path is about 16 µs) and still returns `c000001c` through the dispatcher.

**Runtime `macfinals-113`** (base `-111` plus this hook) is installed in the Steam tool; `-112`
is kept beside it. **Not yet run in THE FINALS or Crab Champions.**

## Follow-up: THE FINALS hung on quit on `macfinals-113` (2026-11-14 15:49)

The first game run on `-113` played normally, but quitting hung. The window was destroyed and
the display mode restored, then the exiting thread waited forever for the loader lock, and
about 25 threads timed out on the threadpool, timer-queue and winmm locks. The three holders
never answered. This was the only such hang in the 46 launches in the log; the next `-113`
launch quit cleanly.

**Cause: a fifth gap, in the guard.** Wine's own real syscalls (class 1/2/3, about 150,000 a
second in game) are diverted into the stub too, and the guard bails them to the stock path. A
signal that lands in the guard is now deferred (before step 1 it crashed). The bail then runs
the real syscall with every signal blocked. If that syscall is a blocking wait, the thread
sleeps with SIGQUIT and SIGUSR1 shut out: it never dies at exit and never answers a
suspension. Rosetta's own `runtime_syscall` routine is never deferred, so Rosetta itself
doesn't have this case.

**Reproducer:** `teardown_check.py --mode waitexit`. Workers make Wine syscalls back to back
(`SwitchToThread`) under the suspend storm, then park in an infinite wait, and each must still
answer one suspension. On `-113`, 2 of 6 rounds hung; with no hook, 0 of 6.

5. **Fix:** the guard's bail checks the pending word. If it's exactly bit 1, it parks the guest
   state and goes through `runtime_exitljmp` (no install). The signal is delivered there, and
   the `ret` lands on the bail word and so on the stock syscall. `ctl[97]` counts these. Flags
   are live in the guard, so the check uses only `eor`/`cbnz` until NZCV is saved.

**Correction, same day: step 5 isn't the cause of the waitexit hang.** With step 5 in place,
waitexit still hung 6 of 20 rounds. `deferred-guard` (`ctl[97]`) stayed 0, and `entered` stayed
22,001 (only the main thread's timing calls), so the workers never run the stub at all. The
server trace (`--winedebug +server`) shows a worker that answered 24 suspensions, then
received a 25th SIGUSR1 (`*sent signal* signal=30`) and never handled it. A/B on waitexit:

| Configuration | Result |
|---|---|
| No hook | 0 / 6 |
| Hook, `MACFINALS_NT_OFF=syscall` (no diverts inserted; append hook and converters active) | 0 / 8 |
| Hook, fast path on, main thread never uses it (`TEARDOWN_NOWARM=1`) | 2 / 10 hang |
| Hook, fast path on | 6 / 20 hang |
| Old `-112` hook (code not relocated) | 3 / 8 Rosetta crashes |

So the trigger is the divert insertion itself (`ins` rises during the storm while new syscall
sites are translated), not the stub, the converters or the relocation. The relocation turned a
crash into a lost signal. Leading hypothesis, unverified: the inserted words throw off Rosetta's
ARM → x86 position map for the rewritten block, so a signal landing in or after them gets a wrong
resume point or state. Step 5 stays (it's correct on its own) but doesn't close this.

**Found: diverts at macOS shared-cache syscall sites.** A per-thread dump (trace builds:
the payload records each translating thread's context in `ctl[128..191]`, and the poll prints
`ctx+0x1c8`/`0x1cc`/`0x8`/`0x1d0`) showed the stuck worker with pending word 2: a deferred
signal never restored, and no exit routine since `runtime_exit_br`.

6. **Fix: no diverts for sites in the shared cache.** The payload now reads each site's x86 RIP
   from its `movz/movk x28` carrier and leaves sites at `≥ 0x7ff000000000` (the macOS shared
   cache: libsystem_kernel's own wrappers) stock. `ctl[98]` counts these skips. A/B on the same
   build: skip disabled, 5 / 20 waitexit rounds hung; skip enabled, 0 / 54.

   Such sites are rare: in most processes none is translated after the hook arms (one skip in
   one of 13 processes in a traced run). Since they hang a process every time, they still
   matter. Most likely mechanism (not confirmed per site): one of them is a signal-return or
   mask syscall (`__sigreturn`, `__pthread_sigmask`) that every Wine signal handler runs. A
   deferral there parks the mask from inside the handler (SIGUSR1 blocked), the guest's
   sigreturn unblocks it, and a later exit routine restores the parked, blocked mask. From then
   on the thread never sees SIGUSR1 again. The fast path is meant for the game's syscalls in
   Windows code, so leaving macOS library sites alone is also the right scope.

   Diverted sites in a traced waitexit run: 895 in Wine's PE DLLs (`0x6ffff…`), 69 in
   `ntdll.so` (`0x20881…`), 12 in the probe (`0x14000…`). trace also dumps them (`sites poll`).

**Result with all six steps, runtime `macfinals-114` (installed; `-113` kept beside it):** waitexit
0 / 30, exit 0 / 20, suspend-only 0 / 10, suspend 0 / 10, mixed 0 / 10, and the installed `-114`
passes waitexit 0 / 10. The fast path costs 61 ns per call. Not yet run in game.

Still open: a late bail with bit 1 plus other flags takes the plain stock path (not seen so far),
and `ntfix_trapfast` (the opt-in thunk-patch path, quarantined) doesn't have steps 2, 4 or 5.

`ctl[36]` now counts resumes through `runtime_exitljmp` (before: installs seen after our own
`table[0]` call).

## Not the cause, but a gap: no video or AAC decoding

Written down in case in-game video or audio ever turns out to need it.

- In Wine 11.0 `msmpeg2vdec.dll` (H.264) and `msauddecmft.dll` (AAC) are thin wrappers. They
  create `CLSID_wg_h264_decoder` / `CLSID_wg_aac_decoder`, which live in **winegstreamer**. Our
  Wine is configured `--without-gstreamer`, so the game asks for the decoders and gets
  "class not registered".
- `winedmo` (FFmpeg) only demuxes in Wine 11.0, and our build doesn't link FFmpeg either.
- No outside patch is needed, because winegstreamer is upstream Wine; CrossOver and Proton both
  ship GStreamer. Enabling it means building GStreamer (core, plugins-base, plugins-good,
  gst-libav with FFmpeg's H.264/AAC decoders, plus glib and orc) for x86_64, bundling the dylibs
  and plugins in the runtime, setting `GST_PLUGIN_PATH`/`GST_PLUGIN_SYSTEM_PATH` in the Steam
  tool, and dropping `--without-gstreamer` from `build_portable_runtime.py`.
- Symptom to watch for: videos (menu backgrounds, trailers, tutorial clips) that stay black or
  are skipped, or no sound in them.
