#!/usr/bin/env bash
# A2F_HOST/A2F_PORT come from GOTHIC_A2F_URL through scripts/deploy-gpu.sh; A2F_MODEL_DIR from the unit.
set -euo pipefail
cd "$(dirname "$0")/.."
exec .venv/bin/gothic-a2f serve \
  --host "${A2F_HOST:-127.0.0.1}" --port "${A2F_PORT:-47839}" --lib native/build/liba2f_engine.so "$@"
