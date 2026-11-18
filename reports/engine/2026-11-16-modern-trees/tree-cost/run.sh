#!/usr/bin/env bash
# The runs of tree-cost.md: every variant of the forest on one Ryzen build, in one quiet queue job, two rounds,
# the second in the reverse order. Each run is `scripts/perf.sh __run <out> trees` (scripts/bench/perf-trees.bench)
# with its own PERF_ENV; then one run of the full forest with OPENGOTHIC_VSM_STATS=1 for the counters of the shadow
# maps (it waits for the GPU: not a timing run). The sets of `scripts/content/fir_set.py forest` must already lie in
# ~/gothic-ai/tree-cost/content/<set>/ on Ryzen (rsync, see tree-cost.md, «Как повторить»).
# With variant names after the output, only those run, in that order, as round 3 (round 4 for a name given twice):
# a repeat of runs a hang stopped, without the counters' run; NO_BUILD=1 keeps the stand's build.
#   reports/engine/2026-11-16-modern-trees/tree-cost/run.sh /absolute/local/out [variant...]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
OUT="${1:?usage: run.sh /absolute/local/out [variant...]}"
shift
ONLY=("$@")
HOST="${RYZEN_HOST:-ryzen}"
source "$ROOT/scripts/perf.sh"          # perf_checkout, source_fingerprint
FINGERPRINT="$(source_fingerprint)"
HOME_R="$(ssh -o BatchMode=yes "$HOST" 'printf "%s" "$HOME"')"
SOURCE="$HOME_R/gothic-ai/perf-stand/$(perf_checkout "$ROOT")/source"
CONTENT="$HOME_R/gothic-ai/tree-cost/content"
RUN="tree-cost-$(date -u +%Y%m%dT%H%M%SZ)"
REMOTE_OUT="$HOME_R/gothic-ai/tree-cost/runs/$RUN"
COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
DIRTY="$(git -C "$ROOT" status --porcelain | wc -l | tr -d ' ')"

ssh "$HOST" mkdir -p "$SOURCE/OpenGothic" "$SOURCE/scripts/bench" "$REMOTE_OUT"
rsync -a --checksum --no-times --delete --exclude='build*/' --exclude='cmake-build-*/' --exclude='.git' \
  "$ROOT/OpenGothic/" "$HOST:$SOURCE/OpenGothic/"
rsync -a "$ROOT"/scripts/{ryzen-build.sh,perf.sh,perf_report.py,base-ensure.sh,ryzen_game.py} "$HOST:$SOURCE/scripts/"
rsync -a --delete --exclude='/build*/' "$ROOT/scripts/convert/" "$HOST:$SOURCE/scripts/convert/"
rsync -a "$ROOT"/scripts/bench/perf-*.bench "$HOST:$SOURCE/scripts/bench/"

ENV=(PERF_REMOTE=1 PERF_VID_RES=0 PERF_INI= PERF_WINDOW=2560x1440 PERF_REPEAT=1 PERF_WAIT=600 PERF_ARGS=
     "PERF_COMMIT=$COMMIT" "PERF_DIRTY=$DIRTY" "PERF_SOURCE_FINGERPRINT=$FINGERPRINT")
if [[ -z "${NO_BUILD:-}" ]]; then
  "$ROOT/scripts/gq" --host "$HOST" run --class quick --threads 8 --mem 12G --max 30m --wait-max 7200 \
    --name 'build tree-cost stand' -- env "${ENV[@]}" PERF_ENV= RYZEN_JOBS=8 \
    bash "$SOURCE/scripts/perf.sh" __build "$REMOTE_OUT/build"
fi

# name|PERF_ENV after the content and the pass markers
VARIANTS=(
  "C|"
  "F64|"
  "F256|"
  "F1024|"
  "F1024h|"
  "F1024q|"
  "F1024e|"
  "F1024-rt0|OPENGOTHIC_RT=0"
  "C-rt0|OPENGOTHIC_RT=0"
  "F1024-wind0|OPENGOTHIC_FOLIAGE=light"
  "F1024-crown0|OPENGOTHIC_CROWN_LIGHT=0"
)
set_of() {  # the set of a variant (bash 3 of macOS has no maps)
  case "${1%%-*}" in C) echo tree-cost-0 ;; F64) echo tree-cost-64 ;; F256) echo tree-cost-256 ;;
    F1024) echo tree-cost-1024 ;; F1024h) echo tree-cost-1024-h ;; F1024q) echo tree-cost-1024-q ;;
    F1024e) echo tree-cost-1024-e ;; esac
}
JOB="set -u"$'\n'
line() {  # name, round, extra env
  local name="$1" round="$2" extra="$3"
  JOB+="echo \"tree-cost: $name round $round\"; env ${ENV[*]} NO_BUILD=1 PERF_ENV='OPENGOTHIC_CONTENT=$CONTENT/$(set_of "$name") OPENGOTHIC_GPU_PASSES=1 $extra' bash $SOURCE/scripts/perf.sh __run $REMOTE_OUT/$name/round-$round trees || echo \"tree-cost: $name round $round FAILED\""$'\n'
}
if ((${#ONLY[@]})); then
  DONE=" "
  for name in "${ONLY[@]}"; do
    round=3; [[ "$DONE" == *" $name "* ]] && round=4
    DONE+="$name "
    for v in "${VARIANTS[@]}"; do if [[ "${v%%|*}" == "$name" ]]; then line "$name" "$round" "${v#*|}"; fi; done
  done
else
  for v in "${VARIANTS[@]}"; do line "${v%%|*}" 1 "${v#*|}"; done
  for ((k=${#VARIANTS[@]}-1; k>=0; k--)); do v="${VARIANTS[$k]}"; line "${v%%|*}" 2 "${v#*|}"; done
  line F1024-vsmstats 1 "OPENGOTHIC_VSM_STATS=1"
fi
"$ROOT/scripts/gq" --host "$HOST" run --class quiet --threads 4 --mem 5G --gpu-mem 4G --display --max 180m \
  --wait-max 7200 --name 'tree-cost runs' --archive "$REMOTE_OUT" -- bash -c "$JOB" 2>&1 | tee "$OUT.log"
mkdir -p "$OUT"
if ssh "$HOST" test -d "/mnt/shared/gothic-ai/tree-cost/runs/$RUN"; then
  rsync -a --exclude='*.png' "$HOST:/mnt/shared/gothic-ai/tree-cost/runs/$RUN/" "$OUT/"
else
  rsync -a --exclude='*.png' "$HOST:$REMOTE_OUT/" "$OUT/"
fi
echo "$OUT"
