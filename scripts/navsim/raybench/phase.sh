#!/usr/bin/env bash
set -euo pipefail
source_dir="$(cd "$(dirname "$0")" && pwd)"
task="$(cd "$source_dir/../../../.." && pwd)"
stage="${1:?phase}";threads="${2:?threads}";job_class="${3:?class}"
raw=/mnt/shared/gothic-ai/nav-sim-research/raybench
input=/mnt/shared/gothic-ai/nav-sim-research/rays
export PATH="$HOME/.local/bin:$PATH"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1
# All artifacts are this task's. Reference dumps and other jobs remain read-only.
trap 'rm -rf "$task/build" "$task/.venv"' EXIT
df -h "$HOME"
free_kb="$(df -Pk "$HOME" | awk 'NR==2 {print $4}')"
(( free_kb >= 20*1024*1024 )) || { echo 'Less than 20 GiB free' >&2; exit 75; }
mkdir -p "$raw" "$task/deps"
"$HOME/gothic-ai/bin/gq" status > "$raw/gq-status-$stage.txt"
date -u +%FT%TZ > "$raw/start-$stage.txt"
cat /proc/loadavg > "$raw/load-$stage.txt"
lscpu > "$raw/cpu.txt"
nvidia-smi --query-gpu=name,driver_version,memory.used --format=csv,noheader > "$raw/gpu.txt"
embree="$task/deps/embree-4.4.0"
if [[ ! -f "$embree/lib/cmake/embree-4.4.0/embree-config.cmake" ]]; then
  mkdir -p "$embree"
  curl --fail --location --retry 2 \
    https://github.com/RenderKit/embree/releases/download/v4.4.0/embree-4.4.0.x86_64.linux.tar.gz \
    -o "$task/deps/embree.tar.gz"
  sha256sum "$task/deps/embree.tar.gz" > "$raw/embree-archive.sha256"
  tar -xzf "$task/deps/embree.tar.gz" -C "$embree"
  rm "$task/deps/embree.tar.gz"
fi
export LD_LIBRARY_PATH="$embree/lib:${LD_LIBRARY_PATH:-}"
cmake -S "$source_dir" -B "$task/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$embree" -DCMAKE_CXX_FLAGS_RELEASE='-O3 -DNDEBUG -march=x86-64-v3 -ffp-contract=off'
cmake --build "$task/build" --target raybench --parallel 3
cp "$task/build/compile_commands.json" "$raw/compile_commands.json"
"$task/build/raybench" selftest | tee "$raw/selftest.json"
if [[ "$stage" == selftest ]]; then exit 0; fi
sha256sum "$input/static.bin" "$input/rays.bin" "$input/poses.txt" > "$raw/input.sha256"
uv venv "$task/.venv"
uv pip install --python "$task/.venv/bin/python" numpy
"$task/.venv/bin/python" "$source_dir/report.py" prepare "$input" "$raw"
"$task/build/raybench" "$input" "$raw" "$threads" > "$raw/cpu.jsonl"
# Warp installation/runtime failure is an allowed skip, with the exact log retained.
if uv pip install --python "$task/.venv/bin/python" warp-lang > "$raw/warp-install.log" 2>&1; then
  "$task/.venv/bin/python" "$source_dir/warp_bench.py" "$input" "$raw" > "$raw/warp.log" 2>&1 || \
    "$task/.venv/bin/python" "$source_dir/report.py" gpu-skip "$raw" runtime
else
  "$task/.venv/bin/python" "$source_dir/report.py" gpu-skip "$raw" install
fi
"$task/.venv/bin/python" "$source_dir/report.py" summarize "$input" "$raw" "$job_class"
