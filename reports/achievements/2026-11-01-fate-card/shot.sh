#!/bin/bash
# shot.sh NAME "ai fate args" [wait ms]: one launch, one window frame of the final card
S=$(dirname "$0"); B=/Volumes/DevSSD/Dev/projects/gothic/.claude/worktrees/competent-mahavira-60dad2/OpenGothic/build/opengothic/Gothic2Notr
mkdir -p $S/out
printf 'cheat god\nset time 12 0\noverlay off\nui on\nwait 14000\nai fate %s\nwaitai fate card 60000\nwait 600000\n' "$2" > $S/fate.bench
cd "$(dirname "$B")"
GOTHIC_AI_URL=ws://127.0.0.1:47898 OPENGOTHIC_BENCH=$S/fate.bench OPENGOTHIC_BENCH_OUT=$S/out OPENGOTHIC_BENCH_REALTIME=1 \
  "$B" -g "/Volumes/DevSSD/Dev/games/Gothic II" -window -devmode -nomenu -nointro > $S/game-$1.log 2>&1 &
pid=$!
for i in $(seq 1 90); do sleep 1; grep -q "world ready" $S/game-$1.log 2>/dev/null && break; done
sleep ${3:-28}
wid=$($S/winid $pid)
screencapture -x -o -l "$wid" $S/out/$1.png
kill $pid; wait $pid 2>/dev/null
echo "$1 -> $S/out/$1.png (window $wid)"
