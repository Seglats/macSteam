# C08 fix: start the game app only in place of a loader that exists

Fixed in **`macfinals-106`**. Write-up: `tests/rosetta_nt/R24_MICROSTUTTER_GAME_MODE.md`
(section 3).

## Cause

For a 64-bit program, ntdll first tries the other architecture's loader
(`lib/wine/i386-unix/wine`), which this runtime doesn't have; normally that fails quietly and
ntdll uses its own. Recall's `game_mode_exec` didn't check the loader exists. With
`WINE_GAME_MODE=*` (`wine-ntdll-game-mode-all.patch`, `macfinals-104`) every program went through
it, so the game app was started pointing at the missing loader, found no `ntdll.so`, and exited.

## Fix

`patches/wine-ntdll-game-mode-loader-check.patch`: `game_mode_exec` returns early unless
`argv[0]` exists and is executable (`access(argv[0], X_OK)`).

## Verified

`game_mode_check.py` on `-106`: Game Mode came on and the stand-in started. KovaaK's and THE FINALS
started from the game apps.
