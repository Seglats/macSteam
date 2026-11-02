#!/bin/zsh
# Bisect why THE FINALS passes its startup checks on a patched CrossOver Wine (finalsTesting
# cx route, 2026-11-02) but stops with E111000D on MacFinals.
#
# Runs a MacFinals runtime directly, without the app, against the real MacFinals environment
# and Steam, starting from the settings the baseline run had. Each MacFinals-only
# setting can be switched back on by name:
#
#   launch_main.sh                       CrossOver-like baseline
#   launch_main.sh writecopy             + WINE_SIMULATE_WRITECOPY=1 (CW Hack 22996)
#   launch_main.sh gamemode              + WINE_GAME_MODE (Discovery.exe via game-mode/Discovery.app)
#   launch_main.sh mouselook             + WINEMAC_MOUSELOOK
#   launch_main.sh msync avx             + WINEMSYNC=1, ROSETTA_ADVERTISE_AVX=1
#   launch_main.sh app                   everything the app sets (should reproduce E111000D)
#   RUNTIME=macfinals-17 HOOK=0 DEBUG=quiet|full|max launch_main.sh ...
#
# DEBUG=max logs as much as is safe and, after the session, collects Steam's logs and dumps,
# macOS crash reports, the D3DMetal cache listing and the macOS log next to the Wine log.
#
# Quit the MacFinals app, Steam and the game first. Logs go to tests/finals_bisect/logs/.
emulate -LR zsh
setopt NO_UNSET PIPE_FAIL

root="$HOME/Library/Application Support/MacFinals"
engine="$root/runtimes/${RUNTIME:-macfinals-17}"
here=${0:A:h}
[[ -x $engine/bin/wine ]] || { print -u2 "No runtime at $engine"; exit 1; }
if /usr/bin/pgrep -f "$root/runtimes/.*/wineserver" >/dev/null; then
  print -u2 'A MacFinals wineserver is running. Quit the app, Steam and the game first.'; exit 1
fi

typeset -A on
for a in "$@"; do on[$a]=1; done
if (( ${+on[app]} )); then on=(writecopy 1 gamemode 1 mouselook 1 msync 1 avx 1); fi

env=(
  PATH=/usr/bin:/bin:/usr/sbin:/sbin HOME="$root/home" USER=player TMPDIR="$root/tmp"
  WINEPREFIX="$root/environment" WINEARCH=win64
  WINESERVER="$engine/bin/wineserver" WINELOADER="$engine/bin/wine"
  CX_APPLEGPTK_LIBD3DSHARED_PATH="$engine/lib/external/libd3dshared.dylib"
  D3DMETAL_FRAMEWORK_PATH="$engine/lib/external/D3DMetal.framework/D3DMetal"
  CX_ACTIVE_GRAPHICS_BACKEND=d3dmetal CX_GRAPHICS_BACKEND=d3dmetal D3DM_ENABLE_METALFX=0
  WINEDLLOVERRIDES='d3d11,dxgi,d3d10,d3d12,d3d12core=b;d3d10core,winemetal,nvapi64,nvngx='
  WINEESYNC=0
  WEBHELPER_EXTRA_ARGS=--use-angle=vulkan
  ROSETTA_ADVERTISE_AVX=$(( ${+on[avx]} ))
)
# The environment is cleared below, so pass the d3d12 layer's switches through explicitly.
for v in MACFINALS_SKIN_FIX MACFINALS_D3D12_LAYER_LOG; do
  [[ -z ${(P)v:-} ]] || env+=("$v=${(P)v}")
done
(( ! ${+on[msync]} ))     || env+=(WINEMSYNC=1)
(( ! ${+on[writecopy]} )) || env+=(WINE_SIMULATE_WRITECOPY=1)
(( ! ${+on[gamemode]} ))  || env+=(WINE_GAME_MODE=Discovery.exe)
(( ! ${+on[mouselook]} )) || env+=(WINEMAC_MOUSELOOK=Discovery.exe)
if [[ ${HOOK:-1} != 0 && -f $engine/lib/macfinals-nt-hook.dylib ]]; then
  env+=(DYLD_INSERT_LIBRARIES="$engine/lib/macfinals-nt-hook.dylib" DISABLE_AOT=1)
fi
case ${DEBUG:-light} in
  quiet) env+=(WINEDEBUG=-all) ;;
  full)  env+=(WINEDEBUG=+pid,+tid,+timestamp,err+all,fixme+all,+seh,+loaddll,+process,warn+module) ;;
  # Everything short of per-call tracing: every err/warn/fixme, thread names (warn+threadname),
  # DLL loads and process events. +seh is left out on purpose: it logs every exception, and
  # in a 2026-11-01 run it changed the game's behaviour.
  # warn-heap is required: with heap warnings on, Wine gives every heap
  # HEAP_VALIDATE_PARAMETERS_ENABLED and non-zero ForceFlags (dlls/ntdll/heap.c,
  # heap_set_debug_flags), which is what a process started under a debugger looks like.
  # The anti-cheat then stops the game with "disallowed tool" (2026-11-02). Never enable
  # warn+heap, trace+heap, relay or snoop for the game: they change what it can observe.
  max)   env+=(WINEDEBUG=+pid,+tid,+timestamp,err+all,warn+all,warn-heap,fixme+all,+loaddll,+process)
         [[ -n ${MACFINALS_D3D12_LAYER_LOG:-} ]] || env+=(MACFINALS_D3D12_LAYER_LOG=1) ;;
  *)     env+=(WINEDEBUG=+pid,+tid,+timestamp,err+all,+loaddll,+process) ;;
esac

/bin/mkdir -p "$here/logs"
tag=${(j:-:)${(ok)on}}; [[ -n $tag ]] || tag=baseline
log="$here/logs/main-$(/bin/date +%Y%m%d-%H%M%S)-$tag.log"
{ print -r -- "runtime ${engine:t}; on: ${(k)on:-none}"; print -rl -- $env; print } > "$log"
print -r -- "log: $log"
[[ -z ${DRYRUN:-} ]] || { /bin/cat "$log"; /bin/rm -f "$log"; exit 0; }
steam="$root/environment/drive_c/Program Files (x86)/Steam/steam.exe"
steamdir="$root/environment/drive_c/Program Files (x86)/Steam"
started=$(/bin/date '+%Y-%m-%d %H:%M:%S')
marker="$here/logs/.start-$$"; : > "$marker"
/usr/bin/env -i $env "$engine/bin/wine" "$steam" -no-cef-sandbox -noverifyfiles \
  -applaunch 2073850 >> "$log" 2>&1
status=$?

# Collect what other components wrote during the session next to the Wine log, in
# ${log%.log}/. Only files written since the session started; nothing is deleted.
out=${log%.log}; /bin/mkdir -p "$out"
print -r -- "wine exited with $status at $(/bin/date '+%Y-%m-%d %H:%M:%S')" > "$out/session.txt"
/usr/bin/find "$steamdir/logs" -type f -newer "$marker" -exec /bin/cp -p {} "$out/" \; 2>/dev/null
/bin/mkdir -p "$out/steam-dumps"
/usr/bin/find "$steamdir/dumps" -type f -newer "$marker" -exec /bin/cp -p {} "$out/steam-dumps/" \; 2>/dev/null
/bin/mkdir -p "$out/macos-crash-reports"
/usr/bin/find "$HOME/Library/Logs/DiagnosticReports" -type f -newer "$marker" \
  \( -iname '*wine*' -o -iname '*Discovery*' -o -iname '*steam*' -o -iname '*rosetta*' -o -iname '*oah*' \) \
  -exec /bin/cp -p {} "$out/macos-crash-reports/" \; 2>/dev/null
cache="$(/usr/bin/getconf DARWIN_USER_CACHE_DIR)d3dm/Discovery.exe/shaders.cache"
/bin/ls -laR "$cache" > "$out/d3dmetal-cache.txt" 2>&1
# macOS's own log for the session: Rosetta (oahd, the runtime), the Wine processes and the
# kernel's messages about them (signals, code signing, memory).
/usr/bin/log show --style compact --start "$started" \
  --predicate 'process == "oahd" OR process CONTAINS "wine" OR process CONTAINS[c] "rosetta" OR senderImagePath CONTAINS[c] "rosetta" OR (process == "kernel" AND (eventMessage CONTAINS[c] "wine" OR eventMessage CONTAINS[c] "Discovery"))' \
  > "$out/macos-log.txt" 2>&1
/bin/rm -f "$marker"
print -r -- "session files: $out"
exit $status
