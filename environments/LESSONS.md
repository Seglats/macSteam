# Lessons: merging MacFinals into SteamDriver and building environments (2026-11-09)

What broke while `environments/` was set up, and what to do instead. The game-specific lessons
are next to their code: `finals/compat-tool/LESSONS.md` (macOS Steam route) and
`finals/tests/finals_bisect/PROFILING.md` (freeze profiling).

## Repository and merge

- **The merge keeps history without taking MacFinals' tree.** It was `git merge -s ours
  --no-commit macfinals/main`, then the MacFinals files were extracted into `environments/finals/`
  (`git archive macfinals/main <paths> | tar -x`). SteamDriver's app side won every conflict
  because nothing of MacFinals reached the root. MacFinals' history is the merge's second parent.
- **Never run `git stash` during an unfinished merge.** It cleared `MERGE_HEAD`, so the next
  commit would have been an ordinary commit and lost the second parent. The files came back with
  `stash pop`; the merge had to be started again before committing. Run tests in a separate
  worktree instead (`git worktree add --detach <dir> main`).
- **The main environment stays at the repository root for now.** Its build scripts are woven
  into the app build (`build_native_app.py`) and the NotProton driver build
  (`build_mouselook_driver.py`, which compiles `winemac.so` from the same sources). Moving them
  would have broken SteamDriver's working build for no gain. `environments/main/README.md` points
  at them.
- **An environment keeps the old repository layout inside its folder** (`scripts/`, `patches/`,
  `tests/`, `d3d12_layer/`). Its scripts' `ROOT = parents[1]` then lands in the environment
  folder unchanged. Things shared with the root go through `REPO = ROOT.parents[1]`:
  - `config/`, `licenses/`;
  - `runtime/toolchains`, `gptk/` (untracked local inputs).
- **Check every import and path after moving scripts:**
  - `assemble_portable_runtime.py` and `add_nt_hook.py` import `game_mode_app` from the root
    `scripts/` (now added to `sys.path`).
  - They also import `derive_runtime`. MacFinals' copy reads universal Mach-O files (the hook);
    SteamDriver's copy doesn't. The environment keeps its own copy.
  - `launch_main.sh` looked for the stand-in `steamwebhelper.exe` two folders up; it is now four
    folders up.
  - Run every script's `--help` after a move; an import error shows at once.
- **Local untracked inputs differ between checkouts.** The finals build needs `llvm-mingw-20251216`,
  `bison` and Apple's toolkit at `gptk/gptk`. SteamDriver's checkout had a newer llvm-mingw and no
  GPTK. They were copied with `cp -c` (APFS clones, no extra disk space).

## Build workspaces

- **Never clone a built workspace's dependencies into a new one.** FreeType and the native
  dependencies record their workspace's absolute paths. The new build stopped with "FreeType
  configured for a different workspace". Clone only `downloads/`, then run
  `build_portable_dependencies.py` in the new workspace. It's about 15 minutes and is the
  documented step anyway.
- **Use a new workspace for a patch change that needs clean source**, so the known-good one stays.
  SteamDriver's driver patches replace ones early in the finals patch order, so they needed
  `~/SteamDriver-build/finals/phase-2`, leaving `~/MacFinals-build-gcc/phase-2` as it was.
- **Check a patch swap on clean source before a long build.** Extract `wine.tar.gz` and Soju into
  scratch, apply the full `WINE_PATCHES` order with the swapped files (import
  `build_portable_runtime` for the list: its comments contain `]`, so parsing it with `ast` fails).
  Then check the old patch's added lines against the result. The ones missing were parts Recall
  1.6.2 rewrote (raw-mouse timing, the activation resync), not dropped features.
- **Only the last applied patch can be fixed in place.** The source tree's
  `.macfinals-patches.json` records each applied patch's hash, and a change to an applied one
  demands clean source. For the last one:
  1. `git apply -R` it in the tree;
  2. drop its entry from the record;
  3. fix the patch;
  4. rebuild incrementally.
  This was needed twice for `wine-steamclient-lsteamclient.patch`.
- **MinGW-w64 GCC 16 treats incompatible pointer types as errors.** Passing a `BYTE *` where an
  `HMODULE` is expected (`RtlImageDirectoryEntryToData`, `RtlImageNtHeader`) fails the PE build.
  Cast explicitly.
- **Runtime versions are global across workspaces.** `macfinals-48`…`53` came from the new
  workspace, after `-47` from the old one, so no two artifacts share a name.

## The app

- **Every `MacVolvo*.app` and `SteamDriver-N.app` shares the bundle identifier
  `app.macvolvo.launcher`** with single-instance set. Opening a new build while an old one runs
  only focuses the old one.
- **Game tools ship in the app until downloads exist.** `build_native_app.py --game-tool
  finals=<runtime.tar.gz>` bundles `Resources/GameTools/<id>/`:
  - the tool's files;
  - the runtime archive;
  - `tool.json`.

  `GameTools.swift` unpacks it beside Steam's `compatibilitytools.d/<id>` and swaps it in. A
  marker file means a folder the app didn't make is never touched.
- **`~/Library/Application Support/Steam` can be a symlink.** On Rasmus's Mac it points to
  `~/SteamProfiles/main/data`, one of two Steam profiles. Anything written under it lands in the
  active profile only. A symlink meant to survive profile switches should point at the real path.
