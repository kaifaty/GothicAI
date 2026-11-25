#!/usr/bin/env bash
# Forest price probe on one Ryzen build, with two rounds and then the VSM counters.
# Usage: run.sh /absolute/local/out [variant...]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
OUT="${1:?usage: run.sh /absolute/local/out [variant...]}"
shift

variants=(
  "C|"
  "F64|"
  "F256|"
  "F1024|"
  "F1024h|"
  "F1024q|"
  "F1024e|"
  "F1024-rt0|OPENGOTHIC_RT=0"
  "C-rt0|OPENGOTHIC_RT=0"
  "F1024-wind0|OPENGOTHIC_FOLIAGE=light"
  "F1024-crown0|OPENGOTHIC_CROWN_LIGHT=0"
)
variant() {
  local name="$1" set
  case "${name%%-*}" in
    C) set=tree-cost-0 ;;
    F64) set=tree-cost-64 ;;
    F256) set=tree-cost-256 ;;
    F1024) set=tree-cost-1024 ;;
    F1024h) set=tree-cost-1024-h ;;
    F1024q) set=tree-cost-1024-q ;;
    F1024e) set=tree-cost-1024-e ;;
    *) echo "tree-cost: unknown forest variant: $name" >&2; return 2 ;;
  esac
  printf '%s|OPENGOTHIC_CONTENT=%s OPENGOTHIC_GPU_PASSES=1%s\n' \
    "$name" "'\${HOME}/gothic-ai/tree-cost/content/$set'" "${2:+ $2}"
}
timed() {
  local row
  for row in "${variants[@]}"; do variant "${row%%|*}" "${row#*|}"; done
}
counters() {
  local name seen=' '
  if [[ $# == 0 ]]; then
    set -- F1024-vsmstats F64-vsmstats F256-vsmstats F1024h-vsmstats F1024q-vsmstats F1024e-vsmstats
  fi
  for name in "$@"; do
    [[ "$name" == *-vsmstats && "$seen" != *" $name "* ]] || continue
    variant "$name" OPENGOTHIC_VSM_STATS=1 || return
    seen+="$name "
  done
}
PERF_VARIANTS="$(timed)"
PERF_COUNTERS="$(counters "$@")"
BENCH_HOST=ryzen PERF_WINDOW=2560x1440 PERF_VID_RES=0 PERF_REPEAT=1 PERF_WAIT=600 PERF_ARGS= PERF_ENV= PERF_INI= \
  NO_BUILD="${NO_BUILD:+1}" RYZEN_LOCK_WAIT=7200 PERF_OUT="$OUT" \
  PERF_VARIANTS="$PERF_VARIANTS" PERF_COUNTERS="$PERF_COUNTERS" PERF_ONLY="$(printf '%s\n' "$@")" \
  exec "$ROOT/scripts/perf.sh" trees
