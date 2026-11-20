#!/usr/bin/env bash
# The price of two bough schemes in the forest of 1 024 spruces, tree-bough-lod.md: perf-trees.bench on Ryzen in the
# conditions of PR 1653 (Vulkan, 2560x1440, OPENGOTHIC_GPU_PASSES=1 OPENGOTHIC_VSM_STATS=frames), three states, one
# run each, in one windowed quiet job: `off` (the forest of the second scheme, OPENGOTHIC_TREE_BOUGH_LOD=0), then
# `s122` and `b` with the scheme on. The forests are those of `fir_set.py forest --hierarchy --label`; they go to
# ~/gothic-ai/tree-bough-b-accept/content/ on Ryzen and their SHA-256 are compared with the local files before the
# build. The card is warmed for 90 s, the least the stand's `auto` asks for, before the first run only: the stand's
# default would spend up to 300 s of the window (docs/performance.md, the GPU warm-up).
# BUILD_ONLY=1 stops after the build; NO_BUILD=1 keeps the stand's build of the same sources.
# Another measurement of the same forests names its own: COST_TASK, the folder under ~/gothic-ai/ on Ryzen;
# COST_STATES, "name|forest|switch|warm-up seconds[|VAR=value]" separated by spaces, the first one the base of the
# tables; COST_MAX, the limit of the windowed job; COST_BENCH, a scenario to run in place of perf-trees.bench.
#   reports/engine/2026-11-16-modern-trees/tree-bough-lod/cost.sh /absolute/local/out [forest S122] [forest B]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
OUT="${1:?usage: cost.sh /absolute/local/out [forest S122] [forest B]}"
BASE="$HOME/.local/share/gothic-ai/remaster/trees-fir"
SETS=("s122|${2:-$BASE/bough-stagger/forest-1024}" "b|${3:-$BASE/bough-early/forest-b-1024}")
read -r -a STATES <<<"${COST_STATES:-off|b|0|90 s122|s122|1|0 b|b|1|0}"
NAMES=("${STATES[@]%%|*}")
HOST="${RYZEN_HOST:-ryzen}"
source "$ROOT/scripts/perf.sh"          # perf_checkout, source_fingerprint
FINGERPRINT="$(source_fingerprint)"
HOME_R="$(ssh -o BatchMode=yes "$HOST" 'printf "%s" "$HOME"')"
SOURCE="$HOME_R/gothic-ai/perf-stand/$(perf_checkout "$ROOT")/source"
TASK_NAME="${COST_TASK:-tree-bough-b-accept}"
TASK="$HOME_R/gothic-ai/$TASK_NAME"
RUN="$(date -u +%Y%m%dT%H%M%SZ)-$(IFS=-; echo "${NAMES[*]}")"
REMOTE_OUT="$TASK/runs/$RUN"
COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
DIRTY="$(git -C "$ROOT" status --porcelain | wc -l | tr -d ' ')"

ssh -o BatchMode=yes "$HOST" "python3 -c $(printf %q "$(cat "$ROOT/scripts/ryzen_game.py")") --check"
ssh "$HOST" "mkdir -p '$SOURCE/OpenGothic' '$SOURCE/scripts/bench' '$REMOTE_OUT' '$TASK/content'"
mkdir -p "$OUT"
sums() { find . -type f -print0 | LC_ALL=C sort -z | xargs -0 "$@"; }
link=()
for entry in "${SETS[@]}"; do
  name="${entry%%|*}" path="${entry#*|}"
  [[ " ${STATES[*]} " == *"|$name|"* ]] || continue
  rsync -a --delete ${link[@]+"${link[@]}"} "$path/" "$HOST:$TASK/content/forest-$name/"
  link=("--link-dest=$TASK/content/forest-$name/")
  (cd "$path" && sums shasum -a 256) >"$OUT/sha256-$name.txt"
  ssh "$HOST" "cd '$TASK/content/forest-$name' && $(declare -f sums); sums sha256sum" | diff "$OUT/sha256-$name.txt" -
  echo "cost: forest-$name: $(wc -l <"$OUT/sha256-$name.txt" | tr -d ' ') files, SHA-256 equal on $HOST"
done
rsync -a --checksum --no-times --delete --exclude='build*/' --exclude='cmake-build-*/' --exclude='.git' \
  "$ROOT/OpenGothic/" "$HOST:$SOURCE/OpenGothic/"
rsync -a "$ROOT"/scripts/{ryzen-build.sh,perf.sh,perf_report.py,perf_gpu_log.py,base-ensure.sh,ryzen_game.py} "$HOST:$SOURCE/scripts/"
rsync -a --delete --exclude='/build*/' "$ROOT/scripts/convert/" "$HOST:$SOURCE/scripts/convert/"
rsync -a "$ROOT"/scripts/bench/perf-*.bench "$HOST:$SOURCE/scripts/bench/"
[[ -z "${COST_BENCH:-}" ]] || rsync -a "$COST_BENCH" "$HOST:$SOURCE/scripts/bench/perf-trees.bench"

ENV=(PERF_REMOTE=1 PERF_VID_RES=0 PERF_INI= PERF_WINDOW=2560x1440 PERF_REPEAT=1 PERF_WAIT=600 PERF_ARGS=
     "PERF_COMMIT=$COMMIT" "PERF_DIRTY=$DIRTY" "PERF_SOURCE_FINGERPRINT=$FINGERPRINT")
if [[ -z "${NO_BUILD:-}" ]]; then
  "$ROOT/scripts/gq" --host "$HOST" run --class quick --threads 8 --mem 12G --max 30m --wait-max 7200 \
    --name "build $TASK_NAME stand" -- env "${ENV[@]}" PERF_ENV= RYZEN_JOBS=8 \
    bash "$SOURCE/scripts/perf.sh" __build "$REMOTE_OUT/build"
fi
[[ -z "${BUILD_ONLY:-}" ]] || exit 0

# one run each
JOB="set -u"$'\n'
for state in "${STATES[@]}"; do
  IFS='|' read -r name forest switch warm extra <<<"$state"
  JOB+="echo \"cost: $name\"; env ${ENV[*]} NO_BUILD=1 PERF_WARMUP=$warm PERF_ENV='OPENGOTHIC_CONTENT=$TASK/content/forest-$forest OPENGOTHIC_GPU_PASSES=1 OPENGOTHIC_VSM_STATS=frames OPENGOTHIC_TREE_BOUGH_LOD=$switch $extra' bash $SOURCE/scripts/perf.sh __run $REMOTE_OUT/$name trees || echo \"cost: $name FAILED\""$'\n'
done
"$ROOT/scripts/gq" --host "$HOST" run --class quiet --threads 4 --mem 5G --gpu-mem 4G --display --max "${COST_MAX:-20m}" \
  --wait-max 10800 --name "$TASK_NAME cost" --archive "$REMOTE_OUT" -- bash -c "$JOB" 2>&1 | tee "$OUT/ryzen.log"
if ssh "$HOST" test -d "/mnt/shared/gothic-ai/$TASK_NAME/runs/$RUN"; then
  rsync -a "$HOST:/mnt/shared/gothic-ai/$TASK_NAME/runs/$RUN/" "$OUT/"
else
  rsync -a "$HOST:$REMOTE_OUT/" "$OUT/"
fi
python3 "$(dirname "$0")/cost_tables.py" "$OUT" "${NAMES[@]}" | tee "$OUT/tables.md"
echo "$RUN $OUT"
