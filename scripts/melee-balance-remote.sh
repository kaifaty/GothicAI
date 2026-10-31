#!/usr/bin/env bash
# Sync this checkout, build on Ryzen, then run one balance series per quiet job.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${MELEE_BALANCE_HOST:-ryzen}"
TASK="gothic-ai/melee-balance"
ACTION=all
EPISODES=64
SEED=100000
TIMEOUT_MS=60000
RUN_NAME="balance-$(date -u +%Y%m%dT%H%M%SZ)"
SERIES="native-vanilla-1h,native-posture-on-1h,native-vanilla-2h,native-posture-on-2h,p30-vanilla-1h,p30-posture-on-1h"

usage() {
  cat <<'USAGE'
Usage: scripts/melee-balance-remote.sh [all|sync|build|run|report] [options]
  --episodes N       Duels per series (default: 64)
  --seed N           First seed (default: 100000)
  --timeout-ms N     Game-time limit per duel (default: 60000)
  --series LIST      Comma-separated names from the default series below
  --run-name NAME    Fresh output name; reuse it for separate run/report actions

Default series: native-vanilla-1h,native-posture-on-1h,native-vanilla-2h,
native-posture-on-2h,p30-vanilla-1h,p30-posture-on-1h.
MELEE_BALANCE_HOST selects the SSH alias (default: ryzen).
MELEE_BALANCE_PYTHON selects a remote Python with torch (default: body/.venv).
MELEE_POSTURE_SCALE multiplies the knockdown threshold in every series (sweeps).
MELEE_DOWN_TACTICS sets GAME melee_down_tactics in every series (0 or 1).
MELEE_HITSTOP sets GAME melee_hitstop in every series (0 or 1).
USAGE
}

if [[ $# -gt 0 && "$1" != -* ]]; then ACTION="$1"; shift; fi
while [[ $# -gt 0 ]]; do
  case "$1" in
    --episodes|--seed|--timeout-ms|--series|--run-name)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      case "$1" in
        --episodes) EPISODES="$2" ;;
        --seed) SEED="$2" ;;
        --timeout-ms) TIMEOUT_MS="$2" ;;
        --series) SERIES="$2" ;;
        --run-name) RUN_NAME="$2" ;;
      esac
      shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done
case "$ACTION" in all|sync|build|run|report) ;; *) usage >&2; exit 2 ;; esac
[[ "$EPISODES" =~ ^[1-9][0-9]*$ && "$SEED" =~ ^[0-9]+$ && "$TIMEOUT_MS" =~ ^[1-9][0-9]*$ ]] || {
  echo 'Episodes and timeout must be positive integers; seed must be nonnegative' >&2; exit 2;
}
[[ "$RUN_NAME" =~ ^[a-zA-Z0-9][a-zA-Z0-9_-]*$ ]] || { echo 'Invalid run name' >&2; exit 2; }
IFS=, read -r -a selected_series <<<"$SERIES"
[[ ${#selected_series[@]} -gt 0 && "$SERIES" != *, ]] || { echo 'Empty series list' >&2; exit 2; }
seen=','
for name in "${selected_series[@]}"; do
  case "$name" in
    native-vanilla-1h|native-posture-on-1h|native-vanilla-2h|native-posture-on-2h|p30-vanilla-1h|p30-posture-on-1h) ;;
    *) echo "Unknown series: $name" >&2; exit 2 ;;
  esac
  [[ "$seen" != *",$name,"* ]] || { echo "Duplicate series: $name" >&2; exit 2; }
  seen+="$name,"
done

remote_script() {
  local remote_args='' remote_env='' arg key
  for arg in "$@"; do printf -v remote_args '%s %q' "$remote_args" "$arg"; done
  for key in GQ_BIN GQ_PROFILE MELEE_BALANCE_PYTHON MELEE_POSTURE_SCALE MELEE_DOWN_TACTICS MELEE_HITSTOP; do
    if [[ -n "${!key:-}" ]]; then printf -v remote_env '%s %q' "$remote_env" "$key=${!key}"; fi
  done
  { cat "$ROOT/scripts/gq-job.sh"; cat; } |
    ssh -o BatchMode=yes "$HOST" "env$remote_env bash -s --$remote_args"
}

sync_sources() {
  remote_script "$TASK" <<'REMOTE'
set -euo pipefail
mkdir -p "$HOME/$1/source" "$HOME/$1/provenance" "$HOME/$1/runs"
mkdir -p /mnt/shared/gothic-ai/melee-balance
df -h "$HOME" /mnt/shared/gothic-ai/melee-balance
REMOTE
  for directory in OpenGothic scripts body-service; do
    rsync -a --delete --exclude=.git --exclude=build/ --exclude='build-*/' \
      --exclude=__pycache__/ --exclude=.pytest_cache/ --exclude=.venv/ --exclude=.DS_Store \
      "$ROOT/$directory/" "$HOST:$TASK/source/$directory/"
  done
  git -C "$ROOT" rev-parse HEAD | ssh -o BatchMode=yes "$HOST" "cat > '$TASK/provenance/source-commit.txt'"
  git -C "$ROOT" diff --binary HEAD -- OpenGothic scripts body-service |
    ssh -o BatchMode=yes "$HOST" "cat > '$TASK/provenance/source.patch'"
}

build_engine() {
  remote_script "$TASK" <<'REMOTE'
set -euo pipefail
task_root="$HOME/$1"
cd "$task_root"
gq_available || { echo 'gq is required on Ryzen' >&2; exit 127; }
gq_job --class quick --threads 4 --mem 8G --max 60m --name 'melee balance build' -- \
  bash -c '
set -euo pipefail
task_root="$1"
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$VULKAN_SDK/bin:$PATH"
jolt_x86=portable
if grep -qw avx2 /proc/cpuinfo && grep -qw f16c /proc/cpuinfo && grep -qw bmi1 /proc/cpuinfo && grep -qw abm /proc/cpuinfo; then
  jolt_x86=x86-64-v3
fi
cmake -S "$task_root/source/OpenGothic" -B "$task_root/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DOPENGOTHIC_JOLT_X86="$jolt_x86" \
  -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib" \
  > "$task_root/provenance/configure.log" 2>&1 || { cat "$task_root/provenance/configure.log" >&2; exit 1; }
cmake --build "$task_root/build" --target gothic-body-runner Gothic2Notr --parallel 4 \
  > "$task_root/provenance/build.log" 2>&1 || { tail -80 "$task_root/provenance/build.log" >&2; exit 1; }
sha256sum "$task_root/build/opengothic/gothic-body-runner" > "$task_root/provenance/runner.sha256"
tail -8 "$task_root/provenance/build.log"
' bash "$task_root"
REMOTE
}

run_series() {
  remote_script "$TASK" "$RUN_NAME" "$EPISODES" "$SEED" "$TIMEOUT_MS" "$SERIES" <<'REMOTE'
set -euo pipefail
task_root="$HOME/$1"
run_name="$2"
episodes="$3"
seed="$4"
timeout_ms="$5"
IFS=, read -r -a series <<<"$6"
python="${MELEE_BALANCE_PYTHON:-$HOME/gothic-ai/body/.venv/bin/python}"
runner="$task_root/build/opengothic/gothic-body-runner"
reference="$HOME/gothic-ai/body-baseline/runs/melee-reference-stage1-v4"
archive_root="/mnt/shared/gothic-ai/melee-balance/runs/$run_name"
[[ -x "$runner" && -x "$python" ]] || { echo 'Build the runner and select an installed Python first' >&2; exit 1; }
gq_available || { echo 'gq is required on Ryzen' >&2; exit 127; }
cd "$task_root"
for name in "${series[@]}"; do
  out="$task_root/runs/$run_name/$name"
  [[ ! -e "$out" && ! -e "$archive_root/$name" ]] || { echo "Series exists: $name; choose a fresh run name" >&2; exit 1; }
  provider="${name%%-*}"
  weapon="${name##*-}"
  variant="${name#*-}"
  variant="${variant%-*}"
  policy_args=()
  [[ "$provider" != p30 ]] || policy_args=(--checkpoint "$HOME/gothic-ai/body/models/working/best.pt" --policy-only)
  mkdir -p "$out"
  scale_env=()
  if [[ -n "${MELEE_POSTURE_SCALE:-}" ]]; then
    scale_env=("OPENGOTHIC_MELEE_POSTURE_SCALE=$MELEE_POSTURE_SCALE")
    printf '%s\n' "$MELEE_POSTURE_SCALE" > "$out/posture-scale.txt"
  fi
  if [[ -n "${MELEE_DOWN_TACTICS:-}" ]]; then
    scale_env+=("MELEE_DOWN_TACTICS=$MELEE_DOWN_TACTICS")
  fi
  if [[ -n "${MELEE_HITSTOP:-}" ]]; then
    scale_env+=("MELEE_HITSTOP=$MELEE_HITSTOP")
    printf '%s\n' "$MELEE_HITSTOP" > "$out/hitstop.txt"
  fi
  cp "$task_root/provenance/source-commit.txt" "$task_root/provenance/source.patch" \
     "$task_root/provenance/runner.sha256" "$out/"
  # --archive accepts the local source; gq moves it to the same relative NAS path.
  gq_job --class quiet --threads 3 --mem 6G --cpu-quota 200% --max 3h \
    --archive "$out" --name "melee balance $name" -- env \
    "PYTHONPATH=$task_root/source/body-service/src:$task_root/source/scripts" \
    "DISPLAY=" "WAYLAND_DISPLAY=" "GOTHIC_AI=0" "CUDA_VISIBLE_DEVICES=" \
    "OMP_NUM_THREADS=1" "MKL_NUM_THREADS=1" "OPENBLAS_NUM_THREADS=1" \
    "VK_DRIVER_FILES=/nonexistent/melee-balance-no-vulkan.json" \
    "VK_ICD_FILENAMES=/nonexistent/melee-balance-no-vulkan.json" "OPENGOTHIC_TEXTURES=" \
    "OPENGOTHIC_BASE=" "OPENGOTHIC_BASE_CACHE=$task_root/base-cache" \
    "OPENGOTHIC_BASE_STORE=/mnt/shared/gothic-ai/melee-balance" "${scale_env[@]}" \
    bash -c 'out="$1"; shift; exec "$@" > "$out/launcher.log" 2>&1' bash "$out" \
    "$python" "$task_root/source/scripts/body_sim_bench.py" --runner "$runner" \
    --assets "$HOME/gothic-ai/body-baseline/assets" --save "$reference/run/save_slot_1.sav" \
    --reference "$reference" --scenario "$task_root/source/scripts/bench/body-arena.bench" \
    --output "$out/bench" --episodes "$episodes" --seed "$seed" --timeout-ms "$timeout_ms" \
    --weapon "$weapon" --variants "$variant" "${policy_args[@]}"
  [[ -f "$archive_root/$name/bench/bench.json" ]] || {
    echo "Series archive is missing: $archive_root/$name (local copy may remain in $out)" >&2; exit 1;
  }
  echo "Series complete: $archive_root/$name/bench"
done
REMOTE
}

report_series() {
  remote_script "$TASK" "$RUN_NAME" "$SERIES" <<'REMOTE'
set -euo pipefail
task_root="$HOME/$1"
run_name="$2"
IFS=, read -r -a series <<<"$3"
out="$task_root/runs/$run_name/report"
inputs=()
for name in "${series[@]}"; do inputs+=("/mnt/shared/gothic-ai/melee-balance/runs/$run_name/$name/bench"); done
[[ ! -e "$out" && ! -e "/mnt/shared/gothic-ai/melee-balance/runs/$run_name/report" ]] || {
  echo 'Report exists; choose a fresh run name or read the existing report' >&2; exit 1;
}
cd "$task_root"
gq_job --class quick --threads 2 --mem 2G --max 20m --archive "$out" --name 'melee balance report' -- \
  bash -c 'out="$1"; provenance="$2"; shift 2
    mkdir -p "$out/provenance"
    cp "$provenance/"* "$out/provenance/"
    exec "$@"' bash "$out" "$task_root/provenance" \
    python3 "$task_root/source/scripts/melee_balance_report.py" "${inputs[@]}" --output "$out"
echo "Report: /mnt/shared/gothic-ai/melee-balance/runs/$run_name/report"
REMOTE
}

case "$ACTION" in
  all) sync_sources; build_engine; run_series; report_series ;;
  sync) sync_sources ;;
  build) build_engine ;;
  run) run_series ;;
  report) report_series ;;
esac
