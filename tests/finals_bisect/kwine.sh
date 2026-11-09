#!/bin/zsh
# Stop every Wine process on this Mac: Steam, the game, wineserver, leftovers of crashed runs.
# Any Wine, not only MacFinals': meant for development, when something has gone wrong.
# Wine renames its processes after the Windows program (e.g. ".../drive_c/.../steam.exe" or
# "C:\...\steamwebhelper.exe"), so they are matched by that as well as by the Wine binaries.
#
#   kwine.sh        ask each wineserver to end its session, then force-kill what is left
#   kwine.sh -n     only list what would be stopped
emulate -LR zsh

list() {
  /bin/ps -axo pid=,comm= | /usr/bin/awk -v self=$$ '
    $1 == self { next }
    { pid = $1; $1 = ""; c = tolower(substr($0, 2)) }
    c ~ /(^|\/)wine(server|64|-preloader|64-preloader)?$/ || c ~ /\/drive_c\// ||
    c ~ /^[a-z]:\\/ || c ~ /\.exe$/ || c ~ /wine-preloader|wineserver/ { print pid }'
}
show() { local p=($(list)); (( ${#p} )) && /bin/ps -o pid=,comm= -p ${(j:,:)p} | /usr/bin/cut -c1-160 }

if [[ ${1:-} == -n ]]; then show || print 'nothing running'; exit 0; fi
(( ${#$(list)} )) || { print 'no Wine processes'; exit 0 }

# Graceful first: each running wineserver's own binary ends its prefix's session (-k).
for pid in $(/usr/bin/pgrep -x wineserver); do
  ws=$(/bin/ps -o comm= -p $pid)
  [[ -x $ws ]] && "$ws" -k 2>/dev/null &
done
wait
for i in {1..12}; do (( ${#$(list)} )) || break; /bin/sleep 0.25; done

left=($(list))
if (( ${#left} )); then
  print "force-killing ${#left} process(es):"; show
  /bin/kill -9 $left 2>/dev/null
  /bin/sleep 0.5
fi
left=($(list))
if (( ${#left} )); then print -u2 "still running:"; show >&2; exit 1; fi
print 'all Wine processes stopped'
