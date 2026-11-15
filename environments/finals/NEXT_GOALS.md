# MacFinals: next goals

The order Rasmus set on 2026-11-14. Session-by-session state and history live in
`tests/rosetta_nt/NEXT_SESSION.md`; crashes and hangs in `crashes/README.md`.

| # | Goal | Where it stands |
|---|---|---|
| 1 | **Crash and hang fixes in progress** (next session) | Plan in `tests/rosetta_nt/NEXT_SESSION.md`, "Next session"; evidence in `crashes/` |
| 2 | **Performance** | Below |
| 3 | **Keyboard and controllers** | Below |
| – | Later | Below |

## 2. Performance

Where it stands (2026-11-14):

- **The GameThread is the limit:** 75–86 % of one core in play, about half of it in the kernel.
  Little is left to gain inside Wine.
- **Syscall fast path** (the NT hook, `macfinals-114` and later): 58–61 ns per diverted call, against
  about 16 µs through Rosetta's normal `syscall` translation. The game issues about 32,000 such calls a
  second (`rax=0xffffffff`), so this saves about 0.5 s of CPU per second. In-game A/B with
  `MACFINALS_NT_OFF=syscall` against fast path on is still to do once the crash work settles.
- **Rosetta's exception server** is the busiest system thread. In the 2026-11-14 18:02 session it
  ran at 77 % system share, with bursts of 1,500–2,000 handled exceptions a second in the game
  process during loading and round changes. Measure what those faults are (counts only, never the
  anti-cheat's code) and whether Wine's fault path for them can be cheaper.
- **Counters cost:** `STEAMDRIVER_PERF=1` adds work to every NT call; don't compare FPS with and
  without it.

Ideas, not started:

1. **Don't spend work on code we don't target** (proposed 2026-11-14, not approved): skip diverts at
   syscall sites inside Wine's own unix modules, as is already done for the macOS shared cache
   (`ctl[98]`, `shared-skip`). Fewer patched sites means less memory and translation work.
2. ~35,000 failed fixed-address mappings a second on worker threads (`try_map_free_area`). The
   stack hint (`MACFINALS_STACK_HINT`) halved the VM lock time, but FPS didn't change.
3. **Microstutter:** the macOS compositor sometimes shows a finished frame one refresh late. Game
   Mode for every Wine program helps a little (held frames 1.9 % → 1.5 %). Not yet tried with
   THE FINALS' anti-cheat. Details: `tests/rosetta_nt/R24_MICROSTUTTER_GAME_MODE.md`.
4. `DISABLE_AOT=1` is forced by the hook; its cost is unmeasured (it matters little for PE code,
   which Rosetta translates at run time anyway).

Background: `tests/rosetta_nt/R17_PERFORMANCE.md` (2026-11-02 measurements, the syscall cost).

## 3. Keyboard and controllers

- **Keyboard:** holding Control must work as a normal game key in THE FINALS (for example jump on
  Ctrl, held down). Find which macOS or Mac-driver shortcuts get in the way (Control-key
  combinations the system or winemac.drv catches before the game) and turn them off for the game.
  **Cmd-Tab must keep working.**
- **Controllers:** test controller input in the game.
- References to look at:
  - Moonlight (https://github.com/moonlight-stream/moonlight-qt): a game-streaming client that
    captures the keyboard on macOS, system shortcuts included, while letting the user out again.
    Possibly also useful for controller handling.
  - Karabiner-Elements (https://github.com/pqrs-org/Karabiner-Elements): low-level keyboard
    remapping on macOS, for how keys can be seen and changed before the system's shortcuts act.
  - Check each project's licence before reusing any code; reading them for the approach is fine.
- The keyboard and mouse path changes go in SteamDriver's Mac driver patches, so CS2 and
  Deadlock on the main environment must keep working.

## Later

- **H.264/AAC decoding** is missing from the runtime. In Wine 11, `msmpeg2vdec` and `msauddecmft`
  forward to winegstreamer, and the build uses `--without-gstreamer`; winedmo only demuxes. Not a
  crash cause. Details: `tests/rosetta_nt/R25_SIGNAL_IN_STUB.md`, H.264 section.
- Build a SteamDriver app bundle from the newest runtime once the crash work settles, and recheck
  Crab Champions.
- First launch in a fresh prefix shows Denuvo's "issue with starting" dialog; the second launch goes
  in.
