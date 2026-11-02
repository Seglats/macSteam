#!/bin/zsh
# Build the MacFinals d3d12 layer: build.sh OUTPUT_DIR [WINEBUILD]
# Produces OUTPUT_DIR/d3d12.dll (marked as a Wine builtin) with MinGW-w64 GCC, the
# toolchain the rest of the runtime's Windows side is built with, and
# OUTPUT_DIR/d3d12_gptk.dll, the placeholder that reserves the d3d12_gptk name in
# the prefix (the real GPTK d3d12 lives outside the arch dir; see
# scripts/assemble_portable_runtime.py and tests/rosetta_nt/R15_SKIN_CURVE.md).
emulate -LR zsh
setopt ERR_EXIT NO_UNSET PIPE_FAIL
here=${0:A:h}
out=${1:?output directory}
winebuild=${2:-$HOME/MacFinals-build-gcc/phase-2/wine-build/tools/winebuild/winebuild}
cc=/opt/homebrew/opt/mingw-w64/bin/x86_64-w64-mingw32-gcc
mkdir -p "$out"
"$cc" -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -shared -static-libgcc \
    -o "$out/d3d12.dll" "$here/layer.c" "$here/dxil_rewrite.c" "$here/d3d12.def"
# libgcc's prebuilt objects embed the toolchain's Homebrew paths; drop them so no
# build-machine layout ships in the runtime.
/opt/homebrew/opt/mingw-w64/bin/x86_64-w64-mingw32-strip --strip-debug "$out/d3d12.dll"
"$winebuild" --builtin "$out/d3d12.dll"
printf 'LIBRARY d3d12_gptk.dll\n' > "$out/d3d12_gptk.spec"
"$winebuild" --fake-module --dll -o "$out/d3d12_gptk.dll" "$out/d3d12_gptk.spec"
rm "$out/d3d12_gptk.spec"
print -r -- "$out/d3d12.dll" "$out/d3d12_gptk.dll"
