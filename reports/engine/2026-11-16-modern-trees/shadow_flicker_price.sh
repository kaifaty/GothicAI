#!/usr/bin/env bash
# The price of the second round of shadow-flicker.md: scripts/bench/perf-trees.bench with the forest of 1 024 full
# spruces (the set tree-cost-1024 of `scripts/content/fir_set.py forest`), 2560x1440, the GPU's passes
# (OPENGOTHIC_GPU_PASSES=1). Each checkout is built on the perf stand under its own folder; then one quiet queue job
# runs this checkout, main, this checkout again - two runs of the change, the control of main once (the user's limit
# of 2026-11-18). The results come back to <out>/<fix-1|main|fix-2>.
#   reports/engine/2026-11-16-modern-trees/shadow_flicker_price.sh /absolute/local/out /main/checkout [local set]
# NO_BUILD=1 keeps the stand's builds of the same sources.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="${1:?usage: shadow_flicker_price.sh /absolute/local/out /main/checkout [local set]}"
MAIN="$(cd "${2:?the checkout of main}" && pwd)"
SET="${3:-$HOME/.local/share/gothic-ai/remaster/trees-fir/shadow-flicker/sets/tree-cost-1024}"
HOST="${RYZEN_HOST:-ryzen}"
source "$ROOT/scripts/perf.sh"          # perf_checkout, source_fingerprint (of $ROOT)
HOME_R="$(ssh -o BatchMode=yes "$HOST" 'printf "%s" "$HOME"')"
TASK="$HOME_R/gothic-ai/tree-shadow-flicker-2"
CONTENT="$TASK/content/tree-cost-1024"
RUN="price-$(date -u +%Y%m%dT%H%M%SZ)"
REMOTE_OUT="$TASK/runs/$RUN"

ssh "$HOST" "mkdir -p '$REMOTE_OUT' '$CONTENT'"
rsync -a "$SET/" "$HOST:$CONTENT/"

declare -A SRC FP ENVS
for name in fix main; do
  root="$ROOT"
  [[ "$name" == main ]] && root="$MAIN"
  fingerprint="$(ROOT="$root" source_fingerprint)"
  source="$HOME_R/gothic-ai/perf-stand/$(perf_checkout "$root")/source"
  ssh "$HOST" "mkdir -p '$source/OpenGothic' '$source/scripts/bench'"
  rsync -a --checksum --no-times --delete --exclude='build*/' --exclude='cmake-build-*/' --exclude='.git' \
    "$root/OpenGothic/" "$HOST:$source/OpenGothic/"
  rsync -a "$root"/scripts/{ryzen-build.sh,perf.sh,perf_report.py,base-ensure.sh,ryzen_game.py} "$HOST:$source/scripts/"
  rsync -a --delete --exclude='/build*/' "$root/scripts/convert/" "$HOST:$source/scripts/convert/"
  rsync -a "$root"/scripts/bench/perf-*.bench "$HOST:$source/scripts/bench/"
  SRC[$name]="$source"
  FP[$name]="$fingerprint"
  ENVS[$name]="PERF_REMOTE=1 PERF_VID_RES=0 PERF_INI= PERF_WINDOW=2560x1440 PERF_REPEAT=1 PERF_WAIT=600 PERF_ARGS= \
PERF_COMMIT=$(git -C "$root" rev-parse HEAD) PERF_DIRTY=$(git -C "$root" status --porcelain | wc -l | tr -d ' ') \
PERF_SOURCE_FINGERPRINT=$fingerprint"
  echo "price: $name $(git -C "$root" rev-parse --short HEAD) -> $source"
  if [[ -z "${NO_BUILD:-}" ]]; then
    # shellcheck disable=SC2086
    "$ROOT/scripts/gq" --host "$HOST" run --class quick --threads 8 --mem 12G --max 30m --wait-max 7200 \
      --name "build shadow-flicker price $name" -- env ${ENVS[$name]} PERF_ENV= RYZEN_JOBS=8 \
      bash "$source/scripts/perf.sh" __build "$REMOTE_OUT/build-$name"
  fi
done

JOB="set -u"$'\n'
for run in fix-1:fix main:main fix-2:fix; do
  label="${run%%:*}" name="${run#*:}"
  JOB+="echo \"price: $label\"; env ${ENVS[$name]} NO_BUILD=1 PERF_ENV='OPENGOTHIC_CONTENT=$CONTENT OPENGOTHIC_GPU_PASSES=1' bash ${SRC[$name]}/scripts/perf.sh __run $REMOTE_OUT/$label trees || echo \"price: $label FAILED\""$'\n'
done
"$ROOT/scripts/gq" --host "$HOST" run --class quiet --threads 4 --mem 5G --gpu-mem 4G --display --max 45m \
  --wait-max 10800 --name 'shadow-flicker price runs' --archive "$REMOTE_OUT" -- bash -c "$JOB" 2>&1 | tee "$OUT.log"
mkdir -p "$OUT"
if ssh "$HOST" test -d "/mnt/shared/gothic-ai/tree-shadow-flicker-2/runs/$RUN"; then
  rsync -a "$HOST:/mnt/shared/gothic-ai/tree-shadow-flicker-2/runs/$RUN/" "$OUT/"
else
  rsync -a "$HOST:$REMOTE_OUT/" "$OUT/"
fi
for label in fix-1 main fix-2; do
  [[ -d "$OUT/$label" ]] && python3 "$ROOT/scripts/perf_report.py" run "$OUT/$label" >/dev/null || true
done
echo "$OUT"
