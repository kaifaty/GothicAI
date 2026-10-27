#!/usr/bin/env bash
# Freeze read-only inputs, submit one bounded phase, and return its queue ID.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
stage="${1:-build-chain}"
[[ $# -le 1 && ( "$stage" == build-chain || "$stage" == record-chain ) ]] || {
  echo 'Usage: nav-demos-v5-remote.sh [build-chain|record-chain]' >&2; exit 2;
}
export GQ_OWNER=nav-demos-v5
task="$(ssh -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-demos-v5" "$HOME"')"
archive=/mnt/shared/gothic-ai/nav-demos-v5
ssh ryzen "bash -s -- '$task' '$archive'" <<'INPUTS'
set -euo pipefail
task="$1"; archive="$2"
df -h "$HOME" /mnt/shared
test -d /mnt/shared/gothic-ai
python3 -c 'import pathlib,shutil; assert shutil.disk_usage(pathlib.Path.home()).free >= 20*1024**3, "Less than 20 GiB free"'
mkdir -p "$task/source" "$task/input" "$task/recording-input" "$archive"
copy_input() { test -f "$2" || cp "$1" "$2"; }
for name in training-pairs.jsonl ppo-validation.jsonl holdout-cv.jsonl places.json accepted-links.txt \
  object-candidates.jsonl object-probes.jsonl moving-scenarios.jsonl jump-routes.jsonl jump-links.txt save.sav; do
  copy_input "$HOME/gothic-ai/nav-demos-v4/input/$name" "$task/input/$name"
done
for name in point object approach follow jump follow_below_gate moving-all-splits; do
  copy_input "$HOME/gothic-ai/nav-demos-v4/recording-input/$name.jsonl" "$task/recording-input/$name.jsonl"
done
for split in val test; do
  copy_input "$HOME/gothic-ai/nav-dagger3/detours/detour-$split.jsonl" "$task/input/detour-$split.jsonl"
done
copy_input "$HOME/gothic-ai/nav-memory-v4/input/fixtures.json" "$task/input/fixtures.json"
copy_input "$HOME/gothic-ai/nav-memory-v4/input/familiar.bin" "$task/input/familiar.bin"
mkdir -p "$task/discovery"
rsync -rlt --no-links --ignore-existing "$HOME/gothic-ai/nav-memory-v4/discovery/" "$task/discovery/"
INPUTS
rsync -a --no-links --exclude=build --exclude='build-*' --exclude=__pycache__ \
  "$ROOT/OpenGothic" "$ROOT/scripts" "$ROOT/AGENTS.md" "ryzen:$task/source/"
rsync -rt --no-links --ignore-existing \
  "$ROOT/reports/navigation/2026-10-21-demos-v4/manifest.json" "ryzen:$task/input/v4-manifest.json"
ssh ryzen "cat > '$task/phase.sh'" <<'PHASE'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
archive=/mnt/shared/gothic-ai/nav-demos-v5
stage="${1:?phase}"
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export UV_CACHE_DIR="$HOME/.cache/uv"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 OPENGOTHIC_BODY_TRACE=0
export OPENGOTHIC_NAV_JOURNEY_MEMORY=1
unset OPENGOTHIC_NAV_FAMILIAR_MAP
cd "$task/source"
df -h "$HOME" /mnt/shared
python3 -c 'import pathlib,shutil; assert shutil.disk_usage(pathlib.Path.home()).free >= 20*1024**3, "Less than 20 GiB free"'
if [[ "$stage" == build-chain ]]; then
  if [[ ! -x "$task/.venv/bin/python" ]]; then uv venv "$task/.venv"; fi
  uv pip install --python "$task/.venv/bin/python" numpy pytest
  "$task/.venv/bin/python" -m pytest -q scripts/tests/test_nav_demos_v4_collect.py \
    scripts/tests/test_nav_demos_v5*.py
  cmake -S "$task/source/OpenGothic" -B "$task/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
    -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
  cmake --build "$task/build" --target Gothic2Notr gothic-body-runner \
    check-navmemoryteacher --parallel 3
  next_stage=prepare
else
  test -x "$task/build/opengothic/gothic-body-runner"
  next_stage=record
fi
exec "$task/.venv/bin/python" -I scripts/nav_demos_v5_chain.py \
  --task "$task" --archive-root "$archive" --stage "$next_stage" --submit
PHASE
"$ROOT/scripts/gq" submit --class batch --threads 12 --cpu-quota 1200% --mem 8G --max 2h \
  --name "nav demos v5 $stage" -- bash "$task/phase.sh" "$stage"
