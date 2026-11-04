#!/usr/bin/env bash
# OUT=/absolute/path scripts/loadprobe.sh <local|ryzen> <orig|work:world:par|serial|chain>...
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:?OUT must be an absolute output directory}"
[[ "$OUT" == /* ]] || { echo 'OUT must be absolute' >&2; exit 2; }
HOST="${1:?usage: scripts/loadprobe.sh <local|ryzen> <set:world:mode>...}"
shift
[[ "$HOST" == local || "$HOST" == ryzen ]] || { echo 'host must be local or ryzen' >&2; exit 2; }
[[ "$#" -gt 0 ]] || { echo 'at least one set:world:mode is required' >&2; exit 2; }

for tag in "$@"; do
  [[ "$tag" =~ ^(orig|work):([A-Za-z0-9_]+):(par|serial|chain)$ ]] || {
    echo "invalid run: $tag (expected orig|work:world:par|serial|chain)" >&2; exit 2;
  }
  content_set="${BASH_REMATCH[1]}"
  world="${BASH_REMATCH[2]}"
  mode="${BASH_REMATCH[3]}"
  [[ "$mode" != chain || "$world" == NEWWORLD ]] || {
    echo 'chain must start in NEWWORLD' >&2; exit 2;
  }
  out="$OUT/runs/$HOST-$content_set-$world-$mode"
  [[ ! -e "$out" ]] || { echo "output already exists: $out; choose a fresh OUT" >&2; exit 2; }
  mkdir -p "$out"
  envs=("BENCH_HOST=$HOST" "TIMEOUT=${TIMEOUT:-400}" OPENGOTHIC_LOADPROBE=1 OPENGOTHIC_LOADPROBE_SERIAL=0)
  [[ "$HOST" != ryzen ]] || envs+=(RYZEN_BENCH_CLASS=quiet "RYZEN_LOCK_WAIT=${RYZEN_LOCK_WAIT:-3600}")
  if [[ "$content_set" == orig ]]; then
    envs+=(OPENGOTHIC_CONTENT= OPENGOTHIC_TEXTURES=)
  else
    envs+=("OPENGOTHIC_CONTENT=${OPENGOTHIC_CONTENT-$HOME/.local/share/gothic-ai/content}")
  fi
  scenario=loadprobe-idle
  case "$mode" in
    serial) envs+=(OPENGOTHIC_LOADPROBE_SERIAL=1) ;;
    chain) scenario=loadprobe-chain ;;
  esac
  if [[ "$HOST" == local ]]; then
    # This Mac is shared. Skip after a short wait; never disturb other work.
    ready=0
    for attempt in {1..7}; do
      # -x: the process name only; -f also matched agent sessions whose prompt mentions the game.
      if ! LC_ALL=C pgrep -lx Gothic2Notr >"$out/game-before.txt" && python3 -c 'import os, sys; sys.exit(os.getloadavg()[0] > 8)'; then
        ready=1
        break
      fi
      [[ "$attempt" == 7 ]] || sleep 10
    done
    uptime >"$out/host-load.txt"
    [[ "$ready" == 1 ]] || { echo 'loadprobe: Mac busy (game running or load average above 8); skipped' >&2; exit 75; }
  fi
  echo "== $tag"
  env "${envs[@]}" "WORLD=$world.ZEN" "$ROOT/scripts/bench.sh" "$scenario" "$out" >"$out.stdout" 2>&1 || {
    rc=$?; tail -30 "$out.stdout" >&2; exit "$rc";
  }
  grep -q '\[loadprobe\] stage=session_end ' "$out/game.log" || {
    echo "loadprobe: incomplete session in $out/game.log" >&2; exit 1;
  }
  if [[ "$mode" == chain && "$(grep -c '\[loadprobe\] stage=change_end ' "$out/game.log")" != 3 ]]; then
    echo "loadprobe: incomplete world chain in $out/game.log" >&2; exit 1;
  fi
  grep -q '"kind":"rec_stop"' "$out/events.jsonl" || {
    echo "loadprobe: scenario did not finish in $out/events.jsonl" >&2; exit 1;
  }
  if [[ "$HOST" == local ]] && ! python3 - "$out/game.log" <<'PYLOAD'
import re
import sys
text = open(sys.argv[1]).read()
loads = re.findall(r'\[loadprobe-host\] load_1=([0-9.]+)', text)
sys.exit(not loads or float(loads[0]) > 8)
PYLOAD
  then
    echo 'loadprobe: Mac started above load average 8; discard and repeat' >&2
    exit 75
  fi
  echo "$(grep -c '\[loadprobe\]' "$out/game.log") probe lines: $out/game.log"
done
