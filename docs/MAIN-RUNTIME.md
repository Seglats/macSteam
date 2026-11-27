# The two runtime archives an app build needs

`scripts/build_native_app.py` takes two Wine runtime archives. They do different jobs and are
rebuilt at very different rates. State of 2026-11-27.

| | Main runtime | SteamDriver runtime |
|---|---|---|
| Argument | `--runtime-archive` | `--game-tool finals=…` |
| File now | `runtime/phase-2/artifacts/macvolvo-1.2-dev.4.tar.gz` | `~/SteamDriver-build/finals/phase-2/artifacts/macfinals-<N>.tar.gz` (212 now) |
| What it is | Recall 1.6.2's release runtime (`phase2-20261028.2`) with one file replaced: the Mac driver, `lib/wine/x86_64-unix/winemac.so` | CrossOver 26.3's Wine built from source with all our patches (`environments/finals/`) |
| Rebuilt | Once: dev.1 to dev.4 on 2026-11-06/07, not since | All the time: every Wine change is a new number |
| Used for | macSteam's own Steam, and SteamDriver on CrossOver | The SteamDriver compatibility tool, for every game |

## What the main runtime archive is used for

1. **macSteam's own Steam** (Advanced). The whole archive ships inside the app as
   `Contents/Resources/runtime.tar.gz`. It's the Wine that the own Steam unpacks and runs Windows
   Steam in. Its SHA-256 is pinned in `app/Resources/release.json` (`runtimeSHA256`,
   `eb372bea…`), and the app checks it before installing, so it has to stay the same file.
2. **SteamDriver on CrossOver.** The build takes these out of it and lays them over NotProton's
   CrossOver (`NOTPROTON_DRIVER`, `NOTPROTON_DXMT`, `NOTPROTON_PROFILE`):
   - the Mac driver (`winemac.so`), copied once per supported CrossOver build;
   - DXMT;
   - the DXMT renderer profile.
3. **The licence texts.** The archive's `licenses/` folder becomes the app's
   `Contents/Resources/Licenses`.
4. **Checks.** The build reads its version and contents to decide which kind of app it is making.

None of this is SteamDriver itself. The SteamDriver tool comes from the other archive, which
carries its own Wine, Mac driver and licences.

## Where the changes went

Everything since 2026-11-07 went into the SteamDriver runtime, which the app bundles, so all of
it ships:
- THE FINALS' crash and hang fixes, and the rope rendering fix;
- Wine registering as a game, for less WindowServer hitching;
- the controller patches;
- headphone surround;
- that runtime's own copy of the Mac driver, with its UI and activation fixes.

Nothing changed the main runtime's Mac driver. The four Wine patches added at the repository
root's `patches/` on 2026-11-09 came with the THE FINALS work, which now lives in the finals
environment.

## Getting the archive into a new checkout

`runtime/` is ignored by git, so a fresh clone doesn't have the main runtime archive. Copy it
from the checkout that has it rather than rebuilding (`cp -c` makes an APFS clone, which takes no
extra space on the same disk):

```sh
mkdir -p runtime/phase-2/artifacts
cp -c <old checkout>/runtime/phase-2/artifacts/macvolvo-1.2-dev.4.tar.{gz,json} runtime/phase-2/artifacts/
shasum -a 256 runtime/phase-2/artifacts/macvolvo-1.2-dev.4.tar.gz    # must start with eb372bea
```

The app build also needs two toolchains from the same ignored folder: llvm-mingw (it compiles
the small Windows helpers, such as the surround test and the controller check) and libarchive
(the own-Steam worker):

```sh
mkdir -p runtime/toolchains
cp -cR <old checkout>/runtime/toolchains/{llvm-mingw-20261006-ucrt-macos-universal,libarchive-3.7.7} runtime/toolchains/
```

A rebuild gives a different SHA-256, so `release.json` would need updating, and the bundled runtime
would no longer be the one that was tested.

## Rebuilding it

Not needed for 1.0. A full rebuild (Wine and DXMT from source) has never been done for this
runtime, and its pipeline isn't set up on the build Mac. What could be worth doing later is
narrower: bring the SteamDriver runtime's newer Mac driver fixes into the driver used for
SteamDriver on CrossOver. That means porting the driver patches, rebuilding `winemac.so` only,
and re-testing Counter-Strike 2 and Deadlock on CrossOver. The steps are in
[UPSTREAM-RECALL.md](UPSTREAM-RECALL.md) and [BUILDING.md](BUILDING.md).

## Dropping it later

The archive could go from the app, which would make the DMG about 240 MB smaller:
- macSteam's own Steam is a troubleshooting setup and could be dropped or downloaded on demand;
- the CrossOver files (driver, DXMT, profile) could be built and shipped on their own.

For 1.0 the app build requires it.
