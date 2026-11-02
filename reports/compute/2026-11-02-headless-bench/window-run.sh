#!/usr/bin/env bash
# One windowed npc-day smoke with the rand() logger (gq quick --display --display-shared).
set -uo pipefail
task="$HOME/gothic-ai/headless-bench"
name="$1"; shift
out="$task/runs/$name"
rm -rf "$out"; mkdir -p "$out/run"
eval "$(python3 "$task/source/scripts/ryzen_game.py")"
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
for auth in "$XDG_RUNTIME_DIR"/.mutter-Xwaylandauth.* "$HOME/.Xauthority"; do
  [[ -r "$auth" ]] || continue
  for socket in /tmp/.X11-unix/X*; do
    if DISPLAY=":${socket##*/X}" XAUTHORITY="$auth" xdpyinfo >/dev/null 2>&1; then
      export DISPLAY=":${socket##*/X}" XAUTHORITY="$auth"; break 2
    fi
  done
done
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json ALSOFT_DRIVERS=null
cat >"$out/run/Gothic.ini" <<INI
[INTERNAL]
vidResIndex=0
[SOUND]
soundVolume=0
musicVolume=0
musicEnabled=0
worldSounds=1
INI
cd "$out/run"
started=$SECONDS
env TEMPEST_BACKGROUND=1 TEMPEST_WINDOW_SIZE=800x600 OPENGOTHIC_NPC_SIM_TIER=full \
  OPENGOTHIC_BENCH="$task/inputs/${SCENARIO:-npc-day-smoke}.bench" OPENGOTHIC_BENCH_OUT="$out" "$@" \
  timeout "${GAME_TIMEOUT:-900}" "$task/build/opengothic/Gothic2Notr" -g "$GOTHIC_DIR" -window -devmode -nomenu -w NEWWORLD.ZEN -nointro -g2 -window \
  >"$out/game.log" 2>&1
rc=$?
echo "wall $((SECONDS-started))s rc=$rc" | tee "$out/wall.txt"
grep -c "bench\] done" "$out/game.log"
