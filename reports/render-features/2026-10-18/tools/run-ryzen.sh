#!/usr/bin/env bash
# Render-feature survey on Ryzen: one game per configuration, the same bench scenario, a frame profile
# and nvidia-smi samples. Usage: run-ryzen.sh <out dir> <scenario> <configs file>
#   configs file: label<TAB>game flags<TAB>console commands put before the scenario (';'-separated)
set -uo pipefail
R="$HOME/gothic-ai/render-features"
OUT="$1"; SCEN="$2"; CONFIGS="$3"
BIN="$R/build/opengothic/Gothic2Notr"
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
found=0
for auth in "$XDG_RUNTIME_DIR"/.mutter-Xwaylandauth.* "$HOME/.Xauthority"; do
  [[ -r "$auth" ]] || continue
  for socket in /tmp/.X11-unix/X*; do
    [[ -S "$socket" ]] || continue
    if DISPLAY=":${socket##*/X}" XAUTHORITY="$auth" xdpyinfo >/dev/null 2>&1; then
      export DISPLAY=":${socket##*/X}" XAUTHORITY="$auth"; found=1; break 2
    fi
  done
done
[[ "$found" == 1 ]] || { echo "no Xwayland display" >&2; exit 1; }
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json
export ALSOFT_DRIVERS=null TEMPEST_BACKGROUND=1
mkdir -p "$OUT"
{ echo "display $DISPLAY"; xdpyinfo | grep -E "dimensions|resolution"; xrandr --current 2>/dev/null | grep -E '\*' ; nvidia-smi --query-gpu=name,driver_version --format=csv,noheader; uname -r; } >"$OUT/host.txt" 2>&1
while IFS= read -r line; do
  # tab is IFS whitespace in bash: read would merge an empty flags field, so split with cut
  label="$(cut -f1 <<<"$line")"; flags="$(cut -f2 <<<"$line")"; prefix="$(cut -f3 <<<"$line")"
  [[ "$line" == *$'\t'* ]] || flags="" prefix=""
  [[ -z "$label" || "$label" == \#* ]] && continue
  D="$OUT/$label"; rm -rf "$D"; mkdir -p "$D/run"
  cp "$R/save_slot_1.sav" "$D/run/save_slot_1.sav"
  printf '[INTERNAL]\nvidResIndex=0\n[SOUND]\nsoundVolume=0\nmusicVolume=0\nmusicEnabled=0\n' >"$D/run/Gothic.ini"
  { [[ -n "$prefix" ]] && tr ';' '\n' <<<"$prefix"; cat "$SCEN"; } >"$D/scenario.bench"
  echo "$flags" >"$D/flags.txt"
  python3 "$R/gpu-nvidia.py" "$D/gpu.csv" & SAMPLER=$!
  start=$(date +%s.%N)
  # shellcheck disable=SC2086
  (cd "$D/run" && exec env GOTHIC_AI=0 TEMPEST_WINDOW_SIZE="${WINDOW:-1920x1080}" OPENGOTHIC_BENCH="$D/scenario.bench" OPENGOTHIC_BENCH_OUT="$D" \
     OPENGOTHIC_FRAME_PROFILE="$D/frames.csv" OPENGOTHIC_TEXTURES="$R/hd-textures/textures" OPENGOTHIC_BENCH_FFMPEG="$R/discard-ffmpeg.sh" \
     timeout -k 10 "${GAME_TIMEOUT:-300}" "$BIN" -g "$HOME/gothic-ai/game" -window -devmode -save 1 $flags) >"$D/game.log" 2>&1
  rc=$?
  kill "$SAMPLER" 2>/dev/null; wait "$SAMPLER" 2>/dev/null
  echo "$rc" >"$D/exit.txt"
  printf '%s rc=%s %.0fs %s\n' "$label" "$rc" "$(echo "$(date +%s.%N) - $start" | bc)" "$flags"
  rm -rf "$D/run"
  sleep 3
done <"$CONFIGS"
