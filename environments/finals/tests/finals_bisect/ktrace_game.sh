#!/bin/zsh
# Record a few seconds of kernel events for THE FINALS' process while it is in play: Mach
# events (exceptions/faults, Mach traps, IPC, VM, scheduling; class 1) and BSD syscalls
# (subclass 0x040c). Kernel-side tracing only: nothing is attached to or injected into the
# game, and the game's code is not read. The file is analysed for counts (which exception and
# trap types, which threads), nothing else.
#
#   sudo tests/finals_bisect/ktrace_game.sh [seconds]     (default 5)
#
# Writes tests/finals_bisect/logs/ktrace-<time>.ktrace, owned by the invoking user.
emulate -LR zsh
(( EUID == 0 )) || { print -u2 'run with sudo (ktrace needs root)'; exit 1 }
here=${0:A:h}
secs=${1:-5}
# Wine names the game process after the Windows program; pick the biggest such process.
pid=$(/bin/ps -axo pid=,rss=,comm= | /usr/bin/awk 'tolower($0) ~ /binaries.win64.discovery\.exe$/ { if ($2 > m) { m = $2; p = $1 } } END { print p }')
[[ -n $pid ]] || { print -u2 'THE FINALS (Discovery.exe) is not running'; exit 1 }
out="$here/logs/ktrace-$(/bin/date +%Y%m%d-%H%M%S).ktrace"
/bin/mkdir -p "$here/logs"
print "tracing pid $pid for ${secs}s -> $out"
/usr/bin/ktrace dump -T $secs -b 2048 -f C1,S0x040c -p $pid "$out" || exit 1
[[ -n ${SUDO_UID:-} ]] && /usr/sbin/chown $SUDO_UID:${SUDO_GID:-staff} "$out"
/bin/ls -la "$out"
