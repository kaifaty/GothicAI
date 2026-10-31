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
      echo 'Usage: scripts/nav-live.sh --weights PATH[,PATH] --round LABEL [--stage prepare|qualify|requalify|extend|teacher|smoke|baseline|report|clean]'
      echo 'Each invocation submits its phase and exits. Default: prepare, qualify, then baseline after its gate is complete.'
      exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$weights" && "$round" =~ ^[a-zA-Z0-9][a-zA-Z0-9_-]*$ ]] || { echo 'Weights and a safe round label are required' >&2; exit 2; }
case "$stage" in auto|prepare|qualify|requalify|extend|teacher|smoke|baseline|report|clean) ;; *) echo "Unknown stage: $stage" >&2; exit 2 ;; esac
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
if [[ "$stage" == auto || "$stage" == baseline ]]; then
  if ssh ryzen "python3 - '$source/scripts' '$task' '$output' '$weights'" <<'READY'
from pathlib import Path
import sys
sys.path.insert(0,sys.argv[1])
from nav_live import completed_round
if not completed_round(Path(sys.argv[2]),Path(sys.argv[3]),sys.argv[4]):
    raise SystemExit(3)
print(f'Round already complete: {sys.argv[3]}/README.md')
READY
  then
    exit 0
  else
    result=$?
    [[ "$result" == 3 ]] || exit "$result"
  fi
fi
if [[ "$stage" == auto ]]; then
  stage="$(ssh ryzen "bash -s -- '$task' '$output' '$snapshot'" <<'CHOOSE'
set -euo pipefail
python3 - "$2" "$1/source/$3/suite.json" "$1/build/opengothic/Gothic2Notr" "$1" "$3" <<'GATE'
import hashlib
import json
from pathlib import Path
import sys
def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()
try:
    output=Path(sys.argv[1])
    suite_path=output/'qualified-suite.json'
    frozen=suite_path.is_file()
    if not frozen:
        suite_path=Path(sys.argv[2])
    suite=json.loads(suite_path.read_text())
    if frozen:
        sys.path.insert(0, str(Path(sys.argv[2]).parent/'scripts'))
        from nav_live import eligible_cases, suite_cases
        if not eligible_cases(suite, suite_cases(suite)):
            print('report')
            raise SystemExit(0)
    needs_qualification=(suite.get('qualification_status') != 'geometry-qualified'
                         or bool(suite.get('semantic_confirmation_required')))
    if needs_qualification and not frozen:
        print('qualify' if (output/'preparation/complete.json').is_file() else 'prepare')
        raise SystemExit(0)
    runtime, snapshot = Path(sys.argv[4]), sys.argv[5]
    if (not Path(sys.argv[3]).is_file() or not (runtime/f'build-passed-{snapshot}').is_file()
            or not (runtime/'build-snapshot').is_file()
            or (runtime/'build-snapshot').read_text().strip() != snapshot):
        print('prepare')
        raise SystemExit(0)
except (OSError, ValueError):
    print('prepare')
else:
    print('baseline')
GATE
CHOOSE
)"
fi
run_suite="$source/suite.json"
if [[ "$stage" == baseline ]]; then
  run_suite="$(ssh ryzen "python3 - '$output/qualified-suite.json' '$source' '$task'" <<'FREEZE'
import hashlib
import json
import os
from pathlib import Path
import sys
suite_bytes=Path(sys.argv[1]).read_bytes()
suite=json.loads(suite_bytes)
suite['qualified_suite_sha256']=hashlib.sha256(suite_bytes).hexdigest()
pin=suite.get('evaluation_binary')
binary_source=Path(pin['path']) if pin else Path(sys.argv[3])/'build/opengothic/Gothic2Notr'
binary_data=binary_source.read_bytes()
binary_digest=hashlib.sha256(binary_data).hexdigest()
if pin and binary_digest!=pin['sha256']:
    raise SystemExit('The qualified evaluation binary has different contents')
binary=Path(sys.argv[3])/'binaries'/binary_digest/'Gothic2Notr'
binary.parent.mkdir(parents=True,exist_ok=True)
temporary=binary.with_name(binary.name+f'.{os.getpid()}.tmp')
temporary.write_bytes(binary_data)
temporary.chmod(0o555)
try:
    os.link(temporary,binary)
except FileExistsError:
    if hashlib.sha256(binary.read_bytes()).hexdigest()!=binary_digest:
        raise SystemExit('A pinned evaluation binary has different contents')
finally:
    temporary.unlink()
suite['evaluation_binary']={'path':str(binary),'sha256':binary_digest}
data=(json.dumps(suite,ensure_ascii=False,indent=2)+'\n').encode()
digest=hashlib.sha256(data).hexdigest()
target=Path(sys.argv[2])/f'baseline-suite-{digest}.json'
temporary=target.with_name(target.name+f'.{os.getpid()}.tmp')
temporary.write_bytes(data)
try:
    os.link(temporary,target)
except FileExistsError:
    if target.read_bytes()!=data:
        raise SystemExit('A baseline snapshot has different contents')
finally:
    temporary.unlink()
print(target)
FREEZE
)"
fi
if [[ "$stage" == clean ]]; then
  ssh ryzen "python3 - '$source/scripts' '$task' '$output'" <<'CLEAN'
from pathlib import Path
import sys
sys.path.insert(0,sys.argv[1])
from nav_live import cleanup
if not cleanup(Path(sys.argv[2]),Path(sys.argv[3])):
    raise SystemExit('Clean requires a complete accepted subset and its exact smoke gate')
CLEAN
  exit 0
fi
log_prefix="$task/$round/job-logs/$stage-$(date -u +%Y%m%dT%H%M%SZ)-$$"
# The phase body belongs to the snapshot so a subsequent launcher cannot replace it.
ssh ryzen "cat > '$source/phase.sh'" <<'PHASE'
#!/usr/bin/env bash
set -euo pipefail
source="$1"; task="$2"; output="$3"; weights="$4"; phase="$5"; snapshot="$6"; log_dir="$7"; shard="${8:-0}"; run_suite="$9"
# Charge elapsed execution time only; time waiting in gq is not machine time.
# The local lock also serializes report writes and build reuse for this round.
if [[ "${NAVLIVE_BUDGET_CHILD:-0}" != 1 ]]; then
  exec 9>"$task/$(basename "$output")-budget.lock"
  flock 9
  allowance="$(python3 - "$output/runtime.json" "$phase" <<'BUDGET'
import json
from pathlib import Path
import sys
path=Path(sys.argv[1])
used=json.loads(path.read_text()).get('wall_seconds', 0) if path.is_file() else 0
remaining=10800-used-30  # Reserve termination and final bookkeeping time.
if remaining <= 0:
    raise SystemExit('Round exhausted its 10800-second machine-time budget')
print(min(570 if sys.argv[2] == 'teacher' else 3300, remaining))
BUDGET
)"
  started_at=$(date +%s)
  deadline_at=$((started_at+allowance))
  result=0
  timeout --signal=TERM --kill-after=10 "$allowance" env NAVLIVE_BUDGET_CHILD=1 NAVLIVE_DEADLINE="$deadline_at" bash "$0" "$@" || result=$?
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
if [[ "$phase" == qualify || "$phase" == requalify || "$phase" == extend || "$phase" == teacher || "$phase" == smoke || "$phase" == baseline ]]; then
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
if [[ "$phase" == extend ]]; then
  if [[ ! -x "$task/.venv/bin/python" ]]; then uv venv --python 3.12 "$task/.venv"; fi
  uv pip install --python "$task/.venv/bin/python" numpy pillow
fi
if [[ "$phase" != extend && ( "$phase" == prepare || "$phase" == qualify || "$(cat "$task/build-snapshot" 2>/dev/null)" != "$snapshot" ) ]]; then
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
  if [[ "$phase" == prepare && "$qualification" == pending && ! -f "$output/preparation/complete.json" ]]; then
    "$task/.venv/bin/python" -B "$source/scripts/nav_live_prepare.py" \
      --runtime "$task" --output "$output" --suite "$source/suite.json"
  fi
  touch "$task/build-passed-$snapshot"
  printf '%s\n' "$snapshot" > "$task/build-snapshot"
fi
if [[ "$phase" == qualify || "$phase" == requalify || "$phase" == extend ]]; then
  "$task/.venv/bin/python" -B "$source/scripts/nav_live_prepare.py" "--$phase" \
    --runtime "$task" --output "$output" --suite "$source/suite.json" --weights "$weights"
elif [[ "$phase" != prepare ]]; then
  "$task/.venv/bin/python" -B "$source/scripts/nav_live.py" "$phase" \
    --suite "$run_suite" --runtime "$task" --output "$output" --weights "$weights" --shard "$shard"
fi
if [[ "$phase" == baseline || "$phase" == extend ]]; then
  python3 - "$source/scripts" "$task" "$output" "$run_suite" <<'CLEAN'
from pathlib import Path
import sys
sys.path.insert(0,sys.argv[1])
from nav_live import cleanup
cleanup(Path(sys.argv[2]),Path(sys.argv[3]))
CLEAN
fi
printf 'phase_wall_seconds=%s\n' "$((SECONDS-started))"
PHASE
if [[ "$stage" == baseline ]]; then shards=(-1); else shards=(0); fi
for shard in "${shards[@]}"; do
  log_dir="$log_prefix-$shard"
  ssh ryzen "mkdir -p '$log_dir'"
  display=()
  [[ "$stage" == qualify || "$stage" == requalify || "$stage" == extend || "$stage" == teacher || "$stage" == smoke || "$stage" == baseline ]] && display=(--display --display-shared)
  max_time=59m
  threads=16; cpu_quota=1200%; memory=10G
  if [[ "$stage" == teacher ]]; then
    max_time=10m; threads=8; cpu_quota=800%; memory=8G
  fi
  identifier=$("$ROOT/scripts/gq" submit --class quick --threads "$threads" --cpu-quota "$cpu_quota" --mem "$memory" --max "$max_time" \
    ${display[@]+"${display[@]}"} --archive "$log_dir" --name "nav live $round $stage $shard" -- \
    bash "$source/phase.sh" "$source" "$task" "$output" "$weights" "$stage" "$snapshot" "$log_dir" "$shard" "$run_suite")
  printf '%s shard %s: %s\n' "$stage" "$shard" "$identifier"
done
