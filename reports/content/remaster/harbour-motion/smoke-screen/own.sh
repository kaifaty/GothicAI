#!/usr/bin/env bash
# own.sh prepare — one gq job without a window: build Gothic2Notr and content-convert of this worktree in the
#   shared remaster engine, import the base of the current format into this task's cache (Ryzen) and store
#   (NAS), print its fingerprint into $TASK/base-fp.txt.
# own.sh capture BENCH ROUND SET... — check own particles and layer of every own-* set, then capture every set
#   through BENCH on that base; frames to captures/<ROUND>/<SET>.
#   A set own-* is harbour-motion-2 on Ryzen (hard links) with $LAYERS/own-*/{harbour-motion.json,particles.d/}.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../../../../.." && pwd)"
LAYERS="${LAYERS:-${TMPDIR:-/tmp}/harbour-motion-own-smoke}"  # output of gen_own.py
TASK=gothic-ai/remaster-harbour-motion; HOST=ryzen; STAGING=~/.local/share/gothic-ai/content-staging
BUILD=gothic-ai/remaster-engine/build/opengothic
BASE_ENV="OPENGOTHIC_BASE_CACHE=\$HOME/$TASK/base-cache OPENGOTHIC_BASE_STORE=/mnt/shared/$TASK"
GQ="env GQ_OWNER=remaster-harbour-motion $ROOT/scripts/gq --host $HOST run --class quick"
case "$1" in
prepare)
  rsync -a --delete --exclude=.git --exclude=build --exclude=__pycache__ "$ROOT/OpenGothic/" "$HOST:gothic-ai/remaster-engine/source/OpenGothic/"
  rsync -a --delete --exclude='/build*/' "$ROOT/scripts/convert/" "$HOST:gothic-ai/remaster-engine/source/scripts/convert/"
  rsync -a "$ROOT/scripts/content/capture-quality-pilot.sh" "$ROOT/scripts/content/capture-quality-pilot.py" \
    "$ROOT/scripts/base-ensure.sh" "$ROOT/scripts/ryzen_game.py" "$HOST:$TASK/scripts/"
  job='set -e; t=$HOME/gothic-ai/remaster-engine
    export VULKAN_SDK=$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0 PATH=$HOME/.local/bin:$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0/bin:$PATH
    cmake -S $t/source/OpenGothic -B $t/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_CXX_FLAGS=-I$VULKAN_SDK/include -DCMAKE_EXE_LINKER_FLAGS=-L$VULKAN_SDK/lib >$t/build/configure.log 2>&1 || { tail -40 $t/build/configure.log; exit 1; }
    cmake --build $t/build --target Gothic2Notr content-convert --parallel 2 >$t/build/build.log 2>&1 || { tail -60 $t/build/build.log; exit 1; }
    tail -2 $t/build/build.log
    mkdir -p $HOME/'"$TASK"'/base-cache /mnt/shared/'"$TASK"'
    base=$(env '"$BASE_ENV"' $t/build/opengothic/content-convert resolve-base $HOME/gothic-ai/game 2>$HOME/'"$TASK"'/base-import.log)
    tail -3 $HOME/'"$TASK"'/base-import.log; echo "base: $base"; basename "$base" >$HOME/'"$TASK"'/base-fp.txt'
  $GQ --threads 4 --mem 6G --max 90m --wait-max 10800 --name 'remaster engine + base (harbour-motion r5)' -- bash -c "$job"
  echo "rc=$?";;
capture)
  benches="$2" round="$3"; shift 3  # BENCH may be a comma-separated list: each set goes through each
  IFS=, read -ra BENCHES <<<"$benches"
  rsync -a "$ROOT/scripts/content/capture-quality-pilot.sh" "$ROOT/scripts/content/capture-quality-pilot.py" \
    "$ROOT/scripts/base-ensure.sh" "$ROOT/scripts/ryzen_game.py" "${BENCHES[@]}" "$HOST:$TASK/scripts/"
  cmd="set -o pipefail; fp=\$(cat $TASK/base-fp.txt); export $BASE_ENV OPENGOTHIC_BASE_FINGERPRINT=\$fp OPENGOTHIC_VIDEOS_FINGERPRINT="
  cmd+=" QUALITY_PILOT_TIMEOUT=600 QUALITY_PILOT_TASK=$TASK; rc=0; mkdir -p $TASK/out/$round"
  cmd+="; b=\$OPENGOTHIC_BASE_CACHE/\$fp"
  for v in "$@"; do
    if [[ $v == own-* ]]; then
      rsync -a --delete "$STAGING/harbour-motion-2/" "$HOST:$TASK/content/harbour-motion-2/"
      ssh -T $HOST "rm -rf ~/$TASK/content/$v && cp -al ~/$TASK/content/harbour-motion-2 ~/$TASK/content/$v"
      rsync -a "$LAYERS/$v/harbour-motion.json" "$HOST:$TASK/content/$v/worlds/NEWWORLD/vobs.d/harbour-motion.json"
      rsync -a "$LAYERS/$v/particles.d" "$HOST:$TASK/content/$v/"
      cmd+="; $BUILD/content-convert check-particles \$b $TASK/content/$v || rc=1"
      cmd+="; $BUILD/content-convert check-vob-layer \$b/worlds/NEWWORLD $TASK/content/$v/worlds/NEWWORLD/vobs.d/harbour-motion.json $TASK/content/$v || rc=1"
    else
      rsync -a --delete "$STAGING/$v/" "$HOST:$TASK/content/$v/"
      if [[ -d "$STAGING/$v/particles.d" ]]; then
        cmd+="; $BUILD/content-convert check-particles \$b $TASK/content/$v || rc=1"
        cmd+="; $BUILD/content-convert check-vob-layer \$b/worlds/NEWWORLD $TASK/content/$v/worlds/NEWWORLD/vobs.d/harbour-motion.json $TASK/content/$v || rc=1"
      fi
    fi
    ssh -T $HOST "rm -rf ~/$TASK/runs/$v ~/$TASK/out/$round/$v"
    for bench in "${BENCHES[@]}"; do
      name=$(basename "$bench")
      cmd+="; QUALITY_PILOT_SCENARIO=$name bash $TASK/scripts/capture-quality-pilot.sh --remote-capture $v || rc=1"
      cmd+="; mkdir -p $TASK/out/$round/$v; cp $TASK/runs/$v/*.png $TASK/out/$round/$v/ 2>/dev/null"
      cmd+="; for f in game.log bench.log; do cp $TASK/runs/$v/\$f $TASK/out/$round/$v/${name%.bench}-\$f; done; rm -f $TASK/runs/$v/*.png"
    done
  done
  cmd+="; exit \$rc"
  echo "== $round $(date +%T) $*"
  $GQ --threads 4 --mem 4G --gpu-mem 4G --display --display-exclusive --max 60m --wait-max 10800 \
    --name "harbour-motion $round" -- bash -c "$cmd"
  rc=$?
  C=~/.local/share/gothic-ai/remaster/harbour-motion/captures/$round; mkdir -p "$C"
  rsync -a "$HOST:$TASK/out/$round/" "$C/"
  echo "rc=$rc $(date +%T)";;
esac
