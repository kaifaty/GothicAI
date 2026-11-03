#!/usr/bin/env bash
# LAUGHTER_HOST/LAUGHTER_PORT come from GOTHIC_LAUGHTER_URL through scripts/deploy-gpu.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
exec .venv/bin/gothic-laughter serve \
  --host "${LAUGHTER_HOST:-127.0.0.1}" --port "${LAUGHTER_PORT:-47838}" "$@"
