#!/usr/bin/env bash
# Load probe runs (needs ../load-probe.patch applied to the engine).
# usage: OUT=<dir> runs.sh <local|ryzen> <set:world:mode>...   set: orig|work, mode: par|serial|chain
set -uo pipefail
ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
S="${OUT:?OUT=<output dir>}"
HOST=$1; shift
for tag in "$@"; do
  IFS=: read -r set world mode <<<"$tag"
  out="$S/runs/$HOST-$set-$world-$mode"
  [[ -f "$out/game.log" ]] && grep -q "loadprobe\] stage=session_end" "$out/game.log" && { echo "skip $tag"; continue; }
  rm -rf "$out"; mkdir -p "$out"
  envs=(BENCH_HOST=$HOST NO_BUILD=1 TIMEOUT=400)
  [[ $HOST == ryzen ]] && envs+=(RYZEN_BENCH_CLASS=quiet RYZEN_LOCK_WAIT=3600)
  if [[ $set == orig ]]; then envs+=(OPENGOTHIC_CONTENT= OPENGOTHIC_TEXTURES=)
  else envs+=(OPENGOTHIC_CONTENT=$HOME/.local/share/gothic-ai/content); fi
  scen=$HERE/loadprobe-idle.bench
  case $mode in
    serial) envs+=(OPENGOTHIC_LOADPROBE_SERIAL=1) ;;
    chain) scen=$HERE/loadprobe-chain.bench ;;
  esac
  if [[ $HOST == local ]]; then
    while pgrep -x Gothic2Notr >/dev/null; do sleep 10; done
  fi
  echo "== $tag $(date +%T)"
  env "${envs[@]}" WORLD=$world.ZEN "$ROOT/scripts/bench.sh" "$scen" "$out" >"$out.stdout" 2>&1
  echo "rc=$? $(grep -c loadprobe "$out/game.log" 2>/dev/null) probe lines"
done
