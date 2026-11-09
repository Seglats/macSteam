#!/bin/zsh
# Build the MacFinals d3d12 layer: build.sh OUTPUT_DIR [WINEBUILD]
# Produces OUTPUT_DIR/d3d12.dll (marked as a Wine builtin) with MinGW-w64 GCC, the
# toolchain the rest of the runtime's Windows side is built with, and
# OUTPUT_DIR/d3d12_gptk.dll, the placeholder that reserves the d3d12_gptk name in
# the prefix (the real GPTK d3d12 lives outside the arch dir; see
# scripts/assemble_portable_runtime.py and tests/rosetta_nt/R15_SKIN_CURVE.md).
#
# DEBUG_CFLAGS (environment) is passed to the compiler: "-DWINE_NO_TRACE_MSGS
# -DWINE_NO_DEBUG_MSGS" compiles the layer's trace/warn/fixme output out, as for a
# release Wine build (assemble_portable_runtime.py --no-debug-msgs sets it).
emulate -LR zsh
setopt ERR_EXIT NO_UNSET PIPE_FAIL
here=${0:A:h}
out=${1:?output directory}
winebuild=${2:-$HOME/MacFinals-build-gcc/phase-2/wine-build/tools/winebuild/winebuild}
cc=/opt/homebrew/opt/mingw-w64/bin/x86_64-w64-mingw32-gcc
debug_cflags=(${=DEBUG_CFLAGS:-})
mkdir -p "$out"
"$cc" -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type $debug_cflags -shared -static-libgcc \
    -o "$out/d3d12.dll" "$here/layer.c" "$here/dxil_rewrite.c" "$here/d3d12.def"
# The layer may import only DLLs every Wine prefix has. A MinGW runtime DLL slipping in
# (e.g. libwinpthread-1.dll, which __thread pulls in through emulated TLS) makes
# d3d12.dll fail to load in every game, silently from the game's side (2026-11-09).
objdump=/opt/homebrew/opt/mingw-w64/bin/x86_64-w64-mingw32-objdump
unexpected=$("$objdump" -p "$out/d3d12.dll" | sed -n 's/^[[:space:]]*DLL Name: //p' |
             grep -v -i -E '^(kernel32\.dll|ntdll\.dll|api-ms-win-crt-[a-z0-9-]+\.dll)$' || true)
if [[ -n $unexpected ]]; then
    print -u2 "d3d12.dll imports DLLs a prefix does not have: ${(f)unexpected}"
    exit 1
fi
# libgcc's prebuilt objects embed the toolchain's Homebrew paths; drop them so no
# build-machine layout ships in the runtime.
/opt/homebrew/opt/mingw-w64/bin/x86_64-w64-mingw32-strip --strip-debug "$out/d3d12.dll"
"$winebuild" --builtin "$out/d3d12.dll"
printf 'LIBRARY d3d12_gptk.dll\n' > "$out/d3d12_gptk.spec"
"$winebuild" --fake-module --dll -o "$out/d3d12_gptk.dll" "$out/d3d12_gptk.spec"
rm "$out/d3d12_gptk.spec"
print -r -- "$out/d3d12.dll" "$out/d3d12_gptk.dll"
