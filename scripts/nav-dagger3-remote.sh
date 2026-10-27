#!/usr/bin/env bash
# Submit one bounded phase, then return its gq ID without waiting.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
stage="${1:-}"
[[ $# == 1 ]] || { echo 'Usage: nav-dagger3-remote.sh prepare-detours|baseline-detours|train-hd|measure-hd' >&2; exit 2; }
case "$stage" in prepare-detours|baseline-detours|train-hd|measure-hd) ;; *) echo "Unknown phase: $stage" >&2; exit 2 ;; esac
export GQ_OWNER=nav-dagger3
task="$(ssh -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-dagger3" "$HOME"')"
ssh ryzen 'df -h "$HOME"'
if [[ "$stage" == measure-hd ]]; then
  ssh ryzen "mkdir -p '$task/source' '$task/logs'"
  rsync -a --exclude=build --exclude='build-*' --exclude=__pycache__ --exclude=.git \
    "$ROOT/OpenGothic" "$ROOT/scripts" "$ROOT/AGENTS.md" "ryzen:$task/source/"
  ssh ryzen "cat > '$task/measure-hd.sh'" <<'MEASURE'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
corpus="$HOME/gothic-ai/nav-demos-v4"
bc3="$HOME/gothic-ai/nav-bc3"
exec > >(tee -a "$task/logs/measure-hd.log") 2>&1
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export UV_CACHE_DIR="$HOME/.cache/uv"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export OPENGOTHIC_BODY_TRACE=0 PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
python="$task/.venv/bin/python"
cd "$task/source"
df -h "$HOME"
"$python" -B -I - "$task" <<'SPACE'
from pathlib import Path
import shutil
import sys
task = Path(sys.argv[1])
if (task / "hd-cache").exists() or (task / "build-hd").exists():
    raise SystemExit("Remove the completed HD cache and redundant build before measurement")
if shutil.disk_usage(task).free < 22 * 1024**3:
    raise SystemExit("HD measurement needs 22 GiB free including the 20-GiB reserve")
SPACE
"$python" -B -m pytest -q -p no:cacheprovider scripts/tests/test_nav_rl.py
cmake -S "$task/source/OpenGothic" -B "$task/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
  -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
cmake --build "$task/build" --target gothic-body-runner --parallel 3
# Both formats use the same recorded observations, core and scalar C++ binary.
"$python" -B -I - "$task" "$bc3" <<'TIMING'
import gzip
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
task, bc3 = map(Path, sys.argv[1:])
policies = dict(bc3=bc3 / "run/it35/policy.navbc", hd=task / "hd-run/policy.navbc")
timing_path = task / "hd-policy-timing.json"
if timing_path.is_file():
    saved = json.loads(timing_path.read_text())
    if saved["weights_sha256"] != {name:hashlib.sha256(path.read_bytes()).hexdigest() for name,path in policies.items()}:
        raise ValueError("Saved C++ timing belongs to different policy weights")
    print(json.dumps(dict(event="reusing_cpp_timing", **saved)))
    sys.exit(0)
fixtures = {name: task / f"hd-parity/v{version}/parity.bin.gz" for name, version in (("bc3", 2), ("hd", 3))}
headers = {}
for name, fixture in fixtures.items():
    with gzip.open(fixture, "rb") as stream:
        headers[name] = json.loads(stream.readline())
if any(headers["bc3"][key] != headers["hd"][key] for key in ("source", "steps", "eye_layout")):
    raise ValueError("Policy timing fixtures do not share the same recorded observations")
cpu = min(os.sched_getaffinity(0))
measurements = []
for repeat in range(3):
    for name in (("bc3", "hd") if repeat % 2 == 0 else ("hd", "bc3")):
        result = subprocess.run(["taskset", "-c", str(cpu), str(task / "gothic-navpolicy-contract"),
            "--benchmark", str(policies[name]), str(fixtures[name]), "--npcs", "1", "--decisions", "2000"],
            check=True, capture_output=True, text=True)
        measured = json.loads(result.stdout)
        if measured["threads"] != 1 or measured["npcs"] != 1:
            raise ValueError("Policy timing did not use one NPC on one thread")
        measurements.append(dict(policy=name, repeat=repeat, **measured))
medians = {name: statistics.median(row["policy_median_us"] for row in measurements if row["policy"] == name)
           for name in policies}
summary = dict(scope="C++ encode + inference; one pinned thread, 16 identical recorded inputs, carried GRU",
    queue_class="batch", cpu=cpu, load_average=os.getloadavg(), measurements=measurements,
    policy_median_us=medians, hd_to_bc3=medians["hd"] / medians["bc3"],
    weights_sha256={name:hashlib.sha256(path.read_bytes()).hexdigest() for name,path in policies.items()})
(task / "hd-policy-timing.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary))
TIMING
mkdir -p "$task/hd-validation/setup-rejections" "$task/hd-measure-cache/ordinary" "$task/hd-measure-cache/jump"
cp -n "$bc3/validation/setup-rejections/"*.json "$task/hd-validation/setup-rejections/"
cp -n "$corpus/recording-cache/v3/"*.navmesh "$task/hd-measure-cache/ordinary/"
cp -n "$corpus/recording-cache/jumps/"*.navmesh "$task/hd-measure-cache/jump/"
for policy in bc3 hd; do
  case "$policy" in
    bc3) weights="$bc3/run/it35/policy.navbc" ;;
    hd) weights="$task/hd-run/policy.navbc" ;;
  esac
  "$python" -B -I -c 'import runpy,sys; runpy.run_path(sys.argv[1])["require_room"](sys.argv[2])' \
    "$task/source/scripts/nav_dagger3_detours.py" "$task"
  output="$task/hd-validation/$policy"
  "$python" -B -I scripts/nav_rl.py drive --manifest "$corpus/manifest.json" --lean \
    --runner "$task/build/opengothic/gothic-body-runner" \
    --assets "$HOME/gothic-ai/game" --save "$corpus/input/save.sav" \
    --cache "$task/hd-measure-cache" --links "$corpus/input/accepted-links.txt" \
    --jump-links "$corpus/input/jump-links.txt" --roads "$corpus/input/places.json" \
    --weights "$weights" --output "$output" --physics cv --parallel 18 \
    --action-mode sample --action-seeds 18102027,18102028,18102029 \
    --safety-fixtures "$HOME/gothic-ai/nav-perception/safety-check-1/fixtures.json" \
    --fence-control --limit-ms 30000 --watchdog-seconds 900 --resume
  "$python" -B -I scripts/nav_rl.py summarize --source "$output" --output "$output" \
    --object-probes "$corpus/input/object-probes.jsonl"
done
# The completed BC3 detour cohort is read, never re-executed.
"$python" -B -I scripts/nav_rl.py drive --detour-scenarios "$task/detours/detour-val.jsonl" \
  --runner "$task/build/opengothic/gothic-body-runner" \
  --assets "$HOME/gothic-ai/game" --save "$corpus/input/save.sav" \
  --cache "$task/hd-measure-cache" --links "$corpus/input/accepted-links.txt" --roads "$corpus/input/places.json" \
  --weights "$task/hd-run/policy.navbc" --output "$task/hd-detour" --physics cv --parallel 18 \
  --action-mode sample --action-seeds 18102027,18102028,18102029 \
  --limit-ms 240000 --watchdog-seconds 900 --resume
"$python" -B -I scripts/nav_rl.py summarize --source "$task/hd-detour" --output "$task/hd-detour"
"$python" -B -I - "$task" "$bc3" <<'CHECK'
import json
from pathlib import Path
import shutil
import sys
task, bc3 = map(Path, sys.argv[1:])
reference = json.loads((bc3 / "validation/bc3/metrics.json").read_text())
identity = lambda episodes: {(ep["id"], ep["action_seed"]) for ep in episodes}
for name in ("bc3", "hd"):
    root = task / "hd-validation" / name
    result = json.loads((root / "metrics.json").read_text())
    episodes = result["episodes"]
    if len(episodes) != 1491 or identity(episodes) != identity(reference["episodes"]):
        raise ValueError(f"Lean validation cohort differs for {name}")
    if any(ep["teacher_control_decisions"] != 0 or ep["recording"] != "text_only" for ep in episodes):
        raise ValueError(f"Lean validation used teacher control or NAVDEMO for {name}")
    if list(root.rglob("samples.bin*")):
        raise ValueError(f"Lean validation wrote NAVDEMO for {name}")
    print(json.dumps(dict(event="hd_validation_complete", policy=name, episodes=len(episodes))))
original = json.loads((task / "detour-baseline/bc3/metrics.json").read_text())
detour = json.loads((task / "hd-detour/metrics.json").read_text())
if len(detour["episodes"]) != 363 or identity(detour["episodes"]) != identity(original["episodes"]):
    raise ValueError("HD detour validation is incomplete")
if any(ep["teacher_control_decisions"] != 0 or ep["recording"] != "text_only" for ep in detour["episodes"]):
    raise ValueError("HD detour validation used teacher control or NAVDEMO")
if shutil.disk_usage(task).free < 20 * 1024**3:
    raise ValueError("HD measurement consumed the 20-GiB disk reserve")
print(json.dumps(dict(event="hd_detour_complete", episodes=363, bc3_arrived=original["all"]["arrived"],
                     hd_arrived=detour["all"]["arrived"])))
CHECK
df -h "$HOME"
MEASURE
  "$ROOT/scripts/gq" submit --class batch --threads 28 --cpu-quota 2800% --mem 20G --max 2h \
    --name "nav dagger3 HD vs BC3 lean validation" -- bash "$task/measure-hd.sh"
  exit
fi
if [[ "$stage" == train-hd ]]; then
  # Keep the running baseline's source and build untouched.
  ssh ryzen "mkdir -p '$task/hd-source' '$task/logs'"
  rsync -a --exclude=build --exclude='build-*' --exclude=__pycache__ --exclude=.git \
    "$ROOT/OpenGothic" "$ROOT/scripts" "$ROOT/AGENTS.md" "ryzen:$task/hd-source/"
  ssh ryzen "cat > '$task/train-hd.sh'" <<'HD'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
corpus="$HOME/gothic-ai/nav-demos-v4"
bc3="$HOME/gothic-ai/nav-bc3"
exec > >(tee -a "$task/logs/train-hd.log") 2>&1
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export UV_CACHE_DIR="$HOME/.cache/uv"
export OMP_NUM_THREADS=6 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export OPENGOTHIC_BODY_TRACE=0 PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
# Reuse the existing Torch environment read-only; -B also applies with -I.
python="$bc3/.venv/bin/python"
cd "$task/hd-source"
df -h "$HOME"
nvidia-smi
"$python" -B -I - "$task" <<'SPACE'
import shutil
import sys
if shutil.disk_usage(sys.argv[1]).free < 22 * 1024**3:
    raise SystemExit("HD preparation needs 22 GiB free including the 20-GiB reserve")
SPACE
"$python" -B -m pytest -q -p no:cacheprovider \
  scripts/tests/test_nav_bc_data.py scripts/tests/test_nav_bc_model.py scripts/tests/test_nav_bc_contract.py
cmake -S "$task/hd-source/OpenGothic" -B "$task/build-hd" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
  -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
cmake --build "$task/build-hd" --target gothic-navpolicy-contract \
  CMakeFiles/OpenGothic.dir/common/navdemos.cpp.o --parallel 3
"$python" -B -I scripts/nav_bc_data.py prepare --source "$corpus" \
  --manifest "$corpus/manifest.json" --output "$task/hd-cache" --input-variant hd
"$python" -B -I - "$bc3/cache/index.json" "$task/hd-cache/index.json" <<'COHORT'
import json
from pathlib import Path
import sys
compact, hd = [json.loads(Path(path).read_text()) for path in sys.argv[1:]]
def population(index):
    return sorted((ep["id"], ep["steps"], ep["split"], tuple(ep["pair"])) for ep in index["episodes"])
if population(compact) != population(hd) or len(hd["episodes"]) != 602:
    raise ValueError("HD training population differs from the 602 BC3 episodes")
for field in ("class_counts", "action_change_weight", "eye_layout"):
    if compact[field] != hd[field]:
        raise ValueError(f"HD training recipe differs: {field}")
steps = sum(ep["steps"] for ep in hd["episodes"])
if steps != 405626:
    raise ValueError(f"HD training decision count differs: {steps}")
print(json.dumps(dict(event="hd_cohort_checked", episodes=602, decisions=steps)))
COHORT
"$python" -B -I scripts/nav_bc_train.py train --cache "$task/hd-cache" --output "$task/hd-run" \
  --init "$bc3/run/it35/last.pt" --epochs 20 --batch 8 --segment 64 \
  --lr .0003 --seed 17 --device cuda --threads 6
for version in 1 2 3; do
  case "$version" in
    1) weights="$HOME/gothic-ai/nav-ppo/run/iter-000035/update/policy.navbc" ;;
    2) weights="$bc3/run/it35/policy.navbc" ;;
    3) weights="$task/hd-run/policy.navbc" ;;
  esac
  "$python" -B -I scripts/check_nav_policy.py --weights "$weights" \
    --source "$corpus/recordings" --output "$task/hd-parity/v$version" --steps 1000 --torch \
    | tee "$task/logs/hd-parity-v$version-python.log"
  "$task/build-hd/opengothic/gothic-navpolicy-contract" \
    "$weights" "$task/hd-parity/v$version/parity.bin.gz" \
    | tee "$task/logs/hd-parity-v$version-cpp.log"
done
sha256sum "$bc3/run/it35/policy.navbc" "$task/hd-run/policy.navbc"
df -h "$HOME"
HD
  "$ROOT/scripts/gq" submit --class batch --threads 6 --cpu-quota 600% --mem 10G --gpu-mem 4G --max 2h \
    --name "nav dagger3 HD BC training and contract" -- bash "$task/train-hd.sh"
  exit
fi
ssh ryzen "mkdir -p '$task/source' '$task/logs'"
rsync -a --exclude=build --exclude='build-*' --exclude=__pycache__ --exclude=.git \
  "$ROOT/OpenGothic" "$ROOT/scripts" "$ROOT/AGENTS.md" "ryzen:$task/source/"
ssh ryzen "cat > '$task/prepare-detours.sh'" <<'PHASE'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
exec > >(tee -a "$task/logs/prepare-detours.log") 2>&1
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export UV_CACHE_DIR="$HOME/.cache/uv"
export OMP_NUM_THREADS=2 OPENBLAS_NUM_THREADS=1 OPENGOTHIC_BODY_TRACE=0
df -h "$HOME"
python3 - "$task" <<'SPACE'
import shutil
import sys
free = shutil.disk_usage(sys.argv[1]).free
if free < 26 * 1024**3:
    raise SystemExit(f"Insufficient build space above 20-GiB reserve: {free} bytes free")
SPACE
cd "$task/source"
source "$task/source/scripts/ryzen-venv.sh"
ryzen_task_venv "$task/.venv"
"$task/.venv/bin/python" -m pytest -q scripts/tests/test_nav_dagger3_detours.py
cmake -S "$task/source/OpenGothic" -B "$task/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
  -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
cmake --build "$task/build" --target Gothic2Notr gothic-body-runner check-navmesh --parallel 3
"$task/.venv/bin/python" -I "$task/source/scripts/nav_dagger3_detours.py" \
  --task "$task" --corpus "$HOME/gothic-ai/nav-demos-v4"
df -h "$HOME"
PHASE
if [[ "$stage" == prepare-detours ]]; then
  "$ROOT/scripts/gq" submit --class batch --threads 12 --cpu-quota 1200% --mem 8G --max 2h \
    --name "nav dagger3 prepare detours" -- bash "$task/prepare-detours.sh"
  exit
fi
# Freezing is a small transformation of existing geometry, with no runner,
# training or policy call. Both cohorts are immutable before queue submission.
ssh ryzen "'$task/.venv/bin/python' -I '$task/source/scripts/nav_dagger3_detours.py' freeze --task '$task' --corpus '${task%/nav-dagger3}/nav-demos-v4'"
ssh ryzen "cat > '$task/baseline-detours.sh'" <<'BASELINE'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
corpus="$HOME/gothic-ai/nav-demos-v4"
exec > >(tee -a "$task/logs/baseline-detours.log") 2>&1
export UV_CACHE_DIR="$HOME/.cache/uv"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1 OPENGOTHIC_BODY_TRACE=0 PYTHONUNBUFFERED=1
python="$task/.venv/bin/python"
cd "$task/source"
"$python" -m pytest -q scripts/tests/test_nav_dagger3_detours.py scripts/tests/test_nav_rl.py
for policy in bc3 it35; do
  "$python" -I -c 'import runpy,sys; runpy.run_path(sys.argv[1])["require_room"](sys.argv[2])' \
    "$task/source/scripts/nav_dagger3_detours.py" "$task"
  case "$policy" in
    bc3) weights="$HOME/gothic-ai/nav-bc3/run/it35/policy.navbc" ;;
    it35) weights="$HOME/gothic-ai/nav-ppo/run/iter-000035/update/policy.navbc" ;;
  esac
  output="$task/detour-baseline/$policy"
  cache="$task/baseline-cache/$policy"
  mkdir -p "$cache/ordinary"
  cp -n "$corpus/recording-cache/v3/"*.navmesh "$cache/ordinary/"
  sha256sum "$weights" "$task/detours/detour-val.jsonl" "$task/detours/detour-test.jsonl"
  "$python" -I scripts/nav_rl.py drive \
    --detour-scenarios "$task/detours/detour-val.jsonl" \
    --runner "$task/build/opengothic/gothic-body-runner" \
    --assets "$HOME/gothic-ai/game" --save "$corpus/input/save.sav" \
    --cache "$cache" --links "$corpus/input/accepted-links.txt" --roads "$corpus/input/places.json" \
    --weights "$weights" --output "$output" --physics cv --parallel 8 \
    --action-mode sample --action-seeds 18102027,18102028,18102029 \
    --limit-ms 240000 --watchdog-seconds 900 --resume
  "$python" -I scripts/nav_rl.py summarize --source "$output" --output "$output"
  "$python" - "$task/detours/detour-val.jsonl" "$output/metrics.json" <<'CHECK'
import json
from pathlib import Path
import sys
cases = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
metrics = json.loads(Path(sys.argv[2]).read_text())
expected = {(case["id"], seed) for case in cases for seed in (18102027, 18102028, 18102029)}
episodes = metrics["episodes"]
if len(episodes) != len(expected) or {(ep["id"], ep["action_seed"]) for ep in episodes} != expected:
    raise ValueError("Baseline detour validation is incomplete")
if any(ep["teacher_control_decisions"] != 0 or ep["recording"] != "text_only" for ep in episodes):
    raise ValueError("Baseline control or recording differs")
print(json.dumps(dict(event="baseline_policy_complete", episodes=len(episodes), **metrics["all"])))
CHECK
done
df -h "$HOME"
BASELINE
"$ROOT/scripts/gq" submit --class batch --threads 24 --cpu-quota 2400% --mem 12G --max 2h \
  --name "nav dagger3 bc3 it35 detour baseline" -- bash "$task/baseline-detours.sh"
