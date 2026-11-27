# Windows games from installers (GOG and others) on SteamDriver

A game's Windows installer becomes a game in macOS Steam's library. It runs on the SteamDriver
tool, with everything a Steam game gets there (profiles, renderer choice, MetalFX upscaling, the
mouse, key combos, benchmarks), and it has the game's own Steam artwork. No GOG login, no GOG
Galaxy. State of 2026-11-26.

## In macSteam: Install a Windows game

1. Download the game's installer. For GOG games that's the **offline backup installer** from
   your library (`setup_<game>_<version>.exe`, plus `.bin` parts for big games, in the same
   folder).
2. Drop it on **Install a Windows game** at the bottom of macSteam's window, or click Choose….
3. Check the name. macSteam guesses it from the file name and looks it up in Steam's store; the
   cover shows the artwork it found. Pick another match or "No artwork" if it's wrong, and
   Search again after editing the name. If the store finds nothing, macSteam asks for the game's
   **Steam store link** (or "Wrong game? Paste a Steam store link…"): paste
   `https://store.steampowered.com/app/367520/Hollow_Knight/` or just the id `367520`. macSteam
   takes the id from it, fetches that game's name (`store.steampowered.com/api/appdetails`) and
   uses its artwork.
4. Click **Add to Steam**. macSteam:
   - copies the installer (and its `.bin` parts) to
     `~/Library/Application Support/SteamDriver/installers/<Name>/`. On the same disk this is an
     APFS clone, so it takes no extra space while the original exists;
   - closes Steam (it rewrites its files when it quits) and adds the game:
     - a non-Steam shortcut pointing at the copy;
     - SteamDriver as its compatibility tool;
     - the game's Steam artwork (cover, wide capsule, hero banner, logo);
   - starts Steam and runs the shortcut, so the installer comes up.
5. Install to the folder the installer suggests and let it finish. Don't start the game from its
   last page.
6. Launch the game from Steam. From now on, the shortcut starts the game.

## By hand in Steam (GOG installers only)

Add a Game → Add a Non-Steam Game → pick the `setup_….exe` (NotProton makes Steam accept Windows
programs), then Properties → Compatibility → SteamDriver. Launch it to install and again to
play. There's no artwork this way.

## How the run script finds the game

`environments/finals/compat-tool/run` treats a launch as an installer when its target is a GOG
`setup_*.exe` (anywhere) or a file in `SteamDriver/installers/` (anything macSteam added).
The installer installs into the shortcut's own prefix, `steamapps/compatdata/<shortcut id>`.
After that, launches start the game instead:

- **GOG:** every GOG installer leaves `goggame-<id>.info` in the game's folder, with the game's
  name and play tasks (the `.exe`, its arguments and working folder). The base game's primary
  play task starts (a DLC's `.info` is skipped).
- **Other installers:** the run script notes when the installer started. After it exits, it
  looks for a Start Menu or Desktop shortcut (`.lnk`) made since then that isn't an uninstaller,
  readme, manual or redistributable. The program it points at is kept in
  `compatdata/<id>/installed-exe`.
- **Remembering the installer:** its file name is kept in `compatdata/<id>/installed-from`.
- **Launch options** from Steam still reach the game.
- **Name:** macSteam's pickers show the game's name, not "App <id>".
- **Steam's app id:** like NotProton, the script unsets `SteamAppId` for non-Steam games (app
  ids from 2147483648 on), so a game sold outside Steam doesn't take the shortcut's id for a
  Steam app and fail an ownership check.

An installer that leaves neither (no `.info`, no shortcut) runs again at the next launch. The
log (`compatdata/<id>/steamdriver-run.log`) says so; point the shortcut at the game's `.exe` in
the prefix then.

## Install to (another folder or disk)

By default a game goes into Steam's folder: its prefix is
`Steam/steamapps/compatdata/<id>`, and the game itself is in
`…/compatdata/<id>/pfx/drive_c/GOG Games/<Game>/` (`C:\GOG Games\<Game>` inside it). **Install
to** in the panel picks another folder, for example on another disk; macSteam remembers it for
later installs.

- **Where it goes:** the whole prefix (game, saves, Wine's files) goes to
  `<folder>/<Name>-<id>/`, and `compatdata/<id>` becomes a link to it, so Steam and the run
  script use the usual path. The installer still installs to its default `C:\GOG Games\…`.
- **Disk format:** the folder has to be on an APFS or Mac OS Extended disk. A Wine prefix needs
  symbolic links (its drive letters are links), which exFAT and FAT disks (common on USB drives)
  can't hold, so macSteam tests the folder first and refuses one that can't.
- **Disk not connected:** launching the game says so (the run script checks the link) instead of
  making an empty prefix.
- **Deleting:** removes the folder there and the link. Only a folder macSteam made for that game
  (`<Name>-<id>`): if `compatdata/<id>` was linked to some other folder by hand, Delete removes
  just the link and leaves the folder alone.
- **Already installed:** a game installed before keeps its prefix where it is.
- **Not checked yet:** removing the shortcut in Steam itself (instead of macSteam's Delete) has
  been seen to delete `compatdata/<id>`. With a link that may remove only the link, leaving the
  game in its folder; use macSteam's Delete.

## While it installs

After Add to Steam, the panel keeps saying what is happening, read from the run script's log
for that launch (`compatdata/<id>/steamdriver-run.log`):

1. Steam starting the installer.
2. Preparing Wine for the game (a new game's prefix, about 15 s, only the first time).
3. The installer opening (Inno Setup unpacks itself from a big installer first).
4. Installing (finish the installer's window).
5. Installed, as soon as the installer's own processes (the `.exe` and Inno Setup's `.tmp`
   stage, named after the installer file) have closed, even if the installer started the game
   from its last page. The panel goes back to its drop zone.

Steam drops a run link that arrives while it is still starting, so macSteam waits for Steam's
`compat_log.txt` to say it mapped the shortcut to SteamDriver (Steam has signed in and read its
shortcuts then). It then sends `steam://rungameid/<game id>` 20 times half a second apart, then
10 times two seconds apart, and stops at the first sign that Steam took it (`GameAction [AppID
<id>` in `console_log.txt`, or `StartSession` in `compat_log.txt`). After that it gives up with
an error under the status line.

If the installer closes without installing anything it can find, the panel says so.

## Deleting a game

The **Installed** square beside the drop zone (shown once a game was installed there; "Review installed games") lists the
games installed with macSteam: cover, size, whether the shortcut is still in Steam. **Delete…**
asks first:

- **Keep Saves and Settings:** before deleting, the prefix's user folder (`drive_c/users`:
  AppData, Documents, Saved Games, without temporary files) and its user registry (`user.reg`,
  where Unity games and others keep settings) are copied to
  `SteamDriver/saved-games/<Name>/`. Install the same game again with macSteam, under the same
  name, and the run script puts them back into the new prefix once, before the installer runs.
  A game that saves inside its own install folder loses those saves.
- **Delete Everything:** nothing is kept.

Then, with Steam closed and reopened afterwards if it was running:
- the shortcut is removed (the others are renumbered as Steam does);
- its SteamDriver entry in `config.vdf` and its artwork in `grid/` go;
- the prefix (`compatdata/<id>`, the installed game), the installer copy in
  `SteamDriver/installers/<Name>/` and macSteam's record (`SteamDriver/games/<id>`) are deleted.

A game whose shortcut was already removed in Steam is still listed (its prefix stays behind
then). macSteam finds its id from the `shortcut` file it writes at install, else from Steam's id
formula.

## Updates, DLC, reinstalling

- **Update:** drop the new installer on macSteam with the same name. That adds it next to the
  old one and points the shortcut at it. Or point the shortcut at it by hand (Properties →
  Target). Its file name carries the new version, so it differs from `installed-from`; the next
  launch runs it over the installed game, and the launch after that starts the game. GOG's
  installers are always the whole game. Some games also have small `patch_….exe` files: point
  the shortcut at one and launch once (it runs as a plain program), then point it back.
- **DLC:** GOG ships DLC as separate `setup_…` installers. Point the shortcut at one, launch it
  once with `STEAMDRIVER_SETUP=1 %command%` in its launch options, then remove the option and
  point the shortcut back.
- **Reinstall:** `STEAMDRIVER_SETUP=1 %command%` runs the installer again even though the game is
  there.

## Steam's files macSteam changes

All are written only while Steam is closed. Each gets one backup beside it the first time
(`.steamdriver-backup`).

| File | What |
|---|---|
| `Steam/userdata/<account>/config/shortcuts.vdf` | the shortcut (binary KeyValues). `appid` is Steam's own formula: CRC-32 of the quoted exe path and the name, top bit set. The account is the newest login in `config/loginusers.vdf`. Other shortcuts stay byte for byte; adding the same installer again updates its shortcut instead of making a second one |
| `Steam/config/config.vdf` | `CompatToolMapping` → `"<id>" { "name" "notproton-steamdriver" … }`, what Properties → Compatibility writes |
| `Steam/userdata/<account>/config/grid/` | `<id>p.jpg` cover (`library_600x900_2x`), `<id>.jpg` capsule (`header`), `<id>_hero.jpg`, `<id>_logo.png`, from Steam's CDN (`cdn.cloudflare.steamstatic.com/steam/apps/<app>/…`). The match comes from Steam's store search (`store.steampowered.com/api/storesearch`) |

## Not included

- GOG Galaxy features (cloud saves, achievements, automatic updates, multiplayer that needs
  Galaxy) and downloading from a link. GOG's downloads need you signed in to GOG.
  `lgogdownloader` could add that and download only changed files, but it needs a GOG login, so
  it's left out for now.
- `.msi` installers. Only `.exe` installers are accepted.

## Tested

- Hollow Knight 1.5.12620 (64-bit, GOG): the installer ran through Steam on SteamDriver into the
  shortcut's prefix, added by hand (2026-11-26).
- Tests (`tests/test_game_installer.py`) cover:
  - Steam's shortcut file and id formula, the compatibility mapping and the artwork;
  - names from installer file names and the `.bin` parts;
  - the run script with a GOG installer, another installer and its Start Menu shortcut, an
    update, a forced reinstall and an installer that left nothing.
  - Delete: everything of that game and nothing of another; Keep Saves and Settings; a game on
    another disk; a game already removed in Steam; a hand-made link to another folder (only the
    link goes; this was a bug until 2026-11-27, when Delete removed the whole folder);
  - the panel's status lines while an installer runs, from the run script's log: a GOG install
    to the end, the installer's own process closing, an installer that left nothing or closed
    early, a game already installed.
- Not yet run in the real app: macSteam's drop zone end to end.
