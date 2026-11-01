#!/usr/bin/env bash
# Manual live-model suites: docs/e2e.md. This runner is outside automatic regression.
# SUITE=haggle RUNS=3 LLM=chatgpt [CASES=a,b] [OUT=/path] scripts/e2e-live.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export SUITE="${SUITE:?Set SUITE to a live scenario (see docs/e2e.md)}"
export RUNS="${RUNS:-3}"
case "${LLM:-}" in
  ''|off|fake) echo 'e2e-live requires a live LLM; set LLM=chatgpt (off/fake are not live)' >&2; exit 2 ;;
esac
if [[ "$SUITE" == interject && -z "${DECIDER_URL:-}" ]]; then
  echo 'interject requires DECIDER_URL for the live behavioral model' >&2
  exit 2
fi
if [[ ! "$RUNS" =~ ^[1-9][0-9]*$ ]]; then
  echo 'RUNS must be a positive integer' >&2
  exit 2
fi
export OUT="${OUT:-$ROOT/.live/$SUITE-$(date +%Y%m%d-%H%M%S)}"
if [[ -e "$OUT/run-1" || -e "$OUT/report.json" ]]; then
  echo 'OUT already contains live results; choose an empty directory' >&2
  exit 2
fi
export LIVE_HISTORY="$ROOT/.live"
(cd "$ROOT/server" && uv run python -c 'from gothic_ai.scenarios.live import main; main()' estimate)
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
export OUT
export E2E_EXIT_CODES=""
for ((run=1; run<=RUNS; run++)); do
  until ! LC_ALL=C pgrep -f '^(/[^ ]*/)?Gothic2Notr( |$)' >/dev/null; do sleep 5; done
  directory="$OUT/run-$run"
  mkdir -p "$directory"
  echo "Live run $run/$RUNS: $directory"
  code=0
  SCENARIO="$SUITE" LOG_DIR="$directory" "$ROOT/scripts/e2e.sh" || code=$?
  E2E_EXIT_CODES+=" $code"
done
(cd "$ROOT/server" && uv run python -c 'from gothic_ai.scenarios.live import main; main()' report)
