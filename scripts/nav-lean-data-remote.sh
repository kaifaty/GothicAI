#!/usr/bin/env bash
# Submit the v4/v5 storage and prepare measurements without waiting for gq.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export GQ_OWNER=nav-lean-data
task="$(ssh -T -o BatchMode=yes -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-lean-data" "$HOME"')"
ssh ryzen "mkdir -p '$task/source/scripts' '$task/input' '$task/scratch' '$task/results'"
rsync -rt --no-links "$ROOT/scripts/nav_data_bench.py" "$ROOT/scripts/nav_bc_data.py" \
  "$ROOT/scripts/nav_demos.py" "ryzen:$task/source/scripts/"
rsync -rt --no-links "$ROOT/reports/navigation/2026-10-22-demos-v5/manifest.json" \
  "ryzen:$task/input/v5-compact.json"
ssh ryzen "cat > '$task/baseline-body.sh'" <<'BODY'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
export PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 MKL_NUM_THREADS=1
df -h "$HOME" /mnt/shared
"$HOME/.local/bin/uv" venv --python 3.12 --allow-existing "$task/.venv"
"$HOME/.local/bin/uv" pip install --python "$task/.venv/bin/python" 'numpy==2.4.2'
for corpus in v4 v5; do
  if [[ "$corpus" == v4 ]]; then
    source=/mnt/shared/gothic-ai/archive/nav-demos-v4-20261029
    manifest="$source/manifest.json"
  else
    source=/mnt/shared/gothic-ai/nav-demos-v5/recordings
    manifest="$task/input/v5-compact.json"
  fi
  "$task/.venv/bin/python" -B -X faulthandler -I "$task/source/scripts/nav_data_bench.py" profile \
    --corpus "$corpus" --source "$source" --manifest "$manifest" \
    --output "$task/results/profile-$corpus" --scratch "$task/scratch/profile-$corpus"
done
"$task/.venv/bin/python" -B -X faulthandler -I "$task/source/scripts/nav_data_bench.py" replay \
  --corpus v4 --source /mnt/shared/gothic-ai/archive/nav-demos-v4-20261029 \
  --manifest /mnt/shared/gothic-ai/archive/nav-demos-v4-20261029/manifest.json \
  --output "$task/results/replay-v4-200-300" --scratch "$task/scratch/replay-v4-200-300"
BODY
ssh ryzen "cat > '$task/baseline.sh'" <<'JOB'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$task/results"
exec > >(tee -a "$task/results/baseline.log") 2>&1
ulimit -c 0
python3 - "$task" <<'SUPERVISE'
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

started = time.monotonic()
print("nav lean-data start: internal deadline=6300s; gq deadline=7200s", flush=True)
child = subprocess.Popen(["bash", str(Path(sys.argv[1]) / "baseline-body.sh")], start_new_session=True)

def send_group(sig):
    try:
        os.killpg(child.pid, sig)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        # macOS can retain a group with no signalable processes after exit.
        if child.poll() is not None:
            return False
        raise

def interrupted(sig, _frame):
    raise SystemExit(128 + sig)

for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
    signal.signal(sig, interrupted)
try:
    while True:
        remaining = 6300 - (time.monotonic() - started)
        if remaining <= 0:
            print("nav lean-data timeout: aborting worker group for Python stacks", flush=True)
            send_group(signal.SIGABRT)
            # Keep supervising descendants even if their immediate bash exits.
            grace = time.monotonic() + 15
            while time.monotonic() < grace and send_group(0):
                time.sleep(1)
            result = 124
            break
        try:
            result = child.wait(timeout=min(30, remaining))
            break
        except subprocess.TimeoutExpired:
            print(f"nav lean-data heartbeat: elapsed={time.monotonic() - started:.1f}s (setup/profile/replay)", flush=True)
finally:
    send_group(signal.SIGKILL)
print(f"nav lean-data end: exit={result} elapsed={time.monotonic() - started:.1f}s", flush=True)
sys.exit(result if result >= 0 else 128 - result)
SUPERVISE
JOB
"$ROOT/scripts/gq" submit --class quiet --threads 6 --cpu-quota 600% --mem 8G --max 2h \
  --name 'nav lean-data v4 v5 baseline and prepare 200-300 replay' \
  --archive "$task/results" -- bash "$task/baseline.sh"
