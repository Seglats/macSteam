# C11: first launch after a change sometimes freezes (Steam TOS assert, minidump)

| | |
|---|---|
| First seen | 2026-11-02, `macfinals-27` |
| Origin | before (Wine: macOS `IP_RECVTOS` control messages) |
| Status | the assert is fixed in `macfinals-31`; the freeze itself is unexplained |
| Reproducer | none for the freeze |
| Fix | [fixes/C11-fix.md](fixes/C11-fix.md) (the assert) |

## What you see

On the first launch after a change the game sometimes freezes hard; the next launch plays.

## Evidence

Every run, 30–70 s in, Steam's networking in the game process asserted
`No control data returned even though we asked for TOS?`, preceded by
`fixme:winsock:convert_control_headers Unhandled IPPROTO_IP message header type 27`. Writing that
assert's minidump could hang. Source: `tests/rosetta_nt/R16_FIRST_LAUNCH_HANG.md`.

## What we think

The assert and its dumps are gone with the fix. Whether they were the whole freeze isn't known.
