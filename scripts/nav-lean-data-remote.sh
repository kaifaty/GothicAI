#!/usr/bin/env bash
# Submit measurements without waiting for or polling gq.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
stage="${1:-baseline}"
[[ $# -le 1 && ( "$stage" == baseline || "$stage" == probe || "$stage" == convert || "$stage" == loader ) ]] || {
  echo 'Usage: bash scripts/nav-lean-data-remote.sh [baseline|probe|convert|loader]' >&2
  exit 2
}
export GQ_OWNER=nav-lean-data
task="$(ssh -T -o BatchMode=yes -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-lean-data" "$HOME"')"
ssh ryzen "mkdir -p '$task/source/scripts' '$task/input' '$task/scratch' '$task/$stage-results'"
rsync -rt --no-links --exclude=__pycache__ "$ROOT/scripts/" "ryzen:$task/source/scripts/"
rsync -rt --no-links "$ROOT/reports/navigation/2026-10-22-demos-v5/manifest.json" \
  "ryzen:$task/input/v5-compact.json"
if [[ "$stage" == convert || "$stage" == loader ]]; then
  git -C "$ROOT" show 0dd559f12:scripts/nav_bc_data.py | \
    ssh ryzen "cat > '$task/input/nav_bc_data_before_lean.py'"
fi
ssh ryzen "cat > '$task/measure-body.sh'" <<'BODY'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
stage="${1:?stage}"
results="$task/$stage-results"
export PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 MKL_NUM_THREADS=1
export UV_CACHE_DIR="$task/uv-cache"
df -h "$HOME" /mnt/shared
"$HOME/.local/bin/uv" venv --python 3.12 --allow-existing "$task/.venv"
"$HOME/.local/bin/uv" pip install --python "$task/.venv/bin/python" 'numpy==2.4.2' pytest
if [[ "$stage" == loader ]]; then
  "$HOME/.local/bin/uv" pip install --python "$task/.venv/bin/python" 'torch==2.9.1+cu128' \
    --index-url https://download.pytorch.org/whl/cu128
  cd "$task/source"
  nvidia-smi
  "$task/.venv/bin/python" -B -X faulthandler -m pytest -q -p no:cacheprovider \
    --basetemp "$task/scratch/pytest-loader" scripts/tests/test_nav_bc_train.py
  for corpus in v5 v4; do
    if [[ "$corpus" == v4 ]]; then
      source=/mnt/shared/gothic-ai/archive/nav-demos-v4-20261029
      manifest="$source/manifest.json"
      converted=/mnt/shared/gothic-ai/nav-lean-data/parity-v4
    else
      source=/mnt/shared/gothic-ai/nav-demos-v5/recordings
      manifest="$task/input/v5-compact.json"
      converted=/mnt/shared/gothic-ai/nav-lean-data/corpus-v5
    fi
    "$task/.venv/bin/python" -B -X faulthandler -I scripts/nav_data_bench.py loader \
      --corpus "$corpus" --source "$source" --manifest "$manifest" --converted-source "$converted" \
      --reference-script "$task/input/nav_bc_data_before_lean.py" \
      --output "$results/loader-$corpus" --scratch "$task/scratch/loader-$corpus"
  done
  exit 0
fi
if [[ "$stage" == convert ]]; then
  "$HOME/.local/bin/uv" pip install --python "$task/.venv/bin/python" 'torch==2.9.1+cpu' \
    --index-url https://download.pytorch.org/whl/cpu
  cd "$task/source"
  "$task/.venv/bin/python" -B -X faulthandler -m pytest -q -p no:cacheprovider \
    --basetemp "$task/scratch/pytest-convert" scripts/tests/test_nav_demos.py \
    scripts/tests/test_nav_bc_data.py scripts/tests/test_nav_bc_train.py
  corpus=/mnt/shared/gothic-ai/nav-demos-v5/recordings
  compact=/mnt/shared/gothic-ai/nav-lean-data/corpus-v5
  "$task/.venv/bin/python" -B -X faulthandler -I scripts/nav_lean_data.py convert \
    --source "$corpus" --manifest "$task/input/v5-compact.json" --output "$compact" \
    --results "$results/conversion-v5.json" --workers 6
  "$task/.venv/bin/python" -I - "$results/conversion-v5.json" <<'THRESHOLD'
import json
from pathlib import Path
import sys
totals = json.loads(Path(sys.argv[1]).read_text())["totals"]
if min(totals["source_over_xz"], totals["records_and_metadata_ratio"]) < 3:
    raise SystemExit("Full-corpus lossless reduction is below 3x; stopping before training: " + str(totals))
THRESHOLD
  for corpus in v4 v5; do
    if [[ "$corpus" == v4 ]]; then
      source=/mnt/shared/gothic-ai/archive/nav-demos-v4-20261029
      manifest="$source/manifest.json"
      converted=/mnt/shared/gothic-ai/nav-lean-data/parity-v4
    else
      source=/mnt/shared/gothic-ai/nav-demos-v5/recordings
      manifest="$task/input/v5-compact.json"
      converted="$compact"
    fi
    "$task/.venv/bin/python" -B -X faulthandler -I scripts/nav_data_bench.py parity \
      --corpus "$corpus" --source "$source" --manifest "$manifest" --converted-source "$converted" \
      --reference-script "$task/input/nav_bc_data_before_lean.py" \
      --output "$results/parity-$corpus" --scratch "$task/scratch/parity-$corpus"
  done
  exit 0
fi
if [[ "$stage" == probe ]]; then
  "$HOME/.local/bin/uv" pip install --python "$task/.venv/bin/python" zstandard
  cd "$task/source"
  "$task/.venv/bin/python" -B -X faulthandler -m pytest -q -p no:cacheprovider \
    --basetemp "$task/scratch/pytest-probe" scripts/tests/test_nav_demos_v4_chain.py \
    scripts/tests/test_nav_demos_v5_chain.py scripts/tests/test_nav_ppo_loop.py scripts/tests/test_nav_jumps.py
  mode=probe
else
  mode=profile
fi
for corpus in v4 v5; do
  if [[ "$corpus" == v4 ]]; then
    source=/mnt/shared/gothic-ai/archive/nav-demos-v4-20261029
    manifest="$source/manifest.json"
  else
    source=/mnt/shared/gothic-ai/nav-demos-v5/recordings
    manifest="$task/input/v5-compact.json"
  fi
  "$task/.venv/bin/python" -B -X faulthandler -I "$task/source/scripts/nav_data_bench.py" "$mode" \
    --corpus "$corpus" --source "$source" --manifest "$manifest" \
    --output "$results/$mode-$corpus" --scratch "$task/scratch/$stage-$corpus"
done
if [[ "$stage" == baseline ]]; then
  "$task/.venv/bin/python" -B -X faulthandler -I "$task/source/scripts/nav_data_bench.py" replay \
    --corpus v4 --source /mnt/shared/gothic-ai/archive/nav-demos-v4-20261029 \
    --manifest /mnt/shared/gothic-ai/archive/nav-demos-v4-20261029/manifest.json \
    --output "$results/replay-v4-200-300" --scratch "$task/scratch/replay-v4-200-300"
fi
BODY
ssh ryzen "cat > '$task/measure-job.sh'" <<'JOB'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
stage="${1:?stage}"
exec > >(tee -a "$task/$stage-results/job.log") 2>&1
exec python3 -I "$task/source/scripts/nav_job.py" --max-seconds 7200 -- \
  bash "$task/measure-body.sh" "$stage"
JOB
resources=(--mem 8G)
if [[ "$stage" == loader ]]; then resources=(--mem 10G --gpu-mem 4G); fi
"$ROOT/scripts/gq" submit --class quiet --threads 6 --cpu-quota 600% "${resources[@]}" --max 2h \
  --name "nav lean-data $stage v4 v5" --archive "$task/$stage-results" -- \
  bash "$task/measure-job.sh" "$stage"
