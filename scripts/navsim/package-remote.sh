#!/usr/bin/env bash
# Export the S4a episode package and check its Detour paths on Ryzen through gq.
# The package is published to /mnt/shared/gothic-ai/nav-sim-research/stand/package/<revision>/.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
if [[ "${1:-}" != run ]]; then
  [[ $# == 0 ]] || { echo 'Usage: scripts/navsim/package-remote.sh' >&2; exit 2; }
  revision="$(git -C "$root" rev-parse HEAD)"
  [[ -z "$(git -C "$root" status --porcelain -- OpenGothic scripts)" ]] || {
    echo 'Commit OpenGothic/ and scripts/ before exporting a revisioned package' >&2; exit 2; }
  task="$(ssh -o BatchMode=yes -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-sim-stand-package" "$HOME"')"
  ssh ryzen "mkdir -p '$task' && mkdir '$task/.package-lock'"
  release() { ssh ryzen "rmdir '$task/.package-lock'" >/dev/null 2>&1 || true; }
  trap release EXIT
  source="$task/source"
  ssh ryzen "mkdir -p '$source'"
  (cd "$root" && rsync -aR --delete --no-links --exclude=build --exclude='build-*' \
    --exclude=__pycache__ --exclude=.pytest_cache --exclude=.venv --exclude=.git \
    OpenGothic scripts "ryzen:$source/")
  GQ_OWNER=nav-sim-stand-package "$root/scripts/gq" run --wait-max 1800 \
    --class quick --threads 4 --cpu-quota 400% --mem 6G --max 1h \
    --archive "$task/raw-$revision" --name "navsim package ${revision:0:12}" -- \
    python3 -X faulthandler -I "$source/scripts/nav_job.py" --max-seconds 3600 -- \
    bash "$source/scripts/navsim/package-remote.sh" run "$task" "$revision"
  exit
fi

[[ $# == 3 ]] || { echo 'Internal run requires task and revision' >&2; exit 2; }
task="$2"; revision="$3"; source="$task/source"; raw="$task/raw-$revision"
build="$task/build-package"
trap 'rm -rf "$build" "$task/build-paths" "$task/venv"' EXIT
export TMPDIR="$task/tmp" UV_CACHE_DIR="$task/uv-cache" CCACHE_DIR="$task/ccache"
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1 OPENGOTHIC_BODY_TRACE=0
unset OPENGOTHIC_NAV_FORCE_PACE OPENGOTHIC_NAV_JOURNEY_MEMORY OPENGOTHIC_NAV_FAMILIAR_MAP
mkdir -p "$TMPDIR" "$raw" "$task/navmesh"
exec > >(tee "$raw/package.log") 2>&1
cd "$source"
df -h "$HOME" /mnt/shared
python3 -c 'import pathlib,shutil; assert shutil.disk_usage(pathlib.Path.home()).free >= 26*1024**3, "Less than 26 GiB free"'
source scripts/ryzen-build.sh
cmake -S OpenGothic -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "${RYZEN_CMAKE_FLAGS[@]}" -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
  -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
cmake --build "$build" --target gothic-body-runner content-convert --parallel 4
python="$task/venv/bin/python"
if [[ ! -x "$python" ]]; then uv venv --python 3.12 "$task/venv"; fi
uv pip install --python "$python" numpy
unset OPENGOTHIC_BASE OPENGOTHIC_BASE_FINGERPRINT OPENGOTHIC_BASE_CACHE OPENGOTHIC_BASE_STORE OPENGOTHIC_VIDEOS_FINGERPRINT
eval "$("$python" scripts/ryzen_game.py --converter "$build/opengothic/content-convert")"
export OPENGOTHIC_BASE="/mnt/shared/gothic-ai/base/$OPENGOTHIC_BASE_FINGERPRINT"
export OPENGOTHIC_BASE_CACHE="$task/base-cache" OPENGOTHIC_BASE_STORE="$task/base-store"
[[ -d "$OPENGOTHIC_BASE" ]] || { echo "No shared base $OPENGOTHIC_BASE" >&2; exit 1; }
cp -n /mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-measure-cache/ordinary/*.navmesh "$task/navmesh/"
"$python" -X faulthandler -B -I - "$task" "$revision" "$raw" "$build/opengothic/gothic-body-runner" <<'PY'
import os
from pathlib import Path
import sys
from types import SimpleNamespace
sys.path.insert(0, str(Path.cwd() / "scripts"))
import nav_rl as rl

task, revision, raw, runner = sys.argv[1:]
task, raw, runner = Path(task), Path(raw), Path(runner)
inputs = Path("/mnt/shared/gothic-ai/nav-demos-v5/input")
args = SimpleNamespace(runner=runner, assets=Path(os.environ["GOTHIC_DIR"]),
    save=inputs / "save.sav", links=inputs / "accepted-links.txt", roads=inputs / "places.json", ini=None,
    cache=task / "navmesh", physics="cv", output=raw, seed=18102027, watchdog_seconds=1200)
process = rl.new_runner(args, "package")
try:
    command = (f"navdemos dump-package {revision} {args.roads} "
        "/mnt/shared/gothic-ai/archive/nav-ppo/training-pairs/pairs.jsonl "
        "/mnt/shared/gothic-ai/archive/nav-ppo/validation/holdout.jsonl "
        "/mnt/shared/gothic-ai/archive/nav-rl/cv-test/holdout.jsonl BAU_954_MALETH")
    rl.scenario(args, process, "package", [command], raw)
finally:
    process.close()
PY
"$python" -B -I scripts/navsim/package_paths.py "$raw/package" "$task/build-paths" "$raw/path-parity.json"
"$python" -B -I scripts/nas_archive.py move "$raw/package" \
  --destination-relative "nav-sim-research/stand/package/$revision" --attempt-id "$revision"
