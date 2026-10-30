#!/usr/bin/env bash
# Submit measurements without waiting for or polling gq.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
stage="${1:-baseline}"
[[ $# -le 1 && ( "$stage" == baseline || "$stage" == probe ) ]] || {
  echo 'Usage: bash scripts/nav-lean-data-remote.sh [baseline|probe]' >&2
  exit 2
}
export GQ_OWNER=nav-lean-data
task="$(ssh -T -o BatchMode=yes -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-lean-data" "$HOME"')"
ssh ryzen "mkdir -p '$task/source/scripts' '$task/input' '$task/scratch' '$task/$stage-results'"
rsync -rt --no-links --exclude=__pycache__ "$ROOT/scripts/" "ryzen:$task/source/scripts/"
rsync -rt --no-links "$ROOT/reports/navigation/2026-10-22-demos-v5/manifest.json" \
  "ryzen:$task/input/v5-compact.json"
ssh ryzen "cat > '$task/measure-body.sh'" <<'BODY'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
stage="${1:?stage}"
results="$task/$stage-results"
export PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 MKL_NUM_THREADS=1
df -h "$HOME" /mnt/shared
"$HOME/.local/bin/uv" venv --python 3.12 --allow-existing "$task/.venv"
"$HOME/.local/bin/uv" pip install --python "$task/.venv/bin/python" 'numpy==2.4.2' pytest
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
"$ROOT/scripts/gq" submit --class quiet --threads 6 --cpu-quota 600% --mem 8G --max 2h \
  --name "nav lean-data $stage v4 v5" --archive "$task/$stage-results" -- \
  bash "$task/measure-job.sh" "$stage"
