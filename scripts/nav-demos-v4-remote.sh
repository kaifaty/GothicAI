#!/usr/bin/env bash
# Submit a bounded phase and return its gq ID. Never trains a student.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
stage="${1:?Usage: nav-demos-v4-remote.sh build-validate|validate-moving|retry|record|record-follow-below-gate|record-chain [EXPECT_EXISTING]}"
case "$stage" in build-validate|validate-moving|retry|record|record-follow-below-gate|record-chain) ;; *) echo "Unknown phase: $stage" >&2; exit 2;; esac
expect_existing="${2:-}"
if [[ -n "$expect_existing" && ! "$expect_existing" =~ ^[1-9][0-9]*$ ]]; then
  echo 'EXPECT_EXISTING must be a positive episode count' >&2; exit 2
fi
if [[ -n "$expect_existing" && "$stage" != record-follow-below-gate ]]; then
  echo 'Existing corpus regression requires record-follow-below-gate' >&2; exit 2
fi
export GQ_OWNER=nav-demos-v4
task="$(ssh -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-demos-v4" "$HOME"')"
ssh ryzen 'df -h "$HOME"'
ssh ryzen "mkdir -p '$task/source' '$task/input'"
rsync -a --exclude=build --exclude='build-*' --exclude=__pycache__ \
  "$ROOT/OpenGothic" "$ROOT/scripts" "$ROOT/AGENTS.md" "ryzen:$task/source/"
rsync -a "$ROOT/server/src/gothic_ai/gamedata/places.json" \
  "$ROOT/reports/navigation/2026-10-16-teacher/raw/accepted-links.txt" \
  "$ROOT/reports/navigation/2026-10-18-rl-env/holdout-cv.jsonl" "ryzen:$task/input/"
# Historical datasets are read-only inputs. All new files belong to this task.
ssh ryzen "bash -s -- '$task'" <<'INPUTS'
set -euo pipefail
task="$1"
copy_input() { test -f "$task/input/$2" || cp "$HOME/gothic-ai/$1" "$task/input/$2"; }
copy_input nav-ppo/save_slot_1.sav save.sav
copy_input nav-ppo/training-pairs/pairs.jsonl training-pairs.jsonl
copy_input nav-ppo/validation/holdout.jsonl ppo-validation.jsonl
copy_input nav-teacher-v3/expanded-scenarios.jsonl moving-scenarios.jsonl
copy_input nav-teacher-v3/expanded-candidates.jsonl object-candidates.jsonl
copy_input nav-teacher-v3/expanded-object-probes.jsonl object-probes.jsonl
copy_input nav-jumps/routes/routes.jsonl jump-routes.jsonl
copy_input nav-jumps/accepted-links-cv.txt jump-links.txt
INPUTS
ssh ryzen "cat > '$task/phase.sh'" <<'PHASE'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
stage="${1:?phase}"
expect_existing="${2:-}"
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export OMP_NUM_THREADS=2 OPENBLAS_NUM_THREADS=1
df -h "$HOME"
cd "$task/source"
if [[ ! -x "$task/.venv/bin/python" ]]; then uv venv "$task/.venv"; fi
uv pip install --python "$task/.venv/bin/python" numpy pytest
if [[ "$stage" == build-validate || "$stage" == retry ]]; then
  cmake -S "$task/source/OpenGothic" -B "$task/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
    -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
  cmake --build "$task/build" --target Gothic2Notr gothic-body-runner \
    check-navmemoryteacher check-navkeyteacher --parallel 3
fi
if [[ "$stage" == record || "$stage" == record-follow-below-gate || "$stage" == record-chain ]]; then
  "$task/.venv/bin/python" -m pytest -q scripts/tests/test_nav_demos_v4_collect.py \
    scripts/tests/test_nav_demos_v4_budget.py scripts/tests/test_nav_demos_v4_chain.py
  if [[ "$stage" == record-chain ]]; then
    exec "$task/.venv/bin/python" -I "$task/source/scripts/nav_demos_v4_chain.py" --task "$task"
  fi
  follow=(--include-follow auto)
  if [[ "$stage" == record-follow-below-gate ]]; then
    follow=(--include-follow no --follow-below-gate)
  fi
  if [[ -n "$expect_existing" ]]; then
    NAV_DEMOS_V4_CORPUS_TASK="$task" NAV_DEMOS_V4_CORPUS_EXPECT="$expect_existing" \
      "$task/.venv/bin/python" -m pytest -q -s scripts/tests/test_nav_demos_v4_corpus.py
  fi
  "$task/.venv/bin/python" -I scripts/nav_demos_v4_collect.py --task "$task" "${follow[@]}" \
    --wall-minutes 100 --max-new 2000 --max-bytes 15000000000
else
  "$task/.venv/bin/python" -m pytest -q scripts/tests/test_nav_demos_v4.py \
    scripts/tests/test_nav_teacher_v3.py scripts/tests/test_nav_teacher_v3_oracle.py \
    scripts/tests/test_nav_demos.py scripts/tests/test_nav_follow_analysis.py
  if [[ -f "$task/validation/attempt-1/metrics.json" ]]; then
    "$task/.venv/bin/python" -I scripts/nav_follow_analysis.py "$task/validation/attempt-1" \
      --output "$task/analysis/attempt-1-follow.json"
  fi
  validation=(--task "$task" --parallel 3)
  if [[ "$stage" == retry ]]; then validation+=(--attempt 2 --episodes-per-kind 10); fi
  "$task/.venv/bin/python" -I scripts/nav_demos_v4.py validate-moving "${validation[@]}"
fi
PHASE
"$ROOT/scripts/gq" submit --class batch --threads 12 --cpu-quota 1200% --mem 8G --max 2h \
  --name "nav demos v4 $stage" -- bash "$task/phase.sh" "$stage" "$expect_existing"
