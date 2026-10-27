#!/usr/bin/env bash
# Submit one bounded perception phase; no teachers or demonstrations are collected.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
stage="${1:?Usage: nav-perception-remote.sh build|compare|evaluate|safety|water}"
case "$stage" in build|compare|evaluate|safety|water) ;; *) echo "Unknown phase: $stage" >&2; exit 2;; esac
export GQ_OWNER=nav-perception
task="$(ssh -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-perception" "$HOME"')"
ssh ryzen "mkdir -p '$task/source' '$task/input'"
rsync -a --exclude=build --exclude='build-*' --exclude=__pycache__ \
  "$ROOT/OpenGothic" "$ROOT/scripts" "$ROOT/AGENTS.md" "ryzen:$task/source/"
rsync -a "$ROOT/server/src/gothic_ai/gamedata/places.json" \
  "$ROOT/reports/navigation/2026-10-16-teacher/raw/accepted-links.txt" "ryzen:$task/input/"
ssh ryzen "test -f '$task/input/save.sav' || cp \"\$HOME/gothic-ai/nav-teacher-v3/input/save.sav\" '$task/input/save.sav'"
ssh ryzen "cat > '$task/phase.sh'" <<'PHASE'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
stage="${1:?phase}"
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 OPENGOTHIC_BODY_TRACE=0
df -h "$HOME"
cd "$task/source"
if [[ ! -x "$task/.venv/bin/python" ]]; then uv venv "$task/.venv"; fi
uv pip install --python "$task/.venv/bin/python" numpy pytest
cmake -S "$task/source/OpenGothic" -B "$task/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
  -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
# Three compiler processes keep the complete build below the 8 GiB job ceiling.
cmake --build "$task/build" --target Gothic2Notr gothic-body-runner \
  check-navigation-eye check-navigation-memory check-navmemoryteacher --parallel 3
if [[ "$stage" == safety ]]; then
  "$task/.venv/bin/python" -m pytest -q scripts/tests/test_nav_demos.py scripts/tests/test_nav_perception_safety.py
else
  "$task/.venv/bin/python" -m pytest -q scripts/tests/test_nav_demos.py \
    scripts/tests/test_nav_teacher_v3.py scripts/tests/test_nav_teacher_v3_coverage.py \
    scripts/tests/test_nav_perception.py scripts/tests/test_nav_perception_eval.py \
    scripts/tests/test_nav_perception_safety.py
fi
if [[ "$stage" == compare ]]; then
  "$task/.venv/bin/python" -I scripts/nav_perception.py prepare --task "$task"
  "$task/.venv/bin/python" -I scripts/nav_perception.py replay --task "$task"
elif [[ "$stage" == water ]]; then
  cat > "$task/input/water-new-seeds.txt" <<'SHORES'
water-new-port 41 -125 780
water-new-lighthouse -3187 -421 12378
water-new-ship -4180 -594 -19312
water-new-shipwreck -319 -455 22600
water-new-mountainlake 42515 2918 -25361
SHORES
  safety_status=0
  "$task/.venv/bin/python" -I scripts/nav_perception_safety.py --task "$task" \
    --runner "$task/build/opengothic/gothic-body-runner" --assets "$HOME/gothic-ai/game" \
    --save "$task/input/save.sav" --cache "$task/safety-cache" --roads "$task/input/places.json" \
    --links "$HOME/gothic-ai/nav-jumps/accepted-links-cv.txt" \
    --routes "$HOME/gothic-ai/nav-jumps/routes/routes.jsonl" \
    --records "$HOME/gothic-ai/nav-demos-jumps/final" --mode gaze --memory-version 3 \
    --water-baseline "$task/safety-check-1/fixtures.json" --new-shores 3 \
    --shore-seeds "$task/input/water-new-seeds.txt" --output "$task/water-check-1" || safety_status=$?
  evaluation_status=0
  "$task/.venv/bin/python" -I scripts/nav_perception_eval.py --task "$task" \
    --mode gaze --teachers memory --memory-version 3 --parallel 3 \
    --output "$task/evaluation-water-1" || evaluation_status=$?
  coverage_status=0
  "$task/.venv/bin/python" -I scripts/nav_perception.py replay --task "$task" \
    --mode gaze --output "$task/replay-water" || coverage_status=$?
  if [[ "$safety_status" != 0 || "$evaluation_status" != 0 || "$coverage_status" != 0 ]]; then
    echo "Water measurements: safety=$safety_status evaluation=$evaluation_status coverage=$coverage_status" >&2
    exit 1
  fi
elif [[ "$stage" == evaluate || "$stage" == safety ]]; then
  # Both independent measurements run even if one reports a failed criterion.
  safety_status=0
  "$task/.venv/bin/python" -I scripts/nav_perception_safety.py --task "$task" \
    --runner "$task/build/opengothic/gothic-body-runner" --assets "$HOME/gothic-ai/game" \
    --save "$task/input/save.sav" --cache "$task/safety-cache" --roads "$task/input/places.json" \
    --links "$HOME/gothic-ai/nav-jumps/accepted-links-cv.txt" \
    --routes "$HOME/gothic-ai/nav-jumps/routes/routes.jsonl" \
    --records "$HOME/gothic-ai/nav-demos-jumps/final" --mode gaze --memory-version 3 \
    --output "$task/safety-check-1" --per-kind 3 || safety_status=$?
  evaluation_status=0
  if [[ "$stage" == evaluate ]]; then
    "$task/.venv/bin/python" -I scripts/nav_perception_eval.py --task "$task" \
      --mode gaze --memory-version 3 --parallel 3 || evaluation_status=$?
  fi
  if [[ "$safety_status" != 0 || "$evaluation_status" != 0 ]]; then
    echo "Perception measurements: safety=$safety_status evaluation=$evaluation_status" >&2
    exit 1
  fi
fi
PHASE
"$ROOT/scripts/gq" submit --class batch --threads 12 --mem 8G --max 2h --cpu-quota 1200% \
  --name "nav perception $stage" -- bash "$task/phase.sh" "$stage"
