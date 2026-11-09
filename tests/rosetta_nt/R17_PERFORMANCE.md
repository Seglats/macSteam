# THE FINALS performance on MacFinals: first measurements (2026-11-02)

FPS is low and roughly flat across resolutions, so the CPU limits it, not the GPU. The other
project's author measured on CrossOver 26.3 with ktrace: ~16k exceptions/s and ~34k Mach port
cleanups/s during play, ~0.6 s of CPU per second spent there.

## How to measure

`PERF=1 tests/finals_bisect/launch_main.sh avx [msync]` makes every Wine process write ten lines
a second to `logs/<session>/perf/wine.<pid>` (our ntdll's `WINE_SYSCALL_LOG`): Windows syscall
counts and time, BSD/Mach syscalls, page faults, context switches, Wine's exception/fault
counters, VM-lock times, the busiest threads' user+system CPU. Output only. Find the game's
file by its Windows thread ids (`tid=…` in the "long" lines).

Note: the script's defaults are the CrossOver-like bisect baseline. **It does not enable MSync
or AVX unless asked** (`msync`, `avx`); the app enables both. Measure with both.

## First run (`macfinals-31`, `avx`, no MSync), last minute of the tutorial

| per second | |
|---|---|
| Windows syscalls | 214,000, of which **NtQueryPerformanceCounter 154,000** |
| NtSetEvent / NtWaitForSingleObject / NtResetEvent | 16,600 / 15,500 / 11,100 (to the wineserver: no MSync in this run) |
| context switches | 216,000 |
| BSD / Mach syscalls | 361,000 / 387,000 |
| faults Wine sees (segv / raise / pagefault) | ~280 / 265 / 273 |

Busiest threads (ms CPU per second, user + system): **GameThread 350 + 331 (68% of a core,
half in the kernel)**, Foreground_Worker_#1 194 + 31, **com.apple.rosetta.exceptionserver
47 + 172 (22% of a core)**, Foreground_Worker_#0 148 + 25, RenderThread_0 76 + 22.

Readings:

1. **QueryPerformanceCounter is a syscall.** CrossOver's `RtlQueryPerformanceCounter` always
   calls `NtQueryPerformanceCounter`. Windows reads the TSC in user mode, scaled with the
   KUSER_SHARED_DATA fields QpcBias/QpcShift/QpcBypassEnabled (Proton does the same).
   `tests/windows_probe/qpc_probe.c` on Proton: 62 ns per QPC, RDTSC 8 ns, invariant TSC.
   **On MacFinals (`macfinals-31`, idle machine): 36 ns per QPC without the hook, 37 ns with it
   (`DISABLE_AOT=1`); RDTSC 12 ns, 1000 MHz, invariant TSC bit set, never backwards in 2M reads.**
   154k calls/s × 36 ns ≈ 5.5 ms of CPU per second, under 1%. So the QPC syscall is not the FPS
   problem and a user-mode QPC patch is not worth it unless a loaded measurement says otherwise.
2. **No MSync in this run**, so every event operation is a wineserver round trip. Rerun with
   `msync` before drawing conclusions about the context switches.
3. **Rosetta's exception server is busy (22%, mostly kernel), while Wine sees only ~280 faults
   a second.** So most of the exceptions the other project counted are handled inside Rosetta
   and never reach Wine: likely faults Rosetta takes for its own reasons (writes to pages that
   hold translated code, for example). Wine can only influence these indirectly.
4. The NT hook requires `DISABLE_AOT=1`, so every x86 instruction is translated at run time
   instead of ahead of time. Its cost is not measured yet (a run without the hook would show
   it, if the game passes its checks without the hook).

The tutorial run crashed after ~2 minutes: a null read (address 8) in the game's own code
(`Saved/Crashes/UECC-Windows-C233C22148469A3BF6A04ABCA9FD3B0D_0000`, stack all Discovery.exe and
system DLLs). Nothing on our side was logged before it. It was the first run with AVX
advertised; if AVX runs keep crashing and others do not, suspect Rosetta's AVX translation.

## Kernel trace (2026-11-02, `macfinals-33`, avx+msync, 5 s of play)

`sudo tests/finals_bisect/ktrace_game.sh` (kernel-side tracing only; counts, no code read).
Per second, in THE FINALS' process:

- **~27,000 `brk #5` traps on one thread (GameThread), from one fixed Rosetta address.** Each goes
  through Rosetta's exception server (thread_suspend, 2 Mach messages, thread_resume, 2 port
  deallocations), then the thread runs a signal handler (pthread_sigmask x2, sigreturn).
  This is the game thread's kernel time and the exception server's load.
- The cause: **direct `syscall` instructions from Windows code.** Wine's own ntdll stubs never
  execute `syscall` (they test KUSER_SHARED_DATA 0x308 and call the dispatcher), so a direct one
  becomes an invalid macOS syscall, which Rosetta turns into SIGSYS via its exception server, and
  Wine's `sigsys_handler` redirects it to the dispatcher. Crab Champions has none.
- `tests/windows_probe/direct_syscall_probe.c`, one NtQueryPerformanceCounter:

  | | Proton | MacFinals |
  |---|---|---|
  | via ntdll | 61 ns | 55 ns |
  | direct `syscall` | 2,151 ns | **16,187 ns** (hook or not) |

  25k/s x 16 us = ~0.4 s per second on GameThread.
- Separately, ~35,000 failed `mach_vm_map` calls a second (KERN_NO_SPACE, 1 MB, fixed address)
  spread evenly over ~15 worker threads. Not yet attributed (Wine's `anon_mmap_tryfixed` is
  a candidate).

`macfinals-35` adds counters (`wine-diag-sigsys-tryfixed.patch`): `sigsys=` with the top direct
syscalls by name, and `tryfixed-fail=`.

## What the direct syscalls are (`macfinals-37`, 2.5 min of play)

Per second, by raw rax in Wine's SIGSYS handler (`wine-diag-sigsys-rax.patch`):

| rax | per s | |
|---|---|---|
| **0xffffffff** | **32,000** | not a service number; Wine returns STATUS_INVALID_SYSTEM_SERVICE, as Windows does |
| 0x50 NtProtectVirtualMemory | 960 | |
| 0x43 NtContinue | 500 | |
| others | < 50 each | |

So the calls are not a retry loop on a call Wine lacks; Wine's answer already matches Windows.
The count is the caller's (not analysed, boundary 3). At ~16 us each under Rosetta (2 us on
Proton) that is ~0.5 s of GameThread per second. All but ~1-2 us of it is inside Rosetta
(brk #5 -> exception server -> emulated SIGSYS) before Wine runs. Nothing Wine-side lowers it;
the remaining lever is how Rosetta translates `syscall`, which is outside the agreed hook scope
(POPF/PUSHF/IRET) and is Rasmus's decision.
