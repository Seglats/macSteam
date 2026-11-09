# Lessons: THE FINALS from the macOS Steam app (2026-11-09)

How the "SteamDriver: THE FINALS" compatibility tool came about, what broke on the way, and
the facts about NotProton it rests on. Sources: NotProton's GPL source
(github.com/NotProtonNot/NotProton, read for understanding only; nothing is copied) and our logs.

## Why a tool of our own, and not a NotProton option

- **NotProton has a page builder, but it can't choose a runtime.**
  - `notproton/plugins/*.conf` rows become switches, dropdowns (`type = choice`) and sliders on a
    game's Compatibility page. They set `env`, `overrides` (DLL overrides), `stage` (files into
    system32) and `inject` (load a file into the game).
  - `notproton/variants/*.conf` lay a folder over NotProton's CrossOver runner.
  - No key picks a different runner. Every game runs in NotProton's CrossOver copy.
- **Variants may not replace `ntdll` or `lsteamclient`,** because they carry NotProton's Steam
  bridge. NotProton binary-patches the copied CrossOver `ntdll.dll` (`ntdll-patch/detour.c`, about
  180 lines, fitted per CrossOver build by hash). When `steamclient64.dll` loads, the patch points
  its exports at `lsteamclient.dll` (Proton's Steam bridge, ported to macOS). Replacing `ntdll`
  removes that, and the game can't reach Steam.
- **Our finals Wine needs its own `ntdll`.** 15 of its 21 patches touch it. The one the storm fix
  can't do without is `wine-syscall-fastpath-publish`, a 9-line pointer publication for the hook.
  CrossOver's `ntdll.so` is stripped, so the hook can't find the dispatcher there by name.
- **The earlier `notproton-finals-cx263-compat` tool (October) is the off-limits closed package.**
  It patched memory from outside and needed developer mode. Rasmus wants none of that. Ours
  builds the bridge into Wine from source, and everything runs inside the game's process.

## How the tool works

1. Steam runs `run <verb> …` through NotProton's launch hook.
2. `run` copies NotProton's bridge files, unchanged, into the prefix's
   `C:\Program Files (x86)\Steam`:
   - `lsteamclient.dll`/`.so`, `steam.exe`;
   - `steamclient64.dll`, `steamclient.dll`, `tier0_s64.dll`, `vstdlib_s64.dll`.
3. `run` puts that folder on `WINEDLLPATH` and sets `steamclient=n;steamclient64=n;lsteamclient=b`.
4. It starts the game as `wine "C:\…\Steam\steam.exe" <game> <args>`.
5. Our `wine-steamclient-lsteamclient.patch` (in `build_module`) does what NotProton's detour
   does, from source:
   - it loads `lsteamclient.dll`;
   - it writes a `movabs rax; jmp rax` jump over each matching export, unprotecting only those
     bytes and restoring them afterwards;
   - it stubs the entry point to return TRUE;
   - it leaves the imports unresolved.
   It is inert without `lsteamclient.dll`, so the own-Steam prefix is unaffected.
6. NotProton's `lsteamclient.so` needs 11 symbols from `ntdll.so`, all present in our 26.3 build.
   The DLL imports only standard Wine DLLs.

## What broke, in order

1. **`lsteamclient.dll` not found, so `steamclient64.dll` loaded unchanged.** The fail-safe path
   worked, but the redirect never happened. CrossOver's loader (CW HACK 20810,
   `find_builtin_without_file`) loads a builtin only when its file is on the search path; it
   doesn't search `WINEDLLDIR`/`WINEDLLPATH` without a file. A game loads `steamclient64.dll` with
   the Steam folder on its search path; our probe loaded it by full path. Fix: the patch looks for
   `lsteamclient.dll` in `steamclient64.dll`'s own folder first, then on the normal search path.
   Proof: `tests/windows_probe/steamclient_probe.c` through `run runinprefix` showed 40 exports
   redirected and `CreateInterface` jumping into `lsteamclient.dll`.
2. **"OS Error 260" (1): the install script.** On first launch Steam runs the game's install
   script through the tool: verb `run`, `iscriptevaluator.exe legacycompat\evaluatorscript_<id>.vdf`.
   That path is relative to Steam's folder. `run` had changed into the game's folder for every
   launch verb, so the evaluator exited with 1. Fix: only `waitforexitandrun` (the game) changes
   folder; helpers keep the folder Steam gives them, as NotProton's script does.
3. **"OS Error 260" (2): the game.** Steam's log showed it creating the process
   `…/steamdriver-finals/./run waitforexitandrun …/Discovery.exe`, which failed with Cocoa's
   260 ("file not found"). NotProton's launch hook (`dylib/hooks/hook_launch.c` →
   `np_compat_runs_tool`) passes the command to `/bin/sh -c` only when its path contains
   `/compatibilitytools.d/notproton…/run`; otherwise macOS Steam can't start it. The October tool
   worked only because of its name. Fix: our folder is `notproton-steamdriver-finals`; the display
   name stays "SteamDriver: THE FINALS". NotProton's cleanup (`remove_unlisted_tools`) only removes
   `notproton*` folders it wrote itself (a `flavor` file or its own `run` header), so ours stays.
   After a rename, Steam needs a restart and the game's Compatibility choice has to be set again.

## Where to look when it fails

| Log | Shows |
|---|---|
| `~/SteamProfiles/main/data/logs/console_log.txt` | Steam's launch steps (`GameAction … LaunchApp`), the exact command and environment it spawned, the error |
| `…/logs/compat_log.txt` | which tool Steam mapped the game to, and the command prefix it built |
| `~/Library/Application Support/notproton/notproton.log` | `compatsvc` mapping and `[launch] handed to /bin/sh -c` (missing means the hook didn't wrap our tool) |
| `<library>/steamapps/compatdata/2073850/steamdriver-run.log` | our `run`: verb, folder, settings, exit codes |
| `…/compatdata/2073850/steamdriver-wine.log` | Wine and the game (`+loaddll` by default; `steamclient_redirect` lines show the bridge) |

Steam deletes `legacycompat/evaluatorscript_<id>.vdf` after use, so the install-script step can
only be replayed by launching again.

## Game files

The game was moved from the MacFinals prefix's library into macOS Steam's (same disk, so the move
was a rename), with `appmanifest_2073850.acf` copied. A symlink at the old place points at the real
profile path, so `launch_main.sh` and older runtimes still work. Both Steams now share one copy:
don't let them update it at the same time.
