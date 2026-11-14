#!/usr/bin/env bash
# Called only inside the launcher's gq jobs.
set -euo pipefail
action=$1 state=$2 run=$3
source "$state/source/scripts/ryzen-build.sh"
if [[ "$action" == build ]]; then
  export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
  export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
  cmake -S "$state/source/OpenGothic" -B "$state/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    "${RYZEN_CMAKE_FLAGS[@]}" \
    -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib" \
    >"$state/configure.log" 2>&1 || { tail -80 "$state/configure.log"; exit 76; }
  cmake --build "$state/build" --target engine-island gothic-island-program-test --parallel 12 \
    >"$state/build.log" 2>&1 || { tail -80 "$state/build.log"; exit 76; }
  "$state/build/opengothic/gothic-island-program-test"
  exit
fi
[[ "$action" == run ]] || exit 76
mkdir -p "$run"/{home,output,frames}
cp -a "$state/build/island" "$run/data"
cp "$state/source/OpenGothic/island/scripts/route.txt" "$run/route.txt"
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
display='' auth=''
for candidate in "$XDG_RUNTIME_DIR"/.mutter-Xwaylandauth.* "$HOME/.Xauthority"; do
  [[ -r "$candidate" ]] || continue
  for socket in /tmp/.X11-unix/X*; do
    [[ -S "$socket" ]] || continue
    if DISPLAY=":${socket##*/X}" XAUTHORITY="$candidate" xdpyinfo >/dev/null 2>&1; then
      display=":${socket##*/X}" auth="$candidate"
      break 2
    fi
  done
done
[[ -n "$display" ]] || { echo 'island: no usable Xwayland display'; exit 76; }
cp "$auth" "$run/home/Xauthority"
cd "$run"
set +e
# Nothing from the shell's game/model environment reaches the child. The window and GPU use only these paths.
strace -f -yy -s 65535 -o "$run/output/strace.log" \
  -e trace=%file,clone,clone3,fork,vfork \
  /usr/bin/env -i \
  HOME="$run/home" XDG_CONFIG_HOME="$run/home/config" XDG_CACHE_HOME="$run/home/cache" \
  XDG_DATA_HOME="$run/home/data" XDG_STATE_HOME="$run/home/state" \
  DISPLAY="$display" XAUTHORITY="$run/home/Xauthority" XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
  VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json \
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json \
  OPENGOTHIC_CENSUS="$run/output/census.json" \
  "$state/build/opengothic/engine-island" --data "$run/data" --script "$run/route.txt" \
  --events "$run/output/events.jsonl" --capture "$run/frames" --capture-every 2 --size 1280x720 \
  >"$run/output/program.log" 2>&1
rc=$?
set -e
python3 - "$run/output/report.json" "$run" "$state/build/opengothic" "$(id -u)" "$rc" <<'PY'
import json, sys
from pathlib import Path
p, root, binary, uid, code = sys.argv[1:]
Path(p).write_text(json.dumps(dict(run_root=root, binary_dir=binary, uid=int(uid), program_exit=int(code)), indent=2)+'\n')
PY
# Shots are written beside frames, then collected without copying the full sequence to the Mac.
for name in beach rocks hill; do
  [[ ! -f "$run/$name.png" ]] || mv "$run/$name.png" "$run/output/"
done
gzip "$run/output/strace.log"
ffmpeg -hide_banner -loglevel error -framerate 15 -i "$run/frames/frame-%05d.png" \
  -c:v libx264 -preset fast -crf 20 -pix_fmt yuv420p -r 30 "$run/output/clip.mp4" || exit 76
# The caller fetches output after gq's verified archive move.
[[ "$rc" == 0 ]] || exit 1
