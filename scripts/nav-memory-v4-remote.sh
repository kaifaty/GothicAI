#!/usr/bin/env bash
# Submit memory-v4 phases and return their queue IDs; never wait or poll.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
stage="${1:?Usage: nav-memory-v4-remote.sh build|bake|measure|evaluate|safety|cleanup}"
case "$stage" in build|bake|measure|evaluate|safety|cleanup) ;; *) echo "Unknown phase: $stage" >&2; exit 2;; esac
export GQ_OWNER=nav-memory-v4
task="$(ssh -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-memory-v4" "$HOME"')"
ssh ryzen "TASK='$task' bash -s" <<'PREPARE'
set -euo pipefail
df -h "$HOME"
python3 -c 'import pathlib,shutil; assert shutil.disk_usage(pathlib.Path.home()).free >= 20*1024**3, "Less than 20 GiB free"'
mkdir -p "$TASK/source" "$TASK/input"
PREPARE
if [[ "$stage" != cleanup ]]; then
  rsync -a --exclude=build --exclude='build-*' --exclude=__pycache__ \
    "$ROOT/OpenGothic" "$ROOT/scripts" "$ROOT/AGENTS.md" "ryzen:$task/source/"
  rsync -a --ignore-existing "$ROOT/server/src/gothic_ai/gamedata/places.json" "ryzen:$task/input/"
  ssh ryzen "TASK='$task' bash -s" <<'INPUT'
set -euo pipefail
# Foreign experiment directories are sources only. Freeze copies in this task.
copy_input() {
  local source="$1" destination="$2"
  if [[ ! -f "$destination" ]]; then
    cp "$source" "$destination"
  fi
}
copy_input "$HOME/gothic-ai/nav-teacher-v3/expanded-scenarios.jsonl" "$TASK/input/expanded-scenarios.jsonl"
copy_input "$HOME/gothic-ai/nav-dagger3/detours/detour-val.jsonl" "$TASK/input/detour-val.jsonl"
copy_input "$HOME/gothic-ai/nav-perception/safety-check-1/fixtures.json" "$TASK/input/fixtures.json"
copy_input "$HOME/gothic-ai/nav-teacher-v3/input/save.sav" "$TASK/input/save.sav"
copy_input "$HOME/gothic-ai/nav-teacher-v3/input/accepted-links.txt" "$TASK/input/accepted-links.txt"
INPUT
fi
ssh ryzen "cat > '$task/phase.sh'" <<'PHASE'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
stage="${1:?phase}"
shift
df -h "$HOME"
python3 -c 'import pathlib,shutil; assert shutil.disk_usage(pathlib.Path.home()).free >= 20*1024**3, "Less than 20 GiB free"'
if [[ "$stage" == cleanup ]]; then
  [[ "$task" == "$HOME/gothic-ai/nav-memory-v4" ]]
  rm -rf -- "$task/build"
  exit 0
fi
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export UV_CACHE_DIR="$HOME/.cache/uv"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 OPENGOTHIC_BODY_TRACE=0
cd "$task/source"
if [[ "$stage" == build ]]; then
  if [[ ! -x "$task/.venv/bin/python" ]]; then uv venv "$task/.venv"; fi
  uv pip install --python "$task/.venv/bin/python" numpy pytest
  cmake -S "$task/source/OpenGothic" -B "$task/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
    -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
  cmake --build "$task/build" --target gothic-body-runner \
    check-navigation-memory check-navmemoryteacher --parallel 3
  "$task/.venv/bin/python" -m pytest -q scripts/tests/test_nav_memory_v4.py scripts/tests/test_nav_demos.py
elif [[ "$stage" == measure-mode ]]; then
  test -x "$task/build/opengothic/gothic-body-runner"
  "$task/.venv/bin/python" -c 'import json,sys; result=json.load(open(sys.argv[1])); assert result["unique_npc_count"] > 0 and result["npc_count"] == 300 and 0 < result["total_bytes_for_300_npcs"] <= 50_000_000' \
    "$task/bake-npcs/memory-cost.json"
  mode="${1:?memory comparison mode}"
  shift
  timeout 110m "$task/.venv/bin/python" -I scripts/nav_memory_v4.py evaluate \
    --task "$task" --budget-seconds 6500 --mode "$mode" "$@"
  timeout 8m "$task/.venv/bin/python" -I scripts/nav_memory_v4.py safety \
    --task "$task" --budget-seconds 420 --mode "$mode" "$@"
else
  test -x "$task/build/opengothic/gothic-body-runner"
  timeout 110m "$task/.venv/bin/python" -I scripts/nav_memory_v4.py "$stage" --task "$task" --budget-seconds 6500 "$@"
fi
PHASE
if [[ "$stage" == measure ]]; then
  # A 16-thread batch cannot overlap another such batch on the 28-thread host.
  # FIFO therefore finishes bake/discovery before any mode reads their outputs.
  for mode in bake baseline baseline-stranger journey journey-stranger familiar-local familiar-stranger; do
    phase_args=(measure-mode "$mode")
    [[ "$mode" != bake ]] || phase_args=(bake)
    identifier=$("$ROOT/scripts/gq" submit --class batch --threads 16 --mem 8G --max 2h --cpu-quota 1200% \
      --name "nav memory v4 $mode" -- bash "$task/phase.sh" "${phase_args[@]}" "${@:2}")
    printf '%s %s\n' "$mode" "$identifier"
  done
  exit 0
fi
"$ROOT/scripts/gq" submit --class batch --threads 12 --mem 8G --max 2h --cpu-quota 1200% \
  --name "nav memory v4 $stage" -- bash "$task/phase.sh" "$stage" "${@:2}"
