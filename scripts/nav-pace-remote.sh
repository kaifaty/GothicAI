#!/usr/bin/env bash
# Pace-input navigation policy (.navbc v5) checks on Ryzen; each stage is one gq job under nav_job.py.
#   bash scripts/nav-pace-remote.sh smoke    # targeted tests, runner build, HD -> v5 conversion,
#                                            # Torch/NumPy/C++ parity, one training epoch on a v4/v5 slice
#   bash scripts/nav-pace-remote.sh drive    # after smoke: v5 drives with forced run/walk, refusal without it,
#                                            # HD without the flag on the main and branch runners
#   bash scripts/nav-pace-remote.sh teacher  # teacher walk share by family, obstacles and goal distance
# HD and r1 weights are only read. Results: /mnt/shared/gothic-ai/nav-pace-input/; work: ~/gothic-ai/nav-pace-input/.
# Remove the work folder with: bash scripts/nav-pace-remote.sh clean
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
usage='Usage: bash scripts/nav-pace-remote.sh smoke | drive | teacher | clean'
stage="${1:-}"
archive=/mnt/shared/gothic-ai/nav-pace-input
hd=/mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-run
v4=/mnt/shared/gothic-ai/archive/nav-demos-v4-20261029
v5=/mnt/shared/gothic-ai/nav-lean-data/corpus-v5
v5_input=/mnt/shared/gothic-ai/nav-demos-v5/input
v5_validation=/mnt/shared/gothic-ai/nav-demos-v5/manifest.json
base_fingerprint=032e902aee6fef6ccc54fd01fdc4f018e0a2cb80ed470cdae2e79d5ee150b14b
videos_fingerprint=6ce7e0b2ed13967e04bb8efcd8fb4bbbbbe63d58acb60a50c6703d2a1a2083e7
seed=18102027

if [[ "$stage" != run ]]; then
  [[ $# == 1 && "$stage" =~ ^(smoke|drive|teacher|clean)$ ]] || { echo "$usage" >&2; exit 2; }
  task="$(ssh -o BatchMode=yes -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-pace-input" "$HOME"')"
  if [[ "$stage" == clean ]]; then
    ssh ryzen "rm -rf '$task'; df -h ~ | tail -1"
    exit
  fi
  ssh ryzen "mkdir -p '$task/source' '$task/input' '$archive/logs'"
  rsync -a --delete --no-links --exclude=build --exclude='build-*' --exclude=__pycache__ --exclude=.pytest_cache \
    --exclude=.venv --exclude=.git "$ROOT/OpenGothic" "$ROOT/scripts" "$ROOT/AGENTS.md" "ryzen:$task/source/"
  rsync -t "$ROOT/reports/navigation/2026-10-22-demos-v5/manifest.json" "ryzen:$task/input/v5-compact.json"
  if [[ "$stage" == drive ]]; then
    # The unchanged main runner is the reference for byte-equal HD actions without the flag.
    main="$(mktemp -d)"
    git -C "$ROOT" fetch -q origin main
    git -C "$ROOT" worktree add -q --detach "$main/tree" origin/main
    trap 'git -C "$ROOT" worktree remove --force "$main/tree"; rm -rf "$main"' EXIT
    ssh ryzen "mkdir -p '$task/main-source'"
    rsync -a --delete --no-links --exclude=build --exclude='build-*' --exclude=.git \
      "$main/tree/OpenGothic" "ryzen:$task/main-source/"
    git -C "$main/tree" rev-parse HEAD | ssh ryzen "cat > '$task/main-source/REVISION'"
  fi
  case "$stage" in
    smoke) resources=(--class batch --threads 12 --cpu-quota 1200% --mem 20G --gpu-mem 4G --max 2h) ;;
    drive) resources=(--class batch --threads 12 --cpu-quota 1200% --mem 16G --max 2h) ;;
    teacher) resources=(--class batch --threads 12 --cpu-quota 1200% --mem 24G --max 2h) ;;
  esac
  GQ_OWNER=nav-pace-input "$ROOT/scripts/gq" submit "${resources[@]}" --name "nav pace input $stage" -- \
    python3 -X faulthandler -I "$task/source/scripts/nav_job.py" --max-seconds 7200 -- \
    bash "$task/source/scripts/nav-pace-remote.sh" run "$stage"
  exit
fi

# Ryzen job body.
stage="${2:?stage}"
task="$HOME/gothic-ai/nav-pace-input"
mkdir -p "$archive/$stage" "$archive/logs" "$task/tmp"
exec > >(tee -a "$archive/logs/$stage.log") 2>&1
trap 'printf "nav_pace_%s_exit=%s\n" "$stage" "$?"' EXIT
export TMPDIR="$task/tmp" UV_CACHE_DIR="$HOME/.cache/uv"
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1 OPENGOTHIC_BODY_TRACE=0
unset OPENGOTHIC_NAV_FORCE_PACE OPENGOTHIC_NAV_JOURNEY_MEMORY OPENGOTHIC_NAV_FAMILIAR_MAP
cd "$task/source"
df -h "$HOME" /mnt/shared
python3 -c 'import pathlib,shutil; assert shutil.disk_usage(pathlib.Path.home()).free >= 26*1024**3, "Less than 26 GiB free"'
python="$task/.venv/bin/python"
if [[ ! -x "$python" ]]; then uv venv --python 3.12 "$task/.venv"; fi
uv pip install --python "$python" numpy pytest pillow matplotlib
build() { # SOURCE BUILD
  source "$task/source/scripts/ryzen-build.sh"
  cmake -S "$1" -B "$2" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    "${RYZEN_CMAKE_FLAGS[@]}" -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
    -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
  cmake --build "$2" --target "${@:3}" --parallel 12
}
contract="$task/build/opengothic/gothic-navpolicy-contract"

if [[ "$stage" == teacher ]]; then
  "$python" -X faulthandler -B -I scripts/nav_pace_teacher.py --output "$archive/teacher/teacher-pace.json" \
    --workers 12 --corpus v4 "$v4" "$v4/manifest.json" --corpus v5 "$v5" "$task/input/v5-compact.json"
  exit
fi

if [[ "$stage" == smoke ]]; then
  uv pip install --python "$python" 'torch==2.9.1+cu128' --index-url https://download.pytorch.org/whl/cu128
  "$python" -X faulthandler -B -m pytest -q -p no:cacheprovider --basetemp "$task/tmp/pytest" \
    scripts/tests/test_nav_bc_model.py scripts/tests/test_nav_bc_train.py scripts/tests/test_nav_bc_contract.py \
    scripts/tests/test_nav_bc_data.py scripts/tests/test_nav_rl.py scripts/tests/test_nav_drive.py
  build "$task/source/OpenGothic" "$task/build" gothic-body-runner gothic-navpolicy-contract
  "$contract" --live-format
  parity() { # NAME WEIGHTS [synthetic version]: Torch/NumPy fixture, then the C++ reader on the same steps.
    local fixture="$task/parity/$1"
    if [[ $# == 3 ]]; then
      "$python" -B -I scripts/check_nav_policy.py --output "$fixture" --weight-version "$3" --steps 1000 --torch
    else
      "$python" -B -I scripts/check_nav_policy.py --output "$fixture" --weights "$2" --source "$v4" --steps 1000 --torch
    fi | tee "$archive/smoke/parity-$1.json"
    "$contract" "$2" "$fixture/parity.bin.gz" | tee "$archive/smoke/contract-$1.json"
    "$contract" --memory "$2" | tee "$archive/smoke/memory-$1.json"
    rm -rf "$fixture"
  }
  for version in 1 2 3 4 5; do parity "synthetic-v$version" "$task/parity/synthetic-v$version/fixture.navbc" "$version"; done
  "$python" -B -I scripts/nav_pace_convert.py --source "$hd" --output "$archive/pace-from-hd" --steps 256
  parity hd "$hd/policy.navbc"
  parity pace-from-hd "$archive/pace-from-hd/policy.navbc"
  # One epoch from the converted HD on the first eight training episodes of v4 and of v5.
  cp -n "$v5_input/accepted-links.txt" "$task/input/accepted-links.txt"
  "$python" -X faulthandler -B -I - "$task" "$v4" "$v5" <<'PREPARE'
from pathlib import Path
import sys
sys.path.insert(0, str(Path.cwd() / "scripts"))
import nav_bc_data as data
task, v4, v5 = map(Path, sys.argv[1:])
for name, source, manifest, links in (("v4", v4, v4 / "manifest.json", None),
                                      ("v5", v5, task / "input/v5-compact.json", task / "input/accepted-links.txt")):
    data.prepare_manifest(source, task / "cache" / name, manifest, input_variant="hd",
                          maneuver_links=links, episode_range=(0, 8))
PREPARE
  "$python" -X faulthandler -B -I scripts/nav_bc_train.py train --cache "$task/cache/v4" \
    --aggregate-cache "$task/cache/v5" --pace-input --init "$archive/pace-from-hd/last.pt" \
    --epochs 1 --output "$archive/smoke/run" --threads 6 --device cuda
  parity smoke-run "$archive/smoke/run/policy.navbc"
  rm -rf "$task/cache" "$task/parity"
  touch "$archive/smoke/passed"
  exit
fi

# drive
test -f "$archive/smoke/passed" && test -x "$task/build/opengothic/gothic-body-runner"
build "$task/main-source/OpenGothic" "$task/build-main" gothic-body-runner
for name in save.sav places.json accepted-links.txt; do cp -n "$v5_input/$name" "$task/input/$name"; done
if [[ ! -e "$task/input/base" ]]; then ln -s "/mnt/shared/gothic-ai/base/$base_fingerprint" "$task/input/base"; fi
for worker in 0 1 2; do
  mkdir -p "$task/navmesh/ordinary/$worker"
  cp -n /mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-measure-cache/ordinary/*.navmesh "$task/navmesh/ordinary/$worker/"
done
export GOTHIC_DIR="$HOME/gothic-ai/game-settings" OPENGOTHIC_BASE="$task/input/base"
export OPENGOTHIC_BASE_FINGERPRINT="$base_fingerprint" OPENGOTHIC_VIDEOS_FINGERPRINT="$videos_fingerprint"
export OPENGOTHIC_BASE_CACHE="$task/base-cache" OPENGOTHIC_BASE_STORE="$task/base-store"
"$python" -X faulthandler -B -I - "$task" "$archive" "$hd/policy.navbc" "$v5_validation" "$seed" <<'DRIVE'
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys

import numpy as np

sys.path.insert(0, str(Path.cwd() / "scripts"))
import nav_rl as rl

task, archive, hd, manifest = map(Path, sys.argv[1:5])
seed = int(sys.argv[5])
pace_weights = archive / "pace-from-hd/policy.navbc"
families = "point,object,follow_below_gate"
values = rl.validation_scenarios(manifest, families)
indices = [next(index for index, value in enumerate(values) if value["family"] == family)
           for family in families.split(",")]


def drive(name, runner, weights, pace=None, configure=None):
    output = task / "drive" / name
    shutil.rmtree(output, ignore_errors=True)
    argv = ["nav_rl.py", "drive", "--runner", runner, "--assets", os.environ["GOTHIC_DIR"],
            "--save", task / "input/save.sav", "--cache", task / "navmesh",
            "--links", task / "input/accepted-links.txt", "--roads", task / "input/places.json",
            "--weights", weights, "--output", output, "--physics", "cv", "--parallel", 3,
            "--action-mode", "sample", "--action-seeds", seed, "--manifest", manifest, "--lean",
            "--families", families, "--indices", ",".join(map(str, indices)), "--watchdog-seconds", 1800]
    if pace:
        argv += ["--force-pace", pace]
    os.environ.pop("OPENGOTHIC_NAV_FORCE_PACE", None)
    sys.argv = list(map(str, argv))
    original = rl.configure_forced_pace
    if configure is not None:
        rl.configure_forced_pace = configure
    try:
        rl.main()
    finally:
        rl.configure_forced_pace = original
        os.environ.pop("OPENGOTHIC_NAV_FORCE_PACE", None)
    episodes = {}
    for path in sorted(output.glob("validation/*/*/seed-*/episode.json")):
        episode = rl.read_json(path)
        actions = np.loadtxt(path.parent / "actions.txt", ndmin=2)
        episodes[episode["family"]] = dict(
            id=episode["id"], success=episode["success"], elapsed_ms=episode["elapsed_ms"],
            decisions=episode["decisions"], forced_pace=episode.get("forced_pace"), pace=episode.get("pace"),
            pace_input=episode.get("pace_input"), walk_fraction=float((actions[:, 4] != 0).mean()),
            actions_sha256=hashlib.sha256((path.parent / "actions.txt").read_bytes()).hexdigest(),
            trajectory_sha256=hashlib.sha256((path.parent / "trajectory.txt").read_bytes()).hexdigest(),
            directory=str(path.parent))
    if len(episodes) != 3:
        raise ValueError(f"{name}: expected 3 episodes, found {sorted(episodes)}")
    return episodes


branch = task / "build/opengothic/gothic-body-runner"
main = task / "build-main/opengothic/gothic-body-runner"
result = dict(seed=seed, families=families, indices=indices, manifest=str(manifest),
              main_revision=(task / "main-source/REVISION").read_text().strip(),
              base_fingerprint=os.environ["OPENGOTHIC_BASE_FINGERPRINT"])
for pace in ("run", "walk"):
    result[f"v5_{pace}"] = drive(f"v5-{pace}", branch, pace_weights, pace)
    for family, episode in result[f"v5_{pace}"].items():
        expected = 0. if pace == "run" or family == "follow_below_gate" else 1.
        if episode["walk_fraction"] != expected or episode["pace_input"] is not True:
            raise ValueError(f"v5 {pace} {family}: walk fraction {episode['walk_fraction']} instead of {expected}")
try:
    drive("v5-none", branch, pace_weights)
    raise AssertionError("nav_rl drive accepted v5 weights without a forced pace")
except ValueError as error:
    result["refusal_python"] = str(error)


def bypass(args):
    args.force_pace = None
try:
    drive("v5-runner-none", branch, pace_weights, configure=bypass)
    raise AssertionError("runner accepted v5 weights without a forced pace")
except Exception as error:  # The runner's refusal reaches Python through the collector.
    if "need OPENGOTHIC_NAV_FORCE_PACE" not in str(error):
        raise
    result["refusal_runner"] = str(error)
result["hd_main"] = drive("hd-main", main, hd)
result["hd_branch"] = drive("hd-branch", branch, hd)
result["hd_equal"] = {family: all(result["hd_main"][family][key] == result["hd_branch"][family][key]
                                  for key in ("actions_sha256", "trajectory_sha256"))
                      for family in result["hd_main"]}
(archive / "drive/drive.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps({key: value for key, value in result.items() if key.startswith(("hd_equal", "refusal"))}, indent=2))
if not all(result["hd_equal"].values()):
    raise AssertionError("HD actions without the flag differ between main and branch runners")
DRIVE
rm -rf "$task/drive"
