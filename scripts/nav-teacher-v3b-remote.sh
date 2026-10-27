#!/usr/bin/env bash
# Submit bounded teacher-v3b phases and return queue IDs without waiting.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
stage="${1:?Usage: nav-teacher-v3b-remote.sh build|measure|retry}"
[[ ( "$stage" == build || "$stage" == measure || "$stage" == retry ) && $# == 1 ]] || { echo "Expected build, measure or retry" >&2; exit 2; }
export GQ_OWNER=nav-teacher-v3b
task="$(ssh -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-teacher-v3b" "$HOME"')"
results=/mnt/shared/gothic-ai/nav-teacher-v3b
input_source="$(dirname "$task")/nav-memory-v4"
if [[ "$stage" == retry ]]; then
  input_source="$results"
  results="$results/variant2"
fi
ssh ryzen "TASK='$task' RESULTS='$results' INPUT_SOURCE='$input_source' bash -s" <<'PREPARE'
set -euo pipefail
df -h "$HOME" /mnt/shared
python3 -c 'import pathlib,shutil; assert shutil.disk_usage(pathlib.Path.home()).free >= 20*1024**3, "Less than 20 GiB free"'
mkdir -p "$TASK/source" "$TASK/pre-cache/game" "$RESULTS/input"
rm -f "$RESULTS/build-passed"
# Existing experiment inputs are read-only; freeze ordinary copies on storage.
for name in expanded-scenarios.jsonl detour-val.jsonl fixtures.json save.sav accepted-links.txt places.json familiar.bin; do
  if [[ ! -f "$RESULTS/input/$name" ]]; then
    cp "$INPUT_SOURCE/input/$name" "$RESULTS/input/$name"
  fi
done
if [[ ! -f "$RESULTS/input/detour-test.jsonl" && -f "$HOME/gothic-ai/nav-dagger3/detours/detour-test.jsonl" ]]; then
  cp "$HOME/gothic-ai/nav-dagger3/detours/detour-test.jsonl" "$RESULTS/input/detour-test.jsonl"
fi
python3 - "$RESULTS/input" <<'PY'
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
for name in ("detour-val.jsonl", "detour-test.jsonl"):
    path = root / name
    if not path.exists() and name == "detour-test.jsonl":
        continue
    rows = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    assert rows and (name != "detour-val.jsonl" or len(rows) == 121), (name, len(rows))
    assert len({row["id"] for row in rows}) == len(rows), name
    assert all(row["goal_kind"] in ("point", "object") and isinstance(row["seed"], int) for row in rows), name
    print(f"Frozen {name}: {len(rows)} cases")
PY
PREPARE
if [[ "$stage" == measure || "$stage" == retry ]]; then
  ssh ryzen "RESULTS='$results' INPUT_SOURCE='$input_source' STAGE='$stage' bash -s" <<'INPUT'
set -euo pipefail
# Keep existing discovery and physics caches identical, in ordinary owned copies.
for name in cache discovery bake-npcs; do
  mkdir -p "$RESULTS/$name"
  rsync -rlt --ignore-existing "$INPUT_SOURCE/$name/" "$RESULTS/$name/"
done
# Preserve the pre-change water evidence for the final before/after analysis.
if [[ "$STAGE" == measure ]]; then
  mkdir -p "$RESULTS/before-water"
  rsync -rlt --ignore-existing --include='*/' --include='episode.json' \
    --include='actions.txt' --include='teacher-paths.jsonl' \
    --include='perception-safety-frames.jsonl' --include='perception-water-observations.jsonl' \
    --include='body-control-events.jsonl' --exclude='*' \
    "$HOME/gothic-ai/nav-memory-v4/safety/" "$RESULTS/before-water/"
fi
INPUT
fi
rsync -a --exclude=build --exclude='build-*' --exclude=__pycache__ \
  "$ROOT/OpenGothic" "$ROOT/scripts" "ryzen:$task/source/"
# Compile the same offline benchmark against the memory immediately before the
# cache change. These hot files stay outside the repository and are never edited.
for name in navigationmemory.h navigationmemory.cpp; do
  git -C "$ROOT" show "6bd8656c^:OpenGothic/common/game/$name" | \
    ssh ryzen "cat > '$task/pre-cache/game/$name'"
done
ssh ryzen "cat > '$task/phase.sh'" <<'PHASE'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
results="${1:?result storage}"
shift
phase="${1:?phase}"
shift
exec > >(tee "$results/$phase-${1:-prepare}-$(date -u +%Y%m%dT%H%M%SZ).log") 2>&1
df -h "$HOME" /mnt/shared
python3 -c 'import pathlib,shutil; assert shutil.disk_usage(pathlib.Path.home()).free >= 20*1024**3, "Less than 20 GiB free"'
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export UV_CACHE_DIR="$HOME/.cache/uv"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 OPENGOTHIC_BODY_TRACE=0
cd "$task/source"
if [[ "$phase" == mode ]]; then
  test -f "$results/build-passed"
  mode="${1:?memory mode}"
  timeout 108m "$task/.venv/bin/python" -I scripts/nav_teacher_v3b.py evaluate \
    --task "$results" --runtime "$task" --assets "$HOME/gothic-ai/game" \
    --mode "$mode" --parallel 3 --budget-seconds 6300
  timeout 10m "$task/.venv/bin/python" -I scripts/nav_teacher_v3b.py safety \
    --task "$results" --runtime "$task" --assets "$HOME/gothic-ai/game" \
    --mode "$mode" --budget-seconds 540
  exit 0
fi
if [[ "$phase" == cost ]]; then
  test -f "$results/build-passed"
  # The offline replay below uses only actual eye/pose streams recorded by the
  # predeclared cases; neither teacher nor game is run again for this phase.
  "$task/.venv/bin/python" -I scripts/nav_teacher_v3b.py cost \
    --task "$results" --runtime "$task" --assets "$HOME/gothic-ai/game"
  exit 0
fi
if [[ "$phase" == retry-build ]]; then
  # Only the familiar hazard price changed. Journey and memory/cache checks
  # already passed; retain their first-round measurements as controls.
  cmake --build "$task/build" --target gothic-body-runner check-navmemoryteacher --parallel 3
  touch "$results/build-passed"
  exit 0
fi
[[ "$phase" == build ]] || { echo "Unknown phase: $phase" >&2; exit 2; }
if [[ ! -x "$task/.venv/bin/python" ]]; then uv venv "$task/.venv"; fi
uv pip install --python "$task/.venv/bin/python" numpy pytest
cmake -S "$task/source/OpenGothic" -B "$task/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
  -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
cmake --build "$task/build" --target gothic-body-runner \
  check-navigation-memory check-navmemoryteacher --parallel 3
cxx_args=(-std=c++20 -O3 -DNDEBUG -Wall -Wconversion -Werror
  -I"$task/source/OpenGothic/common" -I"$task/source/OpenGothic/common/game"
  -I"$task/source/OpenGothic/lib/Tempest/Engine/include"
  -I"$task/source/OpenGothic/lib/Tempest/Engine")
c++ "${cxx_args[@]}" OpenGothic/tests/navmemory_cost.cpp \
  OpenGothic/common/game/navigationmemory.cpp -o "$task/build/navmemory-cost"
c++ -I"$task/pre-cache" "${cxx_args[@]}" OpenGothic/tests/navmemory_cost.cpp \
  "$task/pre-cache/game/navigationmemory.cpp" -o "$task/build/navmemory-cost-before"
"$task/.venv/bin/python" -m pytest -q -o cache_dir="$results/pytest-cache" \
  --basetemp="$results/pytest-tmp" scripts/tests/test_nav_teacher_v3b.py
touch "$results/build-passed"
PHASE
build_phase=build
[[ "$stage" != retry ]] || build_phase=retry-build
identifier=$("$ROOT/scripts/gq" submit --class batch --threads 16 --mem 8G --max 2h --cpu-quota 1200% \
  --name "nav teacher v3b $build_phase" -- bash "$task/phase.sh" "$results" "$build_phase")
printf 'build %s\n' "$identifier"
if [[ "$stage" == measure || "$stage" == retry ]]; then
  # Sixteen declared threads serialize these phases on the 28-thread host. Each
  # mode includes both route cohorts and the nine frozen physical safety cases.
  modes=(journey journey-stranger familiar-local familiar-stranger)
  [[ "$stage" != retry ]] || modes=(familiar-local familiar-stranger)
  for mode in "${modes[@]}"; do
    identifier=$("$ROOT/scripts/gq" submit --class batch --threads 16 --mem 8G --max 2h --cpu-quota 1200% \
      --name "nav teacher v3b $stage $mode" -- bash "$task/phase.sh" "$results" mode "$mode")
    printf '%s %s\n' "$mode" "$identifier"
  done
fi
if [[ "$stage" == measure ]]; then
  identifier=$("$ROOT/scripts/gq" submit --class batch --threads 28 --mem 8G --max 2h --cpu-quota 1200% \
    --name "nav teacher v3b paired cost" -- bash "$task/phase.sh" "$results" cost)
  printf 'paired-cost %s\n' "$identifier"
fi
