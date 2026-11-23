#!/usr/bin/env bash
# Prepare installation content before starting the selected game executable.
set -euo pipefail
[[ $# == 1 ]] || { echo 'usage: base-ensure.sh <gothic dir>' >&2; exit 2; }
[[ -z "${OPENGOTHIC_BASE:-}${OPENGOTHIC_BASE_FINGERPRINT:-}" ]] || exit 0
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME_BIN="${GAME_BIN:-$ROOT/OpenGothic/build/opengothic/Gothic2Notr}"
CONTENT_CONVERT="${CONTENT_CONVERT:-$(dirname "$GAME_BIN")/content-convert}"
[[ -x "$CONTENT_CONVERT" ]] || { echo "base-ensure: missing content-convert: $CONTENT_CONVERT" >&2; exit 1; }
exec "$CONTENT_CONVERT" resolve "$1" >&2
