#!/usr/bin/env bash
# The price of a round of shadow-flicker.md: scripts/bench/perf-trees.bench, 2560x1440, the GPU's passes
# (OPENGOTHIC_GPU_PASSES=1), on sets of `scripts/content/fir_set.py forest`. Each checkout is built on the perf stand
# under its own folder; then one quiet queue job runs the runs of PRICE_RUNS on each set. The results come back to
# <out>/<label> with one set, <out>/<set>/<label> with more.
#   reports/engine/2026-11-16-modern-trees/shadow_flicker_price.sh /absolute/local/out /main/checkout [local set]
# The second round (the defaults): the forest of 1 024 full spruces, this checkout, main, this checkout again - two
# runs of the change, the control of main once (the user's limit of 2026-11-18). The third round:
#   PRICE_TASK=tree-shadow-rhythm PRICE_RUNS='fix:0 main:1' PRICE_ENV='OPENGOTHIC_MESH_LEVELS=1' \
#   PRICE_SETS="levels=$S/round-3/sets/tree-cost-1024-levels control=$S/sets/tree-cost-0" shadow_flicker_price.sh ...
# PRICE_TASK: the folder of the task on the host (~/gothic-ai/<task>, /mnt/shared/gothic-ai/<task>); PRICE_SETS:
# name=local path, separated by spaces; PRICE_RUNS: label:checkout (0 this one, 1 main) in their order; PRICE_ENV: more
# of the game's environment. NO_BUILD=1 keeps the stand's builds of the same sources.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="${1:?usage: shadow_flicker_price.sh /absolute/local/out /main/checkout [local set]}"
MAIN="$(cd "${2:?the checkout of main}" && pwd)"
SET="${3:-$HOME/.local/share/gothic-ai/remaster/trees-fir/shadow-flicker/sets/tree-cost-1024}"
SETS="${PRICE_SETS:-tree-cost-1024=$SET}"
RUNS="${PRICE_RUNS:-fix-1:0 main:1 fix-2:0}"
TASK_NAME="${PRICE_TASK:-tree-shadow-flicker-2}"
HOST="${RYZEN_HOST:-ryzen}"
source "$ROOT/scripts/perf.sh"          # perf_checkout, source_fingerprint (of $ROOT)
HOME_R="$(ssh -o BatchMode=yes "$HOST" 'printf "%s" "$HOME"')"
TASK="$HOME_R/gothic-ai/$TASK_NAME"
RUN="price-$(date -u +%Y%m%dT%H%M%SZ)"
REMOTE_OUT="$TASK/runs/$RUN"

ssh "$HOST" "mkdir -p '$REMOTE_OUT'"
read -r -a SET_LIST <<< "$SETS"
for set in "${SET_LIST[@]}"; do
  ssh "$HOST" "mkdir -p '$TASK/content/${set%%=*}'"
  rsync -a "${set#*=}/" "$HOST:$TASK/content/${set%%=*}/"
done

# 0: this checkout, 1: main (bash 3 of macOS: no associative arrays)
NAMES=(fix main)
ROOTS=("$ROOT" "$MAIN")
SRC=() ENVS=()
for i in 0 1; do
  name="${NAMES[$i]}" root="${ROOTS[$i]}"
  fingerprint="$(ROOT="$root" source_fingerprint)"
  source="$HOME_R/gothic-ai/perf-stand/$(perf_checkout "$root")/source"
  ssh "$HOST" "mkdir -p '$source/OpenGothic' '$source/scripts/bench'"
  rsync -a --checksum --no-times --delete --exclude='build*/' --exclude='cmake-build-*/' --exclude='.git' \
    "$root/OpenGothic/" "$HOST:$source/OpenGothic/"
  rsync -a "$root"/scripts/{ryzen-build.sh,perf.sh,perf_report.py,perf_gpu_log.py,base-ensure.sh,ryzen_game.py} "$HOST:$source/scripts/"
  rsync -a --delete --exclude='/build*/' "$root/scripts/convert/" "$HOST:$source/scripts/convert/"
  rsync -a "$root"/scripts/bench/perf-*.bench "$HOST:$source/scripts/bench/"
  SRC[$i]="$source"
  ENVS[$i]="PERF_REMOTE=1 PERF_VID_RES=0 PERF_INI= PERF_WINDOW=2560x1440 PERF_REPEAT=1 PERF_WAIT=600 PERF_ARGS= \
PERF_COMMIT=$(git -C "$root" rev-parse HEAD) PERF_DIRTY=$(git -C "$root" status --porcelain | wc -l | tr -d ' ') \
PERF_SOURCE_FINGERPRINT=$fingerprint"
  echo "price: $name $(git -C "$root" rev-parse --short HEAD) -> $source"
  if [[ -z "${NO_BUILD:-}" ]]; then
    # shellcheck disable=SC2086
    "$ROOT/scripts/gq" --host "$HOST" run --class quick --threads 8 --mem 12G --max 30m --wait-max 7200 \
      --name "build shadow-flicker price $name" -- env ${ENVS[$i]} PERF_ENV= RYZEN_JOBS=8 \
      bash "$source/scripts/perf.sh" __build "$REMOTE_OUT/build-$name"
  fi
done

JOB="set -u"$'\n'
LABELS=()
read -r -a RUN_LIST <<< "$RUNS"
for set in "${SET_LIST[@]}"; do
  for run in "${RUN_LIST[@]}"; do
    label="${run%%:*}" i="${run#*:}"
    [[ ${#SET_LIST[@]} == 1 ]] || label="${set%%=*}/$label"
    LABELS+=("$label")
    JOB+="echo \"price: $label\"; env ${ENVS[$i]} NO_BUILD=1 PERF_ENV='OPENGOTHIC_CONTENT=$TASK/content/${set%%=*} OPENGOTHIC_GPU_PASSES=1 ${PRICE_ENV:-}' bash ${SRC[$i]}/scripts/perf.sh __run $REMOTE_OUT/$label trees || echo \"price: $label FAILED\""$'\n'
  done
done
"$ROOT/scripts/gq" --host "$HOST" run --class quiet --threads 4 --mem 5G --gpu-mem 4G --display \
  --max "$((15*${#LABELS[@]}))m" --wait-max 10800 --name 'shadow-flicker price runs' --archive "$REMOTE_OUT" \
  -- bash -c "$JOB" 2>&1 | tee "$OUT.log"
mkdir -p "$OUT"
if ssh "$HOST" test -d "/mnt/shared/gothic-ai/$TASK_NAME/runs/$RUN"; then
  rsync -a "$HOST:/mnt/shared/gothic-ai/$TASK_NAME/runs/$RUN/" "$OUT/"
else
  rsync -a "$HOST:$REMOTE_OUT/" "$OUT/"
fi
for label in "${LABELS[@]}"; do
  [[ -d "$OUT/$label" ]] && python3 "$ROOT/scripts/perf_report.py" run "$OUT/$label" >/dev/null || true
done
echo "$OUT"
