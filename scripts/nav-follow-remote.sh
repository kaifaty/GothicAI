#!/usr/bin/env bash
# Follow trail carrot checks on Ryzen; each stage is one gq job under nav_job.py.
#   bash scripts/nav-follow-remote.sh smoke                    # build runner, game, converter and the trail
#                                                              # contract; resolve the base; two short episodes
#   bash scripts/nav-follow-remote.sh validation-teacher-trail # the follow teacher on the frozen validation
#   bash scripts/nav-follow-remote.sh validation-{teacher,v5}-{trail,line}  # line = the previous straight carrot
#                                                              # and teacher pace; NAV_FOLLOW_PACE=run|own|moves overrides
#   bash scripts/nav-follow-remote.sh live-trail|live-line     # 03-follow-hero, teacher and v5, three seeds, video
#   bash scripts/nav-follow-remote.sh report                   # tables from all finished stages
#   NAV_FOLLOW_IDS=id,id [NAV_FOLLOW_PACE=run|own] bash scripts/nav-follow-remote.sh diagnose-teacher-trail
#                                                              # full traces of chosen routes; the pace overrides
#                                                              # the default (run for trail, own for line)
#   bash scripts/nav-follow-remote.sh clean                    # remove ~/gothic-ai/nav-follow-trail on Ryzen
# Jobs run from immutable snapshots in ~/gothic-ai/nav-follow-trail/source/<hash>; the engine is built
# once per OpenGothic snapshot. Results: /mnt/shared/gothic-ai/nav-follow-trail/. v5 weights are only read.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
usage='Usage: bash scripts/nav-follow-remote.sh smoke|validation-{teacher,v5}-{trail,line}|diagnose-{teacher,v5}-{trail,line}|live-{trail,line}|report|clean'
stage="${1:-}"
archive="${NAV_FOLLOW_ARCHIVE:-/mnt/shared/gothic-ai/nav-follow-trail}"
task_name=nav-follow-trail
weights=/mnt/shared/gothic-ai/nav-pace-v5/run/policy.navbc
inputs=/mnt/shared/gothic-ai/nav-demos-v5/input
manifest=/mnt/shared/gothic-ai/nav-demos-v5/manifest.json
navmesh_source=/mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-measure-cache/ordinary
rejections_source=/mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-validation/setup-rejections

if [[ "$stage" != run ]]; then
  [[ $# == 1 && "$stage" =~ ^(smoke|(validation|diagnose)-(teacher|v5)-(trail|line)|live-(trail|line)|report|clean)$ ]] || { echo "$usage" >&2; exit 2; }
  task="$(ssh -o BatchMode=yes -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai" "$HOME"')/$task_name"
  if [[ "$stage" == clean ]]; then
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
    smoke) resources=(--class quick --threads 4 --cpu-quota 400% --mem 6G --max 40m) ;;
    validation-*) resources=(--class batch --threads 8 --cpu-quota 800% --mem 7G --max 2h) ;;
    live-*) resources=(--class batch --threads 6 --cpu-quota 600% --mem 10G --gpu-mem 3G --display --display-shared --max 1h) ;;
    report) resources=(--class quick --threads 1 --cpu-quota 100% --mem 2G --max 10m) ;;
    diagnose-*) resources=(--class quick --threads 2 --cpu-quota 200% --mem 3G --max 30m) ;;
  esac
  GQ_OWNER="$task_name" "$ROOT/scripts/gq" submit "${resources[@]}" --name "nav follow $stage $snapshot" -- \
    python3 -X faulthandler -I "$task/source/$snapshot/scripts/nav_job.py" --max-seconds 7200 -- \
    env NAV_FOLLOW_ARCHIVE="$archive" "NAV_FOLLOW_IDS=${NAV_FOLLOW_IDS:-}" "NAV_FOLLOW_PACE=${NAV_FOLLOW_PACE:-}" bash "$task/source/$snapshot/scripts/nav-follow-remote.sh" run "$stage" "$snapshot" "$engine"
  exit
fi

# Ryzen job body: everything below reads only this job's snapshot.
stage="${2:?stage}"; snapshot="${3:?snapshot}"; engine="${4:?engine}"
task="$HOME/gothic-ai/$task_name"
source="$task/source/$snapshot"
build_dir="$task/build/$engine"
stop() { echo "nav-follow $stage: $*" >&2; exit 1; }
[[ -d "$source/scripts" ]] || stop "missing source snapshot $source"
mkdir -p "$archive/$stage" "$archive/logs" "$task/tmp/$stage" "$task/input"
exec > >(tee -a "$archive/logs/$stage.log") 2>&1
trap 'printf "nav_follow_%s_exit=%s snapshot=%s engine=%s\n" "$stage" "$?" "$snapshot" "$engine"' EXIT
export TMPDIR="$task/tmp/$stage" UV_CACHE_DIR="$task/uv-cache" CCACHE_DIR="$task/ccache"
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1 OPENGOTHIC_BODY_TRACE=0
unset OPENGOTHIC_NAV_FORCE_PACE OPENGOTHIC_NAV_JOURNEY_MEMORY OPENGOTHIC_NAV_FAMILIAR_MAP
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
  # One link per stage: replacing a link shared with a running job is not atomic
  # (2026-11-04: a concurrent ln -sfn removed the base under a running validation).
  local base link="$task/input/$stage/base"
  mkdir -p "$task/input/$stage"
  for base in "/mnt/shared/gothic-ai/base/$OPENGOTHIC_BASE_FINGERPRINT" "$HOME/.cache/gothic-ai/base/$OPENGOTHIC_BASE_FINGERPRINT"; do
    if [[ -d "$base" ]]; then ln -sfn "$base" "$link"; break; fi
  done
  [[ -e "$link" ]] || stop "no ready base $OPENGOTHIC_BASE_FINGERPRINT"
  export OPENGOTHIC_BASE="$link" OPENGOTHIC_BASE_CACHE="$task/base-cache" OPENGOTHIC_BASE_STORE="$task/base-store"
}
navmesh() { # WORKERS: the archived ordinary navmesh, one copy per worker
  local worker
  for ((worker=0; worker<$1; ++worker)); do
    mkdir -p "$task/navmesh/$worker"
    cp -n "$navmesh_source"/*.navmesh "$task/navmesh/$worker/"
  done
}
validate() { # OUTPUT MODEL GUIDANCE [extra args]
  local output="$1" model="$2" guidance="$3"
  shift 3
  mkdir -p "$archive/validation/setup-rejections"
  cp -n "$rejections_source"/*.json "$archive/validation/setup-rejections/"
  "$python" -X faulthandler -B -I scripts/nav_follow_trail.py validation --model "$model" --guidance "$guidance" \
    --output "$output" --runner "$build_dir/opengothic/gothic-body-runner" --assets "$GOTHIC_DIR" \
    --save "$inputs/save.sav" --links "$inputs/accepted-links.txt" --roads "$inputs/places.json" \
    --cache "$task/navmesh" --manifest "$manifest" --rejections "$archive/validation/setup-rejections" "$@"
}

if [[ "$stage" == smoke ]]; then
  # One build per engine snapshot: CMake keeps the first source path, later snapshots of the
  # same OpenGothic content (only scripts changed) reuse the finished binaries.
  if [[ ! -f "$build_dir/built" ]]; then
    source "$source/scripts/ryzen-build.sh"
    cmake -S "$source/OpenGothic" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      "${RYZEN_CMAKE_FLAGS[@]}" -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
      -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
    cmake --build "$build_dir" --target gothic-body-runner Gothic2Notr content-convert gothic-followtrail-contract --parallel 4
    printf '%s\n' "$snapshot" > "$build_dir/built"
  fi
  "$build_dir/opengothic/gothic-followtrail-contract" | tee "$archive/smoke/contract.txt"
  mkdir -p "$archive/smoke/$engine"
  "$python" -B -I scripts/ryzen_game.py --converter "$build_dir/opengothic/content-convert" > "$archive/smoke/$engine/game.env"
  game_environment
  navmesh 2
  # Two short episodes: the stand writes the follow fields and the trail carrot is in use.
  short="$task/tmp/smoke/episodes"
  rm -rf "$short"
  for model in teacher "$weights"; do
    name=teacher; [[ "$model" == teacher ]] || name=v5
    validate "$short/$name" "$model" trail --parallel 2 --first 2 --limit-ms 20000 || stop "short $name validation failed"
  done
  "$python" -B -I - "$short" <<'SMOKE' | tee "$archive/smoke/$engine/episodes.json"
import json, sys
from pathlib import Path
root = Path(sys.argv[1])
rows = [json.loads(path.read_text()) for path in sorted(root.glob("*/episodes/*/follow.json"))]
assert rows, "no short follow episodes"
assert all(row["guidance"] == "trail" for row in rows)
assert sum(sum(row["sources"].values()) for row in rows) == sum(row["decisions"] for row in rows)
print(json.dumps([{key: row[key] for key in ("id", "decisions", "visible_decisions", "sources", "contacts", "walk_decisions")}
                  for row in rows], indent=2))
SMOKE
  rm -rf "$short"
  printf '%s\n' "$engine" > "$archive/smoke/$engine/passed"
  exit
fi

[[ -f "$archive/smoke/$engine/passed" ]] || stop "no smoke for engine $engine; run smoke first"

if [[ "$stage" == validation-* ]]; then
  IFS=- read -r _ model guidance <<< "$stage"
  game_environment
  navmesh 8
  target="$weights"; [[ "$model" == v5 ]] || target=teacher
  # NAV_FOLLOW_PACE=moves: the teacher runs except its careful escape steps (diagnosis).
  pace=(); [[ -z "${NAV_FOLLOW_PACE:-}" ]] || pace=(--teacher-pace "$NAV_FOLLOW_PACE")
  validate "$archive/validation/$model-$guidance${NAV_FOLLOW_PACE:+-$NAV_FOLLOW_PACE}" "$target" "$guidance" --parallel 8 "${pace[@]}"
  exit
fi

if [[ "$stage" == diagnose-* ]]; then
  IFS=- read -r _ model guidance <<< "$stage"
  [[ -n "${NAV_FOLLOW_IDS:-}" ]] || stop "NAV_FOLLOW_IDS names the routes to trace"
  game_environment
  navmesh 2
  target="$weights"; [[ "$model" == v5 ]] || target=teacher
  pace=(); [[ -z "${NAV_FOLLOW_PACE:-}" ]] || pace=(--teacher-pace "$NAV_FOLLOW_PACE")
  output="$archive/diagnose/$model-$guidance${NAV_FOLLOW_PACE:+-$NAV_FOLLOW_PACE}-$snapshot"
  rm -rf "$output"
  validate "$output" "$target" "$guidance" --parallel 2 --ids "$NAV_FOLLOW_IDS" --keep-traces "${pace[@]}"
  exit
fi

if [[ "$stage" == live-* ]]; then
  guidance="${stage#live-}"
  game_environment
  # Windowed game on the admitted gq display slot, as in scripts/nav-pace-remote.sh.
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
  "$python" -X faulthandler -B scripts/nav_follow_trail.py live --guidance "$guidance" --suite "$suite" \
    --binary "$build_dir/opengothic/Gothic2Notr" --runtime "$task" --output "$archive/live" --weights "$weights" \
    --budget-seconds 3300
  exit
fi

if [[ "$stage" == report ]]; then
  "$python" -B -I scripts/nav_follow_trail.py report --root "$archive"
  exit
fi
stop "unknown stage"
