#!/usr/bin/env bash
# Submit calibration or a bounded collection chain and return its queue ID.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ $# == 1 && ( "$1" == --help || "$1" == -h ) ]]; then
  echo 'Usage: bash scripts/nav-dagger4-remote.sh smoke | calibrate | collect-r1 PHASE (1..6)'
  exit 0
fi
stage="${1:-}"
phase="${2:-}"
if ! { [[ $# == 1 && ( "$stage" == smoke || "$stage" == calibrate ) ]] || [[ $# == 2 && "$stage" == collect-r1 && "$phase" =~ ^[1-6]$ ]]; }; then
  echo 'Usage: bash scripts/nav-dagger4-remote.sh smoke | calibrate | collect-r1 PHASE (1..6)' >&2
  exit 2
fi
export GQ_OWNER=nav-dagger4
task="$(ssh -o BatchMode=yes -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-dagger4" "$HOME"')"
archive=/mnt/shared/gothic-ai/nav-dagger4
if [[ "$stage" == collect-r1 || "$stage" == calibrate ]]; then
  ssh ryzen "bash -s -- '$task' '$archive' '$stage' '$phase'" <<'ROUND_INPUTS'
set -euo pipefail
task="$1"; archive="$2"; stage="$3"; phase="$4"
df -h "$HOME" /mnt/shared
test -f "$archive/smoke/passed"
test -x "$task/build/opengothic/gothic-body-runner"
test -x "$task/.venv/bin/python"
if [[ "$stage" == calibrate ]]; then
  test -f "$archive/round1/phase-1/passed"
  test ! -e "$archive/calibration/quick/passed"
  cmp "$task/input/round1-train.jsonl" "$archive/round1/round1-train.jsonl"
else
  test ! -e "$archive/round1/phase-$phase/passed"
  if (( phase == 2 )); then
    test -f "$archive/calibration/quick/passed"
    test ! -e "$archive/calibration/follow-check/passed"
    python3 - "$archive" <<'ACCEPT_CALIBRATION'
import json
from pathlib import Path
import sys
archive = Path(sys.argv[1])
metrics = json.loads((archive / "calibration/quick/control-metrics.json").read_text())
families = metrics["by_family"]
if metrics["all"]["episodes"] != 6 or len(families) != 6:
    raise ValueError("Accepted calibration must contain all six train families")
violations = {family: dict(teacher_control_fraction=value["teacher_control_fraction"],
                          limit=.7 if family == "jump" else .75 if family in ("follow", "follow_below_gate") else .5)
    for family, value in families.items()
    if value["teacher_control_fraction"] > (.7 if family == "jump" else .75 if family in ("follow", "follow_below_gate") else .5)}
if violations:
    raise ValueError(f"Accepted calibration exceeds family limits: {violations}")
(archive / "calibration/passed").write_text("User accepted calibration with jump <=70%, follow <=75%, other families <=50%; original evidence preserved\n")
print(json.dumps(dict(event="dagger4_calibration_accepted", **metrics)), flush=True)
ACCEPT_CALIBRATION
  elif (( phase >= 3 )); then
    test -f "$archive/calibration/passed"
    test -f "$archive/calibration/follow-check/passed"
  fi
fi
# One hundred full NAVDEMO episodes fit even at their uncompressed upper bound.
python3 -c 'import pathlib,shutil; assert shutil.disk_usage(pathlib.Path.home()).free >= 70*1024**3, "Collection needs 70 GiB free including its reserve"'
mkdir -p "$task/input" "$archive/round1" "$archive/logs"
ROUND_INPUTS
  rsync -rt --no-links --exclude=__pycache__ "$ROOT/scripts/" "ryzen:$task/source/scripts/"
  rsync -rt --no-links "$ROOT/OpenGothic/common/navdemos.cpp" "ryzen:$task/source/OpenGothic/common/"
  if [[ "$stage" == calibrate ]]; then
    rsync -rt --no-links "$ROOT/OpenGothic/tests/navpolicy_contract.cpp" "ryzen:$task/source/OpenGothic/tests/"
  else
  ssh ryzen "'$task/.venv/bin/python' -B -I - '$task' '${task%/*}/nav-demos-v4' '${task%/*}/nav-demos-v5' '/mnt/shared/gothic-ai/nav-demos-v5'" <<'ROUND_SELECT'
import json
from collections import Counter
from pathlib import Path
import sys

task, v4, v5, stored_v5 = map(Path, sys.argv[1:])
# All input corpora are read-only. Only this task receives a frozen selection.
sys.path.insert(0, str(task / "source/scripts"))
import nav_demos_v4_collect as collect
import nav_rl as rl

nodes = rl.read_json(v5 / "knownroads/roads-graph.json")["nodes"]
positions = collect.v3.load_start_positions(stored_v5 / "diagnostics/discovery/points-v3.jsonl")
snapshots = list(rl.rows(v5 / "knownroads/roads-snapshots.jsonl"))
recorded = {entry["id"] for entry in rl.read_json(stored_v5 / "recordings/collection-metrics.json")["episodes"]}
counts = dict(point=100, object=100, approach=100, follow_below_gate=100, jump=102, water=98)
# Native snapping maps these train routes onto too few distinct connected points.
degenerate = {"follow-d790094b6039c0bf", "follow-3c00a35e82d9bfec"}
selected = {}
for family, count in counts.items():
    pool = list(rl.rows(v5 / "recording-input" / f"{family}.jsonl"))
    if family != "water":
        old = {value["id"]: value for value in rl.rows(v4 / "recording-input" / f"{family}.jsonl")}
        if old != {value["id"]: value for value in pool}:
            raise ValueError(f"Frozen v4/v5 train pools differ: {family}")
    if any(value.get("split") != "train" for value in pool):
        raise ValueError(f"Held-out scenario in frozen train pool: {family}")
    pool = [value for value in pool if value["id"] not in degenerate]
    key = "place_id" if family == "object" else "site_id" if family == "jump" else "region"
    unused = collect.balanced([value for value in pool if value["id"] not in recorded], key, seed=23102026)
    used = collect.balanced([value for value in pool if value["id"] in recorded], key, seed=23102026)
    take_unused = min(len(unused), count // 2)
    take_used = min(len(used), count - take_unused)
    take_unused = count - take_used
    chosen = unused[:take_unused] + used[:take_used]
    if len(chosen) != count:
        raise ValueError(f"Insufficient train scenarios for {family}")
    selected[family] = []
    for original in collect.balanced(chosen, key, seed=23102026):
        value = collect.resident_scenario(original, (nodes, positions, snapshots))
        value.update(family=family, limit_ms=240000, dagger_round=1,
                     unused_in_v5=original["id"] not in recorded)
        if value["npc"] == value.get("target"):
            raise ValueError("DAgger actor and moving target must differ")
        selected[family].append(value)
# Interleave families before cutting six mixed allocations.
values = [selected[family][index] for index in range(max(counts.values()))
          for family in counts if index < len(selected[family])]
target = task / "input/round1-train.jsonl"
if target.exists():
    if list(rl.rows(target)) != values:
        raise ValueError("Existing frozen round 1 selection differs")
else:
    rl.write_pairs(target, values)
rl.dagger_scenarios(target)
if len(values) != 600 or len({value["id"] for value in values}) != 600:
    raise ValueError("Round 1 must contain 600 distinct train scenarios")
for phase in range(6):
    group = values[phase * 100:(phase + 1) * 100]
    if len(group) != 100:
        raise ValueError("Each collection phase must contain 100 scenarios")
    print(json.dumps(dict(event="dagger4_round1_selection", phase=phase + 1, episodes=len(group),
        by_family=dict(Counter(value["family"] for value in group)),
        unused_in_v5=sum(value["unused_in_v5"] for value in group))))
ROUND_SELECT
  fi
  ssh ryzen "cat > '$task/collect-r1.sh'" <<'ROUND_PHASE'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
archive=/mnt/shared/gothic-ai/nav-dagger4
phase="${1:?phase}"
test ! -e "$archive/round1/phase-$phase/passed"
if (( phase >= 2 )); then
  test -f "$archive/calibration/passed"
  test -f "$archive/calibration/follow-check/passed"
  cmp "$task/input/round1-train.jsonl" "$archive/round1/round1-train.jsonl"
fi
output="$task/round1/phase-$phase"
destination="$archive/round1/phase-$phase"
exec > >(tee -a "$archive/logs/collect-r1-$phase.log") 2>&1
export TMPDIR="$task/tmp" UV_CACHE_DIR="$HOME/.cache/uv"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export OPENGOTHIC_BODY_TRACE=0 PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
export OPENGOTHIC_NAV_JOURNEY_MEMORY=1
unset OPENGOTHIC_NAV_FAMILIAR_MAP
cd "$task/source"
python="$task/.venv/bin/python"
df -h "$HOME" /mnt/shared
"$python" -B -I -c 'import pathlib,shutil; assert shutil.disk_usage(pathlib.Path.home()).free >= 70*1024**3, "Collection needs 70 GiB free including its reserve"'
indices="$("$python" -B -I -c 'import sys; phase=int(sys.argv[1]); print(",".join(map(str,range((phase-1)*100,phase*100))))' "$phase")"
"$python" -B -I scripts/nav_rl.py drive --dagger-scenarios "$task/input/round1-train.jsonl" \
  --indices "$indices" --runner "$task/build/opengothic/gothic-body-runner" \
  --assets "$HOME/gothic-ai/game" --save "$task/input/save.sav" \
  --cache "$task/smoke-cache" --links "$task/input/accepted-links.txt" \
  --jump-links "$task/input/jump-links.txt" --roads "$task/input/places.json" \
  --weights "$archive/smoke/policy.navbc" --output "$output" --physics cv --parallel 12 \
  --teacher-beta 0.1 --action-mode sample --action-seeds 23102027 --limit-ms 240000 \
  --watchdog-seconds 900 --resume
"$python" -B -I - "$output" "$destination" "$task/input/round1-train.jsonl" <<'ROUND_ARCHIVE'
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path.cwd() / "scripts"))
import nav_rl as rl

source, target, selection = map(Path, sys.argv[1:])
for path in source.glob("dagger/*/*/seed-*/episode.json"):
    episode = rl.read_json(path)
    if episode.get("dagger_control_version") != 3:
        raise ValueError(f"Collection needs calibrated DAgger controls: {path}")
metrics = rl.dagger_control_metrics(source)
if metrics["all"]["episodes"] != 100:
    raise ValueError("Collection phase must finish all 100 selected train episodes")
rl.write_json(source / "control-metrics.json", metrics)
def hashes(root):
    result = {}
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"Symlink in collection output: {path}")
        if path.is_file():
            with path.open("rb") as stream:
                result[str(path.relative_to(root))] = hashlib.file_digest(stream, "sha256").hexdigest()
    return result
expected = hashes(source)
target.mkdir(parents=True, exist_ok=True)
subprocess.run(["rsync", "-rt", "--no-links", str(source) + "/", str(target) + "/"], check=True)
if hashes(target) != expected:
    raise ValueError("Collection archive checksum mismatch; retaining all local recordings")
frozen = target.parent / "round1-train.jsonl"
if not frozen.exists():
    shutil.copyfile(selection, frozen)
if frozen.read_bytes() != selection.read_bytes():
    raise ValueError("Archived round selection differs")
(target / "passed").write_text("100 train episodes; archive SHA256 verified\n")
shutil.rmtree(source)
print(json.dumps(dict(event="dagger4_round1_phase_complete", archive=str(target), **metrics)))
ROUND_ARCHIVE
ROUND_PHASE
  ssh ryzen "cat > '$task/chain-r1.py'" <<'ROUND_CHAIN'
#!/usr/bin/env python3
"""Run one DAgger allocation and submit its bounded continuation."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--task", type=Path, required=True)
    parser.add_argument("--phase", type=int, choices=range(1, 7), required=True)
    parser.add_argument("--enqueue", type=Path)
    args = parser.parse_args()
    task, phase = args.task, args.phase
    if sys.platform != "linux" or not task.is_absolute() or not task.is_dir():
        parser.error("Run on Ryzen with an existing absolute task directory")
    sys.path.insert(0, str(task / "source/scripts"))
    import nav_demos_v4_chain as v4
    archive = Path("/mnt/shared/gothic-ai/nav-dagger4")

    def emit(event, **values):
        content = json.dumps(dict(event=event, **values))
        print(content, flush=True)
        with (archive / "logs/collection-chain.log").open("a") as stream:
            stream.write(content + "\n")

    if args.enqueue is not None:
        identifier = None
        try:
            v4.disk_check()
            command = [str(v4.GQ), "submit", "--class", "batch", "--threads", "18",
                "--cpu-quota", "1800%", "--mem", "16G", "--max", "2h", "--name",
                f"nav dagger4 round1 collect phase {phase}/6", "--",
                str(task / ".venv/bin/python"), "-B", "-I", str(Path(__file__).resolve()),
                "--task", str(task), "--phase", str(phase)]
            result = subprocess.run(command, check=True, capture_output=True, text=True,
                                    env={**os.environ, "GQ_OWNER": "nav-dagger4"})
            identifier = result.stdout.strip()
            if not re.fullmatch(r"[0-9a-f]{12}", identifier):
                raise ValueError(f"Invalid continuation ID: {identifier!r}")
            v4.atomic_json(args.enqueue, dict(status="submitted", job=identifier))
        except Exception as error:
            if identifier and re.fullmatch(r"[0-9a-f]{12}", identifier):
                subprocess.run([str(v4.GQ), "cancel", identifier], check=False,
                               env={**os.environ, "GQ_OWNER": "nav-dagger4"})
            v4.atomic_json(args.enqueue, dict(status="failed", detail=str(error)))
            raise
        # The separate transient unit keeps gq's pump alive after its parent exits.
        result = subprocess.run([str(v4.GQ), "wait", identifier], check=False,
                                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        v4.atomic_json(args.enqueue, dict(status="finished", job=identifier,
                                         returncode=result.returncode))
        return result.returncode

    with v4.own_task(task):
        if phase >= 2 and not (archive / "calibration/passed").is_file():
            raise ValueError("Calibrated quick check must pass before phase 2–6")
        if phase >= 2 and not (archive / "calibration/follow-check/passed").is_file():
            raise ValueError("Follow-band stall quick check must pass before phase 2–6")
        subprocess.run(["bash", str(task / "collect-r1.sh"), str(phase)], check=True)
        metrics = v4.read_json(archive / f"round1/phase-{phase}/control-metrics.json")
        emit("dagger4_chain_phase_control", phase=phase, **metrics)
        violations = {family: dict(teacher_control_fraction=value["teacher_control_fraction"],
                                  limit=.7 if family == "jump" else .75 if family in ("follow", "follow_below_gate") else .5)
            for family, value in metrics["by_family"].items()
            if value["teacher_control_fraction"] > (.7 if family == "jump" else .75 if family in ("follow", "follow_below_gate") else .5)}
        if violations:
            emit("dagger4_chain_stopped", phase=phase, reason="teacher_fraction_above_family_limit",
                 violating_families=violations)
            return 0
        if phase == 6:
            emit("dagger4_chain_complete", phase=phase)
            return 0
        identifier = v4.submit_next(task, script=__file__, owner="nav-dagger4",
                                    extra=("--phase", str(phase + 1)))
        emit("dagger4_chain_continued", phase=phase, next_phase=phase + 1, next_job=identifier)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
ROUND_CHAIN
  if [[ "$stage" == calibrate ]]; then
    ssh ryzen "cat > '$task/calibrate.sh'" <<'CALIBRATE_PHASE'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
archive=/mnt/shared/gothic-ai/nav-dagger4
mkdir -p "$task/calibration" "$task/tmp" "$archive/calibration/offline" "$archive/logs"
exec > >(tee -a "$archive/logs/calibration.log") 2>&1
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export UV_CACHE_DIR="$HOME/.cache/uv" TMPDIR="$task/tmp"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export OPENGOTHIC_BODY_TRACE=0 PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
export OPENGOTHIC_NAV_JOURNEY_MEMORY=1
unset OPENGOTHIC_NAV_FAMILIAR_MAP
python="$task/.venv/bin/python"
cd "$task/source"
test -f "$archive/round1/phase-1/passed"
test ! -e "$archive/calibration/quick/passed"
cmp "$task/input/round1-train.jsonl" "$archive/round1/round1-train.jsonl"
cmake --build "$task/build" --target gothic-body-runner gothic-navpolicy-contract --parallel 3
"$python" -B -I scripts/nav_rl.py dagger-calibrate \
  --source "$archive/round1/phase-1" --output "$archive/calibration/offline" \
  --weights "$archive/smoke/policy.navbc" --runner "$task/build/opengothic/gothic-navpolicy-contract"
"$python" -B -I - "$archive/calibration/offline/calibration.json" "$task/input/round1-train.jsonl" <<'CALIBRATE_OFFLINE'
import json
from pathlib import Path
import sys
result = json.loads(Path(sys.argv[1]).read_text())
baseline = result["baseline"]["all"]
if not result["approximate"] or baseline["episodes"] != 100 or baseline["decisions"] != 102845:
    raise ValueError("Calibration must replay the complete, unchanged phase 1")
verification = result["native_replay_verification"]
if verification["mismatching_learner_steps"] or verification["compared_learner_steps"] != baseline["decisions"] - baseline["teacher_control_decisions"]:
    raise ValueError("Native pre-mix replay must match every recorded learner step")
selection = [json.loads(line) for line in Path(sys.argv[2]).read_text().splitlines()]
if {value["family"] for value in selection[:6]} != {"point", "object", "approach", "follow_below_gate", "jump", "water"}:
    raise ValueError("Calibration quick must cover six distinct frozen train families")
if any(value["split"] != "train" or value["limit_ms"] != 240000 for value in selection[:6]):
    raise ValueError("Calibration quick must use full train episodes")
print(json.dumps(dict(event="dagger4_offline_threshold_estimate", approximate=True,
    note="Fixed phase-1 trajectories; changed interventions would change the visited states.",
    **result["estimate"])), flush=True)
CALIBRATE_OFFLINE
"$task/build/opengothic/gothic-navpolicy-contract" --memory "$archive/smoke/policy.navbc"
"$python" -B -I scripts/nav_rl.py drive --dagger-scenarios "$task/input/round1-train.jsonl" \
  --indices 0,1,2,3,4,5 --runner "$task/build/opengothic/gothic-body-runner" \
  --assets "$HOME/gothic-ai/game" --save "$task/input/save.sav" \
  --cache "$task/smoke-cache" --links "$task/input/accepted-links.txt" \
  --jump-links "$task/input/jump-links.txt" --roads "$task/input/places.json" \
  --weights "$archive/smoke/policy.navbc" --output "$task/calibration/quick" --physics cv --parallel 6 \
  --teacher-beta 0.1 --action-mode sample --action-seeds 23102027 --limit-ms 240000 \
  --watchdog-seconds 900 --resume
"$python" -B -I - "$task" "$archive" <<'CALIBRATE_VERIFY'
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
task, archive = map(Path, sys.argv[1:])
sys.path.insert(0, str(task / "source/scripts"))
import nav_rl as rl
source, target = task / "calibration/quick", archive / "calibration/quick"
selection = list(rl.rows(task / "input/round1-train.jsonl"))[:6]
expected = {value["id"]: value for value in selection}
episodes = list(source.glob("dagger/*/*/seed-*/episode.json"))
if len(episodes) != 6:
    raise ValueError(f"Calibration quick needs six episodes, found {len(episodes)}")
for path in episodes:
    value = rl.read_json(path)
    if value["id"] not in expected:
        raise ValueError(f"Unexpected calibration scenario: {path}")
    scenario = expected.pop(value["id"])
    if (value.get("dagger_control_version") != 3 or value.get("navigation_memory_version") != 4
            or value.get("teacher_beta") != .1 or value.get("action_mode") != "sample"
            or value.get("drive") is not True or value.get("family") != scenario["family"]
            or value.get("dagger_scenario") != scenario or value.get("sampling_seed") != 23102027
            or value.get("teacher_version") != (2 if scenario["family"] == "jump" else 3)):
        raise ValueError(f"Calibration learner controls differ: {path}")
    steps = list(rl.rows(path.parent / "samples.jsonl"))
    if not steps or len(steps) != value["decisions"]:
        raise ValueError(f"Calibration per-step counters differ: {path}")
if expected:
    raise ValueError("Calibration quick did not finish every selected episode")
metrics = rl.dagger_control_metrics(source)
rl.write_json(source / "control-metrics.json", metrics)
def hashes(root):
    result = {}
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"Symlink in calibration output: {path}")
        if path.is_file():
            with path.open("rb") as stream:
                result[str(path.relative_to(root))] = hashlib.file_digest(stream, "sha256").hexdigest()
    return result
expected_hashes = hashes(source)
target.mkdir(parents=True, exist_ok=True)
subprocess.run(["rsync", "-rt", "--no-links", str(source) + "/", str(target) + "/"], check=True)
if hashes(target) != expected_hashes:
    raise ValueError("Calibration archive checksum mismatch; retaining local recordings")
(target / "passed").write_text("6 full train episodes; controls v3; memory4; SHA256 verified\n")
shutil.rmtree(source)
print(json.dumps(dict(event="dagger4_calibration_quick_complete", archive=str(target), **metrics)), flush=True)
violations = {family: dict(teacher_control_fraction=value["teacher_control_fraction"],
                          limit=.7 if family == "jump" else .75 if family in ("follow", "follow_below_gate") else .5)
    for family, value in metrics["by_family"].items()
    if value["teacher_control_fraction"] > (.7 if family == "jump" else .75 if family in ("follow", "follow_below_gate") else .5)}
if violations:
    print(json.dumps(dict(event="dagger4_calibration_stopped", reason="teacher_fraction_above_family_limit",
                          violating_families=violations)), flush=True)
    raise SystemExit(0)
(archive / "calibration/passed").write_text("6 train episodes; jump <=70%, follow <=75%, other families <=50%\n")
print(json.dumps(dict(event="dagger4_calibration_passed", next_action="bash scripts/nav-dagger4-remote.sh collect-r1 2")), flush=True)
CALIBRATE_VERIFY
CALIBRATE_PHASE
    "$ROOT/scripts/gq" submit --class quick --threads 12 --cpu-quota 1200% --mem 16G --max 45m \
      --name 'nav dagger4 threshold replay and six-family quick' -- bash "$task/calibrate.sh"
    exit
  fi
  if [[ "$phase" == 2 ]]; then
    ssh ryzen "cat > '$task/follow-check.sh'" <<'FOLLOW_CHECK_PHASE'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
archive=/mnt/shared/gothic-ai/nav-dagger4
mkdir -p "$task/tmp" "$archive/logs"
exec > >(tee -a "$archive/logs/follow-check.log") 2>&1
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export UV_CACHE_DIR="$HOME/.cache/uv" TMPDIR="$task/tmp"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export OPENGOTHIC_BODY_TRACE=0 PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
export OPENGOTHIC_NAV_JOURNEY_MEMORY=1
unset OPENGOTHIC_NAV_FAMILIAR_MAP
python="$task/.venv/bin/python"
cd "$task/source"
test -f "$archive/calibration/passed"
test ! -e "$archive/calibration/follow-check/passed"
cmp "$task/input/round1-train.jsonl" "$archive/round1/round1-train.jsonl"
cmake --build "$task/build" --target gothic-body-runner --parallel 3
"$python" -B -m pytest -q -p no:cacheprovider --basetemp "$task/tmp/pytest-follow-check" \
  scripts/tests/test_nav_rl.py -k dagger
"$python" -B -I scripts/nav_rl.py drive --dagger-scenarios "$task/input/round1-train.jsonl" \
  --indices 27,87 --runner "$task/build/opengothic/gothic-body-runner" \
  --assets "$HOME/gothic-ai/game" --save "$task/input/save.sav" \
  --cache "$task/smoke-cache" --links "$task/input/accepted-links.txt" \
  --jump-links "$task/input/jump-links.txt" --roads "$task/input/places.json" \
  --weights "$archive/smoke/policy.navbc" --output "$task/calibration/follow-check" --physics cv --parallel 2 \
  --teacher-beta 0.1 --action-mode sample --action-seeds 23102027 --limit-ms 240000 \
  --watchdog-seconds 900 --resume
"$python" -B -I - "$task" "$archive" <<'FOLLOW_CHECK_VERIFY'
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
task, archive = map(Path, sys.argv[1:])
sys.path.insert(0, str(task / "source/scripts"))
import nav_rl as rl
source, target = task / "calibration/follow-check", archive / "calibration/follow-check"
selection = list(rl.rows(task / "input/round1-train.jsonl"))
expected = {selection[index]["id"]: selection[index] for index in (27, 87)}
if any(value["family"] not in ("follow", "follow_below_gate") or value["split"] != "train"
       or value["limit_ms"] != 240000 for value in expected.values()):
    raise ValueError("Follow stall quick needs the two frozen full train follow episodes")
episodes = list(source.glob("dagger/*/*/seed-*/episode.json"))
if len(episodes) != 2:
    raise ValueError(f"Follow stall quick needs two episodes, found {len(episodes)}")
band_decisions = 0
for path in episodes:
    value = rl.read_json(path)
    if value["id"] not in expected:
        raise ValueError(f"Unexpected follow stall scenario: {path}")
    scenario = expected.pop(value["id"])
    if (value.get("dagger_control_version") != 3 or value.get("navigation_memory_version") != 4
            or value.get("teacher_beta") != .1 or value.get("action_mode") != "sample"
            or value.get("drive") is not True or value.get("family") != scenario["family"]
            or value.get("dagger_scenario") != scenario or value.get("sampling_seed") != 23102027
            or value.get("teacher_version") != 3):
        raise ValueError(f"Follow stall learner controls differ: {path}")
    steps = list(rl.rows(path.parent / "samples.jsonl"))
    if not steps or len(steps) != value["decisions"]:
        raise ValueError(f"Follow stall per-step counters differ: {path}")
    episode_band = 0
    for sample in steps:
        if type(sample["follow_band"]) is not bool or type(sample["stall"]) is not bool:
            raise ValueError(f"Missing follow stall diagnostics: {path}")
        distance = float(sample["target_distance_cm"])
        if (min(abs(distance - 200), abs(distance - 500)) >= .001
                and sample["follow_band"] != (200 <= distance <= 500)):
            raise ValueError(f"Follow band disagrees with physical target distance: {path}")
        if sample["follow_band"]:
            episode_band += 1
            if sample["stall"] or sample["trigger"] == "stall":
                raise ValueError(f"Follow stall intervention inside the target band: {path}")
    band_decisions += episode_band
    print(json.dumps(dict(event="dagger4_follow_stall_episode", id=value["id"],
        decisions=len(steps), in_band_decisions=episode_band,
        teacher_control_fraction=value["teacher_control_decisions"] / len(steps))), flush=True)
if expected or not band_decisions:
    raise ValueError("Follow stall quick must finish both episodes and visit the target band")
metrics = rl.dagger_control_metrics(source)
rl.write_json(source / "control-metrics.json", metrics)
def hashes(root):
    result = {}
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"Symlink in follow stall quick output: {path}")
        if path.is_file():
            with path.open("rb") as stream:
                result[str(path.relative_to(root))] = hashlib.file_digest(stream, "sha256").hexdigest()
    return result
expected_hashes = hashes(source)
target.mkdir(parents=True, exist_ok=True)
subprocess.run(["rsync", "-rt", "--no-links", str(source) + "/", str(target) + "/"], check=True)
if hashes(target) != expected_hashes:
    raise ValueError("Follow stall archive checksum mismatch; retaining local recordings")
(target / "passed").write_text(f"2 full train episodes; controls v3; {band_decisions} in-band decisions without stall; SHA256 verified\n")
shutil.rmtree(source)
print(json.dumps(dict(event="dagger4_follow_stall_passed", in_band_decisions=band_decisions, **metrics)), flush=True)
import nav_demos_v4_chain as v4
identifier = v4.submit_next(task, script=task / "chain-r1.py", owner="nav-dagger4", extra=("--phase", "2"))
print(json.dumps(dict(event="dagger4_follow_stall_continued", next_phase=2, next_job=identifier)), flush=True)
FOLLOW_CHECK_VERIFY
FOLLOW_CHECK_PHASE
    "$ROOT/scripts/gq" submit --class quick --threads 6 --cpu-quota 600% --mem 12G --max 25m \
      --name 'nav dagger4 follow-band stall quick before phase 2' -- bash "$task/follow-check.sh"
    exit
  fi
  "$ROOT/scripts/gq" submit --class batch --threads 18 --cpu-quota 1800% --mem 16G --max 2h \
    --name "nav dagger4 round1 collect phase $phase/6" -- "$task/.venv/bin/python" -B -I "$task/chain-r1.py" --task "$task" --phase "$phase"
  exit
fi
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
