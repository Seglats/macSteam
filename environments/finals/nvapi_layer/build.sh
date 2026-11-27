#!/bin/zsh
# Build the SteamDriver nvapi64 layer: build.sh OUTPUT_DIR [WINEBUILD]
# Produces OUTPUT_DIR/nvapi64.dll (marked as a Wine builtin) with MinGW-w64 GCC, as the d3d12
# layer is built, and OUTPUT_DIR/nvapi64_gptk.dll, the placeholder that reserves the
# nvapi64_gptk name in the prefix (Apple's nvapi64.dll is linked at lib/wine/nvapi64_gptk.dll,
# outside the arch dir; scripts/add_nvapi_layer.py).
emulate -LR zsh
setopt ERR_EXIT NO_UNSET PIPE_FAIL
here=${0:A:h}
out=${1:?output directory}
winebuild=${2:-$HOME/SteamDriver-build/finals/phase-2/wine-build/tools/winebuild/winebuild}
cc=/opt/homebrew/opt/mingw-w64/bin/x86_64-w64-mingw32-gcc
mkdir -p "$out"
"$cc" -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -shared -static-libgcc \
    -o "$out/nvapi64.dll" "$here/layer.c" "$here/nvapi64.def" -ladvapi32
# Only DLLs every Wine prefix has (a MinGW runtime DLL would make it fail to load silently).
objdump=/opt/homebrew/opt/mingw-w64/bin/x86_64-w64-mingw32-objdump
unexpected=$("$objdump" -p "$out/nvapi64.dll" | sed -n 's/^[[:space:]]*DLL Name: //p' |
             grep -v -i -E '^(kernel32\.dll|advapi32\.dll|ntdll\.dll|msvcrt\.dll|api-ms-win-crt-[a-z0-9-]+\.dll)$' || true)
if [[ -n $unexpected ]]; then
    print -u2 "nvapi64.dll imports DLLs a prefix does not have: ${(f)unexpected}"
    exit 1
fi
/opt/homebrew/opt/mingw-w64/bin/x86_64-w64-mingw32-strip --strip-debug "$out/nvapi64.dll"
"$winebuild" --builtin "$out/nvapi64.dll"
printf 'LIBRARY nvapi64_gptk.dll\n' > "$out/nvapi64_gptk.spec"
"$winebuild" --fake-module --dll -o "$out/nvapi64_gptk.dll" "$out/nvapi64_gptk.spec"
rm "$out/nvapi64_gptk.spec"
print -r -- "$out/nvapi64.dll" "$out/nvapi64_gptk.dll"
