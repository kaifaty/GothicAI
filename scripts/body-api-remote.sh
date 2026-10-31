#!/usr/bin/env bash
# Stage 4 combat API on dev-ubuntu. Never runs a simulation on the Mac.
# Usage: scripts/body-api-remote.sh preflight|sync|build|run|smoke|collect|all [run-name]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${GOTHIC_BODY_SSH:-dev-ubuntu}"
# An isolated stage 4 source/build cache keeps other worktrees and archived
# stage 2/3 evidence intact. gq schedules work; older hosts keep the shared lock.
REMOTE="gothic-ai/body-api"
REFERENCE="gothic-ai/body-baseline/runs/melee-reference-stage1-v4"
ACTION="${1:-preflight}"
RUN_NAME="${2:-stage4-api-$(date -u +%Y%m%dT%H%M%SZ)}"
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
[[ -f "$HOME/$2/manifest.json" && -f "$HOME/$2/run/save_slot_1.sav" ]] || {
  echo 'Archived baseline provenance/save missing' >&2; exit 1;
}
[[ -x "$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0/bin/glslangValidator" ]] || {
  echo 'User-local toolchain missing' >&2; exit 1;
}
df -h "$HOME/gothic-ai"
free -h
nvidia-smi --query-gpu=memory.used,memory.free,utilization.gpu --format=csv,noheader
if gq_available; then
  systemctl --user show gothic-voxtral gothic-voxcpm --property=Id,ActiveState,SubState,MainPID
else
  systemctl --user is-active gothic-voxtral gothic-voxcpm
fi
if ! gq_available && pgrep -x 'Gothic2Notr|gothic-body-run' >/dev/null; then
  echo 'Another game/runner is active; wait for its owner' >&2; exit 1
fi
echo "Stage 4 workspace: $HOME/$1"
REMOTE_SCRIPT
}

sync_sources() {
  remote_script "$REMOTE" <<'REMOTE_SCRIPT'
set -euo pipefail
task_root="$HOME/$1"
mkdir -p "$task_root/source/scripts/bench" "$task_root/source/evals/npc-body" "$task_root/provenance" "$task_root/runs"
[[ ! -e "$task_root/build-active" ]] || { echo 'Build owns source tree' >&2; exit 1; }
if ! gq_available && pgrep -x 'Gothic2Notr|gothic-body-run' >/dev/null; then
  echo 'Active simulation owns source/provenance; do not sync' >&2; exit 1
fi
free_kib=$(df -Pk "$task_root" | awk 'NR==2 {print $4}')
[[ "$free_kib" -ge 5242880 ]] || { echo 'Less than 5 GiB free' >&2; exit 1; }
REMOTE_SCRIPT
  rsync -a --delete --exclude=build/ --exclude=.git/ --exclude=.DS_Store --exclude='*.o' --exclude='*.a' \
    "$ROOT/OpenGothic/" "$HOST:$REMOTE/source/OpenGothic/"
  rsync -a --exclude=build/ --exclude=__pycache__/ "$ROOT/scripts/convert" "$HOST:$REMOTE/source/scripts/"
  rsync -a "$ROOT/scripts/body-api-remote.sh" "$ROOT/scripts/body_api_suite.py" "$ROOT/scripts/base-ensure.sh" \
    "$ROOT/scripts/body_api_report.py" "$ROOT/scripts/body_observe_suite.py" \
    "$ROOT/scripts/body_headless_suite.py" \
    "$ROOT/scripts/body_headless_parity.py" "$ROOT/scripts/body_baseline_report.py" "$HOST:$REMOTE/source/scripts/"
  rsync -a "$ROOT/scripts/bench/body-api.bench" "$ROOT/scripts/bench/body-melee.bench" \
    "$HOST:$REMOTE/source/scripts/bench/"
  rsync -a --delete --exclude=__pycache__/ "$ROOT/evals/npc-body/" "$HOST:$REMOTE/source/evals/npc-body/"
  local source_head source_patch
  source_head="$(git -C "$ROOT" rev-parse HEAD)"
  source_patch="$(mktemp)"
  git -C "$ROOT" diff --binary HEAD -- OpenGothic > "$source_patch"
  rsync -a "$source_patch" "$HOST:$REMOTE/provenance/source.patch"
  rm "$source_patch"
  remote_script "$REMOTE" "$source_head" <<'REMOTE_SCRIPT'
set -euo pipefail
printf '%s\n' "$2" > "$HOME/$1/provenance/source-head.txt"
REMOTE_SCRIPT
}

build_runner() {
  remote_script "$REMOTE" <<'REMOTE_SCRIPT'
set -euo pipefail
task_root="$HOME/$1"
exec 9>"$task_root/build.lock"
flock -n 9 || { echo 'Another build owns workspace' >&2; exit 1; }
[[ ! -e "$task_root/build-active" ]] || { echo 'Inspect build marker first' >&2; exit 1; }
touch "$task_root/build-active"
trap 'rm -f "$task_root/build-active"' EXIT
if gq_available; then
  cd "$task_root"
  build_command="$(cat <<'GQ_BUILD'
set -euo pipefail
task_root="$1"
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$VULKAN_SDK/bin:$PATH"
# Jolt on x86-64-v3 (AVX2, F16C, BMI1, LZCNT; never FMA) where the host has it: same results, shorter tick.
jolt_x86=portable
grep -qw avx2 /proc/cpuinfo && grep -qw f16c /proc/cpuinfo && grep -qw bmi1 /proc/cpuinfo &&
  grep -qw abm /proc/cpuinfo && jolt_x86=x86-64-v3
nice -n 10 cmake -S "$task_root/source/OpenGothic" -B "$task_root/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DOPENGOTHIC_JOLT_X86="$jolt_x86" \
  -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib" \
  > "$task_root/provenance/configure.log" 2>&1
nice -n 10 cmake --build "$task_root/build" --target gothic-body-runner Gothic2Notr gothic-body-resolver-test --parallel 2 \
  > "$task_root/provenance/build.log" 2>&1 || { tail -80 "$task_root/provenance/build.log" >&2; exit 1; }
"$task_root/build/opengothic/gothic-body-resolver-test" > "$task_root/provenance/resolver-tests.json"
tail -8 "$task_root/provenance/build.log"
GQ_BUILD
)"
  gq_job --class quick --threads 3 --mem 6G --max 30m --name "body-api-build" -- \
    bash -c "$build_command" bash "$task_root"
else
  export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
  export PATH="$VULKAN_SDK/bin:$PATH"
  jolt_x86=portable
  grep -qw avx2 /proc/cpuinfo && grep -qw f16c /proc/cpuinfo && grep -qw bmi1 /proc/cpuinfo &&
    grep -qw abm /proc/cpuinfo && jolt_x86=x86-64-v3
  nice -n 10 cmake -S "$task_root/source/OpenGothic" -B "$task_root/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DOPENGOTHIC_JOLT_X86="$jolt_x86" \
    -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib" \
    > "$task_root/provenance/configure.log" 2>&1
  nice -n 10 cmake --build "$task_root/build" --target gothic-body-runner Gothic2Notr gothic-body-resolver-test --parallel 2 \
    > "$task_root/provenance/build.log" 2>&1 || { tail -80 "$task_root/provenance/build.log" >&2; exit 1; }
  "$task_root/build/opengothic/gothic-body-resolver-test" > "$task_root/provenance/resolver-tests.json"
  tail -8 "$task_root/provenance/build.log"
fi
REMOTE_SCRIPT
}

run_suite() {
  remote_script "$REMOTE" "$REFERENCE" "$RUN_NAME" "${1:-full}" <<'REMOTE_SCRIPT'
set -euo pipefail
task_root="$HOME/$1"
reference="$HOME/$2"
run_name="$3"
suite_mode="$4"
suite_args=()
[[ "$suite_mode" != smoke ]] || suite_args+=(--smoke)
binary="$task_root/build/opengothic/gothic-body-runner"
out="$task_root/runs/$run_name"
source_root="$task_root/source"
engine_library="$task_root/build/libOpenGothic.a"
if gq_available; then
  # Freeze binaries and their library together; release this workspace lock
  # before waiting in the queue so later builds can prepare their own runs.
  exec 8>"$task_root/build.lock"
  flock -n 8 || { echo 'Another build owns workspace' >&2; exit 1; }
fi
[[ -x "$binary" && -f "$task_root/build/libOpenGothic.a" ]] || { echo 'Build runner first' >&2; exit 1; }
[[ ! -e "$task_root/build-active" ]] || { echo 'Wait for build' >&2; exit 1; }
if ! gq_available; then
  exec 9>"$HOME/gothic-ai/body-simulation.lock"
  flock -n 9 || { echo 'Another simulation owns shared lock' >&2; exit 1; }
fi
if ! gq_available && pgrep -x 'Gothic2Notr|gothic-body-run' >/dev/null; then
  echo 'Another game/runner is active; wait for its owner' >&2; exit 1
fi
[[ ! -e "$out" ]] || { echo 'Run exists; choose fresh name' >&2; exit 1; }
mkdir -p "$out/run" "$out/reference"
cp "$binary" "$out/reference/gothic-body-runner"
cp "$task_root/provenance/source-head.txt" "$task_root/provenance/source.patch" \
   "$task_root/provenance/configure.log" "$task_root/provenance/build.log" \
   "$task_root/provenance/resolver-tests.json" "$out/"
cp "$task_root/build/opengothic/gothic-body-resolver-test" "$out/reference/"
nice -n 10 tar -czf "$out/source.tar.gz" -C "$task_root/source" OpenGothic scripts evals
if gq_available; then
  # Execute the exact archived bytes, including Python imports and scenarios.
  # Sync may replace the development tree while this run waits for admission.
  source_root="$out/reference/source"
  mkdir -p "$source_root"
  tar -xzf "$out/source.tar.gz" -C "$source_root"
  cp "$engine_library" "$out/reference/libOpenGothic.a"
  engine_library="$out/reference/libOpenGothic.a"
  exec 8>&-
fi
unit="gothic-body-api-$run_name"
if gq_available; then
  cd "$out/run"
  launch=(gq_job --class quiet --threads 2 --mem 12G --cpu-quota 200% \
    --max 40m --name "body-api-$run_name" -- env \
    "DISPLAY=" "WAYLAND_DISPLAY=" \
    "GOTHIC_AI=0" "VK_DRIVER_FILES=/nonexistent/body-api-no-vulkan.json" \
    "VK_ICD_FILENAMES=/nonexistent/body-api-no-vulkan.json" "OPENGOTHIC_TEXTURES=" \
    "OPENGOTHIC_BODY_TRACE=1" "OPENGOTHIC_BODY_ANIMATION_RNG=isolated")
else
  trap 'systemctl --user stop "$unit.service" >/dev/null 2>&1 || true' INT TERM
  launch=(systemd-run --user --wait --pipe --collect --unit="$unit" \
  --property=Nice=10 --property=CPUQuota=200% --property=MemoryMax=12G \
  --property=TasksMax=128 --property=RuntimeMaxSec=2400 \
  --working-directory="$out/run" \
  --setenv="DISPLAY=" --setenv="WAYLAND_DISPLAY=" --setenv="GOTHIC_AI=0" \
  --setenv="VK_DRIVER_FILES=/nonexistent/body-api-no-vulkan.json" \
  --setenv="VK_ICD_FILENAMES=/nonexistent/body-api-no-vulkan.json" \
  --setenv="OPENGOTHIC_TEXTURES=" --setenv="OPENGOTHIC_BODY_TRACE=1" \
  --setenv="OPENGOTHIC_BODY_ANIMATION_RNG=isolated")
fi
set +e
"${launch[@]}" \
  /usr/bin/time -v -o "$out/resources.txt" \
  python3 "$source_root/scripts/body_api_suite.py" \
    --runner "$out/reference/gothic-body-runner" \
    --assets "$HOME/gothic-ai/body-baseline/assets" --save "$reference/run/save_slot_1.sav" \
    --reference "$reference" \
    --native-reference "$HOME/gothic-ai/body-headless/runs/stage3-v1/native-off" --source "$source_root/OpenGothic" \
    --engine-library "$engine_library" \
    --config "$source_root/evals/npc-body/body-api-v1.json" \
    --scenario "$source_root/scripts/bench/body-api.bench" \
    --melee-scenario "$source_root/scripts/bench/body-melee.bench" \
    --output "$out" ${suite_args[@]+"${suite_args[@]}"} > "$out/launcher.log" 2>&1
result=$?
set -e
trap - INT TERM
if [[ "$result" -ne 0 ]]; then
  tail -80 "$out/launcher.log" >&2
  echo "Combat API suite failed ($result); evidence remains in $out" >&2
  exit "$result"
fi
echo "Combat API suite saved: $out"
REMOTE_SCRIPT
}

collect_suite() {
  local local_out="$ROOT/data/npc-body/api/$RUN_NAME"
  mkdir -p "$local_out"
  # Keep binaries, assets and large numeric journals on the server.
  rsync -a --exclude=run/ --exclude=reference/ --exclude=source.tar.gz \
    --exclude=observation-batches.jsonl --exclude=observations.jsonl \
    --exclude=observer-timing.jsonl --exclude=events.jsonl --exclude=ipc.jsonl \
    "$HOST:$REMOTE/runs/$RUN_NAME/" "$local_out/"
  echo "Collected compact evidence: $local_out (full numeric journals remain on dev-ubuntu)"
}

case "$ACTION" in
  preflight) preflight ;;
  sync) sync_sources ;;
  build) build_runner ;;
  run) run_suite ;;
  smoke) run_suite smoke ;;
  collect) collect_suite ;;
  all) preflight; sync_sources; build_runner; run_suite; collect_suite ;;
  *) echo 'Usage: body-api-remote.sh preflight|sync|build|run|smoke|collect|all [run-name]' >&2; exit 2 ;;
esac
