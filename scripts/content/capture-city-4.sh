#!/usr/bin/env bash
# City-4: private Ryzen build/captures via gq, or original/city-4 FPS on Mac.
# --prepare | --capture original|city-3|city-4 OUTPUT --inspection NW_MISC.bench
# --compose CAPTURES OUTPUT
# --mac-city-fps OUTPUT (requires existing profiled BIN and GOTHIC_DIR)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HOST="${RYZEN_HOST:-ryzen}"
TASK=gothic-ai/city-4
STAGING="${CONTENT_STAGING_ROOT:-$HOME/.local/share/gothic-ai/content-staging}"

if [[ "${1:-}" == --remote-capture ]]; then
  variant="${2:?variant}"
  [[ "$variant" == original || "$variant" == city-3 || "$variant" == city-4 ]] || exit 2
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
  export OPENGOTHIC_BENCH="$task/scripts/city-4.bench" OPENGOTHIC_BENCH_OUT="$out"
  rc=0
  (cd "$out/run" && timeout 600 "$task/build/opengothic/Gothic2Notr" \
    -g "$HOME/gothic-ai/game" -g2 -window -devmode -save 1) >"$out/game.log" 2>&1 || rc=$?
  printf '%s\n' "$rc" >"$out/exit.txt"
  [[ "$rc" == 0 ]] || { tail -30 "$out/game.log"; exit "$rc"; }
  index="$task/content/$variant/materials/materials.json"
  [[ "$variant" != original ]] || index="$task/content/city-4/materials/materials.json"
  python3 "$task/scripts/capture-city-4.py" check "$out" "$task/scripts/city-4.bench" "$index" "$variant"
  exit 0
fi

if [[ "${1:-}" == --prepare ]]; then
  QUALITY_PILOT_TASK="$TASK" bash "$ROOT/scripts/content/capture-quality-pilot.sh" --prepare
  exit 0
fi

if [[ "${1:-}" == --mac-city-fps ]]; then
  : "${BIN:?--mac-city-fps requires an existing frame-profiled Mac game binary}"
  : "${GOTHIC_DIR:?--mac-city-fps requires GOTHIC_DIR}"
  [[ "$(uname)" == Darwin && -x "$BIN" && -d "$GOTHIC_DIR" ]] || exit 2
  out="${2:?output}"
  mkdir -p "$out"
  out="$(cd "$out" && pwd -P)"
  case "$out" in "$ROOT"|"$ROOT"/*) echo 'capture output must be outside git' >&2; exit 2 ;; esac
  run="$(mktemp -d "$out/$(date -u +%Y%m%dT%H%M%SZ)-XXXXXXXX")"
  mkdir -p "$run/content/before/materials" "$run/content/after" "$run/tools/empty-textures" "$run/captures"
  printf '{}\n' >"$run/content/before/materials/materials.json"
  rsync -a "$STAGING/city-4/" "$run/content/after/"
  python3 - "$run/content/after/materials/materials.json" <<'PY'
import json
from pathlib import Path
import sys
if len(json.loads(Path(sys.argv[1]).read_text())) != 65:
    raise SystemExit('city-4 must have 65 packed materials')
PY
  common="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)"
  cp "${SAVE:-$(dirname "$common")/save_slot_1.sav}" "$run/save_slot_1.sav"
  cp -p "$BIN" "$run/Gothic2Notr"
  tools="$ROOT/reports/render-features/2026-10-18/tools"
  cp "$tools/analyze.py" "$tools/gpu-apple.py" "$tools/discard-ffmpeg.sh" "$run/tools/"
  python3 - "$tools/run-mac.sh" "$run/tools/run-mac.sh" "$ROOT/scripts/content/city.bench" "$run/tools/city.bench" <<'PY'
from pathlib import Path
import sys
source, output, city, scenario = map(Path, sys.argv[1:])
text = source.read_text()
old = 'TEX="$HOME/.local/share/gothic-ai/hd-textures/textures"'
if text.count(old) != 1:
    raise SystemExit('render-feature Mac runner texture-root assignment changed')
output.write_text(text.replace(old, 'TEX="$S/empty-textures"'))
lines = []
for line in city.read_text().splitlines():
    lines.append(line)
    if line == 'shot city_night':
        lines.append('wait 500')
        break
scenario.write_text('\n'.join(lines) + '\n')
PY
  pgrep -fl Gothic2Notr || true
  [[ "${MAC_WAIT_SECONDS:-2400}" =~ ^[0-9]+$ && "${MAC_WAIT_SECONDS:-2400}" -le 2400 ]] || exit 2
  wait_start=$SECONDS
  while pgrep -x Gothic2Notr >/dev/null; do
    (( SECONDS - wait_start < ${MAC_WAIT_SECONDS:-2400} )) || { echo 'Mac occupied; no game started' >&2; exit 75; }
    sleep 5
  done
  export BIN="$run/Gothic2Notr" SAVE="$run/save_slot_1.sav"
  for variant in before after; do
    printf '%s\t\t\n' "$variant" >"$run/config.tsv"
    OPENGOTHIC_CONTENT="$run/content/$variant" OPENGOTHIC_CENSUS="$run/captures/$variant/census.json" \
      bash "$run/tools/run-mac.sh" "$run/captures" "$run/tools/city.bench" "$run/config.tsv"
    bash "$ROOT/scripts/content/capture-city.sh" --check "$run/captures/$variant" \
      "$run/tools/city.bench" "$run/content/after/materials/materials.json" "$variant" seen
  done
  python3 "$run/tools/analyze.py" "$run/captures" city
  bash "$ROOT/scripts/content/capture-city.sh" --check-fps "$run/captures" city
  printf 'Mac original/city-4 FPS (before/after): %s\n' "$run"
  exit 0
fi

if [[ "${1:-}" == --compose ]]; then
  exec uv run --no-project --with pillow python "$ROOT/scripts/content/capture-city-4.py" compose \
    "${2:?captures}" "$2/city-4.bench" "${3:?output}"
fi

if [[ "${1:-}" == --capture ]]; then
  variant="${2:?variant}"; out="${3:?output}"
  [[ "$variant" == original || "$variant" == city-3 || "$variant" == city-4 ]] || exit 2
  [[ "${4:-}" == --inspection && -f "${5:-}" && "$#" == 5 ]] || { echo '--capture requires --inspection with seven NW_MISC cameras' >&2; exit 2; }
  mkdir -p "$out"
  out="$(cd "$out" && pwd -P)"
  case "$out" in "$ROOT"|"$ROOT"/*) echo 'capture output must be outside git' >&2; exit 2 ;; esac
  python3 "$ROOT/scripts/content/capture-city-4.py" prepare "$STAGING/city-4/materials/materials.json" \
    "$out/city-4.bench" "$5"
  ssh -T "$HOST" "mkdir -p ~/$TASK/scripts ~/$TASK/content/original/materials ~/$TASK/content/city-4; df -h ~"
  rsync -a "$ROOT/scripts/content/capture-city-4.sh" "$ROOT/scripts/content/capture-city-4.py" \
    "$ROOT/scripts/content/capture-city.sh" "$out/city-4.bench" "$HOST:$TASK/scripts/"
  # The baseline still needs the city-4 index as its census target list.
  rsync -a --delete "$STAGING/city-4/" "$HOST:$TASK/content/city-4/"
  if [[ "$variant" == city-3 ]]; then
    python3 - "$STAGING/city-3/materials/materials.json" <<'PY'
import json
from pathlib import Path
import sys
if len(json.loads(Path(sys.argv[1]).read_text())) != 58:
    raise SystemExit('city-3 must have 58 packed materials')
PY
    ssh -T "$HOST" "mkdir -p ~/$TASK/content/city-3"
    rsync -a --delete "$STAGING/city-3/" "$HOST:$TASK/content/city-3/"
  elif [[ "$variant" == original ]]; then
    ssh -T "$HOST" "printf '{}\\n' >~/$TASK/content/original/materials/materials.json"
  fi
  rc=0
  GQ_OWNER=city-4 "$ROOT/scripts/gq" --host "$HOST" run --class quick \
    --threads 4 --mem 5G --gpu-mem 4G --display --display-exclusive \
    --max 12m --wait-max 900 --name "city-4 $variant captures" -- \
    bash "$TASK/scripts/capture-city-4.sh" --remote-capture "$variant" || rc=$?
  mkdir -p "$out/$variant"
  rsync -a "$HOST:$TASK/runs/$variant/" "$out/$variant/"
  exit "$rc"
fi
echo 'usage: capture-city-4.sh --prepare | --capture original|city-3|city-4 OUTPUT --inspection NW_MISC.bench | --compose CAPTURES OUTPUT | --mac-city-fps OUTPUT' >&2
exit 2
