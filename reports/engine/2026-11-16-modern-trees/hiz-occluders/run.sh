#!/usr/bin/env bash
# The runs of hiz-occluders.md: the forest of 1 024 spruces of the tree cost stand (tree-cost/run.sh,
# scripts/bench/perf-trees.bench) once with the crowns in the occluder pass (off: OPENGOTHIC_HIZ_SKIP_CROWNS=0, the
# code of main) and once out of it (on), on one Ryzen build, in one quiet queue job (the user's limit of 2026-11-18:
# no control of our own, no repeats; the control without spruces is the probe's). The set is a copy of
# ~/gothic-ai/tree-cost/content/tree-cost-1024 in ~/gothic-ai/tree-hiz/content/. The frames of the views come back
# too (outside git).
# BUILD_ONLY=1 stops after the build; NO_BUILD=1 keeps the stand's build of the same sources.
#   reports/engine/2026-11-16-modern-trees/hiz-occluders/run.sh /absolute/local/out
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
OUT="${1:?usage: run.sh /absolute/local/out}"
HOST="${RYZEN_HOST:-ryzen}"
source "$ROOT/scripts/perf.sh"          # perf_checkout, source_fingerprint
FINGERPRINT="$(source_fingerprint)"
HOME_R="$(ssh -o BatchMode=yes "$HOST" 'printf "%s" "$HOME"')"
SOURCE="$HOME_R/gothic-ai/perf-stand/$(perf_checkout "$ROOT")/source"
CONTENT="$HOME_R/gothic-ai/tree-hiz/content"
RUN="tree-hiz-$(date -u +%Y%m%dT%H%M%SZ)"
REMOTE_OUT="$HOME_R/gothic-ai/tree-hiz/runs/$RUN"
COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
DIRTY="$(git -C "$ROOT" status --porcelain | wc -l | tr -d ' ')"

ssh "$HOST" "mkdir -p '$SOURCE/OpenGothic' '$SOURCE/scripts/bench' '$REMOTE_OUT' '$CONTENT'
  [ -d '$CONTENT/tree-cost-1024' ] || cp -a ~/gothic-ai/tree-cost/content/tree-cost-1024 '$CONTENT/'"
rsync -a --checksum --no-times --delete --exclude='build*/' --exclude='cmake-build-*/' --exclude='.git' \
  "$ROOT/OpenGothic/" "$HOST:$SOURCE/OpenGothic/"
rsync -a "$ROOT"/scripts/{ryzen-build.sh,perf.sh,perf_report.py,perf_gpu_log.py,base-ensure.sh,ryzen_game.py} "$HOST:$SOURCE/scripts/"
rsync -a --delete --exclude='/build*/' "$ROOT/scripts/convert/" "$HOST:$SOURCE/scripts/convert/"
rsync -a "$ROOT"/scripts/bench/perf-*.bench "$HOST:$SOURCE/scripts/bench/"

ENV=(PERF_REMOTE=1 PERF_VID_RES=0 PERF_INI= PERF_WINDOW=2560x1440 PERF_REPEAT=1 PERF_WAIT=600 PERF_ARGS=
     "PERF_COMMIT=$COMMIT" "PERF_DIRTY=$DIRTY" "PERF_SOURCE_FINGERPRINT=$FINGERPRINT")
if [[ -z "${NO_BUILD:-}" ]]; then
  "$ROOT/scripts/gq" --host "$HOST" run --class quick --threads 8 --mem 12G --max 30m --wait-max 7200 \
    --name 'build tree-hiz stand' -- env "${ENV[@]}" PERF_ENV= RYZEN_JOBS=8 \
    bash "$SOURCE/scripts/perf.sh" __build "$REMOTE_OUT/build"
fi
[[ -z "${BUILD_ONLY:-}" ]] || exit 0

# name|set|switch, one run each
VARIANTS=("F-off|tree-cost-1024|0" "F-on|tree-cost-1024|1")
JOB="set -u"$'\n'
line() {  # variant, round
  local name="${1%%|*}" rest="${1#*|}"
  local set="${rest%%|*}" switch="${rest#*|}"
  JOB+="echo \"tree-hiz: $name round $2\"; env ${ENV[*]} NO_BUILD=1 PERF_ENV='OPENGOTHIC_CONTENT=$CONTENT/$set OPENGOTHIC_GPU_PASSES=1 OPENGOTHIC_HIZ_SKIP_CROWNS=$switch' bash $SOURCE/scripts/perf.sh __run $REMOTE_OUT/$name/round-$2 trees || echo \"tree-hiz: $name round $2 FAILED\""$'\n'
}
for v in "${VARIANTS[@]}"; do line "$v" 1; done
"$ROOT/scripts/gq" --host "$HOST" run --class quiet --threads 4 --mem 5G --gpu-mem 4G --display --max 40m \
  --wait-max 10800 --name 'tree-hiz runs' --archive "$REMOTE_OUT" -- bash -c "$JOB" 2>&1 | tee "$OUT.log"
mkdir -p "$OUT"
if ssh "$HOST" test -d "/mnt/shared/gothic-ai/tree-hiz/runs/$RUN"; then
  rsync -a "$HOST:/mnt/shared/gothic-ai/tree-hiz/runs/$RUN/" "$OUT/"
else
  rsync -a "$HOST:$REMOTE_OUT/" "$OUT/"
fi
echo "$OUT"
