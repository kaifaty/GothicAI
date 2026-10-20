#!/usr/bin/env bash
# Capture original / city-3 / pilot-b / pilot-c / pilot-2 / pilot-3a / pilot-3b on Ryzen.
# --prepare builds this worktree's unchanged engine through gq.
# --capture VARIANT OUTPUT copies only the requested staging set and captures it.
# --compose CAPTURES OUTPUT writes seven original / city-3 / B / C boards.
# --compose-2 CAPTURES OUTPUT writes seven original / city-3 / B / pilot-2 boards.
# --compose-3 CAPTURES OUTPUT writes seven original / pilot-2 / pilot-3a / pilot-3b boards.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HOST="${RYZEN_HOST:-ryzen}"
TASK="${QUALITY_PILOT_TASK:-gothic-ai/quality-pilot}"

if [[ "${1:-}" == --remote-prepare ]]; then
  task="$HOME/$TASK"
  export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
  export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
  mkdir -p "$task/build"
  cmake -S "$task/source/OpenGothic" -B "$task/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib" \
    >"$task/build/configure.log" 2>&1 || { tail -40 "$task/build/configure.log"; exit 1; }
  cmake --build "$task/build" --target Gothic2Notr --parallel 8 \
    >"$task/build/build.log" 2>&1 || { tail -60 "$task/build/build.log"; exit 1; }
  tail -5 "$task/build/build.log"
  exit 0
fi

if [[ "${1:-}" == --remote-capture ]]; then
  variant="${2:?variant}"
  [[ "$variant" == original || "$variant" == city-3 || "$variant" == pilot-b || "$variant" == pilot-c || "$variant" == pilot-2 || "$variant" == pilot-3a || "$variant" == pilot-3b ]] || exit 2
  task="$HOME/$TASK"
  out="$task/runs/$variant"
  mkdir -p "$out/run"
  cp "$task/save_slot_1.sav" "$out/run/save_slot_1.sav"
  cat >"$out/run/Gothic.ini" <<'INI'
[INTERNAL]
vidResIndex=0
[SOUND]
soundVolume=0
musicVolume=0
musicEnabled=0
INI
  export XDG_RUNTIME_DIR="/run/user/$(id -u)"
  export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
  connected=0
  for auth in "$XDG_RUNTIME_DIR"/.mutter-Xwaylandauth.* "$HOME/.Xauthority"; do
    [[ -r "$auth" ]] || continue
    for socket in /tmp/.X11-unix/X*; do
      [[ -S "$socket" ]] || continue
      if DISPLAY=":${socket##*/X}" XAUTHORITY="$auth" xdpyinfo >/dev/null 2>&1; then
        export DISPLAY=":${socket##*/X}" XAUTHORITY="$auth"
        connected=1; break 2
      fi
    done
  done
  [[ "$connected" == 1 ]] || { echo 'No accessible Xwayland desktop'; exit 1; }
  export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json
  export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
  export ALSOFT_DRIVERS=null TEMPEST_BACKGROUND=1 TEMPEST_WINDOW_SIZE=1280x720
  export TEMPEST_WINDOW_POS=100,100 GOTHIC_AI=0
  export OPENGOTHIC_CONTENT="$task/content/$variant"
  export OPENGOTHIC_TEXTURES='' OPENGOTHIC_FRACTURE=''
  export OPENGOTHIC_CENSUS="$out/census.json"
  export OPENGOTHIC_BENCH="$task/scripts/quality-pilot.bench" OPENGOTHIC_BENCH_OUT="$out"
  rc=0
  (cd "$out/run" && timeout 240 "$task/build/opengothic/Gothic2Notr" \
    -g "$HOME/gothic-ai/game" -g2 -window -devmode -save 1) >"$out/game.log" 2>&1 || rc=$?
  printf '%s\n' "$rc" >"$out/exit.txt"
  [[ "$rc" == 0 ]] || { tail -30 "$out/game.log"; exit "$rc"; }
  check_args=()
  [[ "$TASK" != gothic-ai/quality-pilot-3 ]] || check_args+=(--iteration-3)
  python3 "$task/scripts/capture-quality-pilot.py" check "$out" "$task/scripts/quality-pilot.bench" "$task/content/$variant/materials/materials.json" "${check_args[@]}"
  exit 0
fi

if [[ "${1:-}" == --compose || "${1:-}" == --compose-2 || "${1:-}" == --compose-3 ]]; then
  exec uv run --no-project --with pillow python "$ROOT/scripts/content/capture-quality-pilot.py" "${1#--}" "${2:?captures}" "${3:?output}"
fi

if [[ "${1:-}" == --prepare ]]; then
  ssh -T "$HOST" "mkdir -p ~/$TASK/source/OpenGothic ~/$TASK/scripts ~/$TASK/content/original/materials ~/$TASK/runs; df -h ~"
  rsync -a --delete --exclude=.git --exclude=build --exclude=__pycache__ \
    "$ROOT/OpenGothic/" "$HOST:$TASK/source/OpenGothic/"
  rsync -a "$ROOT/scripts/content/capture-quality-pilot.sh" "$ROOT/scripts/content/capture-quality-pilot.py" \
    "$ROOT/scripts/content/quality-pilot.bench" "$HOST:$TASK/scripts/"
  common="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)"
  rsync -a "${SAVE:-$(dirname "$common")/save_slot_1.sav}" "$HOST:$TASK/save_slot_1.sav"
  GQ_OWNER="${TASK##*/}" "$ROOT/scripts/gq" --host "$HOST" run --class quick \
    --threads 8 --mem 10G --max 25m --wait-max 900 --name 'pilot engine build' -- \
    env QUALITY_PILOT_TASK="$TASK" bash "$TASK/scripts/capture-quality-pilot.sh" --remote-prepare
  exit 0
fi

if [[ "${1:-}" == --capture ]]; then
  variant="${2:?variant}"; out="${3:?output}"
  [[ "$variant" == original || "$variant" == city-3 || "$variant" == pilot-b || "$variant" == pilot-c || "$variant" == pilot-2 || "$variant" == pilot-3a || "$variant" == pilot-3b ]] || exit 2
  mkdir -p "$out/$variant"
  if [[ "$variant" != original ]]; then
    source_root="${CONTENT_STAGING_ROOT:-$HOME/.local/share/gothic-ai/content-staging}/$variant"
    [[ -s "$source_root/materials/materials.json" ]] || { echo "missing packed set: $source_root"; exit 2; }
    ssh -T "$HOST" "mkdir -p ~/$TASK/content/$variant"
    rsync -a --delete "$source_root/" "$HOST:$TASK/content/$variant/"
  else
    ssh -T "$HOST" "printf '{}\\n' >~/$TASK/content/original/materials/materials.json"
  fi
  rsync -a "$ROOT/scripts/content/capture-quality-pilot.sh" "$ROOT/scripts/content/capture-quality-pilot.py" \
    "$ROOT/scripts/content/quality-pilot.bench" "$HOST:$TASK/scripts/"
  rc=0
  GQ_OWNER="${TASK##*/}" "$ROOT/scripts/gq" --host "$HOST" run --class quick \
    --threads 4 --mem 5G --gpu-mem 4G --display --display-exclusive \
    --max 6m --wait-max 900 --name "pilot $variant captures" -- \
    env QUALITY_PILOT_TASK="$TASK" bash "$TASK/scripts/capture-quality-pilot.sh" --remote-capture "$variant" || rc=$?
  rsync -a "$HOST:$TASK/runs/$variant/" "$out/$variant/"
  exit "$rc"
fi
echo 'usage: capture-quality-pilot.sh --prepare | --capture original|city-3|pilot-b|pilot-c|pilot-2|pilot-3a|pilot-3b OUTPUT | --compose|--compose-2|--compose-3 CAPTURES OUTPUT' >&2
exit 2
