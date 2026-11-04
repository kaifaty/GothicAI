#!/usr/bin/env bash
# Familiar-road carrot on Ryzen; each stage is one gq job under nav_job.py.
#   bash scripts/nav-roads-carrot-remote.sh point    # runners of origin/main and of this tree: v5, run,
#                                                    # the 60 point routes of validation v5, seed 18102027
#   bash scripts/nav-roads-carrot-remote.sh teacher  # the same runners and routes driven by the memory teacher
#   bash scripts/nav-roads-carrot-remote.sh live     # game of this tree: v5 on 05-wall-pocket, 3 seeds,
#                                                    # place pace, after a fresh teacher reference
#   bash scripts/nav-roads-carrot-remote.sh liveset  # game of this tree: the eleven places of pace-v5,
#                                                    # v5 on every seed and the teacher reference
#   bash scripts/nav-roads-carrot-remote.sh replay   # scripts/nav_roads_carrot.py on the shared recordings
#                                                    # (arguments in NAV_ROADS_REPLAY_ARGS, a shell word list)
#   bash scripts/nav-roads-carrot-remote.sh clean    # remove ~/gothic-ai/<task>; refuses while a queued or
#                                                    # running job of this owner uses it
# NAV_ROADS_TASK names the task (default nav-roads-carrot): ~/gothic-ai/<task>/ and /mnt/shared/gothic-ai/<task>/.
# Jobs use immutable snapshots in ~/gothic-ai/<task>/{source,main}/<hash>.
# Results: /mnt/shared/gothic-ai/<task>/{point,teacher}-{before,after}-<runner sha256[:12]>/,
# live-<game sha256[:12]>/ and liveset-<game sha256[:12]>/: resume continues only episodes of the same
# binary. Weights are only read.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
usage='Usage: bash scripts/nav-roads-carrot-remote.sh point|teacher|live|liveset|replay|clean'
stage="${1:-}"
task_name="${NAV_ROADS_TASK:-nav-roads-carrot}"
[[ "$task_name" =~ ^nav-roads-[a-z0-9-]+$ ]] || { echo 'NAV_ROADS_TASK must be nav-roads-<name>' >&2; exit 2; }
archive="${NAV_ROADS_CARROT_ARCHIVE:-/mnt/shared/gothic-ai/$task_name}"
weights=/mnt/shared/gothic-ai/nav-pace-v5/run/policy.navbc
weights_sha256=b70d3185c320af0e3c519adc9fd471d5f5df4abf3844bde7cbb39d96ccd351dc
v5_input=/mnt/shared/gothic-ai/nav-demos-v5/input
v5_validation=/mnt/shared/gothic-ai/nav-demos-v5/manifest.json
seed=18102027

if [[ "$stage" != run ]]; then
  [[ $# == 1 && "$stage" =~ ^(point|teacher|live|liveset|replay|clean)$ ]] || { echo "$usage" >&2; exit 2; }
  task="$(ssh -o BatchMode=yes -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai" "$HOME"')/$task_name"
  if [[ "$stage" == clean ]]; then
    # gq keeps every job in its state file; a job that is not finished still needs the runtime.
    ssh ryzen "python3 - '$task' '$task_name'" <<'ACTIVE'
import json
from pathlib import Path
import sys
task, owner = sys.argv[1:]
jobs = json.loads((Path.home() / "gothic-ai/queue/state.json").read_text())["jobs"]
jobs = jobs.values() if isinstance(jobs, dict) else jobs
active = [job["id"] for job in jobs if job.get("owner") == owner
          and job.get("status") not in {"done", "cancelled", "timeout", "failed"}
          and any(task in str(part) for part in job.get("command", []))]
if active:
    sys.exit(f"Refusing to clean {task}: gq jobs {' '.join(active)} are queued or running")
ACTIVE
    ssh ryzen "rm -rf '$task'; df -h ~ | tail -1"
    exit
  fi
  publish() { # DESTINATION ROOT PATH...: copy once; an existing snapshot is never rewritten.
    local destination="$1" root="$2" incoming
    shift 2
    if ssh ryzen "test -d '$destination'"; then return; fi
    incoming="$destination.incoming-$$"
    ssh ryzen "mkdir -p '$incoming'"
    (cd "$root" && rsync -aR --no-links --exclude=build --exclude='build-*' --exclude=__pycache__ \
      --exclude=.pytest_cache --exclude=.venv --exclude=.git "$@" "ryzen:$incoming/")
    ssh ryzen "mv -T '$incoming' '$destination' 2>/dev/null || rm -rf '$incoming'"
  }
  snapshot="$(git -C "$ROOT" rev-parse --short=12 HEAD)"
  [[ -z "$(git -C "$ROOT" status --porcelain -- OpenGothic scripts)" ]] || {
    echo 'Commit OpenGothic/ and scripts/ first: the snapshot is named by HEAD' >&2; exit 2; }
  git -C "$ROOT" fetch -q origin main
  main_revision="$(git -C "$ROOT" rev-parse --short=12 origin/main)"
  ssh ryzen "mkdir -p '$task/source' '$task/main' '$archive/logs'"
  publish "$task/source/$snapshot" "$ROOT" OpenGothic scripts AGENTS.md
  if [[ "$stage" =~ ^(point|teacher)$ ]] && ! ssh ryzen "test -d '$task/main/$main_revision'"; then
    main="$(mktemp -d)"
    git -C "$ROOT" worktree add -q --detach "$main/tree" "$main_revision"
    trap 'git -C "$ROOT" worktree remove --force "$main/tree"; rm -rf "$main"' EXIT
    publish "$task/main/$main_revision" "$main/tree" OpenGothic scripts
  fi
  case "$stage" in
    # gq peaks of the first round: point 9.66 GiB in 1600 s, live 1.49 GiB in 470 s.
    point|teacher) resources=(--class batch --threads 16 --cpu-quota 1600% --mem 11G --max 2h); seconds=7200 ;;
    live) resources=(--class quick --threads 12 --cpu-quota 1200% --mem 5G --gpu-mem 4G --display --display-shared --max 59m); seconds=3540 ;;
    liveset) resources=(--class batch --threads 12 --cpu-quota 1200% --mem 8G --gpu-mem 4G --display --display-shared --max 2h); seconds=7200 ;;
    replay) resources=(--class quick --threads 4 --cpu-quota 400% --mem 6G --max 59m); seconds=3540 ;;
  esac
  GQ_OWNER="$task_name" "$ROOT/scripts/gq" submit "${resources[@]}" --name "nav roads $stage $snapshot" -- \
    python3 -X faulthandler -I "$task/source/$snapshot/scripts/nav_job.py" --max-seconds "$seconds" -- \
    env NAV_ROADS_TASK="$task_name" NAV_ROADS_CARROT_ARCHIVE="$archive" NAV_ROADS_REPLAY_ARGS="${NAV_ROADS_REPLAY_ARGS:-}" \
    bash "$task/source/$snapshot/scripts/nav-roads-carrot-remote.sh" run "$stage" "$snapshot" "$main_revision"
  exit
fi

# Ryzen job body: everything below reads only this job's snapshots.
stage="${2:?stage}"; snapshot="${3:?snapshot}"; main_revision="${4:?main revision}"
task="$HOME/gothic-ai/$task_name"
source="$task/source/$snapshot"
stop() { echo "nav-roads-carrot $stage: $*" >&2; exit 1; }
[[ -d "$source/scripts" ]] || stop "missing source snapshot $source"
mkdir -p "$archive/logs" "$task/tmp/$stage" "$task/input"
# Stages may run at once: each keeps its own builds and navmesh copies and removes only those.
builds="$task/build-$stage"
export NAV_ROADS_NAVMESH="$task/navmesh-$stage"
exec > >(tee -a "$archive/logs/$stage.log") 2>&1
trap 'printf "%s_%s_exit=%s snapshot=%s main=%s\n" "${task_name//-/_}" "$stage" "$?" "$snapshot" "$main_revision"' EXIT
export TMPDIR="$task/tmp/$stage" UV_CACHE_DIR="$task/uv-cache" CCACHE_DIR="$task/ccache"
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1 OPENGOTHIC_BODY_TRACE=0
unset OPENGOTHIC_NAV_FORCE_PACE OPENGOTHIC_NAV_JOURNEY_MEMORY OPENGOTHIC_NAV_FAMILIAR_MAP
cd "$source"
df -h "$HOME" /mnt/shared
python3 -c 'import pathlib,shutil; assert shutil.disk_usage(pathlib.Path.home()).free >= 26*1024**3, "Less than 26 GiB free"'
[[ "$(sha256sum "$weights" | cut -d' ' -f1)" == "$weights_sha256" ]] || stop "v5 weights differ from $weights_sha256"
python="$task/venv/bin/python"
if [[ ! -x "$python" ]]; then uv venv --python 3.12 "$task/venv"; fi
uv pip install --python "$python" numpy pillow matplotlib
if [[ "$stage" == replay ]]; then
  # Light and offline: no game, world or policy; the navmesh is the oracle of the measurement.
  eval "replay_args=($NAV_ROADS_REPLAY_ARGS)"
  "$python" -X faulthandler -B -I scripts/nav_roads_carrot.py "${replay_args[@]}"
  exit
fi
build() { # SOURCE BUILD TARGET...
  source "$source/scripts/ryzen-build.sh"
  cmake -S "$1" -B "$2" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    "${RYZEN_CMAKE_FLAGS[@]}" -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
    -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
  cmake --build "$2" --target "${@:3}" --parallel 12
}
game_environment() { # CONVERTER: the base of this engine format from the shared store; caches stay in the task.
  local selected
  unset OPENGOTHIC_BASE OPENGOTHIC_BASE_FINGERPRINT OPENGOTHIC_BASE_CACHE OPENGOTHIC_BASE_STORE OPENGOTHIC_VIDEOS_FINGERPRINT
  eval "$("$python" scripts/ryzen_game.py --converter "$1")"
  selected="$OPENGOTHIC_BASE_FINGERPRINT"
  [[ -d "/mnt/shared/gothic-ai/base/$selected" ]] || stop "no shared base $selected for this engine format"
  ln -sfn "/mnt/shared/gothic-ai/base/$selected" "$task/input/base"
  export OPENGOTHIC_BASE="$task/input/base" OPENGOTHIC_BASE_FINGERPRINT="$selected"
  export OPENGOTHIC_BASE_CACHE="$task/base-cache" OPENGOTHIC_BASE_STORE="$task/base-store"
}

if [[ "$stage" == point || "$stage" == teacher ]]; then
  [[ -d "$task/main/$main_revision/OpenGothic" ]] || stop "missing main snapshot $task/main/$main_revision"
  for worker in {0..11}; do
    mkdir -p "$NAV_ROADS_NAVMESH/ordinary/$worker"
    cp -n /mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-measure-cache/ordinary/*.navmesh \
      "$NAV_ROADS_NAVMESH/ordinary/$worker/"
  done
  fingerprints=()
  for revision in before after; do
    tree="$source/OpenGothic"
    if [[ "$revision" == before ]]; then tree="$task/main/$main_revision/OpenGothic"; fi
    build "$tree" "$builds/$revision" gothic-body-runner content-convert
    game_environment "$builds/$revision/opengothic/content-convert"
    fingerprints+=("$OPENGOTHIC_BASE_FINGERPRINT")
    # nav_rl --resume trusts finished episodes; a folder per runner keeps builds apart.
    runner_sha256="$(sha256sum "$builds/$revision/opengothic/gothic-body-runner" | cut -d' ' -f1)"
    output="$archive/$stage-$revision-${runner_sha256:0:12}"
    mkdir -p "$output"
    if [[ -f "$output/runner.sha256" ]]; then
      [[ "$(cat "$output/runner.sha256")" == "$runner_sha256" ]] || stop "$output belongs to another runner"
    else
      printf '%s\n' "$runner_sha256" > "$output/runner.sha256"
    fi
    if [[ "$stage" == teacher ]]; then
      "$python" -X faulthandler -B -I - "$task" "$output" "$v5_validation" "$v5_input" \
        "$builds/$revision/opengothic/gothic-body-runner" <<'TEACHER'
# The memory teacher (navdemos start-v3) on the 60 point routes of validation v5: its own pace,
# the same frozen world seeds, CV physics, 240 s. Nothing is recorded as a demonstration.
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import sys
import time
from types import SimpleNamespace
sys.path.insert(0, str(Path.cwd() / "scripts"))
import nav_rl as rl
task, output, manifest, inputs, runner = map(Path, sys.argv[1:])
values = rl.validation_scenarios(manifest, "point")
assert len(values) == 60, len(values)
started = time.monotonic()
def collect(index, workers):
    worker = SimpleNamespace(runner=runner, assets=Path(os.environ["GOTHIC_DIR"]), save=inputs / "save.sav",
                             links=inputs / "accepted-links.txt", roads=inputs / "places.json", ini=None,
                             cache=Path(os.environ["NAV_ROADS_NAVMESH"]) / "ordinary" / str(index), physics="cv", npc="BAU_954_MALETH",
                             output=output / "workers" / str(index), seed=18102026, watchdog_seconds=1800,
                             weights="teacher", lean=True, familiar_map=None, stranger=False, limit_ms=240000)
    process = rl.new_runner(worker, "teacher")
    try:
        for value in values[index::workers]:
            directory = output / "validation" / "point" / value["id"]
            if (directory / "episode.json").is_file():
                continue
            if directory.exists():
                shutil.rmtree(directory)
            worker.seed = value["seed"]
            commands = rl.policy_commands(worker, value, 0)
            drive = [i for i, command in enumerate(commands) if command.startswith("navdemos drive ")]
            assert len(drive) == 1, commands
            commands[drive[0]] = f"navdemos start-v3 {worker.npc} {value['start']} {value['goal']} {value.get('limit_ms', 240000)}"
            rl.scenario(worker, process, f"teacher-{value['id']}", commands, directory)
            rl.clean_lean_diagnostics(directory)
            episode = rl.read_json(directory / "episode.json")
            assert episode.get("teacher_version") == 3 and not episode.get("drive"), directory
            print(json.dumps(dict(id=value["id"], success=episode["success"], elapsed_ms=episode["elapsed_ms"])), flush=True)
    finally:
        process.close()
    shutil.rmtree(worker.output, ignore_errors=True)
workers = 12
with ThreadPoolExecutor(max_workers=workers) as pool:
    for future in [pool.submit(collect, index, workers) for index in range(workers)]:
        future.result()
episodes = {value["id"]: rl.read_json(output / "validation" / "point" / value["id"] / "episode.json") for value in values}
arrived = sum(bool(ep["success"]) for ep in episodes.values())
summary = dict(model="teacher", episodes=len(episodes), arrived=arrived, arrived_fraction=arrived / len(episodes),
               median_elapsed_s=statistics.median(ep["elapsed_ms"] for ep in episodes.values()) / 1000,
               seconds=time.monotonic() - started, base_fingerprint=os.environ["OPENGOTHIC_BASE_FINGERPRINT"],
               runner_sha256=hashlib.sha256(runner.read_bytes()).hexdigest(),
               episodes_by_id={key: dict(success=ep["success"], elapsed_ms=ep["elapsed_ms"]) for key, ep in episodes.items()})
rl.write_json(output / "summary.json", summary)
print(json.dumps({key: value for key, value in summary.items() if key != "episodes_by_id"}), flush=True)
TEACHER
      continue
    fi
    "$python" -X faulthandler -B -I - "$task" "$output" "$weights" "$v5_validation" \
      "$v5_input" "$seed" "$builds/$revision/opengothic/gothic-body-runner" <<'POINT'
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import statistics
import sys
import time
sys.path.insert(0, str(Path.cwd() / "scripts"))
import nav_rl as rl
task, output, weights, manifest, inputs = map(Path, sys.argv[1:6])
seed, runner = sys.argv[6], Path(sys.argv[7])
argv = ["nav_rl.py", "drive", "--runner", runner, "--assets", os.environ["GOTHIC_DIR"],
        "--save", inputs / "save.sav", "--links", inputs / "accepted-links.txt", "--roads", inputs / "places.json",
        "--weights", weights, "--output", output, "--cache", Path(os.environ["NAV_ROADS_NAVMESH"]), "--physics", "cv",
        "--parallel", 12, "--action-mode", "sample", "--action-seeds", seed, "--force-pace", "run",
        "--lean", "--resume", "--limit-ms", 240000, "--watchdog-seconds", 1800,
        "--manifest", manifest, "--families", "point"]
sys.argv = list(map(str, argv))
started = time.monotonic()
rl.main()
episodes = [rl.read_json(path) for path in sorted(output.glob("validation/point/*/seed-*/drive-metrics.json"))]
assert Counter(ep["family"] for ep in episodes) == Counter(point=60), Counter(ep["family"] for ep in episodes)
assert {ep["action_seed"] for ep in episodes} == {int(seed)}
assert {ep["forced_pace"] for ep in episodes} == {"run"}
assert {ep["base_fingerprint"] for ep in episodes} == {os.environ["OPENGOTHIC_BASE_FINGERPRINT"]}
rl.write_json(output / "metrics.json", rl.validation_summary(episodes, lean=True))
arrived = sum(bool(ep["success"]) for ep in episodes)
summary = dict(episodes=len(episodes), arrived=arrived, arrived_fraction=arrived / len(episodes),
               median_elapsed_s=statistics.median(ep["elapsed_ms"] for ep in episodes) / 1000,
               median_arrived_elapsed_s=statistics.median(ep["elapsed_ms"] for ep in episodes if ep["success"]) / 1000,
               seconds=time.monotonic() - started, base_fingerprint=os.environ["OPENGOTHIC_BASE_FINGERPRINT"],
               runner_sha256=hashlib.sha256(runner.read_bytes()).hexdigest(),
               weights_sha256=hashlib.sha256(weights.read_bytes()).hexdigest(),
               episodes_by_id={ep["id"]: dict(success=ep["success"], elapsed_ms=ep["elapsed_ms"]) for ep in episodes})
rl.write_json(output / "summary.json", summary)
print(json.dumps({key: value for key, value in summary.items() if key != "episodes_by_id"}), flush=True)
POINT
  done
  [[ "${fingerprints[0]}" == "${fingerprints[1]}" ]] || stop "main and branch need different bases: ${fingerprints[*]}"
  rm -rf "$builds" "$NAV_ROADS_NAVMESH"
  exit
fi

# live and liveset
build "$source/OpenGothic" "$builds" Gothic2Notr content-convert
game_environment "$builds/opengothic/content-convert"
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
desktop_found=0
for auth in "$XDG_RUNTIME_DIR"/.mutter-Xwaylandauth.* "$HOME/.Xauthority"; do
  [[ -r "$auth" ]] || continue
  for socket in /tmp/.X11-unix/X*; do
    [[ -S "$socket" ]] || continue
    if DISPLAY=":${socket##*/X}" XAUTHORITY="$auth" xdpyinfo >/dev/null 2>&1; then
      export DISPLAY=":${socket##*/X}" XAUTHORITY="$auth"
      desktop_found=1
      break 2
    fi
  done
done
[[ "$desktop_found" == 1 ]] || stop 'no accessible Xwayland desktop'
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json
[[ -r "$VK_DRIVER_FILES" ]] || stop 'missing NVIDIA Vulkan ICD'
export ALSOFT_DRIVERS=null TEMPEST_BACKGROUND=1 RYZEN_BENCH=1
slot="${GQ_DISPLAY_SLOT:-0}"
export TEMPEST_WINDOW_POS="$((100+(10#$slot % 2)*700)),$((100+(10#$slot / 2)*460))"
mkdir -p "$NAV_ROADS_NAVMESH"
cp -n /mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-measure-cache/ordinary/*.navmesh "$NAV_ROADS_NAVMESH/"
# nav_live refuses a run folder of other inputs; a folder per game binary keeps rounds apart.
game_sha256="$(sha256sum "$builds/opengothic/Gothic2Notr" | cut -d' ' -f1)"
if [[ "$stage" == liveset ]]; then
  "$python" -X faulthandler -B - "$task" "$archive/liveset-${game_sha256:0:12}" "$builds/opengothic/Gothic2Notr" "$weights" <<'LIVESET'
# The eleven frozen places of the pace-v5 live round: a fresh teacher reference per place, then v5
# on every seed at the place pace. The same suite, inputs and seeds; HD is not driven again.
from copy import deepcopy
import json
import os
from pathlib import Path
import sys
import time
from types import SimpleNamespace
sys.path.insert(0, str(Path.cwd() / "scripts"))
import nav_live as live
task, output, binary, weights = map(Path, sys.argv[1:])
finished = live.read_json(Path("/mnt/shared/gothic-ai/nav-live/baseline/complete.json"))
suite = Path(finished["suite_path"])
original = live.read_json(suite)
assert live.sha256(suite) == finished["suite_sha256"]
selected = live.eligible_cases(original, live.suite_cases(original))
assert len(selected) == 11 and sum(len(case["seeds"]) for case in selected) == 27
paces = {case["id"]: case["pace"] for case in live.suite_cases(live.read_json(Path.cwd() / "scripts/nav-live/scenarios.json"))}
inputs = dict(original["inputs"], cache=os.environ["NAV_ROADS_NAVMESH"])
for name, filename in (("save", "save.sav"), ("roads", "places.json"), ("familiar", "familiar.bin"),
                       ("links", "accepted-links.txt")):
    if not Path(inputs[name]).is_file():
        inputs[name] = str(Path("/mnt/shared/gothic-ai/archive/nav-demos-v5-20261030/input") / filename)
output.mkdir(parents=True, exist_ok=True)
args = SimpleNamespace(suite=suite, output=output, binary=binary, inputs=inputs, deadline=time.monotonic() + 6600,
                       input_sha256={name: live.sha256(inputs[name]) for name in ("save", "roads", "familiar", "links")})
assert args.input_sha256 == finished["input_sha256"], "Frozen live inputs differ from the baseline"
model = dict(name="pace-v5", weights=str(weights), sha256=live.sha256(weights), force_place_pace=True,
             **live.pace_input(weights))
rows = []
for previous in selected:
    case = deepcopy(previous)
    case.pop("evaluation_binary", None)
    case.pop("teacher_reference", None)
    case["pace"] = paces[case["id"]]
    reference = live.teacher_directory(args, case)
    teacher = live.run_episode(args, case, None, case["seeds"][0], reference)
    rows.append(dict(place=case["id"], model="teacher", seed=case["seeds"][0], success=teacher.get("success"),
                     elapsed_ms=teacher.get("elapsed_ms"), output=str(reference)))
    for seed in case["seeds"]:
        directory = live.run_directory(output, case, model, seed)
        live.run_episode(args, case, model, seed, directory, reference)
        value = live.read_json(directory / "summary.json")
        rows.append({**value, "place": case["id"], "model": "pace-v5", "seed": seed, "output": str(directory)})
        print(json.dumps(dict(place=case["id"], seed=seed, success=value.get("success"),
                              elapsed_seconds=value.get("elapsed_seconds"))), flush=True)
live.write_json(output / "live-summary.json", dict(places=11, binary_sha256=live.sha256(binary),
    base_fingerprint=os.environ["OPENGOTHIC_BASE_FINGERPRINT"], weights_sha256=model["sha256"], rows=rows))
LIVESET
  rm -rf "$builds"
  exit
fi
"$python" -X faulthandler -B - "$task" "$archive/live-${game_sha256:0:12}" "$builds/opengothic/Gothic2Notr" "$weights" <<'LIVE'
# The frozen 05-wall-pocket of the pace-v5 round: same suite, inputs, place pace and seeds.
from copy import deepcopy
import json
import os
from pathlib import Path
import sys
import time
from types import SimpleNamespace
sys.path.insert(0, str(Path.cwd() / "scripts"))
import nav_live as live
task, output, binary, weights = map(Path, sys.argv[1:])
finished = live.read_json(Path("/mnt/shared/gothic-ai/nav-live/baseline/complete.json"))
suite = Path(finished["suite_path"])
original = live.read_json(suite)
assert live.sha256(suite) == finished["suite_sha256"]
case = deepcopy(next(row for row in live.eligible_cases(original, live.suite_cases(original)) if row["id"] == "05-wall-pocket"))
case.pop("evaluation_binary", None)
case.pop("teacher_reference", None)
case["pace"] = next(row for row in live.suite_cases(live.read_json(Path.cwd() / "scripts/nav-live/scenarios.json"))
                    if row["id"] == case["id"])["pace"]
inputs = dict(original["inputs"], cache=os.environ["NAV_ROADS_NAVMESH"])
for name, filename in (("save", "save.sav"), ("roads", "places.json"), ("familiar", "familiar.bin"),
                       ("links", "accepted-links.txt")):
    if not Path(inputs[name]).is_file():
        inputs[name] = str(Path("/mnt/shared/gothic-ai/archive/nav-demos-v5-20261030/input") / filename)
output.mkdir(parents=True, exist_ok=True)
args = SimpleNamespace(suite=suite, output=output, binary=binary, inputs=inputs, deadline=time.monotonic() + 3000,
                       input_sha256={name: live.sha256(inputs[name]) for name in ("save", "roads", "familiar", "links")})
assert args.input_sha256 == finished["input_sha256"], "Frozen live inputs differ from the baseline"
model = dict(name="pace-v5", weights=str(weights), sha256=live.sha256(weights), force_place_pace=True,
             **live.pace_input(weights))
reference = live.teacher_directory(args, case)
teacher = live.run_episode(args, case, None, case["seeds"][0], reference)
rows = [dict(model="teacher", seed=case["seeds"][0], success=teacher.get("success"), elapsed_ms=teacher.get("elapsed_ms"),
             output=str(reference))]
for seed in case["seeds"]:
    directory = live.run_directory(output, case, model, seed)
    live.run_episode(args, case, model, seed, directory, reference)
    value = live.read_json(directory / "summary.json")
    rows.append({**value, "model": "pace-v5", "seed": seed, "output": str(directory)})
    print(json.dumps(dict(seed=seed, success=value.get("success"), elapsed_seconds=value.get("elapsed_seconds"),
                          path_to_teacher_ratio=value.get("path_to_teacher_ratio"))), flush=True)
live.write_json(output / "live-summary.json", dict(place=case["id"], pace=case["pace"], binary_sha256=live.sha256(binary),
    base_fingerprint=os.environ["OPENGOTHIC_BASE_FINGERPRINT"], weights_sha256=model["sha256"], rows=rows))
LIVE
rm -rf "$builds"
