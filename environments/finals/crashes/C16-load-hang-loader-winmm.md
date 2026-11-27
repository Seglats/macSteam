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

## Second capture, fully instrumented (2026-11-25 07:54, `macfinals-168`, trigger drill)

About a minute after launch, game frontmost. Frames stopped at ~07:54:28; the process stayed
alive (no Unreal crash report: the watchdog thread `FHeartBeatThread` is itself stuck, see
below). Perf session `perf/20261125-075322/wine.59317`; Time Profiler
`cpu-profile/freeze-20261125-075437.trace` (freeze_watch, 6 s in); System Trace
`cpu-profile/storm-20261125-080500.trace` (3 s, ~10 min in).

- **Not C06.** Both lock-holder lines (new in `-168`) read `user lock free` /
  `macdrv win_data lock free` in every dump; no `ui wait`; the Cocoa main thread's lag probe stays
  at 0.02 ms.
- **An exception livelock.** `com.apple.rosetta.exceptionserver` runs a full core (311 ms user +
  681 ms system per second), from 07:54:31 on, and spent all 69.8 s of the Time Profiler capture
  in its `mach_msg` loop inside `/usr/libexec/rosetta/runtime` (x86 side, region metadata via
  `proc_pidinfo`; return addresses right after a `syscall`). Per-thread times
  (`proc_pidinfo`, no task port): ~16 threads at ~59 ms/s system, ~3 ms/s user each —
  GameThread, `FHeartBeatThread`, `HttpManagerThread`, `RHIInterruptThread`, the online/RTN
  threads, several `wine_threadpool_worker`s. The System Trace shows the exception server
  doing receive → reply → `mach_port_deallocate(<thread port>)` ~6,300 times a second **per
  thread port** (~16 ports, ~100,000 exceptions/s), plus ~750,000 soft faults/s of its own on two
  fixed addresses (`0x400300000000`, `0xa0300000000`). The app threads themselves barely fault.
- **None of those threads reaches Wine.** Their counters show no NT calls (the dump: `014c not in
  an NT call; last … returned 885 s ago`), while threads already parked inside Wine (audio,
  network, waits) carry on. Every NT call from the game is an x86 `syscall` (fast path or
  Rosetta's exception route into Wine), so: **from 07:54:30 any thread that issues a new NT call
  bounces between the kernel and the exception server forever.**
- **The moment:** the first dump (07:54:30.3) has the GameThread `in NtProtectVirtualMemory` —
  its last NT call. The rows before show the GameThread busy with `NtFreeVirtualMemory` /
  `NtProtectVirtualMemory` (hundreds per 100 ms) while loading.
- **Also in this session** (between 07:53:59 and 07:55:32 in the Wine log, not timestamped
  closer): a Metal assertion `-[MTLComputePipelineDescriptorInternal setComputeFunction:withType:]:
  failed assertion 'computeFunction must not be nil.'` (an always-on Metal assertion, which
  normally aborts the process), right after a burst of `virtual_handle_fault HACK: exec fault
  on executable page` fixmes.
- The 2026-11-14 occurrence had the fast path **off** (`MACFINALS_NT_OFF=syscall,reloc`), with
  the same exception-server numbers, so the fast path is not required for this.

**Leads (unproven):** (a) a protection change hits memory every syscall's path needs (the
GameThread's last call was `NtProtectVirtualMemory`), so each thread's next syscall faults and
the fault is never resolved; (b) the Metal assertion's `abort()` on a translated thread wedges
signal/exception delivery for the whole process. Next: log the `NtProtectVirtualMemory`
arguments of the call in flight (address, size, new protection, module/region) in the stuck
dump, and the thread + time of any `SIGABRT`.

**Diagnostics in place (2026-11-25, `macfinals-171`, `wine-diag-vm-ring-abrt.patch`):** the stuck
dump's `vm:` lines (last memory-protection/free/exec-region calls, calls in flight) and
`SIGABRT:` line answer both leads at the next occurrence. Passive capture during normal play;
procedure in `tests/finals_bisect/README.md` ("Passive capture").
