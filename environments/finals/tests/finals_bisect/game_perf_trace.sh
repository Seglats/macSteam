#!/bin/zsh
# Record a running game's frame pacing with Instruments. By default only the Display instrument:
# how long each frame stays on each screen, vsyncs, and whether the game goes straight to the
# display (about 25 MB and a few seconds of saving per second recorded). FULL=1 adds Metal
# Application for the game's own presents (frame rate and intervals), which costs far more:
# 2.2 GB and about 6 minutes of saving for a minute. For KovaaK's, which has no anti-cheat; never
# point it at THE FINALS. (The Game Performance template keeps only 1-2 s of this detail, and the
# GPU instrument added 12 GB a minute.)
#
#   game_perf_trace.sh LABEL [SECONDS] [PROCESS]
#   game_perf_trace.sh 160fps                      15 s of KovaaK's -> ~/Desktop/kovaaks-160fps-<time>.trace
#   FULL=1 game_perf_trace.sh 160fps 60            also the game's presents, for a minute
#   GPU=1 game_perf_trace.sh gpu 5                 also the GPU's work per frame (about 200 MB a second)
#
# Waits 30 s, so you can switch back to the game and settle into play. Glass sound = recording, Hero sound = the
# recording time is over (you can stop playing; Instruments then saves for a while).
# If attaching fails it records all processes instead (Glass sounds again; the time counts from
# then). Needs developer tools access (`sudo DevToolsSecurity -enable`; undo with -disable), and
# no leftover DTServiceHub from an earlier failed run, or Instruments' service refuses.
# Read a trace with tests/finals_bisect/frame_pacing.py.
emulate -LR zsh
label=${1:?usage: game_perf_trace.sh LABEL [SECONDS] [PROCESS]}
secs=${2:-15}
process=${3:-FPSAimTrainer-Win64-Shipping}
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer

[[ $process == *Discovery* ]] && { print -u2 'Not for THE FINALS.'; exit 1; }
pid=$(/usr/bin/pgrep -f "$process" | head -1)
[[ -n $pid ]] || { print -u2 "$process is not running"; exit 1; }
out=~/Desktop/kovaaks-$label-$(/bin/date +%H%M%S).trace

print "Switch to the game: recording $secs s in 30 s -> $out"
sleep 30
# Hero when the recording time is over, not when Instruments has finished saving.
done_sound() { ( sleep $secs; /usr/bin/afplay /System/Library/Sounds/Hero.aiff ) & timer=$!; }
/usr/bin/afplay /System/Library/Sounds/Glass.aiff &
done_sound
instruments=(--instrument Display)
[[ ${FULL:-0} != 0 ]] && instruments+=(--instrument 'Metal Application')
[[ ${GPU:-0} != 0 ]] && instruments+=(--instrument GPU)
if ! xcrun xctrace record $instruments --attach "$pid" --time-limit ${secs}s --output "$out" 2>/dev/null; then
  /bin/rm -rf "$out"
  kill $timer 2>/dev/null
  print 'Attaching failed; recording all processes'
  /usr/bin/afplay /System/Library/Sounds/Glass.aiff &
  done_sound
  xcrun xctrace record $instruments --all-processes --time-limit ${secs}s --output "$out"
fi
print "Saved $out"
