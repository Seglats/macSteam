# C19: the start hangs with the minimum-spec box open (maybe; seen once)

| | |
|---|---|
| First seen | 2026-11-25 12:48, ARC Raiders (Steam app 1808500), `macfinals-176`, upscaling off |
| Origin | unknown; seen once, and in a launch with nothing set to answer the box |
| Status | maybe, infrequent: one sighting. Mitigated: the box is answered unseen after the first launch per prefix (THE FINALS and ARC Raiders, `compat-tool/run`, `WINE_MSGBOX_ACCEPT`) |
| Reproducer | none yet |
| Fix | – |

## What you see

The game starts, Embark's "Minimum Hardware Requirements Not Met" box comes up, and the game
never gets further. A second launch from Steam hangs as well: its launcher stub waits on a
window of the first, still-running session.

## Evidence

From `compatdata/1808500/steamdriver-wine.log`, game process thread `015c`:

- `err:dialog:DIALOG_CreateIndirect dialog L"Minimum Hardware Requirements Not Met" created,
  modeless, accept list 0 chars`. The launch began at 12:48:05 on the old run script, before
  ARC Raiders was added to the auto-answer list (its marker is stamped 12:49), so nothing answered
  the box.
- Right after it: `015c` waits on `tid 00f8` (`WM_USER`, no reply after 5 s), `00ec` (the
  launcher stub) waits on the same thread, and another thread waits on `015c` (`WM_GETTEXT`).
- Then five threads time out on `loader_section`, held by `0504`.
- The launch at 12:51:31 (new script, marker present) hung in the launcher stub:
  `wait_message_reply ... tid 05e0 (WM_NULL)`, in the same Wine session as the stuck game.

In the earlier launches (12:07, 12:25) the same box came up, Rasmus clicked OK and the game ran.
So an open box alone doesn't hang the game. Whether this one was hidden behind something or
clicked while the cross-process sends were already stuck is not known.

## What we think

Probably the same family as C17: a cross-thread or cross-process `SendMessage` whose target
doesn't process messages, and the loader lock held behind it. The 12:25 session's quit showed
the C17 pattern too (`wine client error: write: Bad file descriptor`, then `loader_section` and
threadpool lock timeouts).

## Next steps

Nothing unless it comes back. If it does, note whether the box was visible and whether it was
clicked, and take the stuck dump (C16/C17 lock-holder lines) before quitting.
