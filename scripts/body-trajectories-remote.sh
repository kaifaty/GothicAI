#!/usr/bin/env bash
# Stage 6 collection and dataset building run only on dev-ubuntu.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${GOTHIC_BODY_SSH:-dev-ubuntu}"
REMOTE="gothic-ai/body"
ENGINE="gothic-ai/body-api"
REFERENCE="gothic-ai/body-baseline/runs/melee-reference-stage1-v4"
ACTION="${1:-preflight}"
RUN_NAME="${2:-stage6-trajectories-$(date -u +%Y%m%dT%H%M%SZ)}"
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

preflight() {
  remote_script "$REMOTE" "$REFERENCE" <<'REMOTE_SCRIPT'
set -euo pipefail
[[ -f "$HOME/$2/manifest.json" && -f "$HOME/$2/run/save_slot_1.sav" && \
   -f "$HOME/gothic-ai/body-headless/runs/stage3-v1/native-off/manifest.json" ]] || {
  echo 'Archived baseline/save/native reference missing' >&2; exit 1;
}
[[ -x "$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0/bin/glslangValidator" ]] || {
  echo 'User-local toolchain missing' >&2; exit 1;
}
df -h "$HOME/gothic-ai"
free -h
nvidia-smi --query-gpu=memory.used,memory.free,utilization.gpu --format=csv,noheader
# Collection is CPU-only. Preserve each service's actual baseline, including an
# intentionally inactive service; the independent report verifies no state/PID change.
systemctl --user show gothic-voxtral gothic-voxcpm --property=Id,ActiveState,SubState,MainPID
if ! gq_available && pgrep -x 'Gothic2Notr|gothic-body-run' >/dev/null; then
  echo 'Another game/runner is active; wait for its owner' >&2; exit 1
fi
mkdir -p "$HOME/$1/data" "$HOME/$1/source/scripts" "$HOME/$1/source/evals/npc-body" "$HOME/$1/provenance"
echo "Stage 6 data: $HOME/$1/data"
REMOTE_SCRIPT
}

sync_sources() {
  "$ROOT/scripts/body-arena-remote.sh" sync
  remote_script "$REMOTE" <<'REMOTE_SCRIPT'
set -euo pipefail
mkdir -p "$HOME/$1/source/scripts/bench" "$HOME/$1/source/evals/npc-body" "$HOME/$1/provenance" "$HOME/$1/data"
REMOTE_SCRIPT
  rsync -a "$ROOT/scripts/body-trajectories-remote.sh" "$ROOT/scripts/body_trajectory_recorder.py" "$ROOT/scripts/base-ensure.sh" \
    "$ROOT/scripts/body_trajectory_dataset.py" "$ROOT/scripts/body_trajectory_suite.py" \
    "$ROOT/scripts/body_trajectory_report.py" \
    "$ROOT/scripts/body_arena_env.py" "$ROOT/scripts/body_arena_suite.py" "$ROOT/scripts/body_arena_report.py" \
    "$ROOT/scripts/body_api_suite.py" "$ROOT/scripts/body_api_report.py" "$ROOT/scripts/body_observe_suite.py" \
    "$ROOT/scripts/body_headless_suite.py" "$ROOT/scripts/body_headless_parity.py" "$ROOT/scripts/body_baseline_report.py" \
    "$HOST:$REMOTE/source/scripts/"
  rsync -a "$ROOT/scripts/bench/body-arena.bench" "$ROOT/scripts/bench/body-melee.bench" "$HOST:$REMOTE/source/scripts/bench/"
  rsync -a --delete --exclude=__pycache__/ "$ROOT/evals/npc-body/" "$HOST:$REMOTE/source/evals/npc-body/"
}

build_runner() {
  "$ROOT/scripts/body-arena-remote.sh" build
  remote_script "$REMOTE" "$ENGINE" <<'REMOTE_SCRIPT'
set -euo pipefail
mkdir -p "$HOME/$1/provenance"
cp "$HOME/$2/provenance/source-head.txt" "$HOME/$2/provenance/source.patch" \
   "$HOME/$2/provenance/configure.log" "$HOME/$2/provenance/build.log" \
   "$HOME/$2/provenance/resolver-tests.json" "$HOME/$1/provenance/"
REMOTE_SCRIPT
}

run_suite() {
  remote_script "$REMOTE" "$ENGINE" "$REFERENCE" "$RUN_NAME" "${1:-full}" <<'REMOTE_SCRIPT'
set -euo pipefail
task_root="$HOME/$1"
engine_root="$HOME/$2"
reference="$HOME/$3"
run_name="$4"
suite_mode="$5"
suite_args=()
[[ "$suite_mode" != smoke ]] || suite_args+=(--smoke)
out="$task_root/data/$run_name"
binary="$engine_root/build/opengothic/gothic-body-runner"
[[ -x "$binary" && -f "$engine_root/build/libOpenGothic.a" ]] || { echo 'Build runner first' >&2; exit 1; }
if gq_available; then
  exec 8>"$engine_root/build.lock"
  flock -n 8 || { echo 'Build owns the engine snapshot; wait for its owner' >&2; exit 1; }
fi
[[ ! -e "$engine_root/build-active" ]] || { echo 'Wait for build' >&2; exit 1; }
if ! gq_available; then
  exec 9>"$HOME/gothic-ai/body-simulation.lock"
  flock -n 9 || { echo 'Another simulation owns shared lock' >&2; exit 1; }
fi
if ! gq_available && pgrep -x 'Gothic2Notr|gothic-body-run' >/dev/null; then
  echo 'Another game/runner is active; wait for its owner' >&2; exit 1
fi
[[ ! -e "$out" ]] || { echo 'Dataset run exists; choose fresh name' >&2; exit 1; }
mkdir -p "$out/run" "$out/reference"
cp "$binary" "$out/reference/gothic-body-runner"
cp "$task_root/provenance/source-head.txt" "$task_root/provenance/source.patch" \
   "$task_root/provenance/configure.log" "$task_root/provenance/build.log" \
   "$task_root/provenance/resolver-tests.json" "$out/"
cp "$engine_root/build/opengothic/gothic-body-resolver-test" "$out/reference/"
nice -n 10 tar -czf "$out/source.tar.gz" -C "$engine_root/source" OpenGothic -C "$task_root/source" scripts evals
suite_source="$task_root/source"
engine_source="$engine_root/source/OpenGothic"
engine_library="$engine_root/build/libOpenGothic.a"
if gq_available; then
  suite_source="$out/reference/source"
  mkdir -p "$suite_source"
  tar -xzf "$out/source.tar.gz" -C "$suite_source"
  engine_source="$suite_source/OpenGothic"
  cp "$engine_library" "$out/reference/libOpenGothic.a"
  engine_library="$out/reference/libOpenGothic.a"
  flock -u 8
  exec 8>&-
fi
unit="gothic-body-trajectories-$run_name"
if gq_available; then
  echo 'gq: batch queue wait has no timeout (FIFO with fairness between sessions)' >&2
  cd "$out/run"
  launch=(gq_job --class batch --threads 2 --mem 12G --cpu-quota 200% \
    --max 40m --name "body-trajectories-$run_name" -- env \
    "DISPLAY=" "WAYLAND_DISPLAY=" \
    "GOTHIC_AI=0" "VK_DRIVER_FILES=/nonexistent/body-trajectories-no-vulkan.json" \
    "VK_ICD_FILENAMES=/nonexistent/body-trajectories-no-vulkan.json" "OPENGOTHIC_TEXTURES=" \
    "OPENGOTHIC_BODY_TRACE=1" "OPENGOTHIC_BODY_ANIMATION_RNG=isolated")
else
  trap 'systemctl --user stop "$unit.service" >/dev/null 2>&1 || true' INT TERM
  launch=(systemd-run --user --wait --pipe --collect --unit="$unit" \
  --property=Nice=10 --property=CPUQuota=200% --property=MemoryMax=12G \
  --property=TasksMax=128 --property=RuntimeMaxSec=2400 \
  --working-directory="$out/run" \
  --setenv="DISPLAY=" --setenv="WAYLAND_DISPLAY=" --setenv="GOTHIC_AI=0" \
  --setenv="VK_DRIVER_FILES=/nonexistent/body-trajectories-no-vulkan.json" \
  --setenv="VK_ICD_FILENAMES=/nonexistent/body-trajectories-no-vulkan.json" \
  --setenv="OPENGOTHIC_TEXTURES=" --setenv="OPENGOTHIC_BODY_TRACE=1" \
  --setenv="OPENGOTHIC_BODY_ANIMATION_RNG=isolated")
fi
collector=(/usr/bin/time -v -o "$out/resources.txt" \
  python3 "$suite_source/scripts/body_trajectory_suite.py" \
    --runner "$out/reference/gothic-body-runner" \
    --assets "$HOME/gothic-ai/body-baseline/assets" --save "$reference/run/save_slot_1.sav" \
    --reference "$reference" --native-reference "$HOME/gothic-ai/body-headless/runs/stage3-v1/native-off" \
    --source "$engine_source" --engine-library "$engine_library" \
    --config "$suite_source/evals/npc-body/body-trajectories-v1.json" \
    --scenario "$suite_source/scripts/bench/body-arena.bench" \
    --melee-scenario "$suite_source/scripts/bench/body-melee.bench" \
    --output "$out" ${suite_args[@]+"${suite_args[@]}"})
set +e
if gq_available; then
  # Both CPU-heavy stages belong to one admitted batch job. Stop at the
  # first failure, preserving the collector or reporter's actual exit code.
  "${launch[@]}" bash -c '
    out=$1; suite_source=$2; native_reference=$3; suite_mode=$4
    shift 4
    "$@" || exit "$?"
    if [[ "$suite_mode" != smoke ]]; then
      python3 "$suite_source/scripts/body_trajectory_report.py" "$out" \
        --config "$suite_source/evals/npc-body/body-trajectories-v1.json" \
        --native-reference "$native_reference" --output-dir "$out/validated" \
        > "$out/report-launcher.log" 2>&1 || {
          result=$?; tail -60 "$out/report-launcher.log" >&2; exit "$result"
        }
    fi
  ' gq-body-trajectories "$out" "$suite_source" \
    "$HOME/gothic-ai/body-headless/runs/stage3-v1/native-off" "$suite_mode" \
    "${collector[@]}" > "$out/launcher.log" 2>&1
else
  "${launch[@]}" "${collector[@]}" > "$out/launcher.log" 2>&1
fi
result=$?
set -e
trap - INT TERM
if [[ "$result" -ne 0 ]]; then
  tail -60 "$out/launcher.log" >&2
  echo "Trajectory suite failed ($result); evidence remains in $out" >&2
  exit "$result"
fi
if ! gq_available && [[ "$suite_mode" != smoke ]]; then
  python3 "$suite_source/scripts/body_trajectory_report.py" "$out" \
    --config "$suite_source/evals/npc-body/body-trajectories-v1.json" \
    --native-reference "$HOME/gothic-ai/body-headless/runs/stage3-v1/native-off" \
    --output-dir "$out/validated" > "$out/report-launcher.log" 2>&1 || {
      tail -60 "$out/report-launcher.log" >&2; exit 2;
    }
fi
echo "Trajectory dataset saved: $out"
REMOTE_SCRIPT
}

collect_suite() {
  local local_out="$ROOT/data/npc-body/trajectories/$RUN_NAME"
  mkdir -p "$local_out"
  rsync -a --exclude=run/ --exclude=reference/ --exclude=source.tar.gz \
    --exclude=records.jsonl --exclude=raw-trajectory.jsonl --exclude=capture-packets.jsonl --exclude=ipc.jsonl \
    --exclude=events.jsonl --exclude=observations.jsonl --exclude=observation-batches.jsonl \
    --exclude=observer-timing.jsonl --exclude=arena-events.jsonl --exclude=body-control-events.jsonl \
    "$HOST:$REMOTE/data/$RUN_NAME/" "$local_out/"
  echo "Collected manifests and reports: $local_out (raw trajectories remain on dev-ubuntu)"
}

case "$ACTION" in
  preflight) preflight ;;
  sync) sync_sources ;;
  build) build_runner ;;
  run) run_suite ;;
  smoke) run_suite smoke ;;
  collect) collect_suite ;;
  all) preflight; sync_sources; build_runner; run_suite; collect_suite ;;
  *) echo 'Usage: body-trajectories-remote.sh preflight|sync|build|run|smoke|collect|all [run-name]' >&2; exit 2 ;;
esac
