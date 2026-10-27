#!/usr/bin/env bash
# Submit one phase and return its queue ID without waiting or chaining jobs.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ $# == 1 && ( "$1" == --help || "$1" == -h ) ]]; then
  echo 'Usage: bash scripts/nav-bc4-remote.sh smoke|train|validate-main|validate-detour|validate-follow|validate-jump|validate-safety|diagnose-quick|diagnose-data|diagnose-memory|diagnose-familiar'
  exit 0
fi
[[ $# == 1 ]] || { echo 'Usage: bash scripts/nav-bc4-remote.sh smoke|train|validate-main|validate-detour|validate-follow|validate-jump|validate-safety|diagnose-quick|diagnose-data|diagnose-memory|diagnose-familiar' >&2; exit 2; }
stage="$1"
case "$stage" in smoke|train|validate-main|validate-detour|validate-follow|validate-jump|validate-safety|diagnose-quick|diagnose-data|diagnose-memory|diagnose-familiar) ;; *) echo "Unknown phase: $stage" >&2; exit 2 ;; esac
export GQ_OWNER=nav-bc4
task="$(ssh -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-bc4" "$HOME"')"
archive=/mnt/shared/gothic-ai/nav-bc4
ssh ryzen "bash -s -- '$task' '$archive' '$stage'" <<'INPUTS'
set -euo pipefail
task="$1"; archive="$2"; stage="$3"
df -h "$HOME" /mnt/shared
python3 -c 'import pathlib,shutil; assert shutil.disk_usage(pathlib.Path.home()).free >= 22*1024**3, "Less than 22 GiB free"'
mkdir -p "$task/source" "$task/input/hd" "$archive/logs"
# Other experiments and v5 are read-only input sources.
copy_input() { test -f "$2" || cp "$1" "$2"; }
for name in last.pt policy.navbc; do
  copy_input "$HOME/gothic-ai/nav-dagger3/hd-run/$name" "$task/input/hd/$name"
done
for name in save.sav places.json accepted-links.txt familiar.bin; do
  copy_input "$HOME/gothic-ai/nav-demos-v5/input/$name" "$task/input/$name"
done
if [[ "$stage" == validate-* || "$stage" == diagnose-quick || "$stage" == diagnose-memory || "$stage" == diagnose-familiar ]]; then
  for name in jump-links.txt object-probes.jsonl fixtures.json detour-val.jsonl; do
    copy_input "$HOME/gothic-ai/nav-demos-v5/input/$name" "$task/input/$name"
  done
  modes=(ordinary)
  if [[ "$stage" == validate-main || "$stage" == validate-jump ]]; then modes+=(jump); fi
  for mode in "${modes[@]}"; do
    if [[ "$stage" == validate-detour || "$stage" == diagnose-memory || "$stage" == diagnose-familiar ]]; then
      destination="$task/validation-detour-cache/$mode"
    elif [[ "$stage" == diagnose-quick ]]; then
      destination="$task/smoke-cache"
    else
      destination="$task/validation-main-cache/$mode"
    fi
    mkdir -p "$destination"
    for source in "$HOME/gothic-ai/nav-dagger3/hd-measure-cache/$mode/"*.navmesh; do
      copy_input "$source" "$destination/$(basename "$source")"
    done
  done
fi
if [[ "$stage" == validate-* && "$stage" != validate-detour ]]; then
  mkdir -p "$archive/validation/setup-rejections"
  for source in "$HOME/gothic-ai/nav-dagger3/hd-validation/setup-rejections/"*.json; do
    copy_input "$source" "$archive/validation/setup-rejections/$(basename "$source")"
  done
fi
INPUTS
rsync -a --no-links --exclude=build --exclude='build-*' --exclude=__pycache__ --exclude=.git \
  "$ROOT/OpenGothic" "$ROOT/scripts" "$ROOT/AGENTS.md" "ryzen:$task/source/"
rsync -rt --no-links "$ROOT/reports/navigation/2026-10-22-demos-v5/manifest.json" \
  "ryzen:$task/input/v5-compact.json"
ssh ryzen "cat > '$task/phase.sh'" <<'PHASE'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
archive=/mnt/shared/gothic-ai/nav-bc4
corpus=/mnt/shared/gothic-ai/nav-demos-v5/recordings
stage="${1:?phase}"
exec > >(tee -a "$archive/logs/$stage.log") 2>&1
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export UV_CACHE_DIR="$HOME/.cache/uv"
export OMP_NUM_THREADS=6 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export OPENGOTHIC_BODY_TRACE=0 PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
# The policy metadata selects memory; the teacher's familiar shortcut stays off.
unset OPENGOTHIC_NAV_JOURNEY_MEMORY OPENGOTHIC_NAV_FAMILIAR_MAP
cd "$task/source"
df -h "$HOME" /mnt/shared
python3 -c 'import pathlib,shutil; assert shutil.disk_usage(pathlib.Path.home()).free >= 22*1024**3, "Less than 22 GiB free"'
python="$task/.venv/bin/python"
if [[ "$stage" == smoke ]]; then
  rm -f -- "$archive/smoke/passed"
  source "$task/source/scripts/ryzen-venv.sh"
  ryzen_task_venv "$task/.venv"
  "$python" -B -m pytest -q -p no:cacheprovider \
    scripts/tests/test_nav_bc_data.py scripts/tests/test_nav_bc_model.py \
    scripts/tests/test_nav_bc_train.py scripts/tests/test_nav_bc_contract.py scripts/tests/test_nav_rl.py
  cmake -S "$task/source/OpenGothic" -B "$task/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
    -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
  cmake --build "$task/build" --target gothic-body-runner gothic-navpolicy-contract --parallel 3
  for version in 1 2 3; do
    demo=2
    if [[ "$version" == 1 ]]; then demo=1; fi
    "$python" -B -I scripts/check_nav_policy.py --weight-version "$version" --navdemo-version "$demo" \
      --output "$archive/smoke/legacy-v$version" --steps 8
    "$task/build/opengothic/gothic-navpolicy-contract" \
      "$archive/smoke/legacy-v$version/fixture.navbc" "$archive/smoke/legacy-v$version/parity.bin.gz"
  done
  "$python" -B -I - "$task" "$archive" "$corpus" <<'INITIALIZE'
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path.cwd() / "scripts"))
import nav_bc_data as data
import nav_demos as demos
from nav_bc_model import NavPolicy, export_weights
from nav_bc_train import initialize_bc3_hd
task, archive, corpus = map(Path, sys.argv[1:])
manifest, entries = data.load_training_manifest(task / "input/v5-compact.json", corpus)
if len(entries) != 700 or sum(entry["decisions"] for entry in entries) != 405599:
    raise ValueError("V5 training cohort differs from 700 episodes / 405599 decisions")
layout = demos.ray_layout(entries[0]["directory"])
model = NavPolicy(data.VECTOR_SIZE, format_version=4, auxiliary=True)
initialization = initialize_bc3_hd(model, task / "input/hd/last.pt", dict(eye_layout=layout.tolist()))
header = export_weights(model, archive / "smoke/initial.navbc",
                        dict(data.BC4_PREPROCESSING, eye_layout=layout.tolist()))
print(json.dumps(dict(event="bc4_initialization", version=header["version"],
    memory_version=header["preprocessing"]["navigation_memory_version"], **initialization)))
INITIALIZE
  "$python" -B -I scripts/check_nav_policy.py --weights "$archive/smoke/initial.navbc" \
    --source "$corpus" --output "$archive/smoke/parity" --steps 1000 --torch
  "$task/build/opengothic/gothic-navpolicy-contract" \
    "$archive/smoke/initial.navbc" "$archive/smoke/parity/parity.bin.gz"
  "$task/build/opengothic/gothic-navpolicy-contract" --memory "$archive/smoke/initial.navbc"
  "$python" -B -I - "$task" "$archive" "$corpus" <<'DRIVE'
import json
from pathlib import Path
import sys
from types import SimpleNamespace
sys.path.insert(0, str(Path.cwd() / "scripts"))
import nav_bc_data as data
import nav_rl as rl
task, archive, corpus = map(Path, sys.argv[1:])
_, entries = data.load_training_manifest(task / "input/v5-compact.json", corpus)
entries = [entry for entry in entries if entry["family"] == "point"][:2]
args = SimpleNamespace(output=archive / "smoke/drive", cache=task / "smoke-cache",
    runner=task / "build/opengothic/gothic-body-runner", assets=Path.home() / "gothic-ai/game",
    save=task / "input/save.sav", links=task / "input/accepted-links.txt", roads=task / "input/places.json",
    weights=archive / "smoke/initial.navbc", familiar_map=task / "input/familiar.bin",
    ini=None, physics="cv", seed=17, watchdog_seconds=900)
rl.checked_paths(args, policy=True)
runner = rl.new_runner(args, "bc4-smoke")
try:
    for index, entry in enumerate(entries):
        value = entry["scenario"]
        npc = value["npc"]
        residency = "on" if value["local"] else "stranger"
        commands = [f"roads load {args.roads}", f"roads familiar-load {args.familiar_map}",
            f"roads {residency} {npc}", "navdemos record off",
            "navdemos perception-mode gaze", "navdemos oracle off",
            f"navdemos drive {npc} {value['start']} {value['goal']} 10000 {args.weights} 0 18102027 sample",
            "navdemos wait"]
        directory = args.output / str(index)
        rl.scenario(args, runner, f"bc4-smoke-{index}", commands, directory)
        ep = rl.read_json(directory / "episode.json")
        if (ep["demo_version"] != 3 or ep["navigation_memory_version"] != 4
                or not ep["journey_memory"] or ep["record"] or ep["teacher_control_decisions"] != 0
                or ep["action_mode"] != "sample" or ep["decisions"] <= 0
                or ep["familiar_map"] != value["local"]):
            raise ValueError(f"BC4 drive identity differs: {directory}")
        print(json.dumps(dict(event="bc4_smoke_episode", id=entry["id"],
            decisions=ep["decisions"], memory_version=ep["navigation_memory_version"])))
finally:
    runner.close()
(archive / "smoke/passed").write_text("BC4 contracts and two train drive episodes passed\n")
DRIVE
elif [[ "$stage" == train ]]; then
  test -f "$archive/smoke/passed"
  test -x "$python"
  test ! -e "$archive/bc4/last.pt"
  nvidia-smi
  "$python" -B -I scripts/nav_bc_data.py prepare --source "$corpus" \
    --manifest "$task/input/v5-compact.json" --output "$task/cache" --input-variant hd-familiar
  "$python" -B -I scripts/nav_bc_train.py train --cache "$task/cache" --output "$archive/bc4" \
    --init "$task/input/hd/last.pt" --epochs 20 --batch 8 --segment 64 \
    --lr .0003 --seed 17 --device cuda --threads 6
  "$python" -B -I scripts/check_nav_policy.py --weights "$archive/bc4/policy.navbc" \
    --source "$corpus" --output "$archive/bc4/parity" --steps 1000 --torch
  "$task/build/opengothic/gothic-navpolicy-contract" \
    "$archive/bc4/policy.navbc" "$archive/bc4/parity/parity.bin.gz"
  rm -rf -- "$task/cache"
elif [[ "$stage" == diagnose-quick ]]; then
  rm -f -- "$archive/diagnostics/smoke-passed"
  "$python" -B -m pytest -q -p no:cacheprovider scripts/tests/test_nav_rl.py
  cmake --build "$task/build" --target gothic-body-runner --parallel 3
  "$python" -B -I - "$task" "$archive" <<'DIAGNOSE'
import json
import os
from pathlib import Path
import sys
from types import SimpleNamespace
sys.path.insert(0, str(Path.cwd() / "scripts"))
import nav_rl as rl
task, archive = map(Path, sys.argv[1:])
value = dict(rl.detour_scenarios(task / "input/detour-val.jsonl")[0], limit_ms=10000)
cases = [("bc4-memory3-stranger", archive / "bc4/policy.navbc", "0", True, 3),
         ("hd-memory4-local", task / "input/hd/policy.navbc", "1", False, 4)]
try:
    for name, weights, override, stranger, memory in cases:
        os.environ["OPENGOTHIC_NAV_JOURNEY_MEMORY"] = override
        args = SimpleNamespace(output=archive / "diagnostics/smoke" / name,
            cache=task / "smoke-cache", runner=task / "build/opengothic/gothic-body-runner",
            assets=Path.home() / "gothic-ai/game", save=task / "input/save.sav",
            links=task / "input/accepted-links.txt", roads=task / "input/places.json",
            familiar_map=task / "input/familiar.bin", weights=weights, npc="BAU_954_MALETH",
            ini=None, physics="cv", seed=17, watchdog_seconds=900, lean=True,
            limit_ms=10000, stranger=stranger)
        rl.checked_paths(args, policy=True)
        runner = rl.new_runner(args, name)
        try:
            directory = args.output / "episode"
            rl.scenario(args, runner, name, rl.validation_commands(args, value, 18102027), directory)
            episode = rl.read_json(directory / "episode.json")
            if (episode["navigation_memory_version"] != memory or episode["demo_version"] != 3
                    or episode["record"] is not False or episode["teacher_beta"] != 0
                    or episode["teacher_control_decisions"] != 0 or episode["action_mode"] != "sample"
                    or episode["decisions"] <= 0 or any(directory.glob("samples.bin*"))):
                raise ValueError(f"BC4 diagnostic drive identity differs: {directory}")
            if name.startswith("bc4") and (
                    episode["familiar_input_decisions"] != episode["decisions"]
                    or episode["familiar_input_nonzero_decisions"] != 0):
                raise ValueError("Memory-3 BC4 diagnostic must receive an all-zero familiar channel")
            print(json.dumps(dict(event="bc4_diagnostic_smoke", case=name,
                decisions=episode["decisions"], memory_version=memory,
                **{key: episode[key] for key in ("familiar_input_decisions",
                    "familiar_input_nonzero_decisions") if key in episode})))
        finally:
            runner.close()
finally:
    os.environ.pop("OPENGOTHIC_NAV_JOURNEY_MEMORY", None)
(archive / "diagnostics/smoke-passed").write_text("Memory override and residency diagnostics passed\n")
DIAGNOSE
elif [[ "$stage" == diagnose-data ]]; then
  "$python" -B -I scripts/nav_bc4_diagnose.py outcomes --task "$task" --archive "$archive"
  "$python" -B -I scripts/nav_bc4_diagnose.py corpus --task "$task" --archive "$archive"
elif [[ "$stage" == diagnose-memory || "$stage" == diagnose-familiar ]]; then
  test -f "$archive/diagnostics/smoke-passed"
  if [[ "$stage" == diagnose-memory ]]; then
    cases=("bc4-memory3|$archive/bc4/policy.navbc|0|off|local"
      "hd-memory4|$task/input/hd/policy.navbc|1|off|local")
  else
    cases=("bc4-memory4-no-familiar|$archive/bc4/policy.navbc|1|off|local"
      "bc4-memory4-local|$archive/bc4/policy.navbc|1|on|local"
      "bc4-memory4-stranger|$archive/bc4/policy.navbc|1|on|stranger")
  fi
  for row in "${cases[@]}"; do
    IFS='|' read -r name weights journey atlas residency <<< "$row"
    export OPENGOTHIC_NAV_JOURNEY_MEMORY="$journey"
    if [[ "$atlas" == off ]]; then
      export OPENGOTHIC_NAV_FAMILIAR_MAP=0
    else
      unset OPENGOTHIC_NAV_FAMILIAR_MAP
    fi
    selection=()
    if [[ "$residency" == stranger ]]; then selection=(--stranger); fi
    output="$archive/diagnostics/detour/$name"
    "$python" -B -I scripts/nav_rl.py drive --detour-scenarios "$task/input/detour-val.jsonl" \
      --runner "$task/build/opengothic/gothic-body-runner" \
      --assets "$HOME/gothic-ai/game" --save "$task/input/save.sav" \
      --cache "$task/validation-detour-cache" --links "$task/input/accepted-links.txt" \
      --roads "$task/input/places.json" --familiar-map "$task/input/familiar.bin" \
      --weights "$weights" --output "$output" --physics cv --parallel 18 \
      --action-mode sample --action-seeds 18102027 --limit-ms 240000 \
      --watchdog-seconds 900 --resume "${selection[@]}"
    "$python" -B -I scripts/nav_rl.py summarize --source "$output" --output "$output"
    "$python" -B -I - "$name" "$output" "$journey" "$atlas" "$residency" <<'CONTROL'
import json
from pathlib import Path
import sys
name, root, journey, atlas, residency = sys.argv[1:]
root = Path(root)
memory = 4 if journey == "1" else 3
reference = json.loads((Path.home() / "gothic-ai/nav-dagger3/hd-detour/metrics.json").read_text())
expected = {(ep["id"], ep["action_seed"]) for ep in reference["episodes"] if ep["action_seed"] == 18102027}
result = json.loads((root / "metrics.json").read_text())
episodes = result["episodes"]
identity = {(ep["id"], ep["action_seed"]) for ep in episodes}
if len(episodes) != 121 or len(expected) != 121 or identity != expected:
    raise ValueError(f"BC4 detour control cohort differs: {name}")
if list(root.rglob("samples.bin*")):
    raise ValueError(f"BC4 detour control wrote NAVDEMO: {name}")
decisions = 0
counts = {key: 0 for key in ("familiar_input_decisions", "familiar_input_nonzero_decisions",
    "familiar_near_input_nonzero_decisions", "familiar_far_input_nonzero_decisions")}
for measured in episodes:
    episode = json.loads((Path(measured["episode"]) / "episode.json").read_text())
    if (episode["navigation_memory_version"] != memory or episode["demo_version"] != 3
            or episode["teacher_control_decisions"] != 0 or episode["teacher_beta"] != 0
            or episode["record"] is not False or episode["action_mode"] != "sample"
            or measured["teacher_control_decisions"] != 0 or measured["recording"] != "text_only"):
        raise ValueError(f"BC4 detour control identity differs: {name}/{measured['id']}")
    decisions += episode["decisions"]
    if name.startswith("bc4"):
        for key in counts:
            counts[key] += episode[key]
if name.startswith("bc4"):
    if counts["familiar_input_decisions"] != decisions:
        raise ValueError(f"BC4 familiar input decision count differs: {name}")
    if (atlas == "off" or residency == "stranger") and counts["familiar_input_nonzero_decisions"] != 0:
        raise ValueError(f"BC4 disabled familiar channel contains nonzero input: {name}")
else:
    counts = {}
print(json.dumps(dict(event="bc4_detour_control_complete", case=name, episodes=len(episodes),
    arrived=result["all"]["arrived"], decisions=decisions, memory_version=memory,
    atlas=atlas, residency=residency, **counts)))
CONTROL
  done
  unset OPENGOTHIC_NAV_JOURNEY_MEMORY OPENGOTHIC_NAV_FAMILIAR_MAP
elif [[ "$stage" == validate-* ]]; then
  test -f "$archive/bc4/policy.navbc"
  test -x "$python"
  test -x "$task/build/opengothic/gothic-body-runner"
  if [[ "$stage" != validate-detour ]]; then
    output="$archive/validation/bc4"
    selection=()
    case "$stage" in
      validate-main) selection=(--jump-links "$task/input/jump-links.txt"
        --safety-fixtures "$task/input/fixtures.json" --fence-control) ;;
      validate-follow) selection=(--families follow_below_gate) ;;
      validate-jump) selection=(--families jump --jump-links "$task/input/jump-links.txt") ;;
      validate-safety) selection=(--families point --indices 0
        --safety-fixtures "$task/input/fixtures.json" --fence-control) ;;
    esac
    "$python" -B -I scripts/nav_rl.py drive \
      --manifest /mnt/shared/gothic-ai/nav-demos-v5/manifest.json --lean \
      --runner "$task/build/opengothic/gothic-body-runner" \
      --assets "$HOME/gothic-ai/game" --save "$task/input/save.sav" \
      --cache "$task/validation-main-cache" --links "$task/input/accepted-links.txt" \
      --roads "$task/input/places.json" \
      --familiar-map "$task/input/familiar.bin" --weights "$archive/bc4/policy.navbc" \
      --output "$output" --physics cv --parallel 18 \
      --action-mode sample --action-seeds 18102027,18102028,18102029 \
      --limit-ms 30000 --watchdog-seconds 900 --resume "${selection[@]}"
    "$python" -B -I scripts/nav_rl.py summarize --source "$output" --output "$output" \
      --object-probes "$task/input/object-probes.jsonl"
  else
    output="$archive/detour-validation/bc4"
    "$python" -B -I scripts/nav_rl.py drive --detour-scenarios "$task/input/detour-val.jsonl" \
      --runner "$task/build/opengothic/gothic-body-runner" \
      --assets "$HOME/gothic-ai/game" --save "$task/input/save.sav" \
      --cache "$task/validation-detour-cache" --links "$task/input/accepted-links.txt" \
      --roads "$task/input/places.json" --familiar-map "$task/input/familiar.bin" \
      --weights "$archive/bc4/policy.navbc" --output "$output" --physics cv --parallel 18 \
      --action-mode sample --action-seeds 18102027,18102028,18102029 \
      --limit-ms 240000 --watchdog-seconds 900 --resume
    "$python" -B -I scripts/nav_rl.py summarize --source "$output" --output "$output"
  fi
  "$python" -B -I - "$stage" "$output" <<'COHORT'
import json
from pathlib import Path
import sys
stage, root = sys.argv[1], Path(sys.argv[2])
baseline = Path.home() / "gothic-ai/nav-dagger3"
references = ([baseline / "hd-validation" / name / "metrics.json" for name in ("bc3", "hd")]
              if stage != "validate-detour" else [baseline / "hd-detour/metrics.json"])
family, expected = {"validate-main": (None, 1491), "validate-detour": ("detour", 363),
    "validate-follow": ("follow_below_gate", 306), "validate-jump": ("jump", 135),
    "validate-safety": ("safety", 30)}[stage]
selected = lambda values: [ep for ep in values if family is None or ep["family"] == family]
result = json.loads((root / "metrics.json").read_text())
episodes = selected(result["episodes"])
identity = lambda values: {(ep["id"], ep["action_seed"]) for ep in values}
if len(episodes) != expected or len(identity(episodes)) != expected:
    raise ValueError(f"BC4 validation cohort size differs: {len(episodes)} != {expected}")
for reference in references:
    original = json.loads(reference.read_text())
    if identity(episodes) != identity(selected(original["episodes"])):
        raise ValueError(f"BC4 validation cohort differs from {reference}")
if stage != "validate-detour" and len(result["setup_rejections"]) != 2:
    raise ValueError("BC4 validation requires the two frozen moving-target setup rejections")
if any(ep["teacher_control_decisions"] != 0 or ep["recording"] != "text_only" for ep in episodes):
    raise ValueError("BC4 validation used teacher control or NAVDEMO")
if any(path.name.startswith("samples.bin") for path in root.rglob("samples.bin*")):
    raise ValueError("BC4 validation wrote NAVDEMO")
for episode in episodes:
    metadata = json.loads((Path(episode["episode"]) / "episode.json").read_text())
    if (metadata["navigation_memory_version"] != 4 or metadata["demo_version"] != 3
            or metadata["observation_source"] != "learner_memory" or metadata["record"] is not False):
        raise ValueError(f"BC4 validation memory/recording identity differs: {episode['id']}")
print(json.dumps(dict(event="bc4_validation_complete", phase=stage, episodes=len(episodes),
                      arrived=sum(bool(ep["success"]) for ep in episodes), memory_version=4)))
COHORT
else
  echo "Unknown phase: $stage" >&2; exit 2
fi
df -h "$HOME" /mnt/shared
PHASE
if [[ "$stage" == smoke ]]; then
  "$ROOT/scripts/gq" submit --class quick --threads 12 --cpu-quota 1200% --mem 12G --max 45m \
    --name 'nav bc4 format and two train smoke episodes' -- bash "$task/phase.sh" smoke
elif [[ "$stage" == train ]]; then
  "$ROOT/scripts/gq" submit --class batch --threads 6 --cpu-quota 600% --mem 10G --gpu-mem 4G --max 2h \
    --name 'nav bc4 v5 training and recorded parity' -- bash "$task/phase.sh" train
elif [[ "$stage" == diagnose-quick ]]; then
  "$ROOT/scripts/gq" submit --class quick --threads 12 --cpu-quota 1200% --mem 12G --max 45m \
    --name 'nav bc4 diagnostic overrides and two train-free smoke episodes' -- bash "$task/phase.sh" diagnose-quick
elif [[ "$stage" == diagnose-data ]]; then
  "$ROOT/scripts/gq" submit --class batch --threads 6 --cpu-quota 600% --mem 4G --max 2h \
    --name 'nav bc4 validation outcomes and corpus diagnostics' -- bash "$task/phase.sh" diagnose-data
elif [[ "$stage" == diagnose-memory || "$stage" == diagnose-familiar ]]; then
  "$ROOT/scripts/gq" submit --class batch --threads 28 --cpu-quota 2800% --mem 20G --max 2h \
    --name "nav bc4 $stage one-seed detour controls" -- bash "$task/phase.sh" "$stage"
else
  "$ROOT/scripts/gq" submit --class batch --threads 28 --cpu-quota 2800% --mem 20G --max 2h \
    --name "nav bc4 $stage three-seed lean validation" -- bash "$task/phase.sh" "$stage"
fi
