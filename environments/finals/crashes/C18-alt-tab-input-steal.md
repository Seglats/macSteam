# C18: after Cmd-Tab away, the game takes the mouse and keyboard back

| | |
|---|---|
| First seen | reported 2026-11-24 by Rasmus (`macfinals-137`; long-standing per his description) |
| Origin | **ours**: REC-45 menu bar/Dock hiding (`updateCoveringPresentation`, `wine-winemac-activation.patch`, shared with main) |
| Status | cause confirmed 2026-11-24 (capture on `macfinals-139`); fix `patches/wine-macdrv-c18-presentation-inactive.patch` installed as `macfinals-141` (probe + teardown 5/5 green), awaiting test in game |
| Reproducer | game windowed on a multi-monitor setup; Cmd-Tab to an app whose windows are on another monitor; click |
| Fix | – |

## What you see

After Cmd-Tab away from THE FINALS, the cursor and keyboard keep going back to the game. It
only stops after switching to an app that has a window **on the same screen** as the game, and
then moving the pointer to a second or third monitor.

## Cause (capture 2026-11-24 07:11, `macfinals-139`, `+macfinals_ui` + macOS system log)

The game was borderless over the whole ultrawide (clip rect 3838×1598), so REC-45's code had set
"hide menu bar + Dock" for it. After each of six Cmd-Tabs to LibreWolf (on another display):

```
07:13:32.774 SETFRONT LibreWolf (oldFrontPid = game)          Dock: UI mode 2 -> 0 by itself
07:13:32.868 game: PRESENTATIONMODE AllHidden -> Normal        "app changing it is NOT front"
07:13:32.887 WindowServer [SetMenuBars]: game made the front process   (SETFRONT game)
07:13:32.903 game: PRESENTATIONMODE Normal -> AllHidden        (active again, covering)
```

Same at :35, :38, :40, :43 and 07:14:02. The driver trace shows the reactivation as plain
"app active (frontmost)": no click, no driver activation request. Controls:
- 07:12:32, Steam Helper (has a window over the game's screen): the same AllHidden → Normal
  call, **no steal**. That's the "same screen" part of the workaround.
- 07:14:04, VSCodium: options already Normal, **no call, no steal**.

`updateCoveringPresentation` counts covering windows only while the app is active, so on
deactivation it found none and called `setPresentationOptions` from the background to show
the menu bar again. macOS applies presentation options only while the app is frontmost (the
Dock had already switched back at the SETFRONT), so the call bought nothing, and WindowServer
answered it by re-fronting the app via SetMenuBars. The mouselook/`acceptsFirstMouse` chain
below was a wrong suspect; one click-caused reactivation (07:12:39) was a real click.

**Fix** (`wine-macdrv-c18-presentation-inactive.patch`): presentation options are not touched
while the app is inactive; `applicationDidBecomeActive:` brings them up to date.
`+macfinals_ui` traces each change ("menu bar/Dock … "). The main environment's copy of
`wine-winemac-activation.patch` has the same code (CS2/Deadlock borderless) and needs the same fix.

## Evidence (2026-11-24, `macfinals-137`, session 06:46, `+macfinals_ui` on)

- The state traces show the game app flipping inactive → active **34 times** in ~5 minutes,
  each time with the game window `0x20096` resigning and regaining key status. The traces had
  no timestamps then, so how many flips were Rasmus switching on purpose is unknown.
- Window server metadata at 06:55, game inactive (Steam Helper frontmost): the game window is
  **windowed** (1920×1080 at the top-left of the 3840×1600 ultrawide), at **layer 0** (not
  floating), and **first in the whole window stack** — nothing of the active app covers it.
- Mouselook is on for the game (`STEAMDRIVER_MOUSE=precise` → `WINEMAC_MOUSELOOK=Discovery.exe;…`).

## What we thought before the capture (superseded by "Cause")

A chain, each link read in the code but not yet seen together in one capture:

1. **Cmd-Tab away**: `applicationDidResignActive:` ends mouselook. `leaveMouselook`
   deliberately does not move the pointer when the app is inactive ("the user is going
   somewhere else"), so the pointer stays where mouselook froze it — **over the game window**
   (mouselook warps it to the window centre when it starts outside).
2. **The game window stays on top of its screen** when the app switched to has no window there
   (seen live).
3. **The next click there** hits the game window: macOS activates the game, and because
   `-[WineWindow acceptsFirstMouse:]` returns YES the same click is **also delivered into the
   game**. UE treats it as a viewport click and re-captures the mouse (hides the cursor,
   `ClipCursor`), mouselook freezes the pointer again, and the keyboard follows (the game is
   frontmost).
4. **The workaround fits**: an app with a window on the same screen raises it over the game,
   so clicks land there instead; then the pointer can leave for another monitor.

Ruled out along the way:
- **Cursor clipping outliving the deactivation**: the default clipping (confinement) is
  applied by macOS only while the app is frontmost and the window key.
- **The game floating above other apps**: it is windowed at layer 0.
- **Plain `SetForegroundWindow` stealing activation**: the driver only forces activation within
  2 s of a Win32 hotkey press or a systray click (`activate_on_following_focus`).

The cursor is hidden with `[NSCursor hide]`, which macOS scopes to the app — not a global hide.

## Next steps

1. **Capture** on `macfinals-139` with `WINEDEBUG=…,+macfinals_ui` (part of the all-our-logging
   launch line in `tests/rosetta_nt/NEXT_SESSION.md`): Cmd-Tab away, reproduce the steal once,
   note the time. The trace says, with timestamps: which app became frontmost; whether the
   game window was still uncovered 0.5 s later and the pointer over it; whether the game came
   back "activated by a click on hwnd …, also delivered into the game", by a driver request
   (with its call stack), or neither; and mouselook/clip/cursor transitions.
2. **Fix after the capture** (Rasmus chose log first). Candidates: don't deliver the
   activating click into the game (`acceptsFirstMouse:` NO while the app is inactive — cost:
   after switching back, the first click only focuses), or move the released pointer off the
   game window when mouselook ends because the app lost activation.
