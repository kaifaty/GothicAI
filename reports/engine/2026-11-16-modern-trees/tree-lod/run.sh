#!/usr/bin/env bash
# The runs of the price of tree-lod.md: the forest of 1 024 spruces of the tree cost stand (tree-cost/run.sh,
# scripts/bench/perf-trees.bench) with the levels of detail of `scripts/content/fir_set.py forest --levels` once with
# the levels off (OPENGOTHIC_MESH_LEVELS=0: the mesh itself everywhere, in the rays too) and once on, one Ryzen build,
# one quiet queue job (the user's limit of 2026-11-18: no control of our own, no repeats; the control without spruces
# and the noise are those of tree-cost.md). The set goes to ~/gothic-ai/tree-lod/content/ on Ryzen, its files shared
# with ~/gothic-ai/tree-cost/content/tree-cost-1024 by hard links where they are the same. The frames of the views come
# back too (outside git).
# BUILD_ONLY=1 stops after the build; NO_BUILD=1 keeps the stand's build of the same sources.
#   reports/engine/2026-11-16-modern-trees/tree-lod/run.sh /absolute/local/out [local set]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
OUT="${1:?usage: run.sh /absolute/local/out [local set]}"
SET="${2:-$HOME/.local/share/gothic-ai/remaster/trees-fir/tree-lod-1024}"
HOST="${RYZEN_HOST:-ryzen}"
source "$ROOT/scripts/perf.sh"          # perf_checkout, source_fingerprint
FINGERPRINT="$(source_fingerprint)"
HOME_R="$(ssh -o BatchMode=yes "$HOST" 'printf "%s" "$HOME"')"
SOURCE="$HOME_R/gothic-ai/perf-stand/$(perf_checkout "$ROOT")/source"
CONTENT="$HOME_R/gothic-ai/tree-lod/content"
RUN="tree-lod-$(date -u +%Y%m%dT%H%M%SZ)"
REMOTE_OUT="$HOME_R/gothic-ai/tree-lod/runs/$RUN"
COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
DIRTY="$(git -C "$ROOT" status --porcelain | wc -l | tr -d ' ')"

ssh "$HOST" "mkdir -p '$SOURCE/OpenGothic' '$SOURCE/scripts/bench' '$REMOTE_OUT' '$CONTENT'"
rsync -a --link-dest="$HOME_R/gothic-ai/tree-cost/content/tree-cost-1024/" "$SET/" "$HOST:$CONTENT/tree-lod-1024/"
rsync -a --checksum --no-times --delete --exclude='build*/' --exclude='cmake-build-*/' --exclude='.git' \
  "$ROOT/OpenGothic/" "$HOST:$SOURCE/OpenGothic/"
rsync -a "$ROOT"/scripts/{ryzen-build.sh,perf.sh,perf_report.py,base-ensure.sh,ryzen_game.py} "$HOST:$SOURCE/scripts/"
rsync -a --delete --exclude='/build*/' "$ROOT/scripts/convert/" "$HOST:$SOURCE/scripts/convert/"
rsync -a "$ROOT"/scripts/bench/perf-*.bench "$HOST:$SOURCE/scripts/bench/"

ENV=(PERF_REMOTE=1 PERF_VID_RES=0 PERF_INI= PERF_WINDOW=2560x1440 PERF_REPEAT=1 PERF_WAIT=600 PERF_ARGS=
     "PERF_COMMIT=$COMMIT" "PERF_DIRTY=$DIRTY" "PERF_SOURCE_FINGERPRINT=$FINGERPRINT")
if [[ -z "${NO_BUILD:-}" ]]; then
  "$ROOT/scripts/gq" --host "$HOST" run --class quick --threads 8 --mem 12G --max 30m --wait-max 7200 \
    --name 'build tree-lod stand' -- env "${ENV[@]}" PERF_ENV= RYZEN_JOBS=8 \
    bash "$SOURCE/scripts/perf.sh" __build "$REMOTE_OUT/build"
fi
[[ -z "${BUILD_ONLY:-}" ]] || exit 0

# name|switch, one run each
VARIANTS=("L-off|0" "L-on|1")
JOB="set -u"$'\n'
for v in "${VARIANTS[@]}"; do
  name="${v%%|*}" switch="${v#*|}"
  JOB+="echo \"tree-lod: $name\"; env ${ENV[*]} NO_BUILD=1 PERF_ENV='OPENGOTHIC_CONTENT=$CONTENT/tree-lod-1024 OPENGOTHIC_GPU_PASSES=1 OPENGOTHIC_MESH_LEVELS=$switch' bash $SOURCE/scripts/perf.sh __run $REMOTE_OUT/$name/round-1 trees || echo \"tree-lod: $name FAILED\""$'\n'
done
"$ROOT/scripts/gq" --host "$HOST" run --class quiet --threads 4 --mem 5G --gpu-mem 4G --display --max 40m \
  --wait-max 10800 --name 'tree-lod runs' --archive "$REMOTE_OUT" -- bash -c "$JOB" 2>&1 | tee "$OUT.log"
mkdir -p "$OUT"
if ssh "$HOST" test -d "/mnt/shared/gothic-ai/tree-lod/runs/$RUN"; then
  rsync -a "$HOST:/mnt/shared/gothic-ai/tree-lod/runs/$RUN/" "$OUT/"
else
  rsync -a "$HOST:$REMOTE_OUT/" "$OUT/"
fi
echo "$OUT"
