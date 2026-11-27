# Runtimes: shared prefixes and own copies

How the SteamDriver tool makes a new game's Wine prefix, and macSteam's Advanced › Custom
runtimes switch. State of 2026-11-26.

## Shared runtime (default)

Every game has its own Wine prefix, `steamapps/compatdata/<id>/pfx`: a Windows "C: drive" with
its own registry, system folders and user folders. Until 2026-11-26 each was built from scratch
with `wineboot` at the game's first launch, which took about 15 s and wrote about 400 MB of the
same Windows files every time (6.4 GB of `windows` folders across 10 games on the test Mac).

Now the run script (`environments/finals/compat-tool/run`, "First run: make the prefix"):

1. **First prefix of a runtime:** built with `wineboot` as before. Right after, before anything
   of that launch touches it, it is copied to `SteamDriver/prefix-templates/<runtime>/` (for
   example `macfinals-191`), the runtime version from its `runtime.json`.
   - The copy is written under a lock (`<template>.lock`) to a temporary name, and gets
     `.complete` before it is renamed into place, so two games starting at once can't leave a
     half-written template.
2. **Every later new prefix on that runtime:** an APFS clone of the template (`cp -c`). That
   takes about 1 s instead of 15, and costs about 2 MB of disk instead of 608 MB, because a
   clone shares its blocks with the template until a game changes a file. On another disk (a game
   installed elsewhere with Install to) it is a plain copy: still fast, no space saved.

Measured 2026-11-26 on `macfinals-191`: 15 s for the first prefix (template saved), 1 s for the
next, 2 MB of real disk for a clone, and Wine runs in it (`cmd /c echo`).

Nothing game-specific happens when a prefix is made. Everything per game runs at every launch,
on a cloned prefix as on a built one:
- the DXMT, DLSS and NVAPI files;
- NotProton's Steam bridge;
- settings, installer handling and restoring kept saves.

Existing prefixes stay as they are. A runtime update gets its own template the first time one of
its games makes a new prefix. An existing prefix moving to a newer runtime is updated by Wine
itself, as before.

## Own runtime copy (Advanced › Custom runtimes)

macSteam's Advanced panel has **Custom runtimes**. It writes `SteamDriver/custom-runtimes`, and
it lists the runtimes that have a shared template. With it on, a game profile's Settings sheet
has **Own runtime copy**, which writes `profiles/app-<id>/own-runtime`.

For such a game the run script builds the prefix with `wineboot` and doesn't use or make a
template, the way every game worked before. This is for a game whose runtime will be set up
differently.

- It applies when the game's prefix is made: a game that already has a prefix keeps it.
- It needs the switch on: an `own-runtime` file without `custom-runtimes` is ignored, so
  turning the switch off sends every new prefix back to the shared template.

## To do

- **Other runtimes per game:** load a runtime from a `macfinals-N.tar.gz` (the build's
  artifacts) or a folder into `SteamDriver/runtimes/<name>/`, list them under Custom runtimes,
  and let a game profile pick one. The run script would then set `engine` to it for that game.
  - **Checks:** `bin/wine`, `lib/wine` and `runtime.json`; strip the quarantine attribute;
    verify the signature.
  - **Apple's files:** the `lib/apple-gptk` link to the player's Game Porting Toolkit has to be
    set per runtime (the run script does it for its own `engine` already).
  - **Template:** each runtime gets its own template automatically, keyed by `runtime.json`'s
    version.
- **Removing a template:** a button to delete templates of runtimes no longer in use. Existing
  clones keep their data, since APFS only frees blocks no clone uses.
- **Rebuilding a game's prefix** from the template, keeping its saves (the Keep saves code in
  `InstalledGames.swift` does the saving part).
