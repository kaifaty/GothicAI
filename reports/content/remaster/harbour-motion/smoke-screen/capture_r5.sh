#!/usr/bin/env bash
# capture_r5.sh ROUND SET... — one gq job: check the own particles and layer of every r5-* set, then capture every set
#   through harbour-motion-r5.bench (noon, then midnight) on base 15cdc40c; all sets with the same OPENGOTHIC_TEXTURES
#   (the own particle texture); frames to captures/<ROUND>/<SET>.
#   r5-* is harbour-motion-3 (hard links) with $LAYERS/r5-*/{harbour-motion.json,particles.d/}; others come from staging.
#   ENGINE=gothic-ai/... — the game build (default: the shared remaster-engine; r5 ran on its 31.10 22:22 build);
#   INI="SECTION.key=value ..." — Gothic.ini settings for every set (e.g. RENDERER_D3D.zFogRadial=0, the LQ fog).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../../../../.." && pwd)"
LAYERS="${LAYERS:?dir of gen_r5.py sets}"; TEXTURES="${TEXTURES:?dir with OWN_HUT_SMOKE.png from own_texture.py}"
TASK=gothic-ai/remaster-harbour-motion; HOST=ryzen; STAGING=~/.local/share/gothic-ai/content-staging
ENGINE="${ENGINE:-gothic-ai/remaster-engine}"; INI="${INI:-}"
# the layer checks keep the shared build's content-convert: the game build may have only Gothic2Notr
BUILD=gothic-ai/remaster-engine/build/opengothic; BENCH="$ROOT/scripts/content/harbour-motion-r5.bench"
BASE_ENV="OPENGOTHIC_BASE_CACHE=\$HOME/$TASK/base-cache OPENGOTHIC_BASE_STORE=/mnt/shared/$TASK"
round="$1"; shift
rsync -a "$ROOT/scripts/content/capture-quality-pilot.sh" "$ROOT/scripts/content/capture-quality-pilot.py" \
  "$ROOT/scripts/base-ensure.sh" "$ROOT/scripts/ryzen_game.py" "$BENCH" "$HOST:$TASK/scripts/" || exit 1
rsync -a --delete "$TEXTURES/" "$HOST:$TASK/textures/" || exit 1
cmd="set -o pipefail; fp=\$(cat $TASK/base-fp.txt); export $BASE_ENV OPENGOTHIC_BASE_FINGERPRINT=\$fp OPENGOTHIC_VIDEOS_FINGERPRINT="
cmd+=" QUALITY_PILOT_TIMEOUT=600 QUALITY_PILOT_TASK=$TASK QUALITY_PILOT_TEXTURES=\$HOME/$TASK/textures"
cmd+=" QUALITY_PILOT_BUILD_TASK=$ENGINE QUALITY_PILOT_INI='$INI'; rc=0; mkdir -p $TASK/out/$round"
cmd+="; b=\$OPENGOTHIC_BASE_CACHE/\$fp"
for v in "$@"; do
  if [[ $v == r5-* ]]; then
    rsync -a --delete "$STAGING/harbour-motion-3/" "$HOST:$TASK/content/harbour-motion-3/" || exit 1
    ssh -T $HOST "rm -rf ~/$TASK/content/$v && cp -al ~/$TASK/content/harbour-motion-3 ~/$TASK/content/$v" || exit 1
    rsync -a "$LAYERS/$v/harbour-motion.json" "$HOST:$TASK/content/$v/worlds/NEWWORLD/vobs.d/harbour-motion.json" || exit 1
    rsync -a "$LAYERS/$v/particles.d" "$HOST:$TASK/content/$v/" || exit 1
    cmd+="; $BUILD/content-convert check-particles \$b $TASK/content/$v || rc=1"
    cmd+="; $BUILD/content-convert check-vob-layer \$b/worlds/NEWWORLD $TASK/content/$v/worlds/NEWWORLD/vobs.d/harbour-motion.json $TASK/content/$v || rc=1"
  else
    rsync -a --delete "$STAGING/$v/" "$HOST:$TASK/content/$v/" || exit 1
  fi
  ssh -T $HOST "rm -rf ~/$TASK/runs/$v ~/$TASK/out/$round/$v"
  cmd+="; QUALITY_PILOT_SCENARIO=harbour-motion-r5.bench bash $TASK/scripts/capture-quality-pilot.sh --remote-capture $v || rc=1"
  cmd+="; mkdir -p $TASK/out/$round/$v; cp $TASK/runs/$v/*.png $TASK/out/$round/$v/ 2>/dev/null"
  cmd+="; for f in game.log bench.log; do cp $TASK/runs/$v/\$f $TASK/out/$round/$v/\$f; done; rm -f $TASK/runs/$v/*.png"
done
cmd+="; exit \$rc"
echo "== $round $(date +%T) $ENGINE ${INI:+[$INI] }$*"
env GQ_OWNER=remaster-harbour-motion "$ROOT/scripts/gq" --host $HOST run --class quick --threads 4 --mem 4G --gpu-mem 4G \
  --display --display-exclusive --max 80m --wait-max 43200 --name "harbour-motion smoke $round" -- bash -c "$cmd"
rc=$?
C=~/.local/share/gothic-ai/remaster/harbour-motion/captures/$round; mkdir -p "$C"
rsync -a "$HOST:$TASK/out/$round/" "$C/"
echo "rc=$rc $(date +%T)"
