#!/usr/bin/env bash
# Compare two staged content sets on the Ryzen performance stand.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 "$ROOT/scripts/perf_content.py" "$@"
