# SteamDriver

MacVolvo fork: a NotProton companion (per-game Mac driver, renderer and mouse options in
macOS Steam) plus an own-Steam mode that runs Windows Steam in a Wine environment the app
ships. Start with `README.md`, `docs/NOTPROTON.md` and `app/ARCHITECTURE.md`.

- **Wine environments live in `environments/`** (see its README): `base` is SteamDriver's own
  runtime (its files still at the repository root), `finals` is the MacFinals Wine for THE
  FINALS. Adding a game that needs its own Wine means adding an environment, not changing base.
- **Working in `environments/finals/` means its `CLAUDE.md` applies in full**: hard boundaries
  (never touch THE FINALS' files, no spoofing or hiding Wine from the anti-cheat, never analyse
  the anti-cheat's code, never copy NotProton/CrossOver closed code, never modify GPTK or
  Rosetta on disk), the Rosetta method constraints, and its debug-output model.
- The SteamDriver app side and the base environment must keep working for CS2 and Deadlock;
  "breaking DX11 is acceptable" in the finals notes applies only inside that environment.
- Commit or push only when Rasmus asks.
