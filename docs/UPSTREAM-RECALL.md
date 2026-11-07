# Syncing with Recall and rebuilding the runtime

How MacVolvo relates to [Recall](https://github.com/AsherJN/recall), how to bring Recall's
fixes over, and how the runtime and app were rebuilt for `macvolvo-1.2-dev.1`
(branch `port/recall-mouse-fixes`, November 2026). Read this before the next sync.

## Lineage

- **Source:** MacVolvo forked from Recall at `61a3a59` ("Docs: MetalFX upscaling page",
  just after Recall 1.3.0). Recall's history since then is one large commit per release
  ("Recall 1.6.0", …), so you can't cherry-pick individual fixes from it.
- **Runtime:** MacVolvo has never built Wine from scratch.
  - `macvolvo-1.0-signed.1` (app 1.0–1.1) is Recall's `phase2-20261014.1`, re-signed by
    `scripts/sign_runtime.py`. MacVolvo's own patch edits in 1.1 were never compiled into it.
  - `macvolvo-1.2-dev.1` is Recall 1.6.2's release runtime `phase2-20261028.2` with only
    `lib/wine/x86_64-unix/winemac.so` replaced. See [BUILDING.md](BUILDING.md).
- **Upshot:** Each Recall release runtime already contains built versions of Recall's patches.
  Usually only the files MacVolvo changes itself (so far just `winemac.so`) need building.

## Don't `git merge` Recall

A full merge of Recall 1.6.2 gave about 130 conflict hunks in 25 files. Recall's side brings
things MacVolvo doesn't want:
- translations (`L()` on every string)
- Overwatch, Battle.net, Korea and China flows
- Mosaic and Ko-fi promo UI
- its own `release.json`

Port deliberately instead:

1. List what Recall changed since the last sync:
   `git diff --stat <last-synced recall commit> <recall head> -- patches scripts app`.
2. Take files MacVolvo never touched wholesale (`git checkout <recall-ref> -- <file>`).
   Check first with `git diff --stat 61a3a59 main -- <file>`. In this sync that was
   `build_wine_native.py`, `derive_runtime.py`, `mouselook_session.py`, the HID probe,
   `patches/README.md`, and the ntdll, win32u, audio and services patches.
3. For files both sides changed, port the specific feature by hand.
4. Update every patch hash: the `V1_PATCHES` table in `scripts/build_portable_runtime.py`
   and both lists in `docs/SOURCES.json`.

Add Recall as a temporary remote from the local checkout: `git remote add recall ../recall`.
Afterwards, remove the remote and delete Recall's tags that the fetch pulled in, so they
never get pushed with `--tags`.

## Merging Mac driver patches: merge the code, not the patch text

`wine-winemac-activation.patch` is several thousand lines long, and both projects edit it.
Merging patch-of-patch text is unreliable. Instead:

1. Export the pristine driver sources from Recall's dev tree into a scratch git repo:
   `git -C ../recall/runtime/wine-dev/patchbase archive HEAD dlls/winemac.drv`.
   The `HEAD` ("base") commit is CrossOver 26.3 plus `wine-v1-canvas` plus `wine-mouselook`.
2. Apply three versions of the patch on three branches: fork point, MacVolvo, Recall.
3. Re-parent the MacVolvo and Recall branches onto the fork-point branch with
   `git commit-tree <branch>^{tree} -p base`, then merge them normally.
4. Regenerate the patch: `git diff --no-color <pristine> <merged>`.
5. Sanity check: regenerating Recall's own branch must reproduce Recall's patch exactly.
   It did, so the method is reliable.

Last time MacVolvo's only real driver change was 6 lines (the resize-start query uses
`WineQueryNoPreemptWait`), and the merge was clean.

**Always test the whole stack from pristine CrossOver**, in build order:
`wine-v1-canvas` → `wine-mouselook` → `wine-winemac-activation`.
- Pristine CrossOver is `git -C …/patchbase archive c65067d dlls/winemac.drv`.
- MacVolvo's edit to `wine-mouselook.patch` (real clock instead of `macdrv_stat_clock()`)
  stopped Recall's activation patch from applying. It went back to Recall's version, because
  Recall's driver fixes the same thing with `raw_clock()`.
- Patching only the top layer would have hidden the problem.

## Recall's mouse fixes need launcher plumbing

The new driver behaves as before unless the launcher sets environment variables:

| Setting (Settings › General › Mouse) | Environment |
|---|---|
| Standard (default) | `WINEMAC_INPUT_VARIANT=precise` (in `docs/candidate-parity-contract.json`; regenerate the header with `python3 scripts/candidate_contract.py`) |
| Classic | `WINEMAC_INPUT_VARIANT=classic` |
| High-precision | `WINEMAC_RAW_SOURCE=hid`; needs the app's Input Monitoring permission (`app/Sources/InputMonitoring.swift`) |
| Mouse updates slider | `WINEMAC_MOUSELOOK_RAW_INTERVAL_US` (0 = every report); not with Classic |

The app passes `--mouse` and `--mouse-rate` to `launch`. In `portable_setup.m`, the
`clientEnvironment()` function sets the variables. `sessionMouse()` makes a changed setting
reopen an idle Steam, the same way the HUD and Game Mode settings work. Steam passes its
environment on to the games, so a setting only takes effect after Steam reopens.

## Building `winemac.so`

The full runtime pipeline (`runtime/phase-2`, `runtime/source/wine-*`) isn't on this machine.
Recall's dev tree is: `../recall/runtime/wine-dev`.
- `sources/wine`: patched source, the same as `patchbase`'s working tree
- `build-x64`: configured out-of-tree build, with `srcdir = ../sources/wine` (relative)
- `winemac.sh`: the author's dev loop

Never build inside that tree: it holds the Recall author's work in progress. Clone it (APFS
clones are instant, and `-p` keeps timestamps so only changed files rebuild):

```sh
W=<scratch>/wd; D=../recall/runtime/wine-dev; T=../recall/runtime/toolchains
mkdir -p $W/sources && cp -Rpc $D/sources/wine $W/sources/ && cp -Rpc $D/build-x64 $W/
# copy the merged dlls/winemac.drv files over $W/sources/wine/dlls/winemac.drv
env DEVELOPER_DIR=/Library/Developer/CommandLineTools \
    SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk MACOSX_DEPLOYMENT_TARGET=26.0 \
    PATH=$T/llvm-mingw-20251216-ucrt-macos-universal/bin:/opt/homebrew/opt/bison/bin:/usr/bin:/bin:/usr/sbin:/sbin \
    make -C $W/build-x64 -j8 -o dlls/ntdll/ntdll.so -o dlls/win32u/win32u.so dlls/winemac.drv/winemac.so \
    'LDFLAGS=-Wl,-headerpad,0x1000'
```

Package the result like `build_wine_native.py`:
1. `strip -S -x`
2. delete every `LC_RPATH`
3. add the rpaths `@loader_path` and `@loader_path/../..`
4. `codesign --force --sign -`

Then compare it with the official driver:
- `otool -L`: same linked libraries
- `nm -gU`: same exports
- `strings | grep /Users/`: must find nothing

An ad-hoc `winemac.so` loads fine inside the Developer ID–signed runtime; the Recall dev loop
relies on this too.

## Deriving the runtime

```sh
A=runtime/phase-2/artifacts            # gitignored
mkdir -p $A/phase2-20261028.2
cp -c ../recall/runtime/runtime.tar.gz $A/phase2-20261028.2.tar.gz   # check its SHA-256 against Recall's release.json
tar -xzf $A/phase2-20261028.2.tar.gz -C $A/phase2-20261028.2
```

- **Proof file:** `replace_files` needs `<base>.tar.json`. Recall doesn't publish one, so
  write it from facts measured on the archive (version, `archive_sha256`, `archive_bytes`)
  and say it was reconstructed.
- **Skip `derive_runtime.py --native`:** it walks every ancestor's `runtime.json`, and those
  aren't available. Call `derive_runtime.replace_files(base, dest, base_version, version, '-',
  manifest, {rel: built}, replaced)` directly. It still:
  - clones the base
  - checks rpaths and dependencies
  - re-stamps the file to the runtime's minimum macOS (15.0)
  - signs with the hardened runtime
  - refuses if anything other than the replaced file and `runtime.json` changed
- **Run with `python3 -I`** from a script outside the repo, putting `scripts/` on `sys.path`.

Then update `runtimeVersion` and `runtimeSHA256` in `app/Resources/release.json`.

## Building the app

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  python3 scripts/build_native_app.py --output runtime/build/MacVolvo.app \
  --runtime-archive runtime/phase-2/artifacts/<version>.tar.gz --identity -
```

- **Use Xcode's toolchain.** The Command Line Tools' `swiftc` fails on SwiftUI's `@State`
  ("plugin for module 'SwiftUIMacros' not found"). The same applies to a quick
  `swiftc -typecheck -parse-as-library app/Sources/*.swift`.
- **`llvm-mingw-20261006` is pinned** (it builds `steamwebhelper.exe`). Download
  `llvm-mingw-20261006-ucrt-macos-universal.tar.xz` from mstorsjo/llvm-mingw into
  `runtime/downloads/` and check it against the release's published digest
  (`sha256:a7a19b11…6ebf6`). Extract it into `runtime/toolchains/`.
- **libarchive headers** (`runtime/toolchains/libarchive-3.7.7`) are needed by the worker,
  which is compiled with `-Werror`. Build the worker alone with
  `python3 scripts/build_portable_setup.py --output <file> --headers <libarchive dir>`.
- **The build refuses** a runtime archive whose hash or version doesn't match `release.json`.

## Smoke test without Steam or a real install

```sh
W=runtime/build/MacVolvo.app/Contents/Helpers/ow2-setup; R=<scratch>/mvroot
$W install-runtime --root $R --archive <app>/Contents/Resources/runtime.tar.gz --sha256 <sha> --version <version>
$W verify --root $R      # runtime_verified
$W prepare --root $R     # creates the Wine prefix; ends with environment_ready
```

Then check `$R/logs` for errors and `pgrep -fl $R` for Wine processes left running.

## Tests

- **Main suite:** `python3 tests/run_portable.py` (141 tests at this sync). The mouse tests
  (`test_mouselook_session`, `test_mouse_timing_check`, `test_rawinput_trace`) aren't in its
  list; run them with `python3 -m unittest tests.<name>`.
- **Code-extracting tests:** `tests/test_steam_window_fix.py` compiles a slice of `launchClient`
  copied out of `portable_setup.m`. When that slice calls something new, add a stub to the
  fixture (as was done for `sessionMouse()`).
- **Failing before this sync:** `test_mouse_rate_check` and `test_candidate_parity` also fail
  on `main`.
- **Removed tests:** Recall's `ShaderPack` tests in `test_derive_runtime.py` were dropped
  because the DXMT shader pack wasn't ported.

## Signing and release

The only local signing identity is a self-signed "MacFinals Signing" certificate.
`macvolvo-1.2-dev.1` and the app built from it are ad-hoc signed and only suitable for local
testing. Before a release:
1. Re-sign the runtime with a Developer ID using `scripts/sign_runtime.py`
   ([SIGNING.md](SIGNING.md)).
2. Update the runtime hash in `release.json`.
3. Bump `appVersion` and `WhatsNew`.
4. Notarize.

## Shell gotchas (zsh)

- `"$R:scripts/…"` doesn't do what it looks like: zsh reads `:s` as a modifier. Write
  `"${R}:scripts/…"`.
- An unquoted `echo =====` fails: zsh treats a leading `=` as command lookup. Quote it.
- `git stash` around test runs leaves files that were staged by `git checkout <ref> -- file`
  unstaged afterwards.

## Left for a future sync

- Recall's DXMT shader pack (`dxmt-shader-pack.patch`, pipeline preparation changes). The
  winemetal build is already in the 1.6.2 runtime; the app side isn't ported.
- Recall's logging and support bundle (`LogBundle`, `portable_logs.h`).
- The home-screen High-precision invite.
