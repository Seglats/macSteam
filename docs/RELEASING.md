# Releasing macSteam

How a macSteam release is built, signed, notarized and published at
[Seglats/macSteam › Releases](https://github.com/Seglats/macSteam/releases), where users download
it and where macSteam's updater looks (`Brand.releaseFeed`, `app/Sources/UpdateService.swift`).

First release: **1.0** (`appVersion` in `app/Resources/release.json`). The build number
(`APP_BUILD` in `scripts/build_native_app.py`, 9 now) only ever goes up; raise it for every
release.

## Once: Apple Developer ID

1. Join the [Apple Developer Program](https://developer.apple.com/programs/enroll/) (99 USD a
   year; as an individual).
2. Xcode › Settings › Accounts › your Apple ID › Manage Certificates › **+** › **Developer ID
   Application**. It lands in the keychain as `Developer ID Application: <name> (<TEAMID>)`.
3. An app-specific password at [account.apple.com](https://account.apple.com), then store it as
   the notary profile the scripts use:

   ```sh
   xcrun notarytool store-credentials macsteam --apple-id <Apple ID> --team-id <TEAMID>
   ```

4. For the DMG window layout: `python3 -m pip install dmgbuild==1.6.5`.

Every later release must be signed with the **same team**: the updater installs an update only
if it's notarized and signed by the team of the copy that's running.

## Each release

Set `ID="Developer ID Application: <name> (<TEAMID>)"`, `V=1.0` and `W` to the finals workspace
(`~/SteamDriver-build/finals/phase-2`).

1. **Version:** `appVersion` in `app/Resources/release.json`, `APP_BUILD` in
   `scripts/build_native_app.py`, and the notes in `app/Sources/WhatsNew.swift` (shown once after
   an update and from Help).
2. **Tests:** `python3 -m unittest discover tests` must pass.
3. **Build and sign the app** (hardened runtime and timestamp come with a real identity):

   ```sh
   DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer python3 scripts/build_native_app.py \
     --identity "$ID" --runtime-archive runtime/phase-2/artifacts/macvolvo-1.2-dev.4.tar.gz \
     --game-tool finals=$W/artifacts/macfinals-<N>.tar.gz --output runtime/release/$V/macSteam.app
   ```

   `macfinals-<N>` is the newest runtime that has passed the release test games (212 when this
   was written).
4. **Notarize and staple the app:**

   ```sh
   ditto -c -k --keepParent runtime/release/$V/macSteam.app runtime/release/$V/macSteam.zip
   xcrun notarytool submit runtime/release/$V/macSteam.zip --keychain-profile macsteam --wait
   xcrun stapler staple runtime/release/$V/macSteam.app
   spctl -a -vv runtime/release/$V/macSteam.app      # must say: Notarized Developer ID
   ```

   If Apple rejects it, `xcrun notarytool log <submission id> --keychain-profile macsteam` says
   which file. Fix the cause; never ship around a failed check.
5. **The DMG** (signed, notarized, stapled; writes the `.sha256` beside it):

   ```sh
   python3 scripts/build_release_dmg.py --app runtime/release/$V/macSteam.app \
     --identity "$ID" --output runtime/release/macSteam-$V.dmg
   ```

   The file name must be `macSteam-<version>.dmg`: the updater looks for exactly that.
6. **Test the DMG on a clean setup:** install from the DMG, add SteamDriver, add the Game
   Porting Toolkit, and run the games in [RELEASE-TEST-GAMES.md](RELEASE-TEST-GAMES.md). Write
   down each result and the known issues for the release notes.
7. **Publish** on GitHub: a new release with tag `v1.0`, not marked as a pre-release (the updater
   ignores pre-releases and drafts). Attach `macSteam-1.0.dmg` (GitHub records its SHA-256; the
   `.sha256` file is a fallback the updater also reads). The notes need:
   - a **What's new** heading with bullets: the updater shows those;
   - the hidden line `<!-- minimum-macos: 26.5 -->`, or the updater assumes macOS 26;
   - what users need besides the DMG: Steam, NotProton and Apple's Game Porting Toolkit (the
     README's Installing section);
   - the release test results and known issues.
8. Keep the signed app and DMG (`runtime/release/`): the next release's update is checked against
   this one's team.

## What ships, and what doesn't

- The DMG holds only macSteam.app, which carries the SteamDriver tool's runtime and the main
  runtime. It holds **no** Apple D3DMetal: users add their own Game Porting Toolkit.
- Licences ship in the app (`Contents/Resources/Licenses`, `NOTICE.txt`); see
  [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md). Before publishing binaries, check that
  the runtime's dependency sources and notices are complete (`BUILDING.md`).
- NotProton, Steam and the Game Porting Toolkit are the user's own downloads.

MacVolvo's own release history (1.0, 1.1, signed with its team) is in [SIGNING.md](SIGNING.md).
