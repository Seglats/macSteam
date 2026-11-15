# C07: "nested exception on signal stack ... 0xa8" when threads are killed

| | |
|---|---|
| First seen | long-standing |
| Origin | before (Wine) |
| Status | cosmetic, not fixed |
| Reproducer | any quit; any process exit with several threads (8–30 lines per good quit) |
| Fix | – |

## What you see

```
err:virtual:virtual_setup_exception nested exception on signal stack addr 0x208816ee0 ...
  code c0000005 flags 0, 2 params: 0x0 0xa8
  fault in .../ntdll.so+0x37ee0 (NtCompareTokens), thread name ""
```

(`wine-ntdll-nested-exception-where.patch` adds the `fault in` line; the symbol is just the
nearest export.)

## Cause (understood, not fixed)

Wine's `quit_handler` (SIGQUIT, sent when a thread or process is terminated) calls
`user_mode_abort_thread(0, get_syscall_frame())`. For some threads the frame is NULL, so reading
`frame->syscall_cfa` (offset `0xa8`) faults on the signal stack. Wine reports the nested
exception and kills the thread, which is what was happening anyway. It appears in good launches
too, so it's noise, not a cause. Several earlier theories that blamed it were retracted.

## Fix (installed in `macfinals-117`, not yet confirmed)

`patches/wine-ntdll-quit-handler-no-frame.patch` (`macfinals-115` and later; installed with `-117`): `quit_handler` calls
`user_mode_abort_thread` only when the thread has a syscall frame, otherwise it goes straight to
`abort_thread(0)`. The thread ends the same way, without the fault and the log lines. Verify: no
`nested exception ... 0xa8` lines at the next game quits on `-117`. The probe never produced
these lines, so only the game can confirm it.
