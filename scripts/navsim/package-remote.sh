#!/usr/bin/env bash
# Submit S4a package/parity, determinism and reference as one finite gq job.
# Sources are immutable; submission returns immediately without waiting for Ryzen.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
if [[ "${1:-}" != run ]]; then
  [[ $# == 0 ]] || { echo 'Usage: scripts/navsim/package-remote.sh' >&2; exit 2; }
  revision="$(git -C "$root" rev-parse HEAD)"
  baseline="$(git -C "$root" rev-parse origin/main)"
  [[ -z "$(git -C "$root" status --porcelain -- OpenGothic scripts)" ]] || {
    echo 'Commit OpenGothic/ and scripts/ before submitting revisioned runs' >&2; exit 2; }
  task="$(ssh -o BatchMode=yes -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-sim-stand-package" "$HOME"')"
  ssh ryzen "for target in '/mnt/shared/gothic-ai/nav-sim-research/stand/package/$revision' \
    '/mnt/shared/gothic-ai/nav-sim-research/stand/determinism/$revision' \
    '/mnt/shared/gothic-ai/nav-sim-research/stand/reference/$revision'; do
    if [ -e \"\$target\" ] || [ -L \"\$target\" ]; then
      printf 'S4a destination already exists: %s\n' \"\$target\" >&2; exit 2
    fi
  done"
  attempt="$(python3 -c 'import uuid; print(uuid.uuid4().hex)')"
  raw="$task/raw-$revision-$attempt"
  source="$task/source/$revision"
  before="$task/main/$baseline"
  if ! ssh ryzen "test -d '$source'"; then
    incoming="$source.incoming-$$"
    ssh ryzen "mkdir -p '$incoming'"
    (cd "$root" && rsync -aR --no-links --exclude=build --exclude='build-*' \
      --exclude=__pycache__ --exclude=.pytest_cache --exclude=.venv --exclude=.git \
      OpenGothic scripts "ryzen:$incoming/")
    ssh ryzen "mv -T '$incoming' '$source' 2>/dev/null || { test -d '$source' && rm -rf '$incoming'; }"
  fi
  if ! ssh ryzen "test -d '$before'"; then
    incoming="$before.incoming-$$"
    ssh ryzen "mkdir -p '$incoming'"
    git -C "$root" archive "$baseline" OpenGothic scripts | ssh ryzen "tar -xf - -C '$incoming'"
    ssh ryzen "mv -T '$incoming' '$before' 2>/dev/null || { test -d '$before' && rm -rf '$incoming'; }"
  fi
  GQ_OWNER=nav-sim-stand-package "$root/scripts/gq" submit \
    --class batch --threads 4 --cpu-quota 400% --mem 6G --max 2h \
    --archive "$raw" --name "navsim S4a ${revision:0:12}" -- \
    python3 -X faulthandler -I "$source/scripts/nav_job.py" --max-seconds 7200 -- \
    bash "$source/scripts/navsim/package-remote.sh" run "$task" "$revision" "$baseline" "$raw"
  exit
fi

[[ $# == 5 ]] || { echo 'Internal run requires task, revision, baseline and raw output' >&2; exit 2; }
task="$2"; revision="$3"; baseline="$4"; raw="$5"
source="$task/source/$revision"
build="$task/build-$revision"; python="$task/venv-$revision/bin/python"
mkdir -p "$task/locks"
exec {job_lock}>"$task/locks/$revision.lock"
flock -n "$job_lock" || { echo 'This revision already has an active S4a job' >&2; exit 75; }
for target in "/mnt/shared/gothic-ai/nav-sim-research/stand/package/$revision" \
  "/mnt/shared/gothic-ai/nav-sim-research/stand/determinism/$revision" \
  "/mnt/shared/gothic-ai/nav-sim-research/stand/reference/$revision"; do
  if [[ -e "$target" || -L "$target" ]]; then
    printf 'S4a destination already exists: %s\n' "$target" >&2; exit 2
  fi
done
[[ ! -e "$raw" ]] || { echo "S4a output already exists: $raw" >&2; exit 2; }
trap 'rm -rf "$build" "$task/venv-$revision"' EXIT
export TMPDIR="$task/tmp/$revision" UV_CACHE_DIR="$task/uv-cache" CCACHE_DIR="$task/ccache"
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1 OPENGOTHIC_BODY_TRACE=0
unset OPENGOTHIC_NAV_FORCE_PACE OPENGOTHIC_NAV_JOURNEY_MEMORY OPENGOTHIC_NAV_FAMILIAR_MAP OPENGOTHIC_NAV_PASSAGE_TRACE
mkdir -p "$TMPDIR" "$raw" "$task/navmesh-$revision"
exec > >(tee "$raw/run.log") 2>&1
cd "$source"
df -h "$HOME" /mnt/shared
python3 -c 'import pathlib,shutil; assert shutil.disk_usage(pathlib.Path.home()).free >= 26*1024**3, "Less than 26 GiB free"'
uv venv --python 3.12 "$task/venv-$revision"
uv pip install --python "$python" numpy
source scripts/ryzen-build.sh
build_runner() {
  cmake -S "$1/OpenGothic" -B "$2" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "${RYZEN_CMAKE_FLAGS[@]}" -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
    -DPython3_EXECUTABLE="$python" \
    -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
  cmake --build "$2" --target gothic-body-runner content-convert "${@:3}" --parallel 4
}
game_environment() {
  unset OPENGOTHIC_BASE OPENGOTHIC_BASE_FINGERPRINT OPENGOTHIC_BASE_CACHE OPENGOTHIC_BASE_STORE OPENGOTHIC_VIDEOS_FINGERPRINT
  eval "$("$python" scripts/ryzen_game.py --converter "$1/opengothic/content-convert")"
  export OPENGOTHIC_BASE="/mnt/shared/gothic-ai/base/$OPENGOTHIC_BASE_FINGERPRINT"
  export OPENGOTHIC_BASE_CACHE="$task/base-cache" OPENGOTHIC_BASE_STORE="$task/base-store"
  [[ -d "$OPENGOTHIC_BASE" ]] || { echo "No shared base $OPENGOTHIC_BASE" >&2; exit 1; }
}
build_runner "$source" "$build/after" gothic-navpolicy-contract
game_environment "$build/after"
fingerprint="$OPENGOTHIC_BASE_FINGERPRINT"
for version in 2 5; do
  "$python" -B -I scripts/check_nav_policy.py --output "$build/fixture-$version" --steps 3 --weight-version "$version"
  "$build/after/opengothic/gothic-navpolicy-contract" \
    "$build/fixture-$version/fixture.navbc" "$build/fixture-$version/parity.bin.gz"
done
cp -n /mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-measure-cache/ordinary/*.navmesh "$task/navmesh-$revision/"
"$python" -X faulthandler -B -I - "$task" "$revision" "$raw" "$build/after/opengothic/gothic-body-runner" <<'PY'
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
    cache=task / f"navmesh-{revision}", physics="cv", output=raw, seed=18102027, watchdog_seconds=1200)
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
"$python" -B -I scripts/navsim/package_paths.py "$raw/package" "$build/paths" "$raw/path-parity.json"
inputs=/mnt/shared/gothic-ai/nav-demos-v5/input
manifest=/mnt/shared/gothic-ai/nav-demos-v5/manifest.json
old_weights=/mnt/shared/gothic-ai/archive/nav-dagger3-20261030/hd-run/policy.navbc
weights=/mnt/shared/gothic-ai/nav-pace-v5/run/policy.navbc
[[ "$(sha256sum "$old_weights" | cut -d' ' -f1)" == 304ea6c6574e3836139da72b5a299a28217a263fac3648d792b458327d251a6b ]]
[[ "$(sha256sum "$weights" | cut -d' ' -f1)" == b70d3185c320af0e3c519adc9fd471d5f5df4abf3844bde7cbb39d96ccd351dc ]]
common=(--inputs "$inputs" --cache "$task/navmesh-$revision" --manifest "$manifest")
"$python" -B -I scripts/navsim/population_runs.py determinism "${common[@]}" \
  --runner "$build/after/opengothic/gothic-body-runner" --weights "$old_weights" \
  --output "$raw/determinism" --revision "$revision"
comparison_rc=0
if "$python" -c 'import json,sys;sys.exit(not json.load(open(sys.argv[1]))["deterministic"])' "$raw/determinism/determinism.json"; then
  build_runner "$task/main/$baseline" "$build/before"
  game_environment "$build/before"
  [[ "$fingerprint" == "$OPENGOTHIC_BASE_FINGERPRINT" ]]
  "$python" -B -I scripts/navsim/population_runs.py before "${common[@]}" \
    --runner "$build/before/opengothic/gothic-body-runner" --weights "$old_weights" \
    --output "$raw/determinism" --after "$raw/determinism/first" --revision "$baseline" || comparison_rc=$?
else
  echo 'Population is not deterministic: skip before/after comparison; use the reward contract.'
fi
game_environment "$build/after"
OPENGOTHIC_NAV_PASSAGE_TRACE=1 "$python" -B -I scripts/navsim/population_runs.py reference "${common[@]}" \
  --runner "$build/after/opengothic/gothic-body-runner" --weights "$weights" \
  --output "$raw/reference" --revision "$revision"
for item in "package:package/$revision" "determinism:determinism/$revision" "reference:reference/$revision"; do
  "$python" -B -I scripts/nas_archive.py move "$raw/${item%%:*}" \
    --destination-relative "nav-sim-research/stand/${item#*:}" --attempt-id "$revision"
done
exit "$comparison_rc"
