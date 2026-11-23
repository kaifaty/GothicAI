#!/bin/bash
# shot.sh NAME "diary args": one launch of the game, one frame of its window with the diary open.
# The diary pauses the game and the bench with it, so the frame is taken from outside (screencapture -l).
# Needs: chronicle_probe.py listening on 47899, `swiftc winid.swift -o winid` once, GOTHIC_DIR.
set -u
S=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$S/../../.." && pwd)
B="$ROOT/OpenGothic/build/opengothic/Gothic2Notr"; GAME="${GOTHIC_DIR:?set GOTHIC_DIR to the Gothic II folder}"
OUT="${OUT:-$S/out}"; mkdir -p "$OUT"
printf 'cheat god\nset time 12 0\noverlay off\nui on\nwait 14000\ndiary %s\nwait 600000\n' "$2" > "$OUT/diary.bench"
cd "$(dirname "$B")"
GOTHIC_AI_URL=ws://127.0.0.1:47899 OPENGOTHIC_BENCH="$OUT/diary.bench" OPENGOTHIC_BENCH_OUT="$OUT" OPENGOTHIC_BENCH_REALTIME=1 \
  "$B" -g "$GAME" -window -devmode -nomenu -nointro > "$OUT/game-$1.log" 2>&1 &
pid=$!
for i in $(seq 1 90); do sleep 1; grep -q "world ready" "$OUT/game-$1.log" 2>/dev/null && break; done
sleep 28  # the bench's own 14 s plus the chronicle's arrival and the diary's first frames
screencapture -x -o -l "$("$S/winid" $pid)" "$OUT/$1.png"
kill $pid; wait $pid 2>/dev/null
echo "$1 -> $OUT/$1.png"
