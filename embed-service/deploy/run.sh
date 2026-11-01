#!/usr/bin/env bash
# EMBED_HOST/EMBED_PORT come from GOTHIC_EMBED_URL through scripts/deploy-gpu.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
export PYTORCH_CUDA_ALLOC_CONF=expandable_segments:True
exec .venv/bin/gothic-embed serve \
  --host "${EMBED_HOST:-127.0.0.1}" --port "${EMBED_PORT:-47837}" "$@"
