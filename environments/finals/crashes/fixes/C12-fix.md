# C12 fix: build Wine's Windows-side modules with MinGW-w64 GCC

Fixed in **`macfinals-19`** / `MacFinals-14.app`. Source: `tests/rosetta_nt/(removed)`.

## Cause

Wine's PE modules were built with llvm-mingw (clang + lld). CrossOver builds them with MinGW-w64
GCC and GNU binutils, whose PE layout differs (`.edata`, `.xdata`, `.idata`, `.bss`). With the
llvm-mingw layout the game's checks failed with E111000D.

## Fix

`scripts/build_portable_runtime.py` builds the PE side with `x86_64-w64-mingw32-gcc` /
`i686-w64-mingw32-gcc` and refuses to run without MinGW-w64 GCC. **Never go back to llvm-mingw
for PE modules.**

## Verified

On `-19` the real `Discovery.exe` gets past its start-up checks, loads the anti-cheat runtime,
D3D12 and EOS.
