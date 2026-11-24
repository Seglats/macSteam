# C16: hang while loading in, loader lock and winmm lock (Rosetta exception server busy)

| | |
|---|---|
| First seen | 2026-11-14 17:48, `macfinals-117`, `MACFINALS_NT_OFF=syscall,reloc STEAMDRIVER_PERF=1` (alt-tabbed while loading) |
| Origin | unknown; not today's hook changes (fast path and relocation were both off) |
| Status | open |
| Reproducer | none yet |
| Fix | – |

## What you see

The game freezes while loading into a match. No Unreal crash report.

## Evidence

- `perf_report.py` (`perf/20261114-174851`, `wine.63085`): normal until about 80 s, then the
  GameThread at 0 %. From 90 s, 1.5 million Mach traps and 260,000 context switches a second.
  The only busy thread is `com.apple.rosetta.exceptionserver` (30 % user, 75 % system).
- Lock timeouts (`steamdriver-wine.log`):

  | Thread | Holds | Waits for |
  |---|---|---|
  | 06e8 | `WINMM_cs` | `loader_section` |
  | 06f0 | `loader_section` | no lock (never times out); logs nothing |
  | 0210, 0688, 0798 | – | `loader_section` |
  | 06e4 | – | `WINMM_cs` |

- winmm's `timeSetEvent` holds `WINMM_cs` and needs the loader lock when it starts the timer
  thread (`TIME_MMTimeStart` → `GetModuleHandleExW(FROM_ADDRESS)` → `LdrAddRefDll`). That
  explains 06e8.

## What we think

06f0 holds the loader lock (a DLL load, or a thread start or exit with DLL attach/detach) and is
stuck in something that isn't a lock wait. The busy exception server suggests it's looping on
faults or slow-path syscalls. The same ingredients (loader lock, audio: `WINMM_cs`, `AVRT.dll`)
appear in [C03](C03-quit-hang-lost-sigusr1.md) and [C05](C05-quit-hang-silent.md), so these may be
one underlying problem. The alt-tab may have played a part.

## Next steps

1. **Done (`macfinals-119`, installed):** `patches/wine-ntdll-syscall-log-stuck-dump.patch`. With
   `STEAMDRIVER_PERF=1`, when a lock wait times out (the 60 s retry) the counters dump every
   thread's current NT call by Windows thread id: `in NtXxx for N s args=…`, or `not in an NT call;
   last NtXxx returned N s ago`. Checked with `tests/windows_probe/cs_timeout_probe.c` (the owner
   showed as `in NtWaitForSingleObject for 65.6 s`). `ab_launches.py` prints the dumps under each
   launch. At the next freeze, find the lock owner's thread id from the `blocked by` lines and read
   its dump line.
2. A comparison on `macfinals-112` (old runtime and hook) to see whether these freezes happen
   there too.

Update 2026-11-15: the loader-lock holder shape is explained — in C17's live capture the
`loader_section` holder sat in `NtUserThreadDetach`, blocked in its own `NtClose` behind the
wedged `fd_cache_mutex`. If this freeze's owner is the same shape, the root cause is C17's, not
winmm: [C17](C17-slow-leave-and-quit-messagecall-ebadf.md).
