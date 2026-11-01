#!/usr/bin/env bash
# capture_clip.sh ROUND SET... — one gq job: check the own particles and layer of every set, then capture every set
#   through harbour-motion-clip.bench (huts and huts-north: shots after 13 s, then a 15 s clip; noon, then midnight)
#   on base 15cdc40c; frames and clips (MP4) to captures/<ROUND>/<SET>. Sets come from content-staging as they are:
#   no OPENGOTHIC_TEXTURES, so the own smoke sprite must come from the set's content/materials (E4).
#   ENGINE=gothic-ai/... — the game build (default: the shared remaster-engine).
#   LAYERS=DIR — a set with DIR/<SET>/particles.d is BASE_SET (default harbour-motion-4, hard links on Ryzen) with
#   those particle descriptions (gen_r7.py); other sets come from content-staging.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../../../../.." && pwd)"
TASK=gothic-ai/remaster-harbour-motion; HOST=ryzen; STAGING=~/.local/share/gothic-ai/content-staging
ENGINE="${ENGINE:-gothic-ai/remaster-engine}"; LAYERS="${LAYERS:-}"; BASE_SET="${BASE_SET:-harbour-motion-4}"
# the layer checks keep the shared build's content-convert: the game build may have only Gothic2Notr
BUILD=gothic-ai/remaster-engine/build/opengothic; BENCH="$ROOT/scripts/content/harbour-motion-clip.bench"
BASE_ENV="OPENGOTHIC_BASE_CACHE=\$HOME/$TASK/base-cache OPENGOTHIC_BASE_STORE=/mnt/shared/$TASK"
round="$1"; shift
rsync -a "$ROOT/scripts/content/capture-quality-pilot.sh" "$ROOT/scripts/content/capture-quality-pilot.py" \
  "$ROOT/scripts/base-ensure.sh" "$ROOT/scripts/ryzen_game.py" "$BENCH" "$HOST:$TASK/scripts/" || exit 1
cmd="set -o pipefail; fp=\$(cat $TASK/base-fp.txt); export $BASE_ENV OPENGOTHIC_BASE_FINGERPRINT=\$fp OPENGOTHIC_VIDEOS_FINGERPRINT="
cmd+=" QUALITY_PILOT_TIMEOUT=600 QUALITY_PILOT_TASK=$TASK QUALITY_PILOT_BUILD_TASK=$ENGINE; rc=0; mkdir -p $TASK/out/$round"
cmd+="; b=\$OPENGOTHIC_BASE_CACHE/\$fp"
for v in "$@"; do
  if [[ -n "$LAYERS" && -d "$LAYERS/$v/particles.d" ]]; then
    rsync -a --delete "$STAGING/$BASE_SET/" "$HOST:$TASK/content/$BASE_SET/" || exit 1
    ssh -T $HOST "rm -rf ~/$TASK/content/$v && cp -al ~/$TASK/content/$BASE_SET ~/$TASK/content/$v" || exit 1
    rsync -a "$LAYERS/$v/particles.d/" "$HOST:$TASK/content/$v/particles.d/" || exit 1
  else
    rsync -a --delete "$STAGING/$v/" "$HOST:$TASK/content/$v/" || exit 1
  fi
  if [[ -d "$STAGING/$v/particles.d" || -n "$LAYERS" && -d "$LAYERS/$v/particles.d" ]]; then
    cmd+="; $BUILD/content-convert check-particles \$b $TASK/content/$v || rc=1"
    cmd+="; $BUILD/content-convert check-vob-layer \$b/worlds/NEWWORLD $TASK/content/$v/worlds/NEWWORLD/vobs.d/harbour-motion.json $TASK/content/$v || rc=1"
  fi
  ssh -T $HOST "rm -rf ~/$TASK/runs/$v ~/$TASK/out/$round/$v"
  cmd+="; QUALITY_PILOT_SCENARIO=harbour-motion-clip.bench bash $TASK/scripts/capture-quality-pilot.sh --remote-capture $v || rc=1"
  cmd+="; mkdir -p $TASK/out/$round/$v; cp $TASK/runs/$v/*.png $TASK/runs/$v/*.mp4 $TASK/out/$round/$v/ 2>/dev/null"
  cmd+="; for f in game.log bench.log census.json; do cp $TASK/runs/$v/\$f $TASK/out/$round/$v/\$f; done; rm -f $TASK/runs/$v/*.png $TASK/runs/$v/*.mp4"
done
cmd+="; exit \$rc"
echo "== $round $(date +%T) $ENGINE $*"
env GQ_OWNER=remaster-harbour-motion "$ROOT/scripts/gq" --host $HOST run --class quick --threads 4 --mem 4G --gpu-mem 4G \
  --display --display-exclusive --max 60m --wait-max 43200 --name "harbour-motion smoke clip $round" -- bash -c "$cmd"
rc=$?
C=~/.local/share/gothic-ai/remaster/harbour-motion/captures/$round; mkdir -p "$C"
rsync -a "$HOST:$TASK/out/$round/" "$C/"
echo "rc=$rc $(date +%T)"
