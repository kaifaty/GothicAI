#!/usr/bin/env bash
# Submit a bounded phase and return its gq id. Never trains a student.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
stage="${1:?Usage: nav-teacher-v3-remote.sh build|bootstrap|prepare|campaign|evaluate|diagnose|retry|visible|oracle|oracle-resume}"
case "$stage" in build|bootstrap|prepare|campaign|evaluate|diagnose|retry|visible|oracle|oracle-resume) ;; *) echo "Unknown phase: $stage" >&2; exit 2;; esac
export GQ_OWNER=nav-teacher-v3
task="$(ssh -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-teacher-v3" "$HOME"')"
ssh ryzen "mkdir -p '$task/source' '$task/input'"
rsync -a --exclude=build --exclude='build-*' --exclude=__pycache__ \
  "$ROOT/OpenGothic" "$ROOT/scripts" "$ROOT/AGENTS.md" "ryzen:$task/source/"
rsync -a "$ROOT/server/src/gothic_ai/gamedata/places.json" \
  "$ROOT/reports/navigation/2026-10-16-teacher/raw/accepted-links.txt" \
  "$ROOT/reports/navigation/2026-10-18-rl-env/holdout-cv.jsonl" "ryzen:$task/input/"
# Other tasks supply read-only inputs; every writable path belongs to this task.
ssh ryzen "test -f '$task/input/save.sav' || cp \"\$HOME/gothic-ai/nav-ppo/save_slot_1.sav\" '$task/input/save.sav'"
ssh ryzen "test -f '$task/input/training-pairs.jsonl' || cp \"\$HOME/gothic-ai/nav-ppo/training-pairs/pairs.jsonl\" '$task/input/training-pairs.jsonl'"
ssh ryzen "test -f '$task/input/ppo-validation.jsonl' || cp \"\$HOME/gothic-ai/nav-ppo/validation/holdout.jsonl\" '$task/input/ppo-validation.jsonl'"
ssh ryzen "cat > '$task/phase.sh'" <<'PHASE'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
stage="${1:?phase}"
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export OMP_NUM_THREADS=2 OPENBLAS_NUM_THREADS=1
df -h "$HOME"
cd "$task/source"
if [[ ! -x "$task/.venv/bin/python" ]]; then uv venv "$task/.venv"; fi
uv pip install --python "$task/.venv/bin/python" numpy pytest
if [[ "$stage" != prepare && "$stage" != evaluate && "$stage" != visible && "$stage" != oracle-resume ]]; then
  cmake -S "$task/source/OpenGothic" -B "$task/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
    -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
  targets=(Gothic2Notr gothic-body-runner check-navmemoryteacher check-navkeyteacher)
  tests=(scripts/tests/test_nav_teacher_v3.py scripts/tests/test_nav_teacher_v3_campaign.py
    scripts/tests/test_nav_demos.py scripts/tests/test_nav_guidance.py
    scripts/tests/test_nav_drive.py scripts/tests/test_nav_rl.py)
  if [[ "$stage" == diagnose || "$stage" == retry ]]; then
    targets+=(check-navmemoryteacher-diagnostics)
    tests=(scripts/tests/test_nav_teacher_v3_failures.py scripts/tests/test_nav_demos.py)
    if [[ "$stage" == retry ]]; then
      tests+=(scripts/tests/test_nav_teacher_v3.py scripts/tests/test_nav_teacher_v3_campaign.py)
    fi
  fi
  if [[ "$stage" == oracle ]]; then
    targets+=(check-navmesh check-navoracle)
    tests=(scripts/tests/test_nav_teacher_v3_oracle.py scripts/tests/test_nav_teacher_v3_coverage.py
      scripts/tests/test_nav_teacher_v3.py scripts/tests/test_nav_demos.py)
  fi
  cmake --build "$task/build" --target "${targets[@]}" --parallel 3
  "$task/.venv/bin/python" -m pytest -q "${tests[@]}"
fi
if [[ "$stage" == oracle-resume ]]; then
  test -x "$task/build/opengothic/gothic-body-runner"
  test -f "$task/oracle-experiment/state.json"
  "$task/.venv/bin/python" -m pytest -q scripts/tests/test_nav_teacher_v3_oracle.py \
    scripts/tests/test_nav_teacher_v3_coverage.py
fi
if [[ "$stage" == visible ]]; then
  test -x "$task/build/opengothic/gothic-body-runner"
  test -x "$task/build/opengothic/gothic-navmemoryteacher-contract"
  "$task/.venv/bin/python" -m pytest -q scripts/tests/test_nav_teacher_v3_campaign.py \
    scripts/tests/test_nav_teacher_v3_point_analysis.py scripts/tests/test_nav_demos.py
  "$task/.venv/bin/python" -I scripts/nav_teacher_v3_point_analysis.py \
    --task "$task" --data "$HOME/gothic-ai/nav-demos-v3" --output "$task/point-retry-analysis"
fi
if [[ "$stage" == oracle || "$stage" == oracle-resume ]]; then
  "$task/.venv/bin/python" -I scripts/nav_teacher_v3_oracle.py \
    --task "$task" --data "$HOME/gothic-ai/nav-demos-v3" --parallel 3
  "$task/.venv/bin/python" -I scripts/nav_teacher_v3_coverage.py \
    --task "$task" --data "$HOME/gothic-ai/nav-demos-v3" --output "$task/oracle-experiment/coverage"
elif [[ "$stage" == diagnose ]]; then
  analysis=(--task "$task" --data "$HOME/gothic-ai/nav-demos-v3" --output "$task/failures-v3-first")
  "$task/.venv/bin/python" -I scripts/nav_teacher_v3_failures.py prepare "${analysis[@]}"
  "$task/.venv/bin/python" -I scripts/nav_teacher_v3_failures.py detour "${analysis[@]}"
  "$task/.venv/bin/python" -I scripts/nav_teacher_v3_failures.py analyze "${analysis[@]}" \
    --diagnostics "$task/build/opengothic/gothic-navmemoryteacher-diagnostics" \
    --detour-plans "$task/failures-v3-first/detour/detour-plans-v3.jsonl" --parallel 3
elif [[ "$stage" == campaign || "$stage" == evaluate || "$stage" == retry || "$stage" == visible ]]; then
  "$task/.venv/bin/python" -I scripts/nav_teacher_v3_campaign.py \
    --task "$task" --data "$HOME/gothic-ai/nav-demos-v3" --parallel 3 \
    --min-points 30 --per-region 5 --min-objects 30 --min-object-places 30 \
    --stage "$stage"
  if [[ "$stage" == visible ]]; then
    "$task/.venv/bin/python" scripts/nav_demos.py check "$HOME/gothic-ai/nav-demos-v3/v3-visible" \
      --output "$task/visible-navdemo-check.json"
    "$task/.venv/bin/python" scripts/nav_demos.py check "$HOME/gothic-ai/nav-demos-v3/examples" \
      --output "$task/examples-navdemo-check.json"
  fi
elif [[ "$stage" != build ]]; then
  data="$HOME/gothic-ai/nav-demos-v3"
  selection=(--output "$data" --roads "$task/input/places.json"
    --training-pairs "$task/input/training-pairs.jsonl" --holdout "$task/input/holdout-cv.jsonl"
    --ppo-validation "$task/input/ppo-validation.jsonl")
  runner=(--runner "$task/build/opengothic/gothic-body-runner" --assets "$HOME/gothic-ai/game"
    --save "$task/input/save.sav" --cache "$task/cache" --links "$task/input/accepted-links.txt"
    --roads "$task/input/places.json" --output "$data")
  "$task/.venv/bin/python" -I scripts/nav_teacher_v3.py candidates "${selection[@]}"
  "$task/.venv/bin/python" -I scripts/nav_teacher_v3.py discovery "${runner[@]}" \
    --scenarios "$data/candidates.jsonl"
  df -h "$HOME"
  "$task/.venv/bin/python" -I scripts/nav_teacher_v3.py run "${runner[@]}" \
    --scenarios "$data/candidates.jsonl" --teacher-version 2 --goal-kind point --count 24 --resume
  "$task/.venv/bin/python" -I scripts/nav_teacher_v3.py qualify "${selection[@]}" \
    --teacher-root "$data/v2" --probes "$data/discovery/object-candidates.jsonl"
fi
PHASE
threads=4
cpu_args=()
if [[ "$stage" == campaign || "$stage" == evaluate || "$stage" == diagnose || "$stage" == retry || "$stage" == visible || "$stage" == oracle || "$stage" == oracle-resume ]]; then
  threads=12
  cpu_args=(--cpu-quota 1200%)
fi
"$ROOT/scripts/gq" submit --class batch --threads "$threads" --mem 8G --max 2h "${cpu_args[@]}" \
  --name "nav teacher v3 $stage" -- bash "$task/phase.sh" "$stage"
