#!/usr/bin/env bash
# Submit one bounded phase of the fixed navigation suite; never wait on the queue.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
weights=''
round=''
stage=auto
suite="$ROOT/scripts/nav-live/scenarios.json"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --weights) weights="${2:?weights required}"; shift 2 ;;
    --round) round="${2:?round required}"; shift 2 ;;
    --stage) stage="${2:?stage required}"; shift 2 ;;
    -h|--help)
      echo 'Usage: scripts/nav-live.sh --weights PATH[,PATH] --round LABEL [--stage prepare|smoke|baseline|report|clean]'
      echo 'Each invocation submits its phase and exits. Default: prepare, then smoke, then baseline after their gates exist.'
      exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$weights" && "$round" =~ ^[a-zA-Z0-9][a-zA-Z0-9_-]*$ ]] || { echo 'Weights and a safe round label are required' >&2; exit 2; }
case "$stage" in auto|prepare|smoke|baseline|report|clean) ;; *) echo "Unknown stage: $stage" >&2; exit 2 ;; esac
[[ -f "$suite" ]] || { echo "Missing suite: $suite" >&2; exit 2; }
suite="$(cd "$(dirname "$suite")" && pwd)/$(basename "$suite")"
# Inventory the exact submitted code; later invocations never edit queued sources.
snapshot="$(python3 - "$ROOT" "$suite" <<'PY'
import hashlib
from pathlib import Path
import sys
root, suite = map(Path, sys.argv[1:])
hash = hashlib.sha256()
for source in ('OpenGothic', 'scripts'):
    for path in sorted((root/source).rglob('*')):
        relative = path.relative_to(root)
        if (not path.is_file() or any(part.startswith('build') or part in ('.git', '__pycache__', '.pytest_cache')
                                      for part in relative.parts)):
            continue
        hash.update(relative.as_posix().encode() + b'\0')
        hash.update(path.read_bytes())
hash.update(suite.read_bytes())
print(hash.hexdigest()[:20])
PY
)"
task="$(ssh -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-live" "$HOME"')"
output="/mnt/shared/gothic-ai/nav-live/$round"
source="$task/source/$snapshot"
export GQ_OWNER=nav-live
ssh ryzen "bash -s -- '$task' '$source' '$output'" <<'PREFLIGHT'
set -euo pipefail
task="$1"; source="$2"; output="$3"
python3 - <<'PY'
from pathlib import Path
import shutil
free = shutil.disk_usage(Path.home()).free
if free < 22 * 1024**3:
    raise SystemExit(f'Need 22 GiB free including the 20 GiB reserve; available {free} bytes')
PY
mkdir -p "$source" "$output" "$task/logs"
PREFLIGHT
# Own immutable snapshots only; neither game installations nor other tasks are synced.
rsync -a --no-links --exclude=build --exclude='build-*' --exclude=__pycache__ --exclude=.pytest_cache --exclude=.git \
  "$ROOT/OpenGothic" "$ROOT/scripts" "ryzen:$source/"
rsync -rt --no-links "$suite" "ryzen:$source/suite.json"
if [[ "$stage" == auto ]]; then
  stage="$(ssh ryzen "bash -s -- '$task' '$output' '$snapshot'" <<'CHOOSE'
set -euo pipefail
if [[ ! -f "$1/build-passed-$3" || "$(cat "$1/build-snapshot" 2>/dev/null)" != "$3" ]]; then
  echo prepare
else
  python3 - "$2/smoke.json" "$1/source/$3/suite.json" "$1/build/opengothic/Gothic2Notr" <<'GATE'
import hashlib
import json
from pathlib import Path
import sys
def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()
try:
    gate=json.loads(Path(sys.argv[1]).read_text())
    valid=(gate.get('complete') is True and gate.get('suite_sha256') == digest(sys.argv[2])
           and gate.get('binary_sha256') == digest(sys.argv[3]))
except (OSError, ValueError):
    valid=False
print('baseline' if valid else 'smoke')
GATE
fi
CHOOSE
)"
fi
if [[ "$stage" == clean ]]; then
  ssh ryzen "bash -s -- '$task' '$output'" <<'CLEAN'
set -euo pipefail
python3 - "$2" <<'PY'
import json
from pathlib import Path
import sys
output=Path(sys.argv[1])
gate=json.loads((output/'complete.json').read_text())
if gate.get('complete') is not True:
    raise SystemExit('Clean is permitted after the complete round report only')
PY
rm -rf -- "$1/build" "$1/base-cache" "$1/.venv" "$1/uv-cache" "$1/navmesh-cache"
rm -f -- "$1"/build-passed-* "$1/build-snapshot"
CLEAN
  exit 0
fi
log_prefix="$task/$round/job-logs/$stage-$(date -u +%Y%m%dT%H%M%SZ)-$$"
# The phase body belongs to the snapshot so a subsequent launcher cannot replace it.
ssh ryzen "cat > '$source/phase.sh'" <<'PHASE'
#!/usr/bin/env bash
set -euo pipefail
source="$1"; task="$2"; output="$3"; weights="$4"; phase="$5"; snapshot="$6"; log_dir="$7"; shard="${8:-0}"
# Charge elapsed execution time only; time waiting in gq is not machine time.
# The local lock also serializes report writes and build reuse for this round.
if [[ "${NAVLIVE_BUDGET_CHILD:-0}" != 1 ]]; then
  exec 9>"$task/$(basename "$output")-budget.lock"
  flock 9
  allowance="$(python3 - "$output/runtime.json" <<'BUDGET'
import json
from pathlib import Path
import sys
path=Path(sys.argv[1])
used=json.loads(path.read_text()).get('wall_seconds', 0) if path.is_file() else 0
remaining=10800-used-30  # Reserve termination and final bookkeeping time.
if remaining <= 0:
    raise SystemExit('Round exhausted its 10800-second machine-time budget')
print(min(3300, remaining))
BUDGET
)"
  started_at=$(date +%s)
  result=0
  timeout --signal=TERM --kill-after=10 "$allowance" env NAVLIVE_BUDGET_CHILD=1 bash "$0" "$@" || result=$?
  python3 - "$output/runtime.json" "$started_at" <<'CHARGE'
import json
import os
from pathlib import Path
import sys
import time
path=Path(sys.argv[1])
previous=json.loads(path.read_text()).get('wall_seconds', 0) if path.is_file() else 0
value={'schema_version':1, 'wall_seconds':previous+max(0, int(time.time())-int(sys.argv[2])), 'limit_seconds':10800}
temporary=path.with_name(path.name+f'.{os.getpid()}.tmp')
temporary.write_text(json.dumps(value)+'\n')
temporary.replace(path)
CHARGE
  exit "$result"
fi
exec > >(tee "$log_dir/phase-$shard.log") 2>&1
trap 'printf "exit=%s\n" "$?"' EXIT
started=$SECONDS
game_settings="$(python3 "$source/scripts/ryzen_game.py")"
eval "$game_settings"
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export UV_CACHE_DIR="$task/uv-cache"
export PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export OPENGOTHIC_BODY_TRACE=0
# Explicitly choose a ready shared base read-only; all caches/stores are task-owned.
export OPENGOTHIC_BASE_CACHE="$task/base-cache"
export OPENGOTHIC_BASE_STORE="/mnt/shared/gothic-ai/nav-live/base-store"
if [[ -z "${OPENGOTHIC_BASE:-}" ]]; then
  OPENGOTHIC_BASE="$(python3 - <<'PY'
import os
from pathlib import Path
fingerprint=os.environ['OPENGOTHIC_BASE_FINGERPRINT']
for path in (Path('/mnt/shared/gothic-ai/base')/fingerprint,
             Path.home()/'.cache/gothic-ai/base'/fingerprint):
    if (path/'base.json').is_file():
        print(path)
        break
else:
    raise SystemExit('No ready base for the selected fingerprint; no implicit import in nav-live')
PY
)"
  export OPENGOTHIC_BASE
fi
if [[ "$phase" == smoke || "$phase" == baseline ]]; then
  export XDG_RUNTIME_DIR="/run/user/$(id -u)"
  export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
  desktop_found=0
  for auth in "$XDG_RUNTIME_DIR"/.mutter-Xwaylandauth.* "$HOME/.Xauthority"; do
    [[ -r "$auth" ]] || continue
    for socket in /tmp/.X11-unix/X*; do
      [[ -S "$socket" ]] || continue
      if info="$(DISPLAY=":${socket##*/X}" XAUTHORITY="$auth" xdpyinfo 2>/dev/null)"; then
        export DISPLAY=":${socket##*/X}" XAUTHORITY="$auth"
        screen="$(printf '%s\n' "$info" | awk '/dimensions:/ {print $2; exit}')"
        desktop_found=1
        break 2
      fi
    done
  done
  [[ "$desktop_found" == 1 ]] || { echo 'No accessible Xwayland desktop' >&2; exit 1; }
  export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json
  [[ -r "$VK_DRIVER_FILES" ]] || { echo 'Missing NVIDIA Vulkan ICD' >&2; exit 1; }
  export ALSOFT_DRIVERS=null TEMPEST_BACKGROUND=1 TEMPEST_WINDOW_SIZE=640x360
  slot="${GQ_DISPLAY_SLOT:-0}"
  [[ "$slot" =~ ^[0-9]+$ && "$screen" =~ ^([0-9]+)x([0-9]+)$ ]] || { echo 'Invalid display geometry or gq slot' >&2; exit 1; }
  screen_width=$((10#${BASH_REMATCH[1]})); screen_height=$((10#${BASH_REMATCH[2]}))
  columns=$(((screen_width-140)/700)); (( columns > 0 )) || columns=1
  x=$((100+(10#$slot % columns)*700)); y=$((100+(10#$slot / columns)*460))
  (( x+640 <= screen_width && y+360 <= screen_height )) || { echo 'Assigned game window does not fit the desktop' >&2; exit 1; }
  export TEMPEST_WINDOW_POS="$x,$y"
  command -v ffmpeg >/dev/null
  command -v ffprobe >/dev/null
fi
if [[ "$phase" == prepare ]]; then
  python3 - <<'PY'
from pathlib import Path
import shutil
free=shutil.disk_usage(Path.home()).free
if free < 22*1024**3:
    raise SystemExit(f'Need 22 GiB free before economical build; available {free}')
PY
  if [[ ! -x "$task/.venv/bin/python" ]]; then uv venv --python 3.12 "$task/.venv"; fi
  uv pip install --python "$task/.venv/bin/python" numpy pillow pytest
  engine_hash="$(python3 - "$source/OpenGothic" <<'ENGINE'
import hashlib
from pathlib import Path
import sys
root=Path(sys.argv[1])
hash=hashlib.sha256()
for path in sorted(root.rglob('*')):
    if path.is_file():
        hash.update(path.relative_to(root).as_posix().encode()+b'\0')
        hash.update(path.read_bytes())
print(hash.hexdigest())
ENGINE
)"
  if [[ ! -x "$task/build/opengothic/Gothic2Notr" || ! -x "$task/build/opengothic/gothic-body-runner" || ! -x "$task/build/opengothic/gothic-navpolicy-contract" ||
        "$(cat "$task/build/engine.sha256" 2>/dev/null)" != "$engine_hash" ]]; then
    # CMake owns absolute source paths; retain a ready identical engine and
    # rebuild only this task's directory when the engine source changes.
    rm -rf -- "$task/build"
    cmake -S "$source/OpenGothic" -B "$task/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_CXX_FLAGS_RELEASE='-O2 -g0 -DNDEBUG' -DCMAKE_C_FLAGS_RELEASE='-O2 -g0 -DNDEBUG' \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF -DINTERPROCEDURAL_OPTIMIZATION=OFF -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
    -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
    cmake --build "$task/build" --target Gothic2Notr gothic-body-runner gothic-navpolicy-contract --parallel 8
    printf '%s\n' "$engine_hash" > "$task/build/engine.sha256"
  fi
  "$task/build/opengothic/gothic-navpolicy-contract" --live-format
  "$task/.venv/bin/python" -B -m pytest -q -p no:cacheprovider "$source/scripts/tests/test_nav_drive.py" -k live_trace_fixture
  qualification="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["qualification_status"])' "$source/suite.json")"
  if [[ "$qualification" == pending ]]; then
    "$task/.venv/bin/python" -B "$source/scripts/nav_live_prepare.py" \
      --runtime "$task" --output "$output" --suite "$source/suite.json"
  fi
  touch "$task/build-passed-$snapshot"
  printf '%s\n' "$snapshot" > "$task/build-snapshot"
else
  test -f "$task/build-passed-$snapshot"
  [[ "$(cat "$task/build-snapshot")" == "$snapshot" ]]
  "$task/.venv/bin/python" -B "$source/scripts/nav_live.py" "$phase" \
    --suite "$source/suite.json" --runtime "$task" --output "$output" --weights "$weights" --shard "$shard"
fi
printf 'phase_wall_seconds=%s\n' "$((SECONDS-started))"
PHASE
if [[ "$stage" == baseline ]]; then shards=(0 1 2); else shards=(0); fi
for shard in "${shards[@]}"; do
  log_dir="$log_prefix-$shard"
  ssh ryzen "mkdir -p '$log_dir'"
  display=()
  [[ "$stage" == smoke || "$stage" == baseline ]] && display=(--display --display-shared)
  identifier=$("$ROOT/scripts/gq" submit --class quick --threads 16 --cpu-quota 1200% --mem 10G --max 59m \
    "${display[@]}" --archive "$log_dir" --name "nav live $round $stage $shard" -- \
    bash "$source/phase.sh" "$source" "$task" "$output" "$weights" "$stage" "$snapshot" "$log_dir" "$shard")
  printf '%s shard %s: %s\n' "$stage" "$shard" "$identifier"
done
