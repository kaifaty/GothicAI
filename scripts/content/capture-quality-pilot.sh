#!/usr/bin/env bash
# Capture original / city-3 / pilot-b / pilot-c / pilot-2 / pilot-3a / pilot-3b on Ryzen.
# --prepare builds this worktree's unchanged engine through gq.
# --capture VARIANT OUTPUT copies only the requested staging set and captures it.
# --compose CAPTURES OUTPUT writes seven original / city-3 / B / C boards.
# --compose-2 CAPTURES OUTPUT writes seven original / city-3 / B / pilot-2 boards.
# --compose-3 CAPTURES OUTPUT writes seven original / pilot-2 / pilot-3a / pilot-3b boards.
# QUALITY_PILOT_TASK=gothic-ai/market-house selects market-house.bench (three views).
# QUALITY_PILOT_TASK=gothic-ai/harbour-house selects harbour-house.bench (twelve original facade views).
# QUALITY_PILOT_TASK=gothic-ai/harbour-hut selects harbour-hut.bench (six views, normal game FOV; original or a staging set such as hut-1).
# QUALITY_PILOT_TASK=gothic-ai/remaster-ASSET selects ASSET.bench; a set must replace every entry of its own index.
# QUALITY_PILOT_SCENARIO=FILE.bench overrides the scenario, e.g. the same cameras at another time of day.
# --compose-house CAPTURES OUTPUT writes original / pilot-3b / house-1 boards.
# --compose-house-2 CAPTURES OUTPUT writes original / house-1 / house-2 boards.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HOST="${RYZEN_HOST:-ryzen}"
TASK="${QUALITY_PILOT_TASK:-gothic-ai/quality-pilot}"
SCENARIO=quality-pilot.bench
[[ "$TASK" != gothic-ai/market-house ]] || SCENARIO=market-house.bench
[[ "$TASK" != gothic-ai/harbour-house ]] || SCENARIO=harbour-house.bench
[[ "$TASK" != gothic-ai/harbour-hut ]] || SCENARIO=harbour-hut.bench
[[ "$TASK" != gothic-ai/remaster-* ]] || SCENARIO="${TASK#gothic-ai/remaster-}.bench"
SCENARIO="${QUALITY_PILOT_SCENARIO:-$SCENARIO}"
[[ "$SCENARIO" =~ ^[a-z0-9-]+\.bench$ ]] || { echo "bad scenario name: $SCENARIO" >&2; exit 2; }
# Remaster assets share one engine build; each keeps its own scripts, content and runs.
BUILD_TASK="$TASK"
[[ "$TASK" != gothic-ai/remaster-* ]] || BUILD_TASK=gothic-ai/remaster-engine

if [[ "${1:-}" == --remote-prepare ]]; then
  game_environment="$(python3 "$(dirname "$0")/ryzen_game.py")"
  eval "$game_environment"
  task="$HOME/$BUILD_TASK"
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
  [[ "$variant" =~ ^[a-z0-9]+(-[a-z0-9]+)*$ ]] || exit 2
  [[ "$TASK" != gothic-ai/harbour-house || "$variant" == original ]] || exit 2
  task="$HOME/$TASK"
  game_environment="$(python3 "$(dirname "$0")/ryzen_game.py")"
  eval "$game_environment"
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
  export OPENGOTHIC_BENCH="$task/scripts/$SCENARIO" OPENGOTHIC_BENCH_OUT="$out"
  game="$GOTHIC_DIR"
  if [[ "$TASK" == gothic-ai/harbour-house ]]; then
    # Keep the wide survey lens isolated from the shared settings.
    python3 - "$game" "$task/game" <<'PYGAME'
from pathlib import Path
import re
import sys
source, target = map(Path, sys.argv[1:])
if target.is_symlink():
    target.unlink()
target.mkdir(exist_ok=True)
for entry in target.iterdir():
    if entry.is_symlink():
        entry.unlink()
for entry in source.iterdir():
    if entry.name.lower() != 'system' and not (target / entry.name).exists():
        (target / entry.name).symlink_to(entry)
system = next(entry for entry in source.iterdir() if entry.name.lower() == 'system')
local_system = target / system.name
local_system.mkdir(exist_ok=True)
for entry in local_system.iterdir():
    if entry.is_symlink():
        entry.unlink()
for entry in system.iterdir():
    if entry.name.lower() != 'systempack.ini' and not (local_system / entry.name).exists():
        (local_system / entry.name).symlink_to(entry)
original = next((p for p in system.iterdir() if p.name.lower() == 'systempack.ini'), None)
text = original.read_text(errors='replace') if original else '[PARAMETERS]\n'
text = re.sub(r'(?im)^[ \t]*VerticalFOV[ \t]*=.*$', '', text)
text, count = re.subn(r'(?im)^[ \t]*\[PARAMETERS\][ \t]*\r?$', '[PARAMETERS]\nVerticalFOV=125', text, count=1)
if not count:
    text += '\n[PARAMETERS]\nVerticalFOV=125\n'
(local_system / 'SystemPack.ini').write_text(text)
PYGAME
    game="$task/game"
  fi
  GAME_BIN="$HOME/$BUILD_TASK/build/opengothic/Gothic2Notr" \
    bash "$(dirname "$0")/base-ensure.sh" "$game"
  rc=0
  (cd "$out/run" && timeout 240 "$HOME/$BUILD_TASK/build/opengothic/Gothic2Notr" \
    -g "$game" -g2 -window -devmode -save 1) >"$out/game.log" 2>&1 || rc=$?
  printf '%s\n' "$rc" >"$out/exit.txt"
  [[ "$rc" == 0 ]] || { tail -30 "$out/game.log"; exit "$rc"; }
  check_args=()
  [[ "$TASK" != gothic-ai/quality-pilot-3 ]] || check_args+=(--iteration-3)
  [[ "$TASK" != gothic-ai/market-house ]] || check_args+=(--market-house)
  [[ "$TASK" != gothic-ai/harbour-house ]] || check_args+=(--harbour-house)
  [[ "$TASK" != gothic-ai/harbour-hut ]] || check_args+=(--harbour-hut)
  [[ "$TASK" != gothic-ai/remaster-* ]] || check_args+=(--remaster)
  python3 "$task/scripts/capture-quality-pilot.py" check "$out" "$task/scripts/$SCENARIO" "$task/content/$variant/materials/materials.json" "${check_args[@]}"
  exit 0
fi

if [[ "${1:-}" == --compose || "${1:-}" == --compose-2 || "${1:-}" == --compose-3 || "${1:-}" == --compose-house || "${1:-}" == --compose-house-2 ]]; then
  exec uv run --no-project --with pillow python "$ROOT/scripts/content/capture-quality-pilot.py" "${1#--}" "${2:?captures}" "${3:?output}"
fi

if [[ "${1:-}" == --prepare || "${1:-}" == --scenario || "${1:-}" == --capture ]]; then
  GAME_ENV=()
  for name in OPENGOTHIC_BASE OPENGOTHIC_BASE_FINGERPRINT OPENGOTHIC_VIDEOS_FINGERPRINT OPENGOTHIC_BASE_CACHE OPENGOTHIC_BASE_STORE; do
    [[ ! ${!name+x} ]] || GAME_ENV+=("$name=${!name}")
  done
  printf -v GAME_CHECK '%q ' env ${GAME_ENV[@]+"${GAME_ENV[@]}"} python3 -c "$(cat "$ROOT/scripts/ryzen_game.py")" --check
  ssh -T -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "$GAME_CHECK"
fi

if [[ "${1:-}" == --prepare ]]; then
  ssh -T "$HOST" "mkdir -p ~/$BUILD_TASK/source/OpenGothic ~/$BUILD_TASK/source/scripts ~/$TASK/scripts ~/$TASK/content/original/materials ~/$TASK/runs; df -h ~"
  rsync -a --delete --exclude=.git --exclude=build --exclude=__pycache__ \
    "$ROOT/OpenGothic/" "$HOST:$BUILD_TASK/source/OpenGothic/"
  rsync -a --delete --exclude='/build*/' "$ROOT/scripts/convert/" "$HOST:$BUILD_TASK/source/scripts/convert/"
  rsync -a "$ROOT/scripts/content/capture-quality-pilot.sh" "$ROOT/scripts/content/capture-quality-pilot.py" \
    "$ROOT/scripts/base-ensure.sh" "$ROOT/scripts/ryzen_game.py" "$ROOT/scripts/content/$SCENARIO" "$HOST:$TASK/scripts/"
  common="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)"
  rsync -a "${SAVE:-$(dirname "$common")/save_slot_1.sav}" "$HOST:$TASK/save_slot_1.sav"
  GQ_OWNER="${TASK##*/}" "$ROOT/scripts/gq" --host "$HOST" run --class quick \
    --threads 8 --mem 10G --max 25m --wait-max 900 --name 'pilot engine build' -- \
    env ${GAME_ENV[@]+"${GAME_ENV[@]}"} QUALITY_PILOT_TASK="$TASK" bash "$TASK/scripts/capture-quality-pilot.sh" --remote-prepare
  exit 0
fi

if [[ "${1:-}" == --scenario ]]; then
  # Refresh only the scenario and checker of a task: new cameras need no engine rebuild.
  ssh -T "$HOST" "mkdir -p ~/$TASK/scripts ~/$TASK/content/original/materials ~/$TASK/runs"
  common="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)"
  rsync -a "${SAVE:-$(dirname "$common")/save_slot_1.sav}" "$HOST:$TASK/save_slot_1.sav"
  rsync -a "$ROOT/scripts/content/capture-quality-pilot.sh" "$ROOT/scripts/content/capture-quality-pilot.py" \
    "$ROOT/scripts/base-ensure.sh" "$ROOT/scripts/ryzen_game.py" "$ROOT/scripts/content/$SCENARIO" "$HOST:$TASK/scripts/"
  exit 0
fi

if [[ "${1:-}" == --capture ]]; then
  variant="${2:?variant}"; out="${3:?output}"
  [[ "$variant" =~ ^[a-z0-9]+(-[a-z0-9]+)*$ ]] || exit 2
  [[ "$TASK" != gothic-ai/harbour-house || "$variant" == original ]] || exit 2
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
    "$ROOT/scripts/base-ensure.sh" "$ROOT/scripts/ryzen_game.py" "$ROOT/scripts/content/$SCENARIO" "$HOST:$TASK/scripts/"
  rc=0
  GQ_OWNER="${TASK##*/}" "$ROOT/scripts/gq" --host "$HOST" run --class quick \
    --threads 4 --mem 5G --gpu-mem 4G --display --display-exclusive \
    --max 6m --wait-max 900 --name "pilot $variant captures" -- \
    env ${GAME_ENV[@]+"${GAME_ENV[@]}"} QUALITY_PILOT_TASK="$TASK" QUALITY_PILOT_SCENARIO="$SCENARIO" \
    bash "$TASK/scripts/capture-quality-pilot.sh" --remote-capture "$variant" || rc=$?
  rsync -a "$HOST:$TASK/runs/$variant/" "$out/$variant/"
  exit "$rc"
fi
echo 'usage: capture-quality-pilot.sh --prepare | --scenario | --capture original|<staging set> OUTPUT | --compose|--compose-2|--compose-3|--compose-house|--compose-house-2 CAPTURES OUTPUT' >&2
exit 2
