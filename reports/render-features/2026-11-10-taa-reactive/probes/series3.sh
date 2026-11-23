#!/usr/bin/env bash
# series3.sh <runs-dir> <deadline-epoch> <item>...   item = tag:bin:ENV1=v,ENV2=v
# One game at a time: waits until no Gothic2Notr runs. A run counts when it exits 0 and no other game ran beside it;
# views are then selected by the foreign GPU load during each of them (gpumon.jsonl, analyze.py).
# Stop: touch <runs-dir>/STOP (checked between runs). The runner's PID is in <runs-dir>/runner.pid.
set -u
WT=/Volumes/DevSSD/Dev/worktrees/codex/claude-taa-mac-cost/gothic
T=/Volumes/DevSSD/Dev/tmp/taa-mac-cost
RUNS="$1"; DEADLINE="$2"; shift 2
mkdir -p "$RUNS"; echo $$ > "$RUNS/runner.pid"
log() { echo "$(date +%H:%M:%S) $*" | tee -a "$RUNS/series.log"; }
for item in "$@"; do
  tag="${item%%:*}"; rest="${item#*:}"; bin="${rest%%:*}"; envs="${rest#*:}"
  penv="OPENGOTHIC_GPU_PASSES=1 ${envs//,/ }"
  for attempt in 1 2 3; do
    [ -f "$RUNS/STOP" ] && { log "stop file"; exit 0; }
    (( $(date +%s) < DEADLINE )) || { log "deadline"; exit 0; }
    while pgrep -x Gothic2Notr >/dev/null; do sleep 5; done
    before=$("$T/gpuload.sh" 3)
    rm -f "$RUNS/.gstop" "$RUNS/.stop"
    python3 "$T/gpumon.py" "$RUNS/.gpumon.jsonl" "$RUNS/.gstop" & gmon=$!
    ( mx=0; echo 0 > "$RUNS/.maxgames"; while [ ! -f "$RUNS/.stop" ]; do c=$(pgrep -x Gothic2Notr | wc -l | tr -d ' '); (( c > mx )) && mx=$c && echo $mx > "$RUNS/.maxgames"; sleep 1; done ) & mon=$!
    echo "$gmon $mon" > "$RUNS/helpers.pid"
    out=$(cd "$WT" && NO_BUILD=1 BENCH_HOST=local GAME_BIN="$T/bins/$bin/Gothic2Notr" PERF_ENV="$penv" PERF_WAIT=3600 \
          scripts/perf.sh harbour-night market 2>&1 | tee "$RUNS/.last.log" | tail -1)
    rc=${PIPESTATUS[0]}
    touch "$RUNS/.stop" "$RUNS/.gstop"; wait $mon $gmon 2>/dev/null; rm -f "$RUNS/.stop" "$RUNS/.gstop" "$RUNS/helpers.pid"
    maxg=$(cat "$RUNS/.maxgames"); after=$("$T/gpuload.sh" 3)
    dest="$RUNS/$tag-a$attempt"
    if [ -d "$out" ]; then mv "$out" "$dest"; else mkdir -p "$dest"; fi
    cp "$RUNS/.last.log" "$dest/perf.log"; mv "$RUNS/.gpumon.jsonl" "$dest/gpumon.jsonl"
    valid=1; why=""
    [ "$rc" = 0 ] || { valid=0; why="rc=$rc"; }
    (( maxg <= 1 )) || { valid=0; why="$why games=$maxg"; }
    echo "{\"tag\":\"$tag\",\"bin\":\"$bin\",\"env\":\"$penv\",\"attempt\":$attempt,\"before\":$before,\"after\":$after,\"max_games\":$maxg,\"rc\":$rc,\"valid\":$valid,\"why\":\"$why\"}" > "$dest/status.json"
    log "$tag attempt $attempt: load before=$before after=$after games=$maxg rc=$rc valid=$valid $why"
    [ $valid = 1 ] && break
  done
done
log "series done"
