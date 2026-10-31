#!/usr/bin/env bash
# Render-feature survey on the Mac (Metal): same as run-ryzen.sh, with ioreg GPU samples.
# Usage: run-mac.sh <out dir> <scenario> <configs file>
set -uo pipefail
S="$(cd "$(dirname "$0")" && pwd)"
OUT="$1"; SCEN="$2"; CONFIGS="$3"
BIN="${BIN:-$S/build-mac/opengothic/Gothic2Notr}"
GAME="${GOTHIC_DIR:-/Volumes/DevSSD/Dev/games/Gothic II}"
SAVE="${SAVE:-/Volumes/DevSSD/Dev/projects/gothic/save_slot_1.sav}"
TEX="$HOME/.local/share/gothic-ai/hd-textures/textures"
mkdir -p "$OUT"
{ sw_vers; sysctl -n machdep.cpu.brand_string; system_profiler SPDisplaysDataType 2>/dev/null | grep -E "Chipset|Resolution|Cores|Metal"; pmset -g batt | head -2; } >"$OUT/host.txt" 2>&1
while IFS= read -r line; do
  # tab is IFS whitespace in bash: read would merge an empty flags field, so split with cut
  label="$(cut -f1 <<<"$line")"; flags="$(cut -f2 <<<"$line")"; prefix="$(cut -f3 <<<"$line")"
  [[ "$line" == *$'\t'* ]] || flags="" prefix=""
  [[ -z "$label" || "$label" == \#* ]] && continue
  if pgrep -x Gothic2Notr >/dev/null; then echo "another game is running, stop" >&2; exit 3; fi
  D="$OUT/$label"; rm -rf "$D"; mkdir -p "$D/run"
  cp "$SAVE" "$D/run/save_slot_1.sav"
  printf '[INTERNAL]\nvidResIndex=0\n[SOUND]\nsoundVolume=0\nmusicVolume=0\nmusicEnabled=0\n' >"$D/run/Gothic.ini"
  { [[ -n "$prefix" ]] && tr ';' '\n' <<<"$prefix"; cat "$SCEN"; } >"$D/scenario.bench"
  echo "$flags" >"$D/flags.txt"
  # GPU load of the desktop without the game (WindowServer, other apps), and the CPU load average
  python3 "$S/gpu-apple.py" "$D/idle-gpu.csv" & IDLE=$!
  perl -e 'select(undef,undef,undef,5)'; kill "$IDLE" 2>/dev/null; wait "$IDLE" 2>/dev/null
  uptime >"$D/load.txt"
  python3 "$S/gpu-apple.py" "$D/gpu.csv" & SAMPLER=$!
  start=$(date +%s)
  # shellcheck disable=SC2086
  (cd "$D/run" && exec env GOTHIC_AI=0 TEMPEST_BACKGROUND=1 TEMPEST_WINDOW_SIZE="${WINDOW:-960x540}" OPENGOTHIC_BENCH="$D/scenario.bench" OPENGOTHIC_BENCH_OUT="$D" \
     OPENGOTHIC_FRAME_PROFILE="$D/frames.csv" OPENGOTHIC_TEXTURES="$TEX" OPENGOTHIC_BENCH_FFMPEG="$S/discard-ffmpeg.sh" \
     "$BIN" -g "$GAME" -window -devmode -save 1 $flags) >"$D/game.log" 2>&1 &
  PID=$!
  for ((i = 0; i < ${GAME_TIMEOUT:-300} * 4; i++)); do kill -0 "$PID" 2>/dev/null || break; sleep 0.25; done
  if kill -0 "$PID" 2>/dev/null; then echo "timeout" >>"$D/game.log"; kill "$PID"; sleep 3; kill -9 "$PID" 2>/dev/null; fi
  wait "$PID"; rc=$?
  kill "$SAMPLER" 2>/dev/null; wait "$SAMPLER" 2>/dev/null
  echo "$rc" >"$D/exit.txt"
  uptime >>"$D/load.txt"
  echo "$label rc=$rc $(( $(date +%s) - start ))s $flags"
  rm -rf "$D/run"
  sleep 3
done <"$CONFIGS"
