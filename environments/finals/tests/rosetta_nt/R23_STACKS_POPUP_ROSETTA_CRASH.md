# THE FINALS: thread stacks, the minimum-spec popup, and the Rosetta start-up crash (2026-11-12)

Runtime `macfinals-92` is installed in the Steam tool. Results first, details below.

| Topic | Result |
|---|---|
| Thread stack placement | Fixed in Wine; the VM lock is held half as long. No FPS or hitch change. On by default. |
| "Minimum Hardware Requirements Not Met" popup | Shown on the first launch in a prefix, then clicked OK automatically. Works; one launch stuck on an older timing. |
| Denuvo "Just a moment… taking longer than expected" | Not Denuvo, not the popup: Rosetta crashes the game during start-up. Open. |
| Where performance is limited | The game's own GameThread (about 86 % of a core in play). Little left to gain on the Wine side. |

## 1. Thread stacks (`wine-ntdll-thread-stack-hint.patch`)

**Problem.** The game creates about 45 threads a second. For each stack, Wine's
`virtual_alloc_thread_stack` → `map_view` → `try_map_free_area` probed the address space from
the bottom, and each attempt that hit a used area cost a failed fixed `mach_vm_map`
(`anon_mmap_tryfixed`). That was about 780 failures per thread, all under `virtual_mutex`. In the
14:24 session it accounted for 76 % of the VM lock's hold time and 85 % of its waits.

**Fix.** Wine remembers where the last thread stack went and tries there first. If that place
is outside the caller's limits or taken, the normal search runs as before. It's on by default;
`MACFINALS_STACK_HINT=0` turns it off.

**Measured.** Two Steam runs on the same runtime (`macfinals-92`), about 3 minutes each, the
same kind of match, nothing building in the background, `STEAMDRIVER_PERF=1`. Sessions
`perf/20261112-165224` (on) and `perf/20261112-165602` (off), read with
`tests/finals_bisect/perf_report.py`.

| | On | Off |
|---|---|---|
| Failed fixed mappings (`tryfixed-fail`) | 0.8/s | 20,724/s |
| VM lock held | 27.4 ms/s | 59.1 ms/s |
| VM lock waited | 8.2 ms/s | 18.2 ms/s |
| Longest wait | 3.4 ms | 17.6 ms |
| Mach traps | 191k/s | 244k/s |
| GameThread limited by the CPU | 59 % of intervals | 60 % |
| GameThread load in play (40–160 s) | 86 % | 86 % |
| Freezes after 40 s | 7 (0.9 s in total) | 6 (0.7 s in total) |

**Conclusion.** The waste is gone. The game's main thread wasn't waiting on that lock in
practice, though, so frame rate and hitches don't change. Keep it: it's cheaper and harmless.

## 2. The minimum-spec popup (`wine-user32-dialog-accept*.patch`)

**What the popup is.** THE FINALS shows "Minimum Hardware Requirements Not Met" on every start on
a Mac GPU. It's neither `MessageBox` nor `TaskDialog`. It's a **modeless** dialog built from a
template whose caption is "Dialog", with one Edit control (id 1005) and one push button "OK"
(id 1000). The game sets the title after creation. A `+dialog` trace showed this.

**How it's answered.**

- **The setting.** `WINE_MSGBOX_ACCEPT` lists exact titles, separated by `|`. A dialog with one of
  those titles and a single push button is answered with that button.
  - The Steam tool sets it only once the marker `<compatdata>/steamdriver-gpu-warning-seen`
    exists.
  - `compat-tool/run` creates that marker after the first session that lasts longer than 30 s.
  - So the popup is shown on the first launch in a prefix. Deleting the marker shows it once
    more. `STEAMDRIVER_GPU_WARNING=1` always shows it.
- **Modeless dialogs** (`DIALOG_CreateIndirect`):
  - The dialog is marked as pending, and a 1 s safety-net timer starts.
  - The first time the game's own loop passes one of its messages to `IsDialogMessageW`, the
    timer is replaced by one of 0.2 s.
  - When that timer fires, the click is posted once as `WM_COMMAND(id, BN_CLICKED)`.
- **Modal dialogs** (`DIALOG_DoDialogBox`) are answered before they're first shown. THE FINALS
  doesn't use this path.
- **Unused by this game:** the `MessageBoxIndirectW` and `TaskDialogIndirect` patches read the
  same setting. They do nothing for THE FINALS.

**Log lines** (`<compatdata>/steamdriver-wine.log`, channel `dialog`, always on):

```
DIALOG_CreateIndirect dialog L"Minimum Hardware Requirements Not Met" created, modeless, accept list 38 chars
DIALOG_CreateIndirect dialog 0000000000010098: created at 426404739 ms
IsDialogMessageW dialog 0000000000010098: its loop started at 426404788 ms
dialog_accept_click dialog 0000000000010098: clicking at 426404989 ms (0.2 s after its loop started, on screen)
dialog_accepted answered L"Minimum Hardware Requirements Not Met" with its only button (1000) ...
```

The "accept list" length is 0 when the setting is off. Every dialog the game creates is logged
with its title.

**What failed on the way, and why the timing is what it is:**

- **Clicking at creation** (posted before the game's loop ran the dialog): one launch of two left
  the game stuck.
- **Clicking once the window was on screen:** with the game started in the background, the click
  waited until the player switched to it.
- **The game's loop as the signal, 0.1 s after it:** this worked in about ten launches (about
  0.16 s after creation). In one launch, though, it was created and neither the 0.1 s click nor
  the 1 s safety net ever ran, and the popup stayed. The cause is unknown. Both timers depend on
  the dialog thread dispatching messages.
- **0.2 s plus a "loop started" log line (`macfinals-92`):** tested in a handful of launches, all
  clicked. If it gets stuck again:
  - No "loop started" line: the game never ran the dialog's loop, and a different delay won't
    help.
  - "Loop started" but no click: the timer is the problem.

## 3. Rosetta crashes the game during start-up (open)

**Symptom.** Denuvo Anti-Cheat's window says "Just a moment… The game is taking longer to start
than expected", and `denuvo-anti-cheat-crash-report.exe` starts. Denuvo is waiting for a game
that has already died. The Wine log shows this, with nothing from Wine before it:

```
rosetta error: no code fragment associated with the given arm pc
```

**Facts so far:**

- **It hits at different points of start-up:** after `schannel.dll`, after
  `WINHTTP/VERSION/MSWSOCK/imm32`, and, twice, right after `msmpeg2vdec.dll` and
  `MSAudDecMFT.dll`, about 0.3 s after the popup was clicked. In some launches it hits before
  the popup appears.
- **It's older than today's changes:** 6 of 82 launches in the log up to 2026-11-12 15:00, and
  6 of 35 since then.
- **The performance counters make it much more likely:**

  | Launches 15:00–16:50 | Rosetta crashes |
  |---|---|
  | `STEAMDRIVER_PERF=1` | 4 of 9 |
  | Without | 3 of 27 |

  The counters add work to every NT call, so the likely cause is a timing race between threads.
- **The popup fix isn't the cause:** with it off (`STEAMDRIVER_GPU_WARNING=1`), 4–5 launches were
  clean. With it on, the next 5 were also clean. And the crash happens before the popup exists.
- **The thread-stack change is unproven either way:** too few launches with it off to say.

**Not done:** no analysis of the anti-cheat. Rosetta isn't modified. The next step is to log
what each thread is doing (exceptions, protection changes, freed executable memory) in the last
moments before the crash, using our own Wine channels.

## 4. Where performance stands

- **GameThread is the limit:** about 86 % of a core during play and CPU-limited in about 60 % of
  intervals. That's the game's own translated code under Rosetta. Wine's lock waits are now
  small, and `NtQueryPerformanceCounter` costs about 49 ns per call.
- **Not tested here:**
  - `WINEMAC_APPKIT_UPDATE_CYCLE=0` turns off AppKit's display-synchronized event loop
    (`wine-winemac-update-cycle.patch`, found in moonlight-qt's Info.plist). In Recall, keys
    arrived about 1.5 ms sooner. Mouse movement and clicks were unchanged, because they use the
    raw GCMouse path. Off by default.
  - `STEAMDRIVER_GAME_MODE=1` starts `Discovery.exe` from `lib/wine/game-mode/Discovery.app`
    (`add_nt_hook.py` `add_finals_game_app`), so macOS can turn Game Mode on.
- **Profiling tool limits:** Instruments (`xctrace`) can't attach to the Steam-launched game
  ("Cannot find process for provided pid"), and recording all processes failed as well. Use
  `STEAMDRIVER_PERF=1` and `perf_report.py`.

## Other changes in `compat-tool/run`

- **Wine log rotation:** past 200 MB, `steamdriver-wine.log` is moved to `steamdriver-wine.log.old`
  at launch. One old copy is kept.
- **The popup marker and the `STEAMDRIVER_GPU_WARNING` switch** (section 2).

## Open

- Rosetta start-up crash (section 3).
- The one stuck popup on the 0.1 s build. Watch for it on `macfinals-92`.
- Game Mode and the AppKit update cycle, untested in THE FINALS.
