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
#   launch_main.sh mouselook             + WINEMAC_MOUSELOOK (precise timing, SteamDriver's driver)
#   launch_main.sh hid                   + High-precision mouse (implies mouselook)
#   launch_main.sh classic               + the older mouse timing (implies mouselook)
#   launch_main.sh msync avx             + WINEMSYNC=1, ROSETTA_ADVERTISE_AVX=1
#   launch_main.sh app                   everything the app sets (should reproduce E111000D)
#   RUNTIME=macfinals-17 HOOK=0 DEBUG=quiet|full|max|macfinals launch_main.sh ...
#   WINEDEBUG='-all,warn+macfinals_dx12' launch_main.sh ...   any WINEDEBUG, verbatim
#   APP=774801 launch_main.sh ...       Crab Champions (the canary) instead of THE FINALS
#   APP=steam launch_main.sh ...        Steam only, no game
#
# DEBUG=max logs as much as is safe and, after the session, collects Steam's logs and dumps,
# macOS crash reports, the D3DMetal cache listing and the macOS log next to the Wine log.
# MacFinals' own code logs through Wine's debug channels (WINEDEBUG), like Wine itself:
#   macfinals_hook  the in-process Rosetta hook (NT flag, syscall fast path)
#   macfinals_dx12  the d3d12 layer (skin-fix shader rewrite, pipeline-creation timing)
#   macfinals_scan  the hook's heavy arena scan at process exit (trace class only)
# Default err+fixme, like any channel. DEBUG=macfinals shows only those (-all first), with
# Wine's pid/tid/timestamp prefixes; full and max include them; WINEDEBUG overrides DEBUG.
# PERF=1 adds per-process performance counters (10 a second) in that folder's perf/.
# For FPS measurements pass avx: the app advertises AVX by default, this script does not.
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
# The environment is cleared below, so pass the d3d12 layer's switch through explicitly.
for v in MACFINALS_SKIN_FIX; do
  [[ -z ${(P)v:-} ]] || env+=("$v=${(P)v}")
done
(( ! ${+on[msync]} ))     || env+=(WINEMSYNC=1)
(( ! ${+on[writecopy]} )) || env+=(WINE_SIMULATE_WRITECOPY=1)
(( ! ${+on[gamemode]} ))  || env+=(WINE_GAME_MODE=Discovery.exe)
# The Mac driver's mouse, as SteamDriver's NotProton rows set it (docs/NOTPROTON.md at the
# repository root): mouselook = "MacVolvo mouse" (precise timing), hid = High-precision
# (reads the mouse directly; macOS asks for Input Monitoring for the app that starts this
# script), classic = the older timing. hid-status.json in the session log folder says
# whether the High-precision mouse worked.
(( ! ${+on[hid]} && ! ${+on[classic]} )) || on[mouselook]=1
if (( ${+on[mouselook]} )); then
  env+=(WINEMAC_MOUSELOOK=Discovery.exe WINEMAC_INPUT_VARIANT=precise)
  (( ! ${+on[classic]} )) || env+=(WINEMAC_INPUT_VARIANT=classic)
  (( ! ${+on[hid]} ))     || env+=(WINEMAC_RAW_SOURCE=hid WINEMAC_RAW_REQUEST=1)
fi
if [[ ${HOOK:-1} != 0 && -f $engine/lib/macfinals-nt-hook.dylib ]]; then
  env+=(DYLD_INSERT_LIBRARIES="$engine/lib/macfinals-nt-hook.dylib" DISABLE_AOT=1)
fi
ours=+macfinals_hook,+macfinals_dx12
if [[ -n ${WINEDEBUG:-} ]]; then env+=(WINEDEBUG="$WINEDEBUG")
else case ${DEBUG:-light} in
  # MoltenVK (Steam's Vulkan) logs on its own, outside WINEDEBUG: errors only in these two
  quiet) env+=(WINEDEBUG=-all MVK_CONFIG_LOG_LEVEL=1) ;;
  macfinals) env+=(WINEDEBUG=-all,+pid,+tid,+timestamp,$ours MVK_CONFIG_LOG_LEVEL=1) ;;
  full)  env+=(WINEDEBUG=+pid,+tid,+timestamp,err+all,fixme+all,+seh,+loaddll,+process,warn+module,$ours) ;;
  # Everything short of per-call tracing: every err/warn/fixme, thread names (warn+threadname),
  # DLL loads and process events. +seh is left out on purpose: it logs every exception, and
  # in a 2026-11-01 run it changed the game's behaviour.
  # warn-heap is required: with heap warnings on, Wine gives every heap
  # HEAP_VALIDATE_PARAMETERS_ENABLED and non-zero ForceFlags (dlls/ntdll/heap.c,
  # heap_set_debug_flags), which is what a process started under a debugger looks like.
  # The anti-cheat then stops the game with "disallowed tool" (2026-11-02). Never enable
  # warn+heap, trace+heap, relay or snoop for the game: they change what it can observe.
  max)   env+=(WINEDEBUG=+pid,+tid,+timestamp,err+all,warn+all,warn-heap,fixme+all,+loaddll,+process,$ours) ;;
  *)     env+=(WINEDEBUG=+pid,+tid,+timestamp,err+all,+loaddll,+process) ;;
esac; fi

/bin/mkdir -p "$here/logs"
tag=${(j:-:)${(ok)on}}; [[ -n $tag ]] || tag=baseline
log="$here/logs/main-$(/bin/date +%Y%m%d-%H%M%S)-$tag.log"
{ print -r -- "runtime ${engine:t}; on: ${(k)on:-none}"; print -rl -- $env; print } > "$log"
print -r -- "log: $log"
[[ -z ${DRYRUN:-} ]] || { /bin/cat "$log"; /bin/rm -f "$log"; exit 0; }
steam="$root/environment/drive_c/Program Files (x86)/Steam/steam.exe"
steamdir="$root/environment/drive_c/Program Files (x86)/Steam"
# Steam's window is black unless its browser runs through the app's stand-in steamwebhelper.exe
# (--in-process-gpu; scripts/portable_steam.h steamWindowFix). A Steam update puts its own helper
# back; the app repairs that on every start, so do the same here, with the newest app's stand-in.
/usr/bin/python3 -I - "$steamdir/bin/cef/cef.win64" "${here:h:h:h:h}/runtime/build" <<'EOF' || exit 1
import hashlib, os, re, shutil, sys
cef, build = sys.argv[1:]
mark = 'steamwebhelper_real.exe'.encode('utf-16-le')
def stand_in(p): return os.path.isfile(p) and mark in open(p, 'rb').read()
apps = [d for d in os.listdir(build) if re.fullmatch(r'MacFinals-\d+\.app', d)] if os.path.isdir(build) else []
src = None
for d in sorted(apps, key=lambda d: -int(re.findall(r'\d+', d)[0])):
    p = os.path.join(build, d, 'Contents/Resources/steamwebhelper.exe')
    if stand_in(p): src = p; break
helper, real = os.path.join(cef, 'steamwebhelper.exe'), os.path.join(cef, 'steamwebhelper_real.exe')
if not os.path.isfile(helper): sys.exit(0)            # Steam still bootstrapping
if not src: print('no stand-in steamwebhelper.exe in runtime/build; Steam may draw black', file=sys.stderr); sys.exit(0)
h = lambda p: hashlib.sha256(open(p, 'rb').read()).hexdigest()
if not stand_in(helper):                               # Steam's own helper: it becomes the real one
    if os.path.exists(real): os.remove(real)
    os.replace(helper, real); shutil.copy2(src, helper)
    print('steam window fix: stand-in helper put back (Steam had replaced it)')
elif not os.path.isfile(real) or stand_in(real):
    print('steam window fix: real helper missing; start Steam once from the app to repair', file=sys.stderr)
elif h(helper) != h(src):
    shutil.copy2(src, helper); print('steam window fix: stand-in helper updated')
EOF
started=$(/bin/date '+%Y-%m-%d %H:%M:%S')
marker="$here/logs/.start-$$"; : > "$marker"
out=${log%.log}; /bin/mkdir -p "$out"
(( ! ${+on[mouselook]} )) || env+=(WINEMAC_RAW_STATUS="$out/hid-status.json")
# PERF=1: every Wine process writes 10 lines a second to $out/perf/wine.<pid> (our ntdll's
# WINE_SYSCALL_LOG): syscall counts and time, Mach/BSD syscalls, page faults, context
# switches, exception and fault counters, the busiest threads' CPU. Output only.
if [[ -n ${PERF:-} ]]; then
  /bin/mkdir -p "$out/perf"; env+=(WINE_SYSCALL_LOG="$out/perf/wine")
  { print -r -- "runtime ${engine:t}; on: ${(k)on:-none}"; print -rl -- $env; print } > "$log"
fi
launch=(-applaunch ${APP:-2073850}); [[ ${APP:-} != steam ]] || launch=()
/usr/bin/env -i $env "$engine/bin/wine" "$steam" -no-cef-sandbox -noverifyfiles \
  $launch >> "$log" 2>&1
status=$?

# Collect what other components wrote during the session next to the Wine log, in
# ${log%.log}/. Only files written since the session started; nothing is deleted.
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
