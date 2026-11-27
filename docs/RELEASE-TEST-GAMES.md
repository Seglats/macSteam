# Release test games

Games to run before a release, and what each one proves. Go through
[`RELEASING.md`](RELEASING.md) step 5 with this list. Write down each result (pass, fail,
skipped and why) in the release notes.

**Pass** means: it installs and launches from macOS Steam, reaches gameplay, plays for 10
minutes, gets back to the menu (for online games, after a full match), and exits cleanly with
no Wine processes left behind. Write down the renderer and the fps you saw.

Marked **(?)**: not confirmed. Check its anti-cheat on areweanticheatyet.com and how it runs on
ProtonDB before you count it as a test.

## Must pass (block the release)

| Game | Environment | API | What it proves |
|---|---|---|---|
| Counter-Strike 2 | main | DX11 | The main path: VAC, mouse, latency, a full match |
| Deadlock | main | DX11 | Second main title; long matches, many effects |
| THE FINALS | finals | DX12 | D3DMetal, the Rosetta hook, the anti-cheat on truthful Wine, the C06/C16 fixes |
| Crab Champions | finals | DX12 | The DX12 canary with no anti-cheat in the way |

## Should pass (fix or list as a known issue)

### Renderer: heavy DX12, no anti-cheat

| Game | What it proves |
|---|---|
| Cyberpunk 2077 | Big open world, streaming, many shaders |
| Resident Evil 4 (remake) / RE Village | RE Engine DX12; Denuvo copy protection (not an anti-cheat) |
| Monster Hunter Wilds | Very demanding; shader compiling and memory pressure |
| Street Fighter 6 | Steady 60 fps frame pacing and input latency |
| Hogwarts Legacy | UE4 DX12, traversal stutter |
| Lies of P | UE4, fast action, controller |
| Marvel's Spider-Man Remastered | Fast streaming, high draw-call counts |
| Hades II | Light DX12; window and fullscreen switching |

### Renderer: DX11 and older, no anti-cheat

| Game | What it proves |
|---|---|
| Sekiro / Dark Souls III | FromSoftware DX11, fixed 60 fps |
| Hollow Knight | Unity, light; controller |
| Stray | UE4 DX11 |
| Deep Rock Galactic | UE4, online co-op with no client anti-cheat |
| Valheim | Unity, online, long sessions |

### Online and anti-cheat that run under Proton with no Linux module

These test whether the anti-cheat accepts an honest Wine, the same question THE FINALS asks.

| Game | Anti-cheat | Notes |
|---|---|---|
| ARC Raiders (?) | Embark's setup (?) | Same studio as THE FINALS; the closest sibling test |
| Helldivers 2 | nProtect GameGuard | Broken by a recent update (2026-11); retest every release |
| Overwatch 2 | Blizzard's own | DX11; Battle.net account through Steam |
| Marvel Rivals (?) | NetEase's | Plays on Linux; whether it needs a Linux part is unknown |
| Path of Exile 2 | Server-side | DX12 / Vulkan / DX11; heavy effects |
| Warframe | Its own, light | DX12; long sessions |
| Final Fantasy XIV | None on the client | DX11; launcher with a login (separate account) |

## Notes

- The first launch of an anti-cheat game in a new prefix can fail while the anti-cheat sets
  itself up. Launch again before you call it a failure (see
  [`environments/finals/README.md`](../environments/finals/README.md)).
- Steam holds Xbox-protocol controllers for itself. Test one game with a controller
  (Hollow Knight or Street Fighter 6) on every release.
- Every row outside "Must pass" is outside THE FINALS scope. A failure there goes into the
  release notes as a known issue; fixing one is a separate decision.
