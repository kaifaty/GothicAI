#!/usr/bin/env bash
# Submit one screen run and return its gq ID without waiting or polling.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[[ $# == 1 && "$1" == /* ]] || {
  echo 'Usage: bash scripts/nav-eval-screen-remote.sh /absolute/ryzen/path/to/policy.navbc' >&2
  exit 2
}
weights="$1"
run="$(date -u +%Y%m%dT%H%M%SZ)-$(git -C "$ROOT" rev-parse --short HEAD)"
task="$(ssh -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-eval-screen" "$HOME"')"
runtime="$task/work/$run"
output="$task/runs/$run"
printf -v setup 'bash -s -- %q %q %q' "$runtime" "$output" "$weights"
ssh ryzen "$setup" <<'SETUP'
set -euo pipefail
runtime="$1"; output="$2"; weights="$3"
test -f "$weights"
df -h "$HOME" /mnt/shared
python3 -c 'import pathlib,shutil; assert shutil.disk_usage(pathlib.Path.home()).free >= 26*1024**3, "Less than 26 GiB free"'
mkdir -p "$runtime/source" "$runtime/input" "$output"
cp "$weights" "$output/policy.navbc"
inputs=/mnt/shared/gothic-ai/archive/nav-bc4-20261030/input
for name in save.sav accepted-links.txt jump-links.txt places.json familiar.bin; do
  cp "$inputs/$name" "$runtime/input/$name"
done
SETUP
rsync -a --no-links --exclude=build --exclude='build-*' --exclude=__pycache__ --exclude=.git \
  "$ROOT/OpenGothic" "$ROOT/scripts" "ryzen:$runtime/source/"
rsync -rt --no-links "$ROOT/reports/navigation/2026-10-31-eval-screen/eval-screen.jsonl" \
  "ryzen:$runtime/input/"
printf -v phase 'cat > %q' "$runtime/phase.sh"
ssh ryzen "$phase" <<'PHASE'
#!/usr/bin/env bash
set -euo pipefail
runtime="$(cd "$(dirname "$0")" && pwd)"
output="${1:?absolute output directory}"
exec > >(tee "$output/phase.log") 2>&1
started="$(date +%s)"
cleanup() {
  rm -rf -- "$runtime/build" "$runtime/cache" "$runtime/base-cache" "$runtime/base-store" \
    "$runtime/.venv" "$runtime/uv-cache"
}
trap cleanup EXIT
eval "$(python3 "$runtime/source/scripts/ryzen_game.py")"
export OPENGOTHIC_BASE_CACHE="$runtime/base-cache" OPENGOTHIC_BASE_STORE="$runtime/base-store"
# Read the prepared base without publishing or modifying another task's cache.
export OPENGOTHIC_BASE="/mnt/shared/gothic-ai/base/$OPENGOTHIC_BASE_FINGERPRINT"
test -f "$OPENGOTHIC_BASE/base.json"
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH" UV_CACHE_DIR="$runtime/uv-cache"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export OPENGOTHIC_BODY_TRACE=0 PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
unset OPENGOTHIC_NAV_JOURNEY_MEMORY OPENGOTHIC_NAV_FAMILIAR_MAP
cd "$runtime/source"
uv venv --python 3.12 "$runtime/.venv"
python="$runtime/.venv/bin/python"
uv pip install --python "$python" numpy pytest
"$python" -B -m pytest -q -p no:cacheprovider scripts/tests/test_nav_rl.py
runner_source_sha256="$("$python" -B - "$runtime/source" "$output" <<'REUSE'
from pathlib import Path
import sys
sys.path.insert(0, str(Path.cwd() / 'scripts'))
from nav_eval_screen import reuse_runner
print(reuse_runner(Path(sys.argv[1]), Path('/mnt/shared/gothic-ai/nav-eval-screen/runs'), Path(sys.argv[2])))
REUSE
)"
runner_reused=true
if [[ ! -x "$output/gothic-body-runner" ]]; then
  runner_reused=false
  source scripts/ryzen-build.sh
  cmake -S OpenGothic -B "$runtime/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    "${RYZEN_CMAKE_FLAGS[@]}" \
    -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
    -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
  cmake --build "$runtime/build" --target gothic-body-runner --parallel 3
  cp "$runtime/build/opengothic/gothic-body-runner" "$output/gothic-body-runner"
fi
echo "Evaluation runner reused: $runner_reused"
for mode in ordinary jump; do
  mkdir -p "$runtime/cache/$mode"
  cp /mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-measure-cache/"$mode"/*.navmesh "$runtime/cache/$mode/"
done
cp "$runtime/input/eval-screen.jsonl" "$output/eval-screen.jsonl"
"$python" -B - "$runtime" "$output" "$started" "$runner_source_sha256" "$runner_reused" <<'DRIVE'
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time
runtime, output = map(Path, sys.argv[1:3])
started = int(sys.argv[3])
command = [sys.executable, '-B', '-I', 'scripts/nav_rl.py', 'drive',
    '--eval-screen', str(output / 'eval-screen.jsonl'),
    '--runner', str(output / 'gothic-body-runner'),
    '--assets', __import__('os').environ['GOTHIC_DIR'], '--save', str(runtime / 'input/save.sav'),
    '--links', str(runtime / 'input/accepted-links.txt'), '--jump-links', str(runtime / 'input/jump-links.txt'),
    '--roads', str(runtime / 'input/places.json'), '--familiar-map', str(runtime / 'input/familiar.bin'),
    '--cache', str(runtime / 'cache'), '--weights', str(output / 'policy.navbc'),
    '--output', str(output / 'drive'), '--physics', 'cv', '--parallel', '18',
    '--action-mode', 'sample', '--limit-ms', '30000', '--watchdog-seconds', '900']
before = time.monotonic()
subprocess.run(command, check=True)
subprocess.run([sys.executable, '-B', '-I', 'scripts/nav_rl.py', 'summarize',
                '--source', str(output / 'drive'), '--output', str(output)], check=True)
timing = dict(elapsed_s=time.monotonic() - before, preparation_s=time.time() - started - (time.monotonic() - before),
              total_s=time.time() - started, parallel=18,
              runner_source_sha256=sys.argv[4], runner_reused=sys.argv[5] == 'true',
              runner_sha256=hashlib.sha256((output / 'gothic-body-runner').read_bytes()).hexdigest(),
              screen_sha256=hashlib.sha256((output / 'eval-screen.jsonl').read_bytes()).hexdigest(),
              weights_sha256=hashlib.sha256((output / 'policy.navbc').read_bytes()).hexdigest())
(output / 'timing.json').write_text(json.dumps(timing, indent=2) + '\n')
print(json.dumps(dict(event='eval_screen_complete', **timing)))
DRIVE
PHASE
export GQ_OWNER=nav-eval-screen
"$ROOT/scripts/gq" submit --class batch --threads 28 --cpu-quota 2800% --mem 20G --max 90m \
  --archive "$output" --name "nav eval screen $run" -- bash "$runtime/phase.sh" "$output"
