#!/usr/bin/env bash
# Stage 9.2 held-out duels through the gate diagnostics driver (realtime, or lockstep with
# "screening"), BC and round-2 GRU, with the caps and environment of body-bc-remote.sh phases.
# All diagnostic phases record wall-clock policy latency and use quiet with CPUQuota=200%.
# Runs on the server next to body_stage9_realtime_duels.py: ./body-stage9-duels.sh [realtime|screening]
# DUELS_CODE (code root, default the frozen run's), DUELS_CONFIG and DUELS_SUFFIX select a variant;
# DUELS_RUNNER replaces the frozen runner, DUELS_MODELS ("bc round2") the checkpoints and DUELS_SETENV
# adds space-separated KEY=VALUE pairs to the unit environment; DUELS_CHECKPOINTS ("name=path ...")
# names GRU checkpoints beyond bc and round2. DUELS_LOCK_WAIT: legacy flock wait (3600 s).
# With gq, copy gq-job.sh beside this script or into DUELS_CODE/scripts.
# DUELS_ENGINE: the engine workspace (source + build) recorded with DUELS_RUNNER (default body-api).
set -euo pipefail
phase="${1:-realtime}"
suffix="${DUELS_SUFFIX-}"
[[ -n "$suffix" || "$phase" == realtime ]] || suffix="-lockstep"
task_root="$HOME/gothic-ai/body"
engine_root="${DUELS_ENGINE:-$HOME/gothic-ai/body-api}"
run="$task_root/data/stage9-rt-eval-v1"
code="${DUELS_CODE:-$run/code}"
work="${DUELS_WORK:-$task_root/rl/rt-duels-v1}"
python="$task_root/.venv/bin/python"
bc="$task_root/data/stage8-defence-bc-v2/models"
runner="${DUELS_RUNNER:-$run/reference/gothic-body-runner}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
for helper in "$script_dir/gq-job.sh" "$code/scripts/gq-job.sh"; do
  if [[ -r "$helper" ]]; then source "$helper"; break; fi
done
if ! declare -F gq_available >/dev/null; then
  if command -v "${GQ_BIN:-$HOME/gothic-ai/bin/gq}" >/dev/null 2>&1; then
    echo 'Copy gq-job.sh beside body-stage9-duels.sh or into DUELS_CODE/scripts' >&2
    exit 2
  fi
  gq_available() { return 1; }
fi
mkdir -p "$work"
config="${DUELS_CONFIG:-$work/config-v2.json}"
[[ -f "$config" ]] || "$python" "$work/body_stage9_realtime_duels.py" config \
  --base "$code/evals/npc-body/body-stage8-v1.json" --output "$config" --duels 64 --seed-start 90000
if ! gq_available; then
  exec 9>"$HOME/gothic-ai/body-simulation.lock"
  flock -w "${DUELS_LOCK_WAIT:-3600}" 9
fi
for name in ${DUELS_MODELS:-bc round2}; do
  [[ ! -e "$work/$name$suffix" ]] || continue
  gru="$bc/gru/best.pt"
  [[ "$name" == bc ]] || gru="$task_root/rl/round2-train/best.pt"
  for pair in ${DUELS_CHECKPOINTS-}; do [[ "${pair%%=*}" != "$name" ]] || gru="${pair#*=}"; done
  unit_env=("PYTHONPATH=$code/body-service/src:$code/scripts"
    "CUDA_VISIBLE_DEVICES=" "OMP_NUM_THREADS=2" "MKL_NUM_THREADS=2" "OPENBLAS_NUM_THREADS=2"
    "DISPLAY=" "WAYLAND_DISPLAY=" "GOTHIC_AI=0"
    "VK_DRIVER_FILES=/nonexistent/body-bc-no-vulkan.json" "VK_ICD_FILENAMES=/nonexistent/body-bc-no-vulkan.json"
    "OPENGOTHIC_TEXTURES=" "OPENGOTHIC_BODY_TRACE=1" "OPENGOTHIC_BODY_ANIMATION_RNG=isolated")
  for pair in ${DUELS_SETENV-}; do unit_env+=("$pair"); done
  cmd=(
    /usr/bin/time -v -o "$work/resources-$name$suffix.txt" \
    "$python" "$code/scripts/body_policy_diagnostics.py" \
      --runner "$runner" --assets "$HOME/gothic-ai/body-baseline/assets" \
      --save "$HOME/gothic-ai/body-baseline/runs/melee-reference-stage1-v4/run/save_slot_1.sav" \
      --reference "$HOME/gothic-ai/body-baseline/runs/melee-reference-stage1-v4" \
      --source "$engine_root/source/OpenGothic" --engine-library "$engine_root/build" \
      --scenario "$code/scripts/bench/body-arena.bench" --melee-scenario "$code/scripts/bench/body-melee.bench" \
      --native-reference "$HOME/gothic-ai/body-headless/runs/stage3-v1/native-off" \
      --config "$config" --phase "$phase" --policy-code-root "$code" \
      --gru-checkpoint "$gru" --mlp-checkpoint "$bc/mlp/best.pt" --device cpu --output "$work/$name$suffix" --serial-workers
  )
  launch() {
    if gq_available; then
      local previous_dir="$PWD" result
      cd "$work"
      if gq_job --class quiet --threads 3 --mem 6G --max 80m \
        --cpu-quota 200% --name "body duels $phase $name$suffix" -- env "${unit_env[@]}" "${cmd[@]}"; then
        result=0
      else
        result=$?
      fi
      cd "$previous_dir"
      return "$result"
    else
      local legacy_env=() pair
      for pair in "${unit_env[@]}"; do legacy_env+=(--setenv="$pair"); done
      systemd-run --user --wait --pipe --collect --unit="gothic-body-rt-duels-$name$suffix" \
        --property=Nice=10 --property=CPUQuota=200% --property=MemoryMax=6G \
        --property=TasksMax=128 --property=RuntimeMaxSec=4800 --working-directory="$work" \
        "${legacy_env[@]}" "${cmd[@]}"
    fi
  }
  launch > "$work/launcher-$name$suffix.log" 2>&1 || {
    rc=$?
    echo "$name$suffix failed"; tail -20 "$work/launcher-$name$suffix.log"
    if gq_available; then exit "$rc"; else exit 1; fi
  }
  echo "$name$suffix done $(date +%T)"
done
[[ -d "$work/round2" && -d "$work/bc" ]] || exit 0
"$python" "$work/body_stage9_realtime_duels.py" compare --bc-realtime "$work/bc" --candidate-realtime "$work/round2" \
  --bc-lockstep "$task_root/rl/eval-bc" --candidate-lockstep "$task_root/rl/eval-si2" \
  $( [[ -d "$work/round2-lockstep" ]] && echo --bc-gate-lockstep "$work/bc-lockstep" --candidate-gate-lockstep "$work/round2-lockstep" ) \
  --output "$work/compare.json" | head -160
for name in bc round2; do
  [[ -z "$suffix" || "$suffix" == -lockstep || ! -d "$work/$name$suffix" ]] ||
    "$python" "$code/scripts/body_schedule_equivalence.py" --realtime "$work/$name" --lockstep "$work/$name$suffix" \
      --output "$work/equivalence-$name$suffix.json" | head -12 || true
done
