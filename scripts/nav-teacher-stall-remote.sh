#!/usr/bin/env bash
# The memory teacher's dead end without a start cell, on Ryzen; each stage is one gq job under nav_job.py.
#   bash scripts/nav-teacher-stall-remote.sh smoke        # build runner, game, converter and both contracts;
#                                                         # resolve the base; two short episodes
#   NAV_STALL_IDS=id,id NAV_STALL_EXIT=on|off [NAV_STALL_PARALLEL=6] bash scripts/nav-teacher-stall-remote.sh diagnose
#                                                         # follow teacher traces with memory/navmesh dumps at stalls
#   bash scripts/nav-teacher-stall-remote.sh follow-teacher-on-aside      # follow validation: the teacher now
#   bash scripts/nav-teacher-stall-remote.sh follow-teacher-off-last-seen # ... as on main (both switches back)
#   bash scripts/nav-teacher-stall-remote.sh follow-v5-{aside,last-seen}  # v5, the lost rule now / as on main
#   bash scripts/nav-teacher-stall-remote.sh static-teacher-{run,walk}-{on,off}-{point,object,approach,detour}
#                                                         # the teacher with / without the body exit, forced pace
#   bash scripts/nav-teacher-stall-remote.sh replay       # the v5 teacher recordings replayed with --body-exit off/on
#   NAV_STALL_IDS=id,id bash scripts/nav-teacher-stall-remote.sh repeat-v5  # these follow routes of v5 (aside) again:
#                                                         # run-to-run spread of the policy on one runner
#   bash scripts/nav-teacher-stall-remote.sh live         # 03-follow-hero, teacher and v5, three seeds, video
#   bash scripts/nav-teacher-stall-remote.sh report       # tables from all finished stages
#   NAV_STALL_TAG=name bash scripts/nav-teacher-stall-remote.sh follow-*|static-*  # into <stage>-<name> instead
#   bash scripts/nav-teacher-stall-remote.sh clean        # remove ~/gothic-ai/nav-teacher-stall on Ryzen;
#                                                         # refuses while a job of this owner is not finished
# Jobs run from immutable snapshots in ~/gothic-ai/nav-teacher-stall/source/<hash>; the engine is built
# once per OpenGothic snapshot. "As on main" is the same runner with navdemos teacher-exit off and
# follow-lost last-seen. Results: /mnt/shared/gothic-ai/nav-teacher-stall/. v5 weights are only read.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
usage='Usage: bash scripts/nav-teacher-stall-remote.sh smoke|diagnose|replay|repeat-v5|follow-teacher-{on-aside,off-last-seen}|follow-v5-{aside,last-seen}|static-teacher-{run,walk}-{on,off}-{point,object,approach,detour}|live|report|clean'
stage="${1:-}"
archive="${NAV_STALL_ARCHIVE:-/mnt/shared/gothic-ai/nav-teacher-stall}"
task_name=nav-teacher-stall
weights=/mnt/shared/gothic-ai/nav-pace-v5/run/policy.navbc
inputs=/mnt/shared/gothic-ai/nav-demos-v5/input
manifest=/mnt/shared/gothic-ai/nav-demos-v5/manifest.json
detour=/mnt/shared/gothic-ai/archive/nav-bc4-20261030/input/detour-val.jsonl
navmesh_source=/mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-measure-cache/ordinary
rejections_source=/mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-validation/setup-rejections
stages='smoke|diagnose|replay|repeat-v5|follow-teacher-(on-aside|off-last-seen)|follow-v5-(aside|last-seen)|static-teacher-(run|walk)-(on|off)-(point|object|approach|detour)|live|report|clean'

if [[ "$stage" != run ]]; then
  [[ $# == 1 && "$stage" =~ ^($stages)$ ]] || { echo "$usage" >&2; exit 2; }
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
    sys.exit(f"Refusing to clean {task}: gq jobs {' '.join(active)} are queued, starting or running")
ACTIVE
    ssh ryzen "rm -rf '$task'; df -h ~ | tail -1"
    exit
  fi
  hash_tree() { # NAME...: a content hash of the files, without builds and caches
    python3 - "$ROOT" "$@" <<'HASH'
import hashlib
import os
from pathlib import Path
import sys
root = Path(sys.argv[1])
skip = lambda name: name in ('build', '.git', '__pycache__', '.pytest_cache', '.venv') or name.startswith('build-')
files = []
for name in sys.argv[2:]:
    for directory, folders, names in os.walk(root / name):
        folders[:] = sorted(folder for folder in folders if not skip(folder))
        base = Path(directory).relative_to(root)
        files += [base / item for item in names if not skip(item) and not (root / base / item).is_symlink()]
digest = hashlib.sha256()
for relative in sorted(files):
    digest.update(relative.as_posix().encode() + b'\0' + (root / relative).read_bytes())
print(digest.hexdigest()[:20])
HASH
  }
  engine="$(hash_tree OpenGothic)"
  snapshot="$(hash_tree OpenGothic scripts)"
  ssh ryzen "mkdir -p '$task/source' '$archive/logs'"
  if ! ssh ryzen "test -d '$task/source/$snapshot'"; then
    incoming="$task/source/$snapshot.incoming-$$"
    ssh ryzen "mkdir -p '$incoming'"
    (cd "$ROOT" && rsync -aR --no-links --exclude=build --exclude='build-*' --exclude=__pycache__ \
      --exclude=.pytest_cache --exclude=.venv --exclude=.git OpenGothic scripts "ryzen:$incoming/")
    ssh ryzen "mv -T '$incoming' '$task/source/$snapshot' 2>/dev/null || rm -rf '$incoming'"
  fi
  case "$stage" in
    smoke) resources=(--class quick --threads 12 --cpu-quota 1200% --mem 8G --max 1h) ;;
    diagnose) resources=(--class quick --threads 6 --cpu-quota 600% --mem 6G --max 1h) ;;
    replay) resources=(--class quick --threads 8 --cpu-quota 800% --mem 4G --max 1h) ;;
    repeat-v5) resources=(--class batch --threads 8 --cpu-quota 800% --mem 8G --max 1h) ;;
    follow-*|static-*) resources=(--class batch --threads 12 --cpu-quota 1200% --mem 10G --max 2h) ;;
    live) resources=(--class batch --threads 6 --cpu-quota 600% --mem 10G --gpu-mem 3G --display --display-shared --max 1h) ;;
    report) resources=(--class quick --threads 1 --cpu-quota 100% --mem 2G --max 10m) ;;
  esac
  GQ_OWNER="$task_name" "$ROOT/scripts/gq" submit "${resources[@]}" --name "nav stall $stage $snapshot" -- \
    python3 -X faulthandler -I "$task/source/$snapshot/scripts/nav_job.py" --max-seconds 7080 -- \
    env NAV_STALL_ARCHIVE="$archive" "NAV_STALL_IDS=${NAV_STALL_IDS:-}" "NAV_STALL_EXIT=${NAV_STALL_EXIT:-on}" \
    "NAV_STALL_PARALLEL=${NAV_STALL_PARALLEL:-6}" "NAV_STALL_TAG=${NAV_STALL_TAG:-}" \
    bash "$task/source/$snapshot/scripts/nav-teacher-stall-remote.sh" run "$stage" "$snapshot" "$engine"
  exit
fi

# Ryzen job body: everything below reads only this job's snapshot.
stage="${2:?stage}"; snapshot="${3:?snapshot}"; engine="${4:?engine}"
task="$HOME/gothic-ai/$task_name"
source="$task/source/$snapshot"
build_dir="$task/build/$engine"
stop() { echo "nav-teacher-stall $stage: $*" >&2; exit 1; }
[[ -d "$source/scripts" ]] || stop "missing source snapshot $source"
mkdir -p "$archive/logs" "$task/tmp/$stage" "$task/locks"
exec > >(tee -a "$archive/logs/$stage.log") 2>&1
trap 'printf "nav_stall_%s_exit=%s snapshot=%s engine=%s\n" "$stage" "$?" "$snapshot" "$engine"' EXIT
export TMPDIR="$task/tmp/$stage" UV_CACHE_DIR="$task/uv-cache" CCACHE_DIR="$task/ccache"
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1 OPENGOTHIC_BODY_TRACE=0
unset OPENGOTHIC_NAV_FORCE_PACE OPENGOTHIC_NAV_JOURNEY_MEMORY OPENGOTHIC_NAV_FAMILIAR_MAP OPENGOTHIC_NAV_STALL_TRACE
cd "$source"
df -h "$HOME" /mnt/shared
python3 -c 'import pathlib,shutil; assert shutil.disk_usage(pathlib.Path.home()).free >= 26*1024**3, "Less than 26 GiB free"'
python="$task/venv/bin/python"
if [[ ! -x "$python" ]]; then uv venv --python 3.12 "$task/venv"; fi
uv pip install --python "$python" numpy pillow matplotlib
game_environment() { # The base resolved by smoke for this engine, read-only; caches stay in the task.
  [[ -f "$archive/smoke/$engine/game.env" ]] || stop "no $archive/smoke/$engine/game.env; run smoke first"
  # shellcheck disable=SC1090
  source "$archive/smoke/$engine/game.env"
  export GOTHIC_DIR OPENGOTHIC_BASE_FINGERPRINT OPENGOTHIC_VIDEOS_FINGERPRINT
  # The base folder named by its fingerprint never changes: jobs point at it directly.
  local base
  OPENGOTHIC_BASE=
  for base in "/mnt/shared/gothic-ai/base/$OPENGOTHIC_BASE_FINGERPRINT" "$HOME/.cache/gothic-ai/base/$OPENGOTHIC_BASE_FINGERPRINT"; do
    if [[ -d "$base" ]]; then OPENGOTHIC_BASE="$base"; break; fi
  done
  [[ -n "$OPENGOTHIC_BASE" ]] || stop "no ready base $OPENGOTHIC_BASE_FINGERPRINT"
  export OPENGOTHIC_BASE OPENGOTHIC_BASE_CACHE="$task/base-cache" OPENGOTHIC_BASE_STORE="$task/base-store"
}
navmesh() { # WORKERS: the archived ordinary navmesh, one copy per worker
  local worker
  for ((worker=0; worker<$1; ++worker)); do
    mkdir -p "$task/navmesh/$worker"
    cp -n "$navmesh_source"/*.navmesh "$task/navmesh/$worker/"
  done
}
hold() { # PATH: one writer per path for the rest of this job; the lock lives on the local disk
  # (flock on CIFS is not reliable), its descriptor stays open until the job ends.
  local lock_fd
  exec {lock_fd}>"$task/locks/$(printf '%s' "$1" | sha256sum | cut -c1-16).lock"
  flock -n "$lock_fd" || stop "another job is writing $1"
}
validate() { # OUTPUT MODEL [nav_teacher_stall.py validation args]
  local output="$1" model="$2"
  shift 2
  hold "$output"
  mkdir -p "$archive/setup-rejections"
  cp -n "$rejections_source"/*.json "$archive/setup-rejections/"
  "$python" -X faulthandler -B -I scripts/nav_teacher_stall.py validation --model "$model" \
    --output "$output" --runner "$build_dir/opengothic/gothic-body-runner" --assets "$GOTHIC_DIR" \
    --save "$inputs/save.sav" --links "$inputs/accepted-links.txt" --roads "$inputs/places.json" \
    --cache "$task/navmesh" --manifest "$manifest" --detour "$detour" --rejections "$archive/setup-rejections" "$@"
}

if [[ "$stage" == smoke ]]; then
  # One smoke at a time from here on: it builds into the engine folder and clears the
  # short episodes every smoke shares.
  hold "$task/smoke"
  # One build per engine snapshot; later snapshots of the same OpenGothic reuse the binaries.
  if [[ ! -f "$build_dir/built" ]]; then
    source "$source/scripts/ryzen-build.sh"
    cmake -S "$source/OpenGothic" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      "${RYZEN_CMAKE_FLAGS[@]}" -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
      -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
    cmake --build "$build_dir" --target gothic-body-runner Gothic2Notr content-convert \
      gothic-navmemoryteacher-contract gothic-followtrail-contract --parallel 12
    printf '%s\n' "$snapshot" > "$build_dir/built"
  fi
  mkdir -p "$archive/smoke/$engine"
  "$build_dir/opengothic/gothic-navmemoryteacher-contract" --self-test | tee "$archive/smoke/$engine/teacher-contract.txt"
  "$build_dir/opengothic/gothic-followtrail-contract" | tee "$archive/smoke/$engine/followtrail-contract.txt"
  # Publish game.env whole: other stages of this engine may read it while smoke repeats.
  "$python" -B -I scripts/ryzen_game.py --converter "$build_dir/opengothic/content-convert" > "$archive/smoke/$engine/game.env.$$"
  mv -f "$archive/smoke/$engine/game.env.$$" "$archive/smoke/$engine/game.env"
  game_environment
  navmesh 4
  # Short episodes: the teacher on follow and point, v5 on follow; the new fields are written.
  short="$task/tmp/smoke/episodes"
  rm -rf "$short"
  validate "$short/teacher-follow" teacher --families follow_below_gate --parallel 2 --first 2 --limit-ms 20000
  validate "$short/teacher-point" teacher --families point --pace run --parallel 2 --first 2 --limit-ms 20000
  validate "$short/v5-follow" "$weights" --families follow_below_gate --pace run --parallel 2 --first 2 --limit-ms 20000
  "$python" -B -I - "$short" <<'SMOKE' | tee "$archive/smoke/$engine/episodes.json"
import json, sys
from pathlib import Path
root = Path(sys.argv[1])
rows = {path.parent.name: json.loads(path.read_text())["episodes"] for path in sorted(root.glob("*/summary.json"))}
assert set(rows) == {"teacher-follow", "teacher-point", "v5-follow"} and all(len(value) == 2 for value in rows.values())
for name in ("teacher-follow", "teacher-point"):
    for row in rows[name]:
        assert row["body_exit"] == "on" and sum(row["branches"].values()) == row["decisions"], row
for row in rows["teacher-point"]:
    assert row["teacher_pace"] == "run", row
for row in rows["teacher-follow"] + rows["v5-follow"]:
    assert row["follow_lost"] == "aside" and sum(row["follow"]["sources"].values()) == row["decisions"], row
print(json.dumps({name: [{key: row[key] for key in ("id", "decisions", "start_not_found", "far_idle")}
                         for row in value] for name, value in rows.items()}, indent=2))
SMOKE
  rm -rf "$short"
  printf '%s\n' "$engine" > "$archive/smoke/$engine/passed"
  exit
fi

[[ -f "$archive/smoke/$engine/passed" ]] || stop "no smoke for engine $engine; run smoke first"

if [[ "$stage" == diagnose ]]; then
  [[ -n "${NAV_STALL_IDS:-}" ]] || stop "NAV_STALL_IDS names the follow routes to trace"
  game_environment
  navmesh 6
  # One folder per set of routes and switch; a repeated run of the same set resumes it.
  routes="$(printf '%s' "$NAV_STALL_IDS" | sha256sum | cut -c1-10)"
  output="$archive/diagnose/teacher-exit-$NAV_STALL_EXIT-$snapshot-$routes-p$NAV_STALL_PARALLEL"
  # NAV_STALL_PARALLEL=1 runs the routes in manifest order in one runner, as one worker of a validation did.
  validate "$output" teacher --families follow_below_gate --exit "$NAV_STALL_EXIT" --ids "$NAV_STALL_IDS" \
    --parallel "$NAV_STALL_PARALLEL" --keep-traces --trace
  "$python" -B -I scripts/nav_teacher_stall.py stalls --root "$output"
  exit
fi

if [[ "$stage" == replay ]]; then
  # The frozen v5 memory-teacher recordings (2026-10-22): every one with a decision without a
  # start cell, plus 40 others. --body-exit off must replay them exactly; on differs from there.
  "$python" -B -I - "$build_dir/opengothic/gothic-navmemoryteacher-contract" "$archive/replay.json" <<'REPLAY'
import json, random, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
contract, output = sys.argv[1:]
# Only the memory teacher's recordings: jump is recorded by teacher v2.
episodes = {path: json.loads((path.parent / "episode.json").read_text())
            for path in sorted(Path("/mnt/shared/gothic-ai/nav-demos-v5/recordings").glob("*/*/*/samples.bin.gz"))}
recordings = [path for path, episode in episodes.items() if episode.get("teacher_version") == 3]
counts = {str(path): episodes[path].get("start_not_found_decisions") or 0 for path in recordings}
stalled = [path for path in recordings if counts[str(path)] > 0]
others = random.Random(18102027).sample([path for path in recordings if counts[str(path)] == 0], 40)
def replay(path, mode):
    result = subprocess.run([contract, "--body-exit", mode, str(path)], capture_output=True, text=True)
    value = json.loads(result.stdout.splitlines()[-1]) if result.stdout.strip() else {}
    return dict(path=str(path), family=path.parts[-4], mode=mode, start_not_found=counts[str(path)],
                exit=result.returncode, decisions=value.get("decisions"), mismatches=value.get("mismatches"),
                error=result.stderr.strip()[-300:])
with ThreadPoolExecutor(8) as pool:
    rows = list(pool.map(lambda job: replay(*job), [(path, mode) for path in stalled + others for mode in ("off", "on")]))
# The contract exits 1 on a mismatch and on an error: only a complete count with the
# exit code its mismatches imply is a replay.
broken = [row for row in rows if not row["decisions"] or row["mismatches"] is None
          or row["exit"] != (1 if row["mismatches"] else 0)]
if broken:
    sys.exit(f"Replay failed for {len(broken)} of {len(rows)} runs, first: {broken[0]}")
summary = {mode: dict(recordings=sum(row["mode"] == mode for row in rows),
                      decisions=sum(row["decisions"] or 0 for row in rows if row["mode"] == mode),
                      mismatches=sum(row["mismatches"] or 0 for row in rows if row["mode"] == mode),
                      recordings_with_mismatch=sum(bool(row["mismatches"]) for row in rows if row["mode"] == mode),
                      stalled_recordings=sum(row["start_not_found"] > 0 for row in rows if row["mode"] == mode))
           for mode in ("off", "on")}
Path(output).write_text(json.dumps(dict(summary=summary, rows=rows), indent=1) + "\n")
print(json.dumps(summary, indent=1))
assert summary["off"]["mismatches"] == 0, "the teacher with --body-exit off differs from its recordings"
REPLAY
  exit
fi

if [[ "$stage" == repeat-v5 ]]; then
  [[ -n "${NAV_STALL_IDS:-}" ]] || stop "NAV_STALL_IDS names the follow routes to repeat"
  game_environment
  navmesh 8
  routes="$(printf '%s' "$NAV_STALL_IDS" | sha256sum | cut -c1-10)"
  validate "$archive/repeat/v5-aside-$snapshot-$routes" "$weights" --pace run --lost aside --ids "$NAV_STALL_IDS" --parallel 8
  exit
fi

# NAV_STALL_TAG=name writes a validation into its own folder (<stage>-<name>), e.g. the same
# configuration again on a newer runner.
tag="${NAV_STALL_TAG:+-$NAV_STALL_TAG}"
if [[ "$stage" == follow-* ]]; then
  game_environment
  navmesh 12
  case "$stage" in
    follow-teacher-on-aside) validate "$archive/validation/$stage$tag" teacher --exit on --lost aside --parallel 12 ;;
    follow-teacher-off-last-seen) validate "$archive/validation/$stage$tag" teacher --exit off --lost last-seen --parallel 12 ;;
    follow-v5-*) validate "$archive/validation/$stage$tag" "$weights" --pace run --lost "${stage#follow-v5-}" --parallel 12 ;;
  esac
  exit
fi

if [[ "$stage" == static-* ]]; then
  IFS=- read -r _ _ pace body_exit part <<< "$stage"
  game_environment
  navmesh 12
  validate "$archive/validation/teacher-$pace-$body_exit-$part$tag" teacher --families "$part" --pace "$pace" \
    --exit "$body_exit" --parallel 12
  exit
fi

if [[ "$stage" == live ]]; then
  hold "$archive/live"
  game_environment
  # Windowed game on the admitted gq display slot, as in scripts/nav-follow-remote.sh.
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
  mkdir -p "$task/live-navmesh"
  cp -n "$navmesh_source"/*.navmesh "$task/live-navmesh/"
  suite="$("$python" -B -I -c 'import json; print(json.load(open("/mnt/shared/gothic-ai/nav-live/baseline/complete.json"))["suite_path"])')"
  # The default switches of this engine: body exit on, lost aside.
  "$python" -X faulthandler -B scripts/nav_follow_trail.py live --guidance trail --suite "$suite" \
    --binary "$build_dir/opengothic/Gothic2Notr" --runtime "$task" --output "$archive/live" --weights "$weights" \
    --budget-seconds 3300
  exit
fi

if [[ "$stage" == report ]]; then
  "$python" -B -I scripts/nav_teacher_stall.py report --root "$archive"
  exit
fi
stop "unknown stage"
