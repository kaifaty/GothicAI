#!/usr/bin/env bash
# Pace-input navigation policy (.navbc v5) checks on Ryzen; each stage is one gq job under nav_job.py.
#   bash scripts/nav-pace-remote.sh smoke    # targeted tests, runner build, HD -> v5 conversion,
#                                            # Torch/NumPy/C++ parity, one training epoch on a v4/v5 slice
#   bash scripts/nav-pace-remote.sh drive    # after smoke: v5 drives with forced run/walk, refusal without it;
#                                            # v1/v2/v3 without the flag on the main and branch runners
#   bash scripts/nav-pace-remote.sh live     # after smoke: v5 in the windowed game on three live places
#   bash scripts/nav-pace-remote.sh teacher  # teacher walk share by family, obstacles and goal distance
#   bash scripts/nav-pace-remote.sh train-1 # prepare the HD corpus, convert HD, time the first epoch
#   bash scripts/nav-pace-remote.sh train-{5,10,15,20} # resume to the named epoch, one bounded job each
#   bash scripts/nav-pace-remote.sh parity  # final trained weights, Torch/NumPy/C++
# Jobs use immutable snapshots in ~/gothic-ai/{nav-pace-input,nav-pace-v5}/source/<hash>.
# A later launch never rewrites queued/running sources. HD and older weights are only read.
# Check results: /mnt/shared/gothic-ai/nav-pace-input/; trained v5: /mnt/shared/gothic-ai/nav-pace-v5/.
# Remove the respective work folders with clean or clean-v5.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
usage='Usage: bash scripts/nav-pace-remote.sh smoke|drive|live|teacher|train-{1,5,10,15,20}|parity|compare-{hd,v5}-{run,walk}-{main,detour,follow}|live-v5|clean-v5|clean'
stage="${1:-}"
archive="${NAV_PACE_ARCHIVE:-/mnt/shared/gothic-ai/nav-pace-input}"  # tests point it at a temporary folder
task_name=nav-pace-input
if [[ "$stage" =~ ^(train-|parity$|compare-|live-v5$|clean-v5$) || "${2:-}" =~ ^(train-|parity$|compare-|live-v5$) ]]; then
  task_name=nav-pace-v5
  archive="${NAV_PACE_ARCHIVE:-/mnt/shared/gothic-ai/nav-pace-v5}"
fi
hd=/mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-run
v4=/mnt/shared/gothic-ai/archive/nav-demos-v4-20261029
v5=/mnt/shared/gothic-ai/nav-lean-data/corpus-v5
v5_input=/mnt/shared/gothic-ai/nav-demos-v5/input
v5_validation=/mnt/shared/gothic-ai/nav-demos-v5/manifest.json
base_fingerprint=032e902aee6fef6ccc54fd01fdc4f018e0a2cb80ed470cdae2e79d5ee150b14b
videos_fingerprint=6ce7e0b2ed13967e04bb8efcd8fb4bbbbbe63d58acb60a50c6703d2a1a2083e7
seed=18102027
v5_compact=reports/navigation/2026-10-22-demos-v5/manifest.json

if [[ "$stage" != run ]]; then
  [[ $# == 1 && "$stage" =~ ^(smoke|drive|live|teacher|clean|clean-v5|train-(1|5|10|15|20)|parity|compare-(hd|v5)-(run|walk)-(main|detour|follow)|live-v5)$ ]] || { echo "$usage" >&2; exit 2; }
  task="$(ssh -o BatchMode=yes -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai" "$HOME"')/$task_name"
  if [[ "$stage" == clean || "$stage" == clean-v5 ]]; then
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
  snapshot="$(python3 - "$ROOT" OpenGothic scripts AGENTS.md "$v5_compact" <<'HASH'
import hashlib
import os
from pathlib import Path
import sys
root = Path(sys.argv[1])
skip = lambda name: name in ('build', '.git', '__pycache__', '.pytest_cache', '.venv') or name.startswith('build-')
files = []
for name in sys.argv[2:]:
    if (root / name).is_file():
        files.append(Path(name))
        continue
    for directory, folders, names in os.walk(root / name):
        folders[:] = sorted(folder for folder in folders if not skip(folder))
        base = Path(directory).relative_to(root)
        files += [base / item for item in names if not skip(item) and not (root / base / item).is_symlink()]
digest = hashlib.sha256()
for relative in sorted(files):
    digest.update(relative.as_posix().encode() + b'\0' + (root / relative).read_bytes())
print(digest.hexdigest()[:20])
HASH
)"
  ssh ryzen "mkdir -p '$task/source' '$task/main' '$archive/logs'"
  publish "$task/source/$snapshot" "$ROOT" OpenGothic scripts AGENTS.md "$v5_compact"
  main_revision=-
  if [[ "$stage" == drive ]]; then
    # The unchanged main runner is the reference for byte-equal actions of older weights without the flag.
    git -C "$ROOT" fetch -q origin main
    main_revision="$(git -C "$ROOT" rev-parse origin/main)"
    if ! ssh ryzen "test -d '$task/main/$main_revision'"; then
      main="$(mktemp -d)"
      git -C "$ROOT" worktree add -q --detach "$main/tree" "$main_revision"
      trap 'git -C "$ROOT" worktree remove --force "$main/tree"; rm -rf "$main"' EXIT
      publish "$task/main/$main_revision" "$main/tree" OpenGothic scripts
    fi
  fi
  case "$stage" in
    smoke) resources=(--class batch --threads 12 --cpu-quota 1200% --mem 16G --gpu-mem 4G --max 2h) ;;
    drive) resources=(--class batch --threads 12 --cpu-quota 1200% --mem 14G --max 2h) ;;
    live) resources=(--class quick --threads 12 --cpu-quota 1200% --mem 10G --gpu-mem 3G --display --display-shared --max 59m) ;;
    teacher) resources=(--class batch --threads 12 --cpu-quota 1200% --mem 16G --max 2h) ;;
    train-1) resources=(--class batch --threads 12 --cpu-quota 1200% --mem 14G --gpu-mem 4G --max 2h) ;;
    train-*) resources=(--class batch --threads 6 --cpu-quota 600% --mem 8G --gpu-mem 4G --max 2h) ;;
    parity) resources=(--class quick --threads 6 --cpu-quota 600% --mem 8G --gpu-mem 2G --max 30m) ;;
    compare-*) resources=(--class batch --threads 24 --cpu-quota 2400% --mem 14G --max 2h) ;;
    live-v5) resources=(--class batch --threads 12 --cpu-quota 1200% --mem 12G --gpu-mem 4G --display --display-shared --max 2h) ;;
  esac
  GQ_OWNER="$task_name" "$ROOT/scripts/gq" submit "${resources[@]}" --name "nav pace $stage $snapshot" -- \
    python3 -X faulthandler -I "$task/source/$snapshot/scripts/nav_job.py" --max-seconds 7200 -- \
    env NAV_PACE_ARCHIVE="$archive" "NAV_PACE_WEIGHTS=${NAV_PACE_WEIGHTS:-$archive/run/policy.navbc}" \
    bash "$task/source/$snapshot/scripts/nav-pace-remote.sh" run "$stage" "$snapshot" "$main_revision"
  exit
fi

# Ryzen job body: everything below reads only this job's snapshot.
stage="${2:?stage}"; snapshot="${3:?snapshot}"; main_revision="${4:?main revision}"
task="$HOME/gothic-ai/$task_name"
source="$task/source/$snapshot"
stop() { echo "nav-pace $stage: $*" >&2; exit 1; }
[[ -d "$source/scripts" ]] || stop "missing source snapshot $source"
mkdir -p "$archive/$stage" "$archive/logs" "$task/tmp/$stage" "$task/input"
exec > >(tee -a "$archive/logs/$stage.log") 2>&1
trap 'printf "nav_pace_%s_exit=%s snapshot=%s\n" "$stage" "$?" "$snapshot"' EXIT
export TMPDIR="$task/tmp/$stage" UV_CACHE_DIR="$HOME/.cache/uv"
if [[ "$task_name" == nav-pace-v5 ]]; then export UV_CACHE_DIR="$task/uv-cache" CCACHE_DIR="$task/ccache"; fi
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1 OPENGOTHIC_BODY_TRACE=0
unset OPENGOTHIC_NAV_FORCE_PACE OPENGOTHIC_NAV_JOURNEY_MEMORY OPENGOTHIC_NAV_FAMILIAR_MAP
cd "$source"
# Each precondition is its own command with an explicit stop: `a && b` would not stop under set -e.
if [[ "$stage" == drive || "$stage" == live ]]; then
  [[ -f "$archive/smoke/passed" ]] || stop "no $archive/smoke/passed; run the smoke stage first"
  [[ -f "$archive/pace-from-hd/policy.navbc" ]] || stop "no converted weights $archive/pace-from-hd/policy.navbc"
fi
if [[ "$stage" == drive ]]; then
  [[ -d "$task/main/$main_revision/OpenGothic" ]] || stop "missing main snapshot $task/main/$main_revision"
fi
df -h "$HOME" /mnt/shared
python3 -c 'import pathlib,shutil; assert shutil.disk_usage(pathlib.Path.home()).free >= 26*1024**3, "Less than 26 GiB free"'
python="$task/venv-$stage/bin/python"
if [[ "$task_name" == nav-pace-v5 ]]; then python="$task/venv/bin/python"; fi
if [[ ! -x "$python" ]]; then uv venv --python 3.12 "$(dirname "$(dirname "$python")")"; fi
uv pip install --python "$python" numpy pytest pillow matplotlib
build() { # SOURCE BUILD TARGET...
  source "$source/scripts/ryzen-build.sh"
  cmake -S "$1" -B "$2" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    "${RYZEN_CMAKE_FLAGS[@]}" -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
    -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
  cmake --build "$2" --target "${@:3}" --parallel 12
}
build_dir="$task/build/$stage-$snapshot"  # one build folder per job: no two jobs share it
contract="$build_dir/opengothic/gothic-navpolicy-contract"
game_environment() { # Read-only shared base and videos of this engine format; caches stay in the task.
  if [[ ! -e "$task/input/base" ]]; then ln -s "/mnt/shared/gothic-ai/base/$base_fingerprint" "$task/input/base"; fi
  export GOTHIC_DIR="$HOME/gothic-ai/game-settings" OPENGOTHIC_BASE="$task/input/base"
  export OPENGOTHIC_BASE_FINGERPRINT="$base_fingerprint" OPENGOTHIC_VIDEOS_FINGERPRINT="$videos_fingerprint"
  export OPENGOTHIC_BASE_CACHE="$task/base-cache" OPENGOTHIC_BASE_STORE="$task/base-store"
}

if [[ "$stage" == train-* ]]; then
  epochs="${stage#train-}"
  uv pip install --python "$python" 'torch==2.9.1+cu128' --index-url https://download.pytorch.org/whl/cu128
  if [[ "$epochs" == 1 ]]; then
    [[ ! -f "$archive/run/last.pt" ]] || stop 'training already started; resume with train-5'
    build "$source/OpenGothic" "$task/build" gothic-body-runner gothic-navpolicy-contract Gothic2Notr content-convert
    eval "$("$python" scripts/ryzen_game.py --converter "$task/build/opengothic/content-convert")"
    [[ "$OPENGOTHIC_BASE_FINGERPRINT" == "$base_fingerprint" ]] || stop 'prepared base differs from the pinned current-engine base'
    game_environment
    "$task/build/opengothic/gothic-navpolicy-contract" --live-format
    "$python" -B -I scripts/nav_pace_convert.py --source "$hd" --output "$archive/pace-from-hd" --steps 256
    "$python" -X faulthandler -B -I - "$task/cache" "$v4" <<'PREPARE_HD'
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path.cwd() / "scripts"))
from nav_bc_data import prepare_manifest
cache, corpus = map(Path, sys.argv[1:])
index = prepare_manifest(corpus, cache, corpus / "manifest.json", input_variant="hd")
if len(index["episodes"]) != 602 or sum(ep["steps"] for ep in index["episodes"]) != 405626:
    raise ValueError("Training population differs from HD: expected 602 episodes / 405626 decisions")
print(json.dumps(dict(event="hd_training_population", episodes=602, decisions=405626)), flush=True)
PREPARE_HD
  fi
  [[ -f "$task/cache/index.json" ]] || stop 'missing prepared HD cache; run train-1 first'
  resume=()
  if [[ "$epochs" != 1 ]]; then
    [[ -f "$archive/run/last.pt" ]] || stop 'missing trained checkpoint; run train-1 first'
    resume=(--resume)
  fi
  "$python" -X faulthandler -B -I scripts/nav_bc_train.py train --cache "$task/cache" \
    --pace-input --init "$archive/pace-from-hd/last.pt" --epochs "$epochs" --output "$archive/run" \
    --batch 8 --segment 64 --lr .0003 --seed 17 --threads 6 --device cuda "${resume[@]}"
  exit
fi

if [[ "$stage" == parity ]]; then
  [[ -f "$archive/run/last.pt" ]] || stop 'missing trained checkpoint'
  "$python" -B -I - "$archive/run/history.json" <<'FINAL_EPOCH'
import json, sys
assert [row["epoch"] for row in json.load(open(sys.argv[1]))] == list(range(1, 21)), "Expected 20 completed epochs"
FINAL_EPOCH
  "$python" -B -I scripts/check_nav_policy.py --weights "$archive/run/policy.navbc" --source "$v4" \
    --output "$task/parity" --steps 1000 --torch | tee "$archive/parity/python.json"
  "$task/build/opengothic/gothic-navpolicy-contract" "$archive/run/policy.navbc" "$task/parity/parity.bin.gz" \
    | tee "$archive/parity/cpp.json"
  "$task/build/opengothic/gothic-navpolicy-contract" --memory "$archive/run/policy.navbc" | tee "$archive/parity/memory.json"
  rm -rf "$task/parity" "$task/cache"
  exit
fi

if [[ "$stage" == compare-* ]]; then
  IFS=- read -r _ model pace part <<< "$stage"
  [[ "$part" != follow || "$pace" == run ]] || stop 'follow is measured once per model, always run'
  game_environment
  weights="$hd/policy.navbc"
  if [[ "$model" == v5 ]]; then
    [[ -f "$archive/parity/cpp.json" ]] || stop 'final C++ parity must pass before v5 comparison'
    weights="$archive/run/policy.navbc"
  fi
  output="$archive/results/$model-$pace"
  if [[ "$part" == follow ]]; then output="$archive/results/$model-follow-run"; fi
  mkdir -p "$archive/results/setup-rejections"
  cp -n /mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-validation/setup-rejections/*.json \
    "$archive/results/setup-rejections/"
  for worker in {0..15}; do
    mkdir -p "$task/navmesh/ordinary/$worker"
    cp -n /mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-measure-cache/ordinary/*.navmesh \
      "$task/navmesh/ordinary/$worker/"
  done
  "$python" -X faulthandler -B -I - "$task" "$archive" "$output" "$weights" "$pace" "$part" \
    "$v5_validation" "$v5_input" <<'COMPARE'
from collections import Counter
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import time
sys.path.insert(0, str(Path.cwd() / "scripts"))
import nav_rl as rl
task, archive, output, weights = map(Path, sys.argv[1:5])
pace, part = sys.argv[5:7]
manifest, inputs = map(Path, sys.argv[7:9])
limit = math.ceil(240000 * 3.064158265) if pace == "walk" else 240000
original = rl.validation_commands
rl.validation_commands = lambda args, value, seed: original(args, dict(value, limit_ms=limit), seed)
argv = ["nav_rl.py", "drive", "--runner", task / "build/opengothic/gothic-body-runner",
        "--assets", os.environ["GOTHIC_DIR"], "--save", inputs / "save.sav",
        "--links", inputs / "accepted-links.txt", "--roads", inputs / "places.json",
        "--weights", weights, "--output", output, "--cache", task / "navmesh", "--physics", "cv",
        "--parallel", 16, "--action-mode", "sample", "--action-seeds", 18102027,
        "--force-pace", pace, "--lean", "--resume", "--limit-ms", limit, "--watchdog-seconds", 1800]
if part == "detour":
    argv += ["--detour-scenarios", "/mnt/shared/gothic-ai/archive/nav-bc4-20261030/input/detour-val.jsonl"]
else:
    argv += ["--manifest", manifest, "--families", "follow_below_gate" if part == "follow" else "approach,object,point"]
sys.argv = list(map(str, argv))
started = time.monotonic()
rl.main()
episodes = [rl.read_json(path) for path in sorted(output.glob("validation/*/*/seed-*/drive-metrics.json"))]
expected = (Counter(detour=121) if part == "detour" else Counter(follow_below_gate=102) if part == "follow"
            else Counter(approach=101, object=179, point=60))
selected = [ep for ep in episodes if ep["family"] in expected]
assert Counter(ep["family"] for ep in selected) == expected, Counter(ep["family"] for ep in selected)
assert {ep["action_seed"] for ep in selected} == {18102027}
assert {ep["forced_pace"] for ep in selected} == {pace}
assert {ep["base_fingerprint"] for ep in selected} == {os.environ["OPENGOTHIC_BASE_FINGERPRINT"]}
for ep in selected:
    assert ep["action_counts"]["walk"] == (ep["decisions"] if pace == "walk" else 0)
summary = rl.validation_summary(episodes, lean=True)
rl.write_json(output / "metrics.json", summary)
runner_digest = hashlib.sha256((task / "build/opengothic/gothic-body-runner").read_bytes()).hexdigest()
rl.write_json(output / f"{part}-summary.json", dict(episodes=len(selected), seconds=time.monotonic()-started,
              base_fingerprint=os.environ["OPENGOTHIC_BASE_FINGERPRINT"], limit_ms=limit,
              binary_sha256=runner_digest))

# The last completed part produces the full comparison and the live gate.
names = ("v5-run", "v5-walk", "hd-run", "hd-walk", "v5-follow-run", "hd-follow-run")
files = {name: archive / "results" / name / "metrics.json" for name in names}
if not all(path.is_file() for path in files.values()):
    raise SystemExit(0)
measured = {name: rl.read_json(path) for name, path in files.items()}
complete_counts = Counter(approach=101, object=179, point=60, detour=121)
if any(Counter(ep["family"] for ep in value["episodes"]) !=
       (Counter(follow_below_gate=102) if "follow" in name else complete_counts)
       for name, value in measured.items()):
    raise SystemExit(0)
episodes = [ep for value in measured.values() for ep in value["episodes"]]
assert len(episodes) == 2048
assert {ep["action_seed"] for ep in episodes} == {18102027}
assert {ep["base_fingerprint"] for ep in episodes} == {os.environ["OPENGOTHIC_BASE_FINGERPRINT"]}
for name in names:
    for component in (("follow",) if "follow" in name else ("main", "detour")):
        assert rl.read_json(archive / "results" / name / f"{component}-summary.json")["binary_sha256"] == runner_digest
cohort = lambda values: {(ep["family"], ep["id"], ep["action_seed"]) for ep in values}
assert all(cohort(measured[name]["episodes"]) == cohort(measured["hd-run"]["episodes"])
           for name in names[:4])
from nav_dagger4_round import paired_report_difference
paired, failures = [], []
for pair_pace, family in [(pace, family) for pace in ("run", "walk")
                          for family in complete_counts] + [("run", "follow_below_gate")]:
    suffix = "follow-run" if family == "follow_below_gate" else pair_pace
    left, right = [[ep for ep in measured[f"{model}-{suffix}"]["episodes"] if ep["family"] == family]
                   for model in ("v5", "hd")]
    for metric in ("arrival", "elapsed_s", "turn_only", "idle"):
        values = (left, right)
        if metric == "turn_only":
            values = [[{**ep, "action_counts": {**ep["action_counts"], "idle": ep["action_counts"]["turn_only"]}}
                       for ep in group] for group in values]
        result = paired_report_difference(*values, "idle" if metric == "turn_only" else metric)
        assert result["se"] is not None
        delta, se = result["delta"], result["se"]
        number_se = delta / se if se else (0 if delta == 0 else "inf" if delta > 0 else "-inf")
        row = dict(pace=pair_pace, family=family, metric=metric, number_se=number_se, **result)
        paired.append(row)
        if metric == "arrival" and delta < -2 * se:
            failures.append(row)
rows = [dict(configuration=name, family=family, **value)
        for name, value in measured.items() for family, value in value["by_family"].items()]
rl.write_json(archive / "comparison.json", dict(complete=True, episodes=2048, criteria=9,
    gate_passed=not failures, gate_failures=failures, paired=paired, rows=rows,
    base_fingerprint=os.environ["OPENGOTHIC_BASE_FINGERPRINT"], runner_sha256=runner_digest,
    weights_sha256={name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in
                   (("v5", archive / "run/policy.navbc"),
                    ("HD", Path("/mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-run/policy.navbc")))}))
print(json.dumps(dict(event="pace_v5_comparison_complete", episodes=2048, criteria=9,
                      gate_passed=not failures)), flush=True)
COMPARE
  exit
fi

if [[ "$stage" == teacher ]]; then
  "$python" -X faulthandler -B -I scripts/nav_pace_teacher.py --output "$archive/teacher/teacher-pace.json" \
    --workers 12 --corpus v4 "$v4" "$v4/manifest.json" --corpus v5 "$v5" "$source/$v5_compact"
  exit
fi

if [[ "$stage" == smoke ]]; then
  uv pip install --python "$python" 'torch==2.9.1+cu128' --index-url https://download.pytorch.org/whl/cu128
  "$python" -X faulthandler -B -m pytest -q -p no:cacheprovider --basetemp "$task/tmp/$stage/pytest" \
    scripts/tests/test_nav_bc_model.py scripts/tests/test_nav_bc_train.py scripts/tests/test_nav_bc_contract.py \
    scripts/tests/test_nav_bc_data.py scripts/tests/test_nav_rl.py scripts/tests/test_nav_drive.py
  build "$source/OpenGothic" "$build_dir" gothic-body-runner gothic-navpolicy-contract
  "$contract" --live-format
  work="$task/work/smoke-$snapshot"
  parity() { # NAME WEIGHTS [synthetic version]: Torch/NumPy fixture, then the C++ reader on the same steps.
    local fixture="$work/parity/$1"
    if [[ $# == 3 ]]; then
      "$python" -B -I scripts/check_nav_policy.py --output "$fixture" --weight-version "$3" --steps 1000 --torch
    else
      "$python" -B -I scripts/check_nav_policy.py --output "$fixture" --weights "$2" --source "$v4" --steps 1000 --torch
    fi | tee "$archive/smoke/parity-$1.json"
    "$contract" "$2" "$fixture/parity.bin.gz" | tee "$archive/smoke/contract-$1.json"
    "$contract" --memory "$2" | tee "$archive/smoke/memory-$1.json"
    rm -rf "$fixture"
  }
  for version in 1 2 3 4 5; do parity "synthetic-v$version" "$work/parity/synthetic-v$version/fixture.navbc" "$version"; done
  "$python" -B -I scripts/nav_pace_convert.py --source "$hd" --output "$archive/pace-from-hd" --steps 256
  parity hd "$hd/policy.navbc"
  parity pace-from-hd "$archive/pace-from-hd/policy.navbc"
  # One epoch from the converted HD on the first eight training episodes of v4 and of v5.
  "$python" -X faulthandler -B -I - "$work" "$v4" "$v5" "$source/$v5_compact" "$v5_input/accepted-links.txt" <<'PREPARE'
from pathlib import Path
import sys
sys.path.insert(0, str(Path.cwd() / "scripts"))
import nav_bc_data as data
work, v4, v5, compact, links = map(Path, sys.argv[1:])
for name, source, manifest, maneuver_links in (("v4", v4, v4 / "manifest.json", None), ("v5", v5, compact, links)):
    data.prepare_manifest(source, work / "cache" / name, manifest, input_variant="hd",
                          maneuver_links=maneuver_links, episode_range=(0, 8))
PREPARE
  "$python" -X faulthandler -B -I scripts/nav_bc_train.py train --cache "$work/cache/v4" \
    --aggregate-cache "$work/cache/v5" --pace-input --init "$archive/pace-from-hd/last.pt" \
    --epochs 1 --output "$archive/smoke/run" --threads 6 --device cuda
  parity smoke-run "$archive/smoke/run/policy.navbc"
  rm -rf "$work"
  printf '%s\n' "$snapshot" > "$archive/smoke/passed"
  exit
fi

if [[ "$stage" == live || "$stage" == live-v5 ]]; then
  if [[ "$stage" == live-v5 ]]; then
    [[ -f "$archive/comparison.json" ]] || stop 'missing complete comparison and gate'
    "$python" -B -I - "$archive/comparison.json" "$NAV_PACE_WEIGHTS" <<'GATE'
import hashlib, json, sys
from pathlib import Path
comparison = json.loads(Path(sys.argv[1]).read_text())
assert comparison["gate_passed"] is True, "Pace V5 comparison gate did not pass"
assert hashlib.sha256(Path(sys.argv[2]).read_bytes()).hexdigest() == comparison["weights_sha256"]["v5"], "Live weights differ from the compared policy"
GATE
  else
    build "$source/OpenGothic" "$build_dir" Gothic2Notr
  fi
  game_environment
  # Windowed game on the admitted gq display slot, as in scripts/nav-live.sh.
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
  live_cache="$task/live-navmesh"
  if [[ "$stage" == live-v5 ]]; then live_cache="$task/navmesh-cache"; fi
  mkdir -p "$live_cache"
  cp -n /mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-measure-cache/ordinary/*.navmesh "$live_cache/"
  if [[ "$stage" == live-v5 ]]; then
    export NAVLIVE_DEADLINE="$(( $(date +%s) + 6900 ))"
    suite="$("$python" -B -I - <<'LIVE_SUITE'
import json
from pathlib import Path
root = Path('/mnt/shared/gothic-ai/nav-live/baseline')
value = json.loads((root / 'complete.json').read_text())
print(value['suite_path'])
LIVE_SUITE
)"
    "$python" -X faulthandler -B scripts/nav_live.py pace-v5 --suite "$suite" \
      --runtime "$task" --output "$archive/live" --weights "$NAV_PACE_WEIGHTS,$hd/policy.navbc"
    exit
  fi
  "$python" -X faulthandler -B -I - "$task" "$archive" "$build_dir/opengothic/Gothic2Notr" <<'LIVE'
import json
from pathlib import Path
import shutil
import sys
import time
from types import SimpleNamespace

import numpy as np

sys.path.insert(0, str(Path.cwd() / "scripts"))
import nav_live as live

task, archive, binary = map(Path, sys.argv[1:])
suite = Path.cwd() / "scripts/nav-live/scenarios.json"
cases = {case["id"]: case for case in live.suite_cases(live.read_json(suite))}
inputs = dict(live.read_json(suite)["inputs"], cache=str(task / "live-navmesh"))
output = task / "live-output"
shutil.rmtree(output, ignore_errors=True)
output.mkdir(parents=True)
args = SimpleNamespace(suite=suite, output=output, binary=binary, inputs=inputs, deadline=time.monotonic() + 2700,
                       input_sha256={name: live.sha256(inputs[name]) for name in ("save", "roads", "familiar", "links")})
weights = archive / "pace-from-hd/policy.navbc"
model = dict(name="pace-from-hd", weights=str(weights), sha256=live.sha256(weights), order=0, **live.pace_input(weights))
assert model.get("pace_input") is True
result = dict(suite=str(suite), suite_sha256=live.sha256(suite), binary_sha256=live.sha256(binary), runs={})
for case_id in ("03-follow-hero", "02-routine-return-forest-tree", "05-wall-pocket"):
    case = dict(cases[case_id], limit_ms=5000)  # A short smoke: the frozen place and pace, five seconds.
    directory = output / case_id
    episode = live.run_episode(args, case, model, case["seeds"][0], directory, recorded=False)
    actions = np.loadtxt(directory / "actions.txt", ndmin=2)
    # A complete episode ends with the terminal key reset after its last decision.
    decisions = actions[:-1] if len(actions) == episode["decisions"] + 1 else actions
    expected_walk = case["pace"] == "walk" and case["goal_kind"] != "follow"
    row = dict(id=case_id, place_pace=case["pace"], forced_pace=episode.get("forced_pace"), pace=episode.get("pace"),
               pace_input=episode.get("pace_input"), decisions=episode["decisions"], elapsed_ms=episode["elapsed_ms"],
               success=episode.get("success"), failure=episode.get("failure"),
               walk_decisions=int((decisions[:, 4] != 0).sum()), decision_rows=len(decisions))
    result["runs"][case_id] = row
    print(json.dumps(row), flush=True)
    if (episode["decisions"] < 1 or row["forced_pace"] != case["pace"] or row["pace_input"] is not True
            or row["pace"] != ("walk" if expected_walk else "run")
            or row["walk_decisions"] != (row["decision_rows"] if expected_walk else 0)):
        raise AssertionError(f"live pace smoke failed: {row}")
(archive / "live/live.json").write_text(json.dumps(result, indent=2) + "\n")
shutil.rmtree(output)
LIVE
  exit
fi

# drive
build "$source/OpenGothic" "$build_dir" gothic-body-runner
main_build="$task/build-main/$main_revision"
build "$task/main/$main_revision/OpenGothic" "$main_build" gothic-body-runner
game_environment
navmesh="$task/navmesh/$snapshot"
for worker in 0 1 2; do
  mkdir -p "$navmesh/ordinary/$worker"
  cp -n /mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-measure-cache/ordinary/*.navmesh "$navmesh/ordinary/$worker/"
done
"$python" -X faulthandler -B -I - "$task/work/drive-$snapshot" "$archive" "$hd/policy.navbc" "$v5_validation" "$seed" \
    "$build_dir/opengothic/gothic-body-runner" "$main_build/opengothic/gothic-body-runner" "$main_revision" \
    "$navmesh" "$v5_input" <<'DRIVE'
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import sys

import numpy as np

sys.path.insert(0, str(Path.cwd() / "scripts"))
import nav_rl as rl

work, archive, hd, manifest = map(Path, sys.argv[1:5])
seed = int(sys.argv[5])
branch, main = map(Path, sys.argv[6:8])
main_revision, navmesh, inputs = sys.argv[8], Path(sys.argv[9]), Path(sys.argv[10])
pace_weights = archive / "pace-from-hd/policy.navbc"
families = "point,object,follow_below_gate"
values = rl.validation_scenarios(manifest, families)
indices = [next(index for index, value in enumerate(values) if value["family"] == family)
           for family in families.split(",")]


def drive(name, runner, weights, pace=None, configure=None):
    output = work / name
    shutil.rmtree(output, ignore_errors=True)
    argv = ["nav_rl.py", "drive", "--runner", runner, "--assets", os.environ["GOTHIC_DIR"],
            "--save", inputs / "save.sav", "--cache", navmesh,
            "--links", inputs / "accepted-links.txt", "--roads", inputs / "places.json",
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
        # main() reports ValueError/RuntimeError as "nav-rl: ..." and exit 1.
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            rl.main()
    except SystemExit as error:
        raise RuntimeError(f"exit {error.code}: {stderr.getvalue().strip()}") from None
    finally:
        rl.configure_forced_pace = original
        os.environ.pop("OPENGOTHIC_NAV_FORCE_PACE", None)
    episodes = {}
    for path in sorted(output.glob("validation/*/*/seed-*/episode.json")):
        episode = rl.read_json(path)
        actions = np.loadtxt(path.parent / "actions.txt", ndmin=2)
        # The last row is the terminal key reset after the final decision.
        if len(actions) != episode["decisions"] + 1:
            raise ValueError(f"{name}: actions rows differ from decisions + reset: {path.parent}")
        actions = actions[:-1]
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


result = dict(seed=seed, families=families, indices=indices, manifest=str(manifest), main_revision=main_revision,
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
except RuntimeError as error:
    if "need --force-pace walk|run" not in str(error) or any((work / "v5-none").rglob("episode.json")):
        raise
    result["refusal_python"] = str(error)


def bypass(args):
    args.force_pace = None
try:
    drive("v5-runner-none", branch, pace_weights, configure=bypass)
    raise AssertionError("runner accepted v5 weights without a forced pace")
except RuntimeError as error:  # The runner's refusal reaches nav_rl through the collector.
    if "need OPENGOTHIC_NAV_FORCE_PACE" not in str(error) or any((work / "v5-runner-none").rglob("episode.json")):
        raise
    result["refusal_runner"] = str(error)
# Older formats without the flag: main and branch runners must act byte for byte alike.
legacy = dict(v1=Path("/mnt/shared/gothic-ai/archive/nav-ppo/run/iter-000035/update/policy.navbc"),
              v2=Path("/mnt/shared/gothic-ai/archive/nav-bc3-20261029/run/it35/policy.navbc"), v3=hd)
result["legacy"] = {}
for name, weights in legacy.items():
    with weights.open("rb") as stream:
        version = json.loads(stream.readline())["version"]
    if f"v{version}" != name:
        raise ValueError(f"{weights} is .navbc v{version}, expected {name}")
    runs = dict(main=drive(f"{name}-main", main, weights), branch=drive(f"{name}-branch", branch, weights))
    equal = {family: all(runs["main"][family][key] == runs["branch"][family][key]
                         for key in ("actions_sha256", "trajectory_sha256")) for family in runs["main"]}
    result["legacy"][name] = dict(weights=str(weights), sha256=hashlib.sha256(weights.read_bytes()).hexdigest(),
                                  equal=equal, **runs)
(archive / "drive/drive.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(dict(refusal_python=result["refusal_python"], refusal_runner=result["refusal_runner"],
                      legacy_equal={name: value["equal"] for name, value in result["legacy"].items()}), indent=2))
if not all(all(value["equal"].values()) for value in result["legacy"].values()):
    raise AssertionError("older weights without the flag act differently on the main and branch runners")
DRIVE
rm -rf "$task/work/drive-$snapshot"
