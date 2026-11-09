#!/bin/zsh
# Build the in-process Rosetta hook: build_hook.sh [OUTPUT.dylib]
#
# payload_arm64.c -> payload.dylib -> payload_blob.h (mkpayload.py gates it) ->
# hooklib.c x86_64 + arm64 slices -> universal dylib, ad-hoc signed. Stops at the first
# failure: a failed payload step would otherwise leave the previous payload_blob.h in
# place and hooklib would silently embed it (NOTES.md).
#
# DEBUG_CFLAGS (environment) goes to the hooklib compile: "-DWINE_NO_TRACE_MSGS
# -DWINE_NO_DEBUG_MSGS" compiles the macfinals_hook/macfinals_scan trace and warn output
# out, as for a release Wine build. err output always stays. Without it, output is
# chosen at run time with WINEDEBUG (see the header of hooklib.c).
#
# Ship the result with: python3 scripts/add_nt_hook.py ... --hook OUTPUT.dylib
emulate -LR zsh
setopt ERR_EXIT NO_UNSET PIPE_FAIL
here=${0:A:h}
out=${1:-$here/hooklib.dylib}
out=${out:A}
debug_cflags=(${=DEBUG_CFLAGS:-})
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$here"
clang -arch arm64 -O2 -mgeneral-regs-only -dynamiclib -o payload.dylib payload_arm64.c
python3 mkpayload.py payload.dylib payload_blob.h
# minos 26.0: add_nt_hook.py rejects slices built for an older macOS
clang -arch x86_64 -O2 $debug_cflags -mmacosx-version-min=26.0 -dynamiclib \
    -o "$tmp/hooklib.x86_64.dylib" hooklib.c
clang -arch arm64 -O2 -mgeneral-regs-only $debug_cflags -mmacosx-version-min=26.0 -dynamiclib \
    -o "$tmp/hooklib.arm64.dylib" hooklib.c
lipo -create "$tmp/hooklib.x86_64.dylib" "$tmp/hooklib.arm64.dylib" -o "$out"
codesign --force --sign - "$out"
print -r -- "$out (debug messages: ${DEBUG_CFLAGS:-run-time WINEDEBUG})"
