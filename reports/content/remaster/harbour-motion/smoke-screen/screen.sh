#!/usr/bin/env bash
# screen.sh prepare            — rebuild the shared remaster engine from this worktree (gq quick, no window)
# screen.sh capture BENCH SET... — one gq job: every set through BENCH; frames to captures/<round>/<set>
#   a set smoke-* is harbour-motion-2 on Ryzen (hard links) with $LAYERS/smoke-*/harbour-motion.json
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../../../../.." && pwd)"
LAYERS="${LAYERS:-${TMPDIR:-/tmp}/harbour-motion-smoke-layers}"  # output of gen_variants.py
TASK=gothic-ai/remaster-harbour-motion; HOST=ryzen; STAGING=~/.local/share/gothic-ai/content-staging
GQ="env GQ_OWNER=remaster-harbour-motion $ROOT/scripts/gq --host $HOST run --class quick"
case "$1" in
prepare)
  rsync -a --delete --exclude=.git --exclude=build --exclude=__pycache__ "$ROOT/OpenGothic/" "$HOST:gothic-ai/remaster-engine/source/OpenGothic/"
  rsync -a --delete --exclude='/build*/' "$ROOT/scripts/convert/" "$HOST:gothic-ai/remaster-engine/source/scripts/convert/"
  rsync -a "$ROOT/scripts/content/capture-quality-pilot.sh" "$ROOT/scripts/content/capture-quality-pilot.py" \
    "$ROOT/scripts/base-ensure.sh" "$ROOT/scripts/ryzen_game.py" "$HOST:$TASK/scripts/"
  # as capture-quality-pilot.sh --remote-prepare, but two compilers in 4 GB: it fits beside a 16 GB batch job
  build='eval "$(python3 gothic-ai/remaster-harbour-motion/scripts/ryzen_game.py)"; t=$HOME/gothic-ai/remaster-engine
    export VULKAN_SDK=$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0 PATH=$HOME/.local/bin:$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0/bin:$PATH
    cmake -S $t/source/OpenGothic -B $t/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_CXX_FLAGS=-I$VULKAN_SDK/include -DCMAKE_EXE_LINKER_FLAGS=-L$VULKAN_SDK/lib >$t/build/configure.log 2>&1 || { tail -40 $t/build/configure.log; exit 1; }
    cmake --build $t/build --target Gothic2Notr --parallel ${PARALLEL:-2} >$t/build/build.log 2>&1 || { tail -60 $t/build/build.log; exit 1; }
    tail -3 $t/build/build.log'
  $GQ --threads 4 --mem ${MEM:-4G} --max 90m --wait-max 7200 --name 'remaster engine build (harbour-motion r4)' -- \
    bash -c "PARALLEL=${PARALLEL:-2}; $build"
  echo "rc=$?";;
capture)
  bench="$2" round="$3"; shift 3
  rsync -a "$ROOT/scripts/content/capture-quality-pilot.sh" "$ROOT/scripts/content/capture-quality-pilot.py" \
    "$ROOT/scripts/base-ensure.sh" "$ROOT/scripts/ryzen_game.py" "$bench" "$HOST:$TASK/scripts/"
  name=$(basename "$bench")
  cmd="export QUALITY_PILOT_TIMEOUT=600 QUALITY_PILOT_TASK=$TASK QUALITY_PILOT_SCENARIO=$name; rc=0; mkdir -p $TASK/out/$round"
  for v in "$@"; do
    if [[ $v == smoke-* ]]; then
      rsync -a --delete "$STAGING/harbour-motion-2/" "$HOST:$TASK/content/harbour-motion-2/"
      ssh -T $HOST "rm -rf ~/$TASK/content/$v && cp -al ~/$TASK/content/harbour-motion-2 ~/$TASK/content/$v"
      rsync -a "$LAYERS/$v/harbour-motion.json" "$HOST:$TASK/content/$v/worlds/NEWWORLD/vobs.d/harbour-motion.json"
    else
      rsync -a --delete "$STAGING/$v/" "$HOST:$TASK/content/$v/"
    fi
    ssh -T $HOST "rm -rf ~/$TASK/runs/$v ~/$TASK/out/$round/$v"
    cmd+="; bash $TASK/scripts/capture-quality-pilot.sh --remote-capture $v || rc=1"
    cmd+="; mkdir -p $TASK/out/$round/$v; cp $TASK/runs/$v/*.png $TASK/runs/$v/game.log $TASK/runs/$v/bench.log $TASK/out/$round/$v/ 2>/dev/null"
  done
  cmd+="; exit \$rc"
  echo "== $round $(date +%T) $*"
  $GQ --threads 4 --mem 4G --gpu-mem 4G --display --display-exclusive --max 60m --wait-max 7200 \
    --name "harbour-motion $round" -- bash -c "$cmd"
  rc=$?
  C=~/.local/share/gothic-ai/remaster/harbour-motion/captures/$round; mkdir -p "$C"
  rsync -a "$HOST:$TASK/out/$round/" "$C/"
  echo "rc=$rc $(date +%T)";;
esac
