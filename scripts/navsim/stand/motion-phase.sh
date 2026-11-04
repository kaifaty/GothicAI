#!/usr/bin/env bash
# Ryzen side of motion-remote.sh, inside one gq job (docs/nav-sim.md, S2): Embree 4.4.0,
# the stand library with Jolt, the stand's pytest; `accept` adds acceptance 1, `cost` the
# cost on the given pool sizes. Raw results go to RAW, which gq --archive moves to the NAS.
# The build, the venv and the corpus cache stay in the task folder until motion-remote.sh clean.
set -euo pipefail
stand="$(cd "$(dirname "$0")" && pwd)"
source_root="$(cd "$stand/../../.." && pwd)"
task="$(cd "$source_root/.." && pwd)"
stage="${1:?selftest|accept|cost}";threads="${2:?threads}";raw="${3:-}";cost_threads="${4:-1,8,28}"
corpus=/mnt/shared/gothic-ai/nav-demos-v5
table=/mnt/shared/gothic-ai/nav-sim-research/motion/final/table-full.json
rays=/mnt/shared/gothic-ai/nav-sim-research/rays
export PATH="$HOME/.local/bin:$PATH"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1
free_kb="$(df -Pk "$HOME" | awk 'NR==2 {print $4}')"
(( free_kb >= 20*1024*1024 )) || { echo 'Less than 20 GiB free' >&2; exit 75; }
mkdir -p "$task/deps"
embree="$task/deps/embree-4.4.0"
if [[ ! -f "$embree/lib/cmake/embree-4.4.0/embree-config.cmake" ]]; then
  mkdir -p "$embree"
  curl -sS --fail --location --retry 2 -o "$task/deps/embree.tar.gz" \
    https://github.com/RenderKit/embree/releases/download/v4.4.0/embree-4.4.0.x86_64.linux.tar.gz
  # The archive raybench and S1 measured with (data/rays/standalone.json).
  echo "cb3d4402537fc9165c76c3316b8953dcfea523cd1eaf588e2de7639864ee3c57  $task/deps/embree.tar.gz" | sha256sum -c -
  tar -xzf "$task/deps/embree.tar.gz" -C "$embree"
  rm "$task/deps/embree.tar.gz"
fi
flags='-O3 -DNDEBUG -march=x86-64-v3 -ffp-contract=off'
cmake -S "$stand/native" -B "$task/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$embree" -DCMAKE_CXX_FLAGS_RELEASE="$flags" > "$task/cmake.log"
cmake --build "$task/build" --target navsim_stand --parallel "$threads"
export NAVSIM_STAND_LIB="$task/build/libnavsim_stand.so"
[[ -x "$task/.venv/bin/python" ]] || uv venv -q "$task/.venv"
uv pip install -q --python "$task/.venv/bin/python" numpy pytest
py="$task/.venv/bin/python"
(cd "$source_root/scripts" && "$py" -m pytest -q -rs tests/test_navsim_stand_motion.py tests/test_navsim_stand_eye.py)
[[ "$stage" != selftest ]] || exit 0

mkdir -p "$raw"
"$py" - "$rays" "$table" <<'EOF'
import hashlib, json, sys
from pathlib import Path
rays, table = Path(sys.argv[1]), Path(sys.argv[2])
entry = json.loads((rays / "engine.json").read_text())["files"]["static.bin"]
digest = hashlib.sha256(Path(entry["path"]).read_bytes()).hexdigest()
assert digest == entry["sha256"], f"static.bin: {digest} != {entry['sha256']}"
print("static.bin matches engine.json sha256")
EOF
"$py" - "$raw/meta-$stage.json" "$stage" "$threads" "$flags" "$HOME/gothic-ai/bin/gq" <<'EOF'
import json, subprocess, sys, time
from pathlib import Path
out, stage, threads, flags, gq = Path(sys.argv[1]), sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5]
run = lambda *cmd: subprocess.run(cmd, capture_output=True, text=True).stdout
out.write_text(json.dumps({
    "stage": stage, "class": "quick", "threads": threads,
    "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "gq_status": run(gq, "status"), "loadavg_at_start": Path("/proc/loadavg").read_text().strip(),
    "cpu": next(l.split(":", 1)[1].strip() for l in run("lscpu").splitlines() if l.startswith("Model name")),
    "compiler": run("c++", "--version").splitlines()[0], "flags": flags,
    "embree": "4.4.0 release archive, sha256 cb3d4402...3c57",
    "jolt": "OpenGothic/lib/JoltPhysics 5.6.0, engine options, x86-64-v3, CROSS_PLATFORM_DETERMINISTIC"}, indent=1) + "\n")
EOF
cache="$task/cache/test-episodes.pkl"
static="$rays/static.bin"
if [[ "$stage" == accept ]]; then
  status=0
  "$py" "$stand/motion_accept.py" run "$corpus" "$table" "$static" "$raw" --threads "$threads" \
    --cache "$cache" 2>&1 | tee "$raw/accept.log" || status=$?
  # A missed gate is a result, not a failure of the job: accept.json holds it.
  [[ -f "$raw/accept.json" ]] || exit "${status:-1}"
else
  "$py" "$stand/motion_accept.py" cost "$corpus" "$table" "$static" "$raw" --threads "$cost_threads" \
    --cache "$cache" 2>&1 | tee "$raw/cost.log"
fi
echo "raw results in $raw"
