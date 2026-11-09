# Base environment

SteamDriver's own Wine environment: CrossOver 26.3 Wine with Recall's and MacVolvo's
patches, DXMT for Direct3D 11, and the Mac driver with the mouse work. It is the default for
every game that has no environment of its own, and the source of the Mac driver that the
NotProton companion lays over NotProton's runner.

**Its files have not moved here yet.** They stay where SteamDriver has always kept them,
because the app build and the NotProton driver build use them by those paths:

| What | Where |
|---|---|
| Wine patches | [`patches/`](../../patches/) (order: `scripts/build_portable_runtime.py`) |
| Build scripts | [`scripts/build_portable_dependencies.py`](../../scripts/build_portable_dependencies.py), [`build_portable_runtime.py`](../../scripts/build_portable_runtime.py), [`assemble_portable_runtime.py`](../../scripts/assemble_portable_runtime.py), [`derive_runtime.py`](../../scripts/derive_runtime.py) |
| Mac driver (also for NotProton) | [`scripts/build_mouselook_driver.py`](../../scripts/build_mouselook_driver.py), [`docs/UPSTREAM-RECALL.md`](../../docs/UPSTREAM-RECALL.md) |
| Runtime the app ships | `app/Resources/release.json` (`macvolvo-1.2-dev.N`) |
| How to build | [`docs/BUILDING.md`](../../docs/BUILDING.md) |

Moving them into this folder is a later cleanup, done together with the paths in
`build_native_app.py`, `build_mouselook_driver.py` and the docs.
