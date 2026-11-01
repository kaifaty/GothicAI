#!/usr/bin/env bash
# Generate every move folder of a probe directory with Kimodo on dev-ubuntu.
#
#   scripts/content/anim/run_kimodo.sh <dir on dev-ubuntu with move folders from kimodo_moves.py>
#
# Kimodo lives in ~/gothic-ai/kimodo (its own venv, no service). The text encoder (LLM2Vec on
# Meta-Llama-3-8B-Instruct, local copies in ~/gothic-ai/kimodo/models: kimodo_textencoder_local.py)
# runs once on the CPU in bf16 (~16 GB RAM, memory capped so the user's containers are safe) and
# serves every generation on the temporary-server port 47833, bound to localhost. Generation itself
# takes <3 GB of video memory next to the running services.
set -euo pipefail
dir="${1:?probe dir}"
K="$HOME/gothic-ai/kimodo"
export TEXT_ENCODER_DEVICE=cpu GRADIO_SERVER_NAME=127.0.0.1 GRADIO_SERVER_PORT=47833
export TEXT_ENCODER_URL=http://127.0.0.1:47833/ TEXT_ENCODER_MODE=api
free_mb=$(nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits | head -1)
[ "$free_mb" -ge 4000 ] || { echo "only ${free_mb} MiB of video memory free" >&2; exit 75; }

systemd-run --user --scope --quiet -p MemoryMax=22G "$K/.venv/bin/python" "$K/kimodo_textencoder_local.py" \
  >"$dir/textencoder.log" 2>&1 &
srv=$!
trap 'kill "$srv" 2>/dev/null || true' EXIT
for _ in $(seq 1 120); do
  curl -sf "$TEXT_ENCODER_URL" >/dev/null && break
  kill -0 "$srv" 2>/dev/null || { echo "text encoder exited:" >&2; tail -20 "$dir/textencoder.log" >&2; exit 1; }
  sleep 5
done
curl -sf "$TEXT_ENCODER_URL" >/dev/null || { echo "text encoder did not come up" >&2; exit 1; }

for d in "$dir"/*/; do
  [ -f "$d/meta.json" ] || continue
  name=$(basename "$d")
  echo "== $name"
  "$K/.venv/bin/kimodo_gen" --input_folder "$d" --output "$d/out" >"$d/gen.log" 2>&1 \
    || { echo "failed: $name" >&2; tail -20 "$d/gen.log" >&2; }
done
echo "done"
