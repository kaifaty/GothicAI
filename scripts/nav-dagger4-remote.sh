#!/usr/bin/env bash
# Submit one bounded phase and return its queue ID without waiting.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ $# == 1 && ( "$1" == --help || "$1" == -h ) ]]; then
  echo 'Usage: bash scripts/nav-dagger4-remote.sh smoke'
  exit 0
fi
[[ $# == 1 && "$1" == smoke ]] || { echo 'Usage: bash scripts/nav-dagger4-remote.sh smoke' >&2; exit 2; }
export GQ_OWNER=nav-dagger4
task="$(ssh -o BatchMode=yes -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-dagger4" "$HOME"')"
archive=/mnt/shared/gothic-ai/nav-dagger4
ssh ryzen "bash -s -- '$task' '$archive'" <<'INPUTS'
set -euo pipefail
task="$1"; archive="$2"
df -h "$HOME" /mnt/shared
python3 -c 'import pathlib,shutil; assert shutil.disk_usage(pathlib.Path.home()).free >= 22*1024**3, "Less than 22 GiB free"'
mkdir -p "$task/source" "$task/input/hd" "$task/tmp" "$archive/logs"
copy_input() { test -f "$2" || cp "$1" "$2"; }
for name in last.pt policy.navbc; do
  copy_input "$HOME/gothic-ai/nav-dagger3/hd-run/$name" "$task/input/hd/$name"
done
for name in save.sav places.json accepted-links.txt jump-links.txt; do
  copy_input "$HOME/gothic-ai/nav-demos-v5/input/$name" "$task/input/$name"
done
for mode in ordinary jump; do
  mkdir -p "$task/smoke-cache/$mode"
  for source in "$HOME/gothic-ai/nav-dagger3/hd-measure-cache/$mode/"*.navmesh; do
    copy_input "$source" "$task/smoke-cache/$mode/$(basename "$source")"
  done
done
python3 - "$task" <<'SELECT'
import hashlib
import json
from pathlib import Path
import sys
task = Path(sys.argv[1])
weights = task / "input/hd/policy.navbc"
if hashlib.sha256(weights.read_bytes()).hexdigest() != "304ea6c6574e3836139da72b5a299a28217a263fac3648d792b458327d251a6b":
    raise ValueError("HD weights differ from the selected DAgger3 checkpoint")
values = []
for family in ("water", "jump"):
    source = Path.home() / "gothic-ai/nav-demos-v5/recording-input" / f"{family}.jsonl"
    value = json.loads(source.read_text().splitlines()[0])
    if value["split"] != "train":
        raise ValueError("Smoke may use only frozen training scenarios")
    values.append(dict(value, family=family, limit_ms=12000))
target = task / "input/smoke-train.jsonl"
content = "".join(json.dumps(value, sort_keys=True) + "\n" for value in values)
if target.exists() and target.read_text() != content:
    raise ValueError("Existing smoke training scenarios differ")
target.write_text(content)
SELECT
INPUTS
rsync -a --no-links --exclude=build --exclude='build-*' --exclude=__pycache__ --exclude=.git \
  "$ROOT/OpenGothic" "$ROOT/scripts" "$ROOT/AGENTS.md" "ryzen:$task/source/"
ssh ryzen "cat > '$task/smoke.sh'" <<'PHASE'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
archive=/mnt/shared/gothic-ai/nav-dagger4
exec > >(tee -a "$archive/logs/smoke.log") 2>&1
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export UV_CACHE_DIR="$HOME/.cache/uv" TMPDIR="$task/tmp"
export OMP_NUM_THREADS=2 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export OPENGOTHIC_BODY_TRACE=0 PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
export OPENGOTHIC_NAV_JOURNEY_MEMORY=1
unset OPENGOTHIC_NAV_FAMILIAR_MAP
cd "$task/source"
df -h "$HOME" /mnt/shared
python="$task/.venv/bin/python"
if [[ ! -x "$python" ]]; then uv venv --python 3.12 "$task/.venv"; fi
uv pip install --python "$python" numpy pytest
uv pip install --python "$python" 'torch==2.9.1+cu128' --index-url https://download.pytorch.org/whl/cu128
"$python" -B -m pytest -q -p no:cacheprovider --basetemp "$task/tmp/pytest" \
  scripts/tests/test_nav_rl.py scripts/tests/test_nav_bc_data.py scripts/tests/test_nav_bc_train.py
cmake -S "$task/source/OpenGothic" -B "$task/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
  -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
cmake --build "$task/build" --target gothic-body-runner gothic-navpolicy-contract --parallel 3
"$python" -B -I - "$task" <<'EXPORT'
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path.cwd() / "scripts"))
from nav_bc_data import HD_PREPROCESSING, VECTOR_SIZE
from nav_bc_model import NavPolicy, export_weights
from nav_bc_train import initialize_bc3_hd
task = Path(sys.argv[1])
with (task / "input/hd/policy.navbc").open("rb") as stream:
    header = json.loads(stream.readline())
    payload = stream.read()
layout = header["preprocessing"]["eye_layout"]
model = NavPolicy(VECTOR_SIZE, format_version=3, auxiliary=True)
initialized = initialize_bc3_hd(model, task / "input/hd/last.pt", dict(eye_layout=layout))
if initialized["reset"] or initialized["expanded"]:
    raise ValueError("DAgger initialization must preserve all HD tensors")
(task / "smoke").mkdir(exist_ok=True)
weights = task / "smoke/policy.navbc"
export_weights(model, weights, dict(HD_PREPROCESSING, navigation_memory_version=4, eye_layout=layout))
with weights.open("rb") as stream:
    exported = json.loads(stream.readline())
    if stream.read() != payload:
        raise ValueError("Memory-v4 HD export changed the selected policy tensors")
if exported["version"] != 3 or exported["preprocessing"]["navigation_memory_version"] != 4:
    raise ValueError("HD export must retain format 3 and select memory 4")
EXPORT
"$task/build/opengothic/gothic-navpolicy-contract" --memory "$task/smoke/policy.navbc"
"$python" -B -I scripts/nav_rl.py drive --dagger-scenarios "$task/input/smoke-train.jsonl" \
  --runner "$task/build/opengothic/gothic-body-runner" \
  --assets "$HOME/gothic-ai/game" --save "$task/input/save.sav" \
  --cache "$task/smoke-cache" --links "$task/input/accepted-links.txt" \
  --jump-links "$task/input/jump-links.txt" --roads "$task/input/places.json" \
  --weights "$task/smoke/policy.navbc" --output "$task/smoke" --physics cv --parallel 1 \
  --teacher-beta 0.1 --action-mode sample --action-seeds 23102026 --limit-ms 12000 \
  --watchdog-seconds 900 --resume
"$python" -B -I - "$task" "$archive" <<'VERIFY'
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
task, archive = map(Path, sys.argv[1:])
source, target = task / "smoke", archive / "smoke"
episodes = list(source.glob("dagger/*/*/seed-*/episode.json"))
if len(episodes) != 2:
    raise ValueError(f"Smoke needs exactly two episodes, found {len(episodes)}")
for path in episodes:
    value = json.loads(path.read_text())
    if (value.get("navigation_memory_version") != 4 or value.get("teacher_beta", 0) < .1
            or value.get("action_mode") != "sample" or value.get("drive") is not True):
        raise ValueError(f"Smoke learner configuration differs: {path}")
    steps = [json.loads(line) for line in (path.parent / "samples.jsonl").read_text().splitlines()]
    if len(steps) != value["decisions"] or not any(step["teacher_control"] for step in steps):
        raise ValueError(f"Smoke lacks aligned teacher-control metadata: {path}")
    if (any(step["dart"] and not step["teacher_control"] for step in steps)
            or sum(step["teacher_control"] for step in steps) != value["teacher_control_decisions"]
            or sum(step["dart"] for step in steps) != value["dart_decisions"]
            or sum(step["maneuver_mask"] for step in steps) != value["maneuver_masked_decisions"]):
        raise ValueError(f"Smoke intervention counters differ from per-decision metadata: {path}")
    print(json.dumps(dict(event="dagger4_smoke_episode", family=path.parents[2].name,
                         decisions=len(steps), teacher_steps=value["teacher_control_decisions"],
                         interventions=value["dagger_interventions"], dart_steps=value["dart_decisions"],
                         maneuver_masked=value["maneuver_masked_decisions"])))
def checksums(root):
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file()}
expected = checksums(source)
target.mkdir(parents=True, exist_ok=True)
subprocess.run(["rsync", "-rt", "--no-links", str(source) + "/", str(target) + "/"], check=True)
if checksums(target) != expected:
    raise ValueError("Smoke archive checksum mismatch; retaining local recording")
shutil.rmtree(source)
(target / "passed").write_text("2 train episodes; memory4; sampled; labelled teacher control\n")
print(json.dumps(dict(event="dagger4_smoke_passed", episodes=2, archive=str(target))))
VERIFY
PHASE
"$ROOT/scripts/gq" submit --class quick --threads 6 --cpu-quota 600% --mem 12G --max 35m \
  --name 'nav dagger4 train-only water+jump smoke' -- bash "$task/smoke.sh"
