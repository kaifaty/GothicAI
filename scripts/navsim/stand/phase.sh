#!/usr/bin/env bash
# Ryzen side of stand-remote.sh, inside one gq job: Embree 4.4.0, the stand
# library and its pytest. `run` adds acceptance 2 and the cost on 1 and THREADS
# threads, `cost` the cost on THREADS. Results accumulate in the NAS folder,
# mirrored here and copied back with a sha256 check. Builds and the venv go on exit.
set -euo pipefail
stand="$(cd "$(dirname "$0")" && pwd)"
source_root="$(cd "$stand/../../.." && pwd)"
task="$(cd "$source_root/.." && pwd)"
stage="${1:?selftest|run|cost}";threads="${2:?threads}";job_class="${3:?class}"
input=/mnt/shared/gothic-ai/nav-sim-research/rays
nas=/mnt/shared/gothic-ai/nav-sim-research/stand/eye  # stand/ also holds the S3 loop runs
raw="$task/raw"
export PATH="$HOME/.local/bin:$PATH"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1
trap 'rm -rf "$task/build" "$task/.venv"' EXIT
free_kb="$(df -Pk "$HOME" | awk 'NR==2 {print $4}')"
(( free_kb >= 20*1024*1024 )) || { echo 'Less than 20 GiB free' >&2; exit 75; }
mkdir -p "$task/deps"
embree="$task/deps/embree-4.4.0"
if [[ ! -f "$embree/lib/cmake/embree-4.4.0/embree-config.cmake" ]]; then
  mkdir -p "$embree"
  curl -sS --fail --location --retry 2 -o "$task/deps/embree.tar.gz" \
    https://github.com/RenderKit/embree/releases/download/v4.4.0/embree-4.4.0.x86_64.linux.tar.gz
  # The archive raybench measured with (data/rays/standalone.json).
  echo "cb3d4402537fc9165c76c3316b8953dcfea523cd1eaf588e2de7639864ee3c57  $task/deps/embree.tar.gz" | sha256sum -c -
  tar -xzf "$task/deps/embree.tar.gz" -C "$embree"
  rm "$task/deps/embree.tar.gz"
fi
flags='-O3 -DNDEBUG -march=x86-64-v3 -ffp-contract=off'
cmake -S "$stand/native" -B "$task/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$embree" -DCMAKE_CXX_FLAGS_RELEASE="$flags"
cmake --build "$task/build" --parallel 4
export NAVSIM_STAND_LIB="$task/build/libnavsim_stand.so"
uv venv -q "$task/.venv"
uv pip install -q --python "$task/.venv/bin/python" numpy pytest
py="$task/.venv/bin/python"
(cd "$source_root/scripts" && "$py" -m pytest -q -rs tests/test_navsim_stand_eye.py)
[[ "$stage" != selftest ]] || exit 0

rm -rf "$raw";mkdir -p "$raw"
if [[ -d "$nas" ]]; then
  rsync -a --exclude=eye.json --exclude=SHA256SUMS "$nas/" "$raw/"
  # Reuse only a whole publication: the archive must match its own SHA256SUMS, file for file.
  [[ -f "$nas/SHA256SUMS" ]] || { echo "$nas has no SHA256SUMS: an interrupted publication" >&2; exit 1; }
  (cd "$raw" && sha256sum -c --quiet "$nas/SHA256SUMS")
  diff <(cd "$raw" && ls | sort) <(awk '{print $2}' "$nas/SHA256SUMS" | sort)
fi
"$py" - "$input" <<'EOF'
import hashlib, json, sys
from pathlib import Path
root = Path(sys.argv[1])
for name, entry in json.loads((root / "engine.json").read_text())["files"].items():
    if name in ("static.bin", "rays.bin", "poses.txt"):
        digest = hashlib.sha256(Path(entry["path"]).read_bytes()).hexdigest()
        assert digest == entry["sha256"], f"{name}: {digest} != {entry['sha256']}"
print("inputs match engine.json sha256")
EOF
list="$threads";[[ "$stage" != run ]] || list="1,$threads"
"$py" - "$raw/meta-$stage-${list//,/_}.json" "$job_class" "$flags" "$HOME/gothic-ai/bin/gq" <<'EOF'
import json, subprocess, sys, time
from pathlib import Path
out, job_class, flags, gq = Path(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
run = lambda *cmd: subprocess.run(cmd, capture_output=True, text=True).stdout
out.write_text(json.dumps({
    "class": job_class, "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "gq_status": run(gq, "status"), "loadavg_at_start": Path("/proc/loadavg").read_text().strip(),
    "cpu": next(l.split(":", 1)[1].strip() for l in run("lscpu").splitlines() if l.startswith("Model name")),
    "compiler": run("c++", "--version").splitlines()[0], "flags": flags,
    "embree": "4.4.0 release archive, sha256 cb3d4402...3c57",
    "eye_core": "OpenGothic/common/game/navigationeye.cpp compiled into the library as is"}, indent=1) + "\n")
EOF
if [[ "$stage" == run ]]; then "$py" "$stand/eye_accept.py" parity "$input" "$raw" --threads "$threads"
else "$py" "$stand/eye_accept.py" parity "$input" "$raw" --reuse; fi  # the current analysis of the stored replay
"$py" "$stand/eye_accept.py" cost "$input" "$raw" --threads "$list" --agents 2048 | tee "$raw/cost-${list//,/_}.log"
"$py" "$stand/eye_accept.py" summary "$input" "$raw" "$task/eye.json" --raw-root "$nas"
mkdir -p "$nas"
(cd "$raw" && sha256sum -- * > "$task/SHA256SUMS")
rsync -a "$raw/" "$nas/"
(cd "$nas" && sha256sum -c --quiet "$task/SHA256SUMS")
cp "$task/SHA256SUMS" "$task/eye.json" "$nas/"
rm -rf "$raw"
echo "raw data verified on $nas"
