#!/usr/bin/env bash
# Three-game measurement on Ryzen: the condition for leaving e2e_display_exclusive.
# Usage: scripts/ryzen-round.sh TAG SCENARIO [SCENARIO...]
#
# Parallel `scripts/ryzen.sh e2e` calls overlap only while the shared queue is idle:
# a waiting quiet or exclusive request lets shared games in one at a time. So this
# holds one quiet reservation of the shared queue (12 threads, 15G, 6G VRAM, all
# display slots) and starts the scenarios inside it through a private queue on
# Ryzen: three at once, the rest as slots free. Listed scenarios share slots here
# (RYZEN_DISPLAY_EXCLUSIVE=0); one scenario alone is a solo run on a quiet machine.
# Logs: .ryzen/round/TAG-N-SCENARIO/. Env: LLM (fake), TIMEOUT (600),
# RYZEN_ROUND_PREFIX (Ryzen copy names), RYZEN_LOCK_WAIT (reservation wait, 5400 s).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[[ $# -ge 2 ]] || { sed -n '3p' "$0" >&2; exit 2; }
tag=$1; shift
[[ "$tag" =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]] || { echo 'Unsafe tag' >&2; exit 2; }
name="$(basename "$ROOT")"
if [[ "$ROOT" == */worktrees/*/* ]]; then name="$(basename "$(dirname "$ROOT")")"; fi
prefix="${RYZEN_ROUND_PREFIX:-$name-round}"
out="$ROOT/.ryzen/round"
mkdir -p "$out"
cd "$ROOT"

# A unique owner finds this reservation in `gq status`: its id is printed only while queued.
export GQ_OWNER="round-$$"
id=''
holder=''
cleanup() {
  if [[ -n "$id" ]]; then scripts/gq cancel "$id" >/dev/null 2>&1 || true; fi
  if [[ -n "$holder" ]]; then kill "$holder" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

scripts/gq run --class quiet --threads 12 --mem 15G --gpu-mem 6G --display --display-exclusive \
  --max 30m --wait-max "${RYZEN_LOCK_WAIT:-5400}" --name "e2e round $tag" -- /bin/sleep 1800 \
  >"$out/$tag-reservation.log" 2>&1 &
holder=$!
until [[ -n "$id" ]]; do
  sleep 5
  kill -0 "$holder" 2>/dev/null || { echo 'Ryzen: reservation ended before admission' >&2; cat "$out/$tag-reservation.log" >&2; exit 75; }
  id="$(scripts/gq status 2>/dev/null | awk -v o="$GQ_OWNER" '$2 == o && $3 == "quiet" && $4 == "running" {print $1}' | head -1)"
done
echo "Ryzen: reservation $id running at $(date +%T)"

i=0
pids=()
for scenario in "$@"; do
  i=$((i + 1))
  dir="$out/$tag-$i-$scenario"
  rm -rf "$dir"
  mkdir -p "$dir"
  ( GQ_HOME="~/gothic-ai/gq-$prefix" GQ_LEGACY_LOCK="~/gothic-ai/gq-$prefix/legacy.lock" \
    RYZEN_DISPLAY_EXCLUSIVE=0 LLM="${LLM:-fake}" TIMEOUT="${TIMEOUT:-600}" OPENGOTHIC_FPS_LOG=1 \
    RYZEN_WORKTREE="$prefix-$i" SCENARIO="$scenario" LOG_DIR="$dir" \
      scripts/ryzen.sh e2e >"$dir/launcher.log" 2>&1
    echo "rc=$?" >>"$dir/launcher.log" ) &
  pids+=($!)
  sleep 1
done
wait "${pids[@]}"

failed=0
for dir in "$out/$tag"-*/; do
  dir=${dir%/}
  result="$(grep -h 'scenario .*checks' "$dir/launcher.log" | tail -1 | sed 's/.*scenario //')"
  slot="$(grep -ho 'e2e slot [0-9]' "$dir/launcher.log" | tail -1)"
  took="$(grep -ho 'e2e [0-9]*s, exit [0-9]*' "$dir/launcher.log" | tail -1)"
  fps="$(grep -h 'mean_fps' "$dir/game.log" 2>/dev/null | sed 's/.*mean_fps=\([0-9.]*\).*/\1/' |
    awk '{s += $1; n++} END {if (n) printf "%.1f", s / n}')"
  echo "$(basename "$dir"): ${result:-no result}; ${slot:-no slot}; ${took:-?}; fps ${fps:-?}; $(tail -1 "$dir/launcher.log")"
  { grep -hE 'game exited: signal SIGKILL|gothic_ai[.a-z_]*: [a-zA-Z0-9_]+ +failed' "$dir/launcher.log"
    grep -h 'GameError:' "$dir/server.log" 2>/dev/null; } | head -5 | cut -c1-300 | sed 's/^/    /'
  grep -qx 'rc=0' "$dir/launcher.log" || failed=1
done
exit "$failed"
