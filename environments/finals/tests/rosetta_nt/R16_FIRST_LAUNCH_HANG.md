# First-launch hang: Steam's TOS assert, then a minidump that never finishes (2026-11-02)

Symptom: on the first launch after a change, THE FINALS sometimes freezes hard; the next launch
plays. Seen on `macfinals-27` (07:09 run stuck, 07:11 run fine).

## What happens

1. **Every** run, 30–70 s in, Steam's networking inside the game process asserts:
   `Assert( No control data returned even though we asked for TOS? ):…steamnetworkingsockets_socketthread.cpp:2492`
   (comment stream of `Steam/dumps/assert_discovery.exe_*.dmp`; all seven dumps since 03:46 are
   this assert). Just before it, Wine logs
   `fixme:winsock:convert_control_headers Unhandled IPPROTO_IP message header type 27`.
2. The assert is non-fatal: Steam's `crashhandler64.dll` starts
   `steamerrorreporter64.exe -pid=<game>`, which writes a minidump with dbghelp, tries to upload
   it (fails, `12029`) and exits after ~15 s. The game resumes and plays (a run went on 927 s).
3. In the stuck run the reporter stopped 2.3 s in, at the same point in every run: after
   listing modules, entering `fetch_thread_info` (suspend each game thread, read its context).
   Its dump is 280 bytes (header + SystemInfo only). Every game thread went silent at once;
   only one waits on the reporter's process handle.

Hypothesis for (3), not yet proven: suspending/reading the context of a thread that is inside
GPTK's macOS code (D3DMetal runs on Windows threads without going through Wine's syscall path)
can wait forever. A first launch translates new shaders, so such a thread is likelier then.

## Cause of the assert: a Wine fidelity bug on macOS

macOS (like the BSDs) delivers the received TOS byte as an `IP_RECVTOS` (27) control message;
Linux uses `IP_TOS`. Wine's `convert_control_headers` (`dlls/ntdll/unix/socket.c`) only knew
`IP_TOS`, so the data was dropped. `tests/windows_probe/recvtos_probe.c` reproduces it:

| | result |
|---|---|
| Proton (Ubuntu) | `IP_TOS` control message, `0x88` |
| MacFinals `macfinals-27` | no control data, `Unhandled … type 27` |

Fix: `patches/wine-socket-recvtos-cmsg.patch` (last in `WINE_PATCHES`) maps `IP_RECVTOS` to the
same one-byte handling as `IP_TOS`. With the assert gone, Steam no longer dumps the game, which
removes the trigger for the hang. The dbghelp/thread-suspend hang itself stays unexplained.

## 2026-11-02: a second suspend-path failure, and maximum logging

`macfinals-29` (TOS fixed: no reporter, no dumps). Main-menu buttons did not open their menus
in that run; cause unknown. The only anomaly: thread `0ab8`, started 31 s in, was killed with
`nested exception on signal stack addr 0x2087bc67c`, which is `ntdll.so+0x3b67c`:
`usr1_handler`'s first read of the ucontext it was given (`movq 0x30(%r14),%rax`). So the signal
frame Wine received for a suspend/context request could not be read. Same address in 3 of 12
runs, including `macfinals-19` (before the layer and the TOS fix), so not new. A stress probe
(`tests/windows_probe/suspend_probe.c`: 9,000 suspend + GetThreadContext on freshly started
threads) does not reproduce it, with or without the NT hook. Whether it explains the menus is open.

Logging added for the next runs:
- `patches/wine-nested-exception-diag.patch`: on that kill, also log the exception code, fault
  address, the thread's start address/parameter and its stacks (runtime `macfinals-31`).
- `DEBUG=max` in `tests/finals_bisect/launch_main.sh`: err/warn/fixme for every channel, thread
  names (`warn+threadname`), DLL loads, process events, layer per-shader log; afterwards it
  collects Steam's logs and new dumps, macOS crash reports, the D3DMetal cache listing and the
  macOS log (Rosetta, Wine, kernel) into a folder next to the Wine log.

## 2026-11-02: `warn+all` makes the game look debugged

The first `DEBUG=max` run (`macfinals-31`) ended after 80 s with "a disallowed tool is running";
the game requested its own exit (`IsRequestingExit=True`). No new process ran in the session.
Cause: `warn+all` includes `warn+heap`, and with heap warnings on Wine turns on
`FLG_HEAP_VALIDATE_PARAMETERS` for every heap (`heap_set_debug_flags`), which sets the heap's
`HEAP_VALIDATE_PARAMETERS_ENABLED` flag and non-zero ForceFlags: what a process started under a
debugger has. The profile is now `warn+all,warn-heap`. Channels whose state changes behaviour
(grep `WARN_ON\|TRACE_ON`): heap (warn and trace), relay, snoop; the rest only change output.
