# C11 fix (the assert part): Wine passes macOS IP_RECVTOS control messages

Fixed in **`macfinals-31`**. Source: `tests/rosetta_nt/R16_FIRST_LAUNCH_HANG.md`.

## Cause

macOS reports the TOS byte as control message type 27 (`IP_RECVTOS`). Wine's
`convert_control_headers` didn't handle that type, so Steam's networking received no TOS data
after asking for it, asserted, and wrote a minidump on every run. That dump could hang the game.

## Fix

A Wine winsock patch that converts the macOS `IP_RECVTOS` control message (see `WINE_PATCHES` in
`scripts/build_portable_runtime.py` and R16 for the patch name).

## Verified

On `-31` the TOS assert and its minidumps no longer appear. The freeze part of C11 stays open.
