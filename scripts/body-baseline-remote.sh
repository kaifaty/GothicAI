#!/usr/bin/env bash
# Stage 1 reference baseline on dev-ubuntu. This is a rendered Linux game under Xvfb,
# not the future headless runner. No models or game processes start on the Mac.
# Usage: scripts/body-baseline-remote.sh sync|toolchain|build|run|collect|all [run-name]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${GOTHIC_BODY_SSH:-dev-ubuntu}"
REMOTE="gothic-ai/body-baseline"
ACTION="${1:-all}"
RUN_NAME="${2:-melee-$(date -u +%Y%m%dT%H%M%SZ)}"
BUILD_DIR="${BASELINE_BUILD:-build}"
[[ "$BUILD_DIR" == build || "$BUILD_DIR" == build-asan ]] || { echo 'Invalid BASELINE_BUILD' >&2; exit 2; }
[[ "$RUN_NAME" =~ ^[a-zA-Z0-9][a-zA-Z0-9_-]*$ ]] || { echo 'Invalid run name' >&2; exit 2; }

remote_script() {
  local remote_args="" remote_env="" arg key
  for arg in "$@"; do printf -v remote_args '%s %q' "$remote_args" "$arg"; done
  for key in GQ_BIN GQ_OWNER GQ_HOME GQ_LEGACY_LOCK GQ_PROFILE; do
    if [[ -n "${!key:-}" ]]; then
      printf -v remote_env '%s %q' "$remote_env" "$key=${!key}"
    fi
  done
  { cat "$ROOT/scripts/gq-job.sh"; cat; } |
    ssh -o BatchMode=yes "$HOST" "env$remote_env bash -s --$remote_args"
}

sync_sources() {
  local assets="${GOTHIC_DIR:-/Volumes/DevSSD/Dev/games/Gothic II}"
  local save="${BASELINE_SAVE:-$ROOT/save_slot_1.sav}"
  [[ -d "$assets/Data" && -d "$assets/_work" && -f "$save" ]] || {
    echo 'Set GOTHIC_DIR and BASELINE_SAVE to your own Gothic II data and reference save' >&2
    exit 1
  }
  remote_script "$REMOTE" <<'REMOTE_SCRIPT'
set -euo pipefail
task_root="$HOME/$1"
mkdir -p "$task_root/source/scripts/bench" "$task_root/source/evals" \
         "$task_root/assets" "$task_root/reference" "$task_root/runs"
free_kib=$(df -Pk "$task_root" | awk 'NR==2 {print $4}')
[[ "$free_kib" -ge 10485760 ]] || { echo 'Less than 10 GiB free for reference build/assets' >&2; exit 1; }
REMOTE_SCRIPT
  rsync -a --exclude=build/ --exclude=.git/ --exclude=.DS_Store --exclude='*.o' --exclude='*.a' \
    "$ROOT/OpenGothic/" "$HOST:$REMOTE/source/OpenGothic/"
  rsync -a --exclude=build/ --exclude=__pycache__/ "$ROOT/scripts/convert" "$HOST:$REMOTE/source/scripts/"
  rsync -a "$ROOT/scripts/body-baseline-toolchain.sh" "$ROOT/scripts/body-baseline-remote.sh" "$ROOT/scripts/base-ensure.sh" \
    "$ROOT/scripts/body_baseline_report.py" \
    "$HOST:$REMOTE/source/scripts/"
  rsync -a "$ROOT/scripts/bench/body-melee.bench" "$HOST:$REMOTE/source/scripts/bench/"
  rsync -a --exclude=__pycache__/ "$ROOT/evals/npc-body/" "$HOST:$REMOTE/source/evals/npc-body/"
  rsync -a "$assets/Data" "$assets/_work" "$HOST:$REMOTE/assets/"
  rsync -a "$save" "$HOST:$REMOTE/reference/save_slot_1.sav"
  local source_head source_patch
  source_head="$(git -C "$ROOT" rev-parse HEAD)"
  source_patch="$(mktemp)"
  git -C "$ROOT" diff --binary HEAD -- OpenGothic > "$source_patch"
  rsync -a "$source_patch" "$HOST:$REMOTE/reference/source.patch"
  rm "$source_patch"
  remote_script "$REMOTE" "$source_head" <<'REMOTE_SCRIPT'
set -euo pipefail
task_root="$HOME/$1"
# No repository credentials or .env files are uploaded.
printf '%s\n' "$2" > "$task_root/reference/source-head.txt"
REMOTE_SCRIPT
}

prepare_toolchain() {
  remote_script "$REMOTE" <<'REMOTE_SCRIPT'
set -euo pipefail
task_root="$HOME/$1"
if gq_available; then
  cd "$task_root"
  gq_job --class quick --threads 3 --mem 6G --max 1h --name body-baseline-toolchain -- \
    bash "$task_root/source/scripts/body-baseline-toolchain.sh"
else
  bash "$task_root/source/scripts/body-baseline-toolchain.sh"
fi
REMOTE_SCRIPT
}

build_reference() {
  remote_script "$REMOTE" <<'REMOTE_SCRIPT'
set -euo pipefail
task_root="$HOME/$1"
if gq_available; then
  exec 8>"$task_root/build.lock"
  flock -n 8 || { echo 'Another build or snapshot owns this workspace' >&2; exit 1; }
  cd "$task_root"
  build_command="$(cat <<'GQ_BUILD'
set -euo pipefail
task_root="$1"
export VULKAN_SDK="$task_root/toolchain/vulkan-1.4.341.0"
export PATH="$VULKAN_SDK/bin:$PATH"
[[ -x "$VULKAN_SDK/bin/glslangValidator" && -f "$VULKAN_SDK/include/vulkan/vulkan.h" ]] || {
  echo 'Prepare the user-local toolchain first' >&2; exit 1;
}
nice -n 10 cmake -S "$task_root/source/OpenGothic" -B "$task_root/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib" \
  > "$task_root/reference/configure.log" 2>&1
nice -n 10 cmake --build "$task_root/build" --target Gothic2Notr --parallel 2 \
  > "$task_root/reference/build.log" 2>&1 || {
  tail -60 "$task_root/reference/build.log" >&2; exit 1;
}
tail -5 "$task_root/reference/build.log"
GQ_BUILD
)"
  gq_job --class quick --threads 3 --mem 6G --max 30m --name "body-baseline-build" -- \
    bash -c "$build_command" bash "$task_root"
else
  export VULKAN_SDK="$task_root/toolchain/vulkan-1.4.341.0"
  export PATH="$VULKAN_SDK/bin:$PATH"
  [[ -x "$VULKAN_SDK/bin/glslangValidator" && -f "$VULKAN_SDK/include/vulkan/vulkan.h" ]] || {
    echo 'Prepare the user-local toolchain first' >&2; exit 1;
  }
  nice -n 10 cmake -S "$task_root/source/OpenGothic" -B "$task_root/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib" \
    > "$task_root/reference/configure.log" 2>&1
  nice -n 10 cmake --build "$task_root/build" --target Gothic2Notr --parallel 2 \
    > "$task_root/reference/build.log" 2>&1 || {
    tail -60 "$task_root/reference/build.log" >&2; exit 1;
  }
  tail -5 "$task_root/reference/build.log"
fi
REMOTE_SCRIPT
}

run_reference() {
  remote_script "$REMOTE" "$RUN_NAME" "$BUILD_DIR" <<'REMOTE_SCRIPT'
set -euo pipefail
task_root="$HOME/$1"
run_name="$2"
binary="$task_root/$3/opengothic/Gothic2Notr"
out="$task_root/runs/$run_name"
[[ -x "$binary" ]] || { echo 'Build reference engine first' >&2; exit 1; }
exec 9>"$task_root/reference-run.lock"
flock -n 9 || { echo 'Another baseline launcher owns the runner lock' >&2; exit 1; }
if gq_available; then
  exec 8>"$task_root/build.lock"
  flock -n 8 || { echo 'Build owns the reference snapshot; wait for its owner' >&2; exit 1; }
fi
if ! gq_available && pgrep -x 'Gothic2Notr|gothic-body-run' >/dev/null; then
  echo 'Another game/runner is active; wait for its owner' >&2; exit 1
fi
[[ ! -e "$out" ]] || { echo 'Run directory already exists; choose a new run-name' >&2; exit 1; }
mkdir -p "$out/run"
cp "$task_root/reference/save_slot_1.sav" "$out/run/save_slot_1.sav"
# Keep the pre-headless binary and instrumented source even after later builds replace the checkout.
mkdir -p "$out/reference"
cp "$binary" "$out/reference/Gothic2Notr"
if [[ -x "$(dirname "$binary")/content-convert" ]]; then
  cp "$(dirname "$binary")/content-convert" "$out/reference/"
fi
[[ ! -f "$task_root/reference/source.patch" ]] || cp "$task_root/reference/source.patch" "$out/source.patch"
suite_source="$task_root/source"
source_provenance="$task_root/reference"
if gq_available; then
  nice -n 10 tar -czf "$out/source.tar.gz" -C "$task_root/source" OpenGothic scripts evals
  suite_source="$out/reference/source"
  mkdir -p "$suite_source"
  tar -xzf "$out/source.tar.gz" -C "$suite_source"
  [[ ! -f "$task_root/reference/source-head.txt" ]] || cp "$task_root/reference/source-head.txt" "$out/source-head.txt"
  source_provenance="$out"
else
  nice -n 10 tar -czf "$out/source.tar.gz" -C "$task_root/source" OpenGothic
fi
cat > "$out/run/Gothic.ini" <<'INI'
[INTERNAL]
vidResIndex=2
[SOUND]
soundVolume=0
musicVolume=0
INI
python3 - "$task_root" "$out" "$binary" "$suite_source" "$source_provenance" <<'PY'
import datetime, hashlib, json, platform, subprocess, sys
from pathlib import Path
root, out, binary, suite_source, source_provenance = map(Path, sys.argv[1:])
def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()
source = hashlib.sha256()
count = 0
for path in sorted((suite_source / 'OpenGothic').rglob('*')):
    if path.is_file():
        source.update(str(path.relative_to(suite_source)).encode())
        source.update(bytes.fromhex(sha(path)))
        count += 1
assets = []
for path in sorted((root / 'assets').rglob('*')):
    if path.is_file():
        assets.append({'path': str(path.relative_to(root / 'assets')), 'size': path.stat().st_size, 'sha256': sha(path)})
build_cache = {}
for line in (binary.parent.parent / 'CMakeCache.txt').read_text().splitlines():
    if line.startswith(('CMAKE_BUILD_TYPE:', 'CMAKE_CXX_FLAGS:', 'CMAKE_CXX_FLAGS_RELWITHDEBINFO:', 'CMAKE_C_FLAGS:', 'CMAKE_EXE_LINKER_FLAGS:')):
        key, value = line.split('=', 1)
        build_cache[key.split(':', 1)[0]] = value
data = {
    'schema_version': 1, 'kind': 'rendered_reference_before_headless',
    'recorded_at_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'host': platform.node(), 'platform': platform.platform(),
    'source_head': (source_provenance / 'source-head.txt').read_text().strip() if (source_provenance / 'source-head.txt').exists() else None,
    'source_patch_sha256': sha(source_provenance / 'source.patch') if (source_provenance / 'source.patch').exists() else None,
    'source_sha256': source.hexdigest(), 'source_files_hashed': count,
    'binary_sha256': sha(binary), 'build_dir': str(binary.parent.parent), 'build_cache': build_cache,
    'archived_binary': str(out / 'reference/Gothic2Notr'),
    'source_archive': {'path': str(out / 'source.tar.gz'), 'sha256': sha(out / 'source.tar.gz')},
    'scenario_sha256': sha(suite_source / 'scripts/bench/body-melee.bench'),
    'config_sha256': sha(suite_source / 'evals/npc-body/melee-v1.json'),
    'save_sha256': sha(root / 'reference/save_slot_1.sav'), 'assets': assets,
    'renderer': 'lavapipe', 'display': 'Xvfb', 'headless': False,
    'sim_fps': 30, 'realtime': True, 'model': None,
    'trace_enabled': True, 'hd_textures_enabled': False, 'audio_driver': 'OpenAL null',
    'game_version': 'Gothic II Night of the Raven (explicit -g2)',
    'argv': ['-g', str(root / 'assets'), '-g2', '-window', '-devmode', '-save', '1'],
    'settings_sha256': sha(out / 'run/Gothic.ini'), 'asset_hash_coverage': 'all regular files in assets',
    'toolchain': json.loads((root / 'toolchain/vulkan-1.4.341.0/manifest.json').read_text()),
    'runtime_limits': {'cpu_quota_percent': 200, 'memory_max_gib': 12, 'tasks_max': 128, 'wall_timeout_sec': 900, 'renderer_threads': 2},
    'gpu_before': subprocess.run(['nvidia-smi', '--query-gpu=memory.used,memory.free,utilization.gpu', '--format=csv,noheader'], capture_output=True, text=True).stdout.strip(),
    'services_before': {unit: subprocess.run(['systemctl', '--user', 'show', unit, '--property=ActiveState,SubState,MemoryCurrent,CPUUsageNSec'], capture_output=True, text=True).stdout.strip() for unit in ('gothic-voxtral', 'gothic-voxcpm')},
}
(out / 'manifest.json').write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n')
PY
if gq_available; then
  flock -u 8
  exec 8>&-
fi
unit="gothic-body-baseline-$run_name"
baseline_interrupted=0
stop_baseline() {
  if gq_available; then
    baseline_interrupted=1
    # The parent also owns the watchdog. Wait for submit to publish its ID,
    # then cancel the owned request and collect the helper's final status.
    while [[ ! -s "$out/gq-job-id" && -n "${launch_pid:-}" ]] &&
          kill -0 "$launch_pid" 2>/dev/null; do
      sleep 0.1 || true
    done
    if [[ -s "$out/gq-job-id" ]]; then
      GQ_OWNER="${GQ_OWNER:-$(basename "$task_root")}" \
        "${GQ_BIN:-$HOME/gothic-ai/bin/gq}" cancel "$(cat "$out/gq-job-id")" || true
    fi
  else
    systemctl --user stop "$unit.service" >/dev/null 2>&1 || true
  fi
}
if gq_available; then
  cd "$out/run"
  trap 'stop_baseline' INT TERM HUP
  launch=(gq_job --class quiet --threads 3 --mem 12G --display --cpu-quota 200% \
    --max 15m --name "body-baseline-$run_name" -- env \
    "VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json" "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json" \
    "LP_NUM_THREADS=2" "ALSOFT_DRIVERS=null" \
    "GOTHIC_AI=0" "TEMPEST_BACKGROUND=1" \
    "TEMPEST_WINDOW_SIZE=640x480" "OPENGOTHIC_TEXTURES=" \
    "OPENGOTHIC_BODY_TRACE=1" "OPENGOTHIC_BENCH_REALTIME=1" \
    "OPENGOTHIC_BENCH=$suite_source/scripts/bench/body-melee.bench" "OPENGOTHIC_BENCH_OUT=$out" \
    "ASAN_OPTIONS=abort_on_error=0:halt_on_error=1:detect_leaks=0:exitcode=86:allow_addr2line=1:log_path=$out/asan")
else
  # Only this launcher's own transient unit can be stopped by the watchdog.
  trap 'systemctl --user stop "$unit.service" >/dev/null 2>&1 || true' INT TERM
  launch=(systemd-run --user --wait --pipe --collect --unit="$unit" \
  --property=Nice=10 --property=CPUQuota=200% --property=MemoryMax=12G \
  --property=TasksMax=128 --property=RuntimeMaxSec=900 \
  --working-directory="$out/run" \
  --setenv="VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json" \
  --setenv="VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json" \
  --setenv="LP_NUM_THREADS=2" --setenv="ALSOFT_DRIVERS=null" --setenv="GOTHIC_AI=0" \
  --setenv="TEMPEST_BACKGROUND=1" --setenv="TEMPEST_WINDOW_SIZE=640x480" \
  --setenv="OPENGOTHIC_TEXTURES=" --setenv="OPENGOTHIC_BODY_TRACE=1" \
  --setenv="OPENGOTHIC_BENCH_REALTIME=1" \
  --setenv="OPENGOTHIC_BENCH=$task_root/source/scripts/bench/body-melee.bench" \
  --setenv="OPENGOTHIC_BENCH_OUT=$out" \
  --setenv="ASAN_OPTIONS=abort_on_error=0:halt_on_error=1:detect_leaks=0:exitcode=86:allow_addr2line=1:log_path=$out/asan")
fi
GQ_JOB_ID_FILE="$out/gq-job-id" "${launch[@]}" \
  bash -ec 'GAME_BIN="$1" bash "$2" "$3"; shift 3; exec "$@"' bash \
  "$out/reference/Gothic2Notr" "$suite_source/scripts/base-ensure.sh" "$task_root/assets" \
  /usr/bin/time -v -o "$out/resources.txt" \
  /usr/bin/xvfb-run -a -s '-screen 0 640x480x24' "$out/reference/Gothic2Notr" \
  -g "$task_root/assets" -g2 -window -devmode -save 1 > "$out/launcher.log" 2>&1 &
launch_pid=$!
if gq_available && [[ "$baseline_interrupted" == 1 ]]; then stop_baseline; fi
failed=0
while kill -0 "$launch_pid" 2>/dev/null; do
  if grep -Eq 'loading error:|unable to load landscape mesh|Fatal glibc|---crashlog\(|ERROR: AddressSanitizer|bodyduel: setup_failed' "$out/launcher.log" || \
     { [[ -f "$out/run/log.txt" ]] && grep -Eq '^loading error:|^bodyduel: setup_failed' "$out/run/log.txt"; } || \
     { [[ -f "$out/bench.log" ]] && grep -q 'bodyduel: setup_failed' "$out/bench.log"; }; then
    echo 'Reference loading/setup failed; stopping owned baseline unit' >&2
    stop_baseline
    failed=1
    break
  fi
  if gq_available; then sleep 2 || true; else sleep 2; fi
done
launch_result=0
if gq_available; then
  while :; do
    baseline_interrupted=0
    wait "$launch_pid" && launch_result=0 || launch_result=$?
    # A trapped signal interrupts wait before gq wait has returned its code.
    [[ "$baseline_interrupted" == 1 ]] || break
  done
  [[ "$launch_result" == 0 ]] || failed=1
else
  wait "$launch_pid" || { launch_result=$?; failed=1; }
fi
trap - INT TERM HUP
if [[ "$failed" -ne 0 ]]; then
  tail -40 "$out/launcher.log" >&2
  [[ ! -f "$out/run/log.txt" ]] || tail -40 "$out/run/log.txt" >&2
  if gq_available && [[ "$launch_result" -ne 0 ]]; then exit "$launch_result"; fi
  exit 1
fi
cp "$out/run/log.txt" "$out/game.log"
grep -qx 'done' "$out/bench.log" || { echo 'Bench did not finish; reference is incomplete' >&2; exit 1; }
python3 - "$out" <<'PY'
import datetime, json, subprocess, sys
from pathlib import Path
out = Path(sys.argv[1])
path = out / 'manifest.json'
data = json.loads(path.read_text())
data['completed_at_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
data['gpu_after'] = subprocess.run(['nvidia-smi', '--query-gpu=memory.used,memory.free,utilization.gpu', '--format=csv,noheader'], capture_output=True, text=True).stdout.strip()
data['resources'] = (out / 'resources.txt').read_text() if (out / 'resources.txt').exists() else None
data['renderer_gpu_vram_mib'] = 0  # explicitly selected CPU lavapipe ICD
data['services_after'] = {unit: subprocess.run(['systemctl', '--user', 'show', unit, '--property=ActiveState,SubState,MemoryCurrent,CPUUsageNSec'], capture_output=True, text=True).stdout.strip() for unit in ('gothic-voxtral', 'gothic-voxcpm')}
data['service_latency_measurement'] = None  # no live ASR/TTS requests in the native reference
path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n')
PY
python3 "$suite_source/scripts/body_baseline_report.py" "$out" \
  --manifest "$out/manifest.json" --output-dir "$out/report"
echo "Reference run saved: $out"
REMOTE_SCRIPT
}

collect_reference() {
  local local_out="$ROOT/data/npc-body/reference/$RUN_NAME"
  mkdir -p "$local_out"
  rsync -a --exclude=run/ --exclude=reference/ "$HOST:$REMOTE/runs/$RUN_NAME/" "$local_out/"
  echo "Collected reference: $local_out"
}

case "$ACTION" in
  sync) sync_sources ;;
  toolchain) prepare_toolchain ;;
  build) build_reference ;;
  run) run_reference ;;
  collect) collect_reference ;;
  all) sync_sources; prepare_toolchain; build_reference; run_reference; collect_reference ;;
  *) echo 'Usage: body-baseline-remote.sh sync|toolchain|build|run|collect|all [run-name]' >&2; exit 2 ;;
esac
