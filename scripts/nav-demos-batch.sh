#!/usr/bin/env bash
# Short Ryzen batches: gq batch for collection/replay/DAgger, quiet for performance.
# Without gq, retain the shared simulation lock.
# Example (absolute paths):
# scripts/nav-demos-batch.sh collect --runner /.../gothic-body-runner \
#   --assets /.../assets --save /.../save.sav --cache /.../navmesh-cache --resume
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ -r "$ROOT/scripts/gq-job.sh" ]]; then
  source "$ROOT/scripts/gq-job.sh"
else
  if command -v "${GQ_BIN:-$HOME/gothic-ai/bin/gq}" >/dev/null 2>&1; then
    echo 'Copy gq-job.sh into the scripts directory beside this launcher' >&2
    exit 2
  fi
  gq_available() { return 1; }
fi
[[ "$(uname -s)" == Linux ]] || { echo 'Run nav-demos-batch.sh on Ryzen/Linux' >&2; exit 2; }
[[ $# -gt 0 ]] || { python3 "$ROOT/scripts/collect_nav_demos.py" --help; exit 2; }
mode="$1"
shift
batch_size="${NAV_DEMOS_BATCH_SIZE:-2}"
lock_wait="${RYZEN_LOCK_WAIT:-900}"
pause_seconds="${NAV_DEMOS_PAUSE_SECONDS:-1}"
parallel=1
watchdog_seconds=300
collector_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --batch-size) batch_size="$2"; shift 2 ;;
    --lock-wait) lock_wait="$2"; shift 2 ;;
    --pause-seconds) pause_seconds="$2"; shift 2 ;;
    --watchdog-seconds) watchdog_seconds="$2"; collector_args+=("$1" "$2"); shift 2 ;;
    --watchdog-seconds=*) watchdog_seconds="${1#*=}"; collector_args+=("$1"); shift ;;
    --parallel) parallel="$2"; collector_args+=("$1" "$2"); shift 2 ;;
    --parallel=*) parallel="${1#*=}"; collector_args+=("$1"); shift ;;
    *) collector_args+=("$1"); shift ;;
  esac
done
[[ "$batch_size" =~ ^[1-9][0-9]*$ && "$lock_wait" =~ ^[0-9]+$ && "$pause_seconds" =~ ^[0-9]+$ ]] || {
  echo 'batch-size must be positive; lock-wait and pause-seconds must be nonnegative seconds' >&2; exit 2;
}
[[ "$parallel" == 1 || "$mode" == performance ]] || {
  echo 'Concurrent runners are reserved for short performance measurements' >&2; exit 2;
}
if gq_available; then
  # Absolute launcher invocation can start outside the task; retain its identity
  # while preserving the caller's cwd for relative collector arguments.
  if [[ -z "${GQ_OWNER:-}" && "$ROOT" == "$HOME/gothic-ai/"* ]]; then
    task_path="${ROOT#"$HOME/gothic-ai/"}"
    export GQ_OWNER="${task_path%%/*}"
  fi
  echo 'gq: --lock-wait/RYZEN_LOCK_WAIT are compatibility options; batch queue waiting has no timeout.'
else
  command -v flock >/dev/null
fi
mkdir -p "$HOME/gothic-ai"
# Resolve all selection before taking the lock; replay keeps original pair indices.
pending="$(python3 "$ROOT/scripts/collect_nav_demos.py" "$mode" --list-indices "${collector_args[@]}")"
read -r -a indices <<< "$pending"
if [[ ${#indices[@]} == 0 ]]; then
  echo 'All selected episodes are already complete.'
  exit 0
fi
if [[ "$mode" == performance ]]; then
  # One quiet request (or legacy lock) covers the parallel 10-second spans.
  batch_size="${#indices[@]}"
fi
for ((first = 0; first < ${#indices[@]}; first += batch_size)); do
  batch=("${indices[@]:first:batch_size}")
  joined="$(IFS=,; echo "${batch[*]}")"
  if gq_available; then
    job_class=batch
    [[ "$mode" != performance ]] || job_class=quiet
    active_workers=$parallel
    ((active_workers <= ${#batch[@]})) || active_workers=${#batch[@]}
    # Include navmesh preparation (720 s), episode watchdogs, and driver overhead.
    max_seconds=$(python3 -c 'import math,sys; print(min(7200, math.ceil(840 + math.ceil(int(sys.argv[1])/int(sys.argv[2]))*float(sys.argv[3]))))' \
      "${#batch[@]}" "$active_workers" "$watchdog_seconds")
    memory_gib=$(((active_workers * 7 + 9) / 10 + 2))
    df -h "$HOME"
    gq_job --class "$job_class" --threads "$((active_workers + 1))" --mem "${memory_gib}G" \
      --max "${max_seconds}s" --name "nav demos $mode indices=$joined" -- \
      python3 -I "$ROOT/scripts/nav_job.py" --max-seconds "$max_seconds" -- \
      python3 -X faulthandler -I "$ROOT/scripts/collect_nav_demos.py" "$mode" "${collector_args[@]}" --indices "$joined"
  else
    # A subshell releases fd9 even if collection fails.
    (
      exec 9>"$HOME/gothic-ai/body-simulation.lock"
      echo "nav-demos: waiting for body-simulation.lock; $mode indices=$joined"
      flock -w "$lock_wait" 9 || { echo 'Ryzen занят' >&2; exit 75; }
      df -h "$HOME"
      python3 -I "$ROOT/scripts/collect_nav_demos.py" "$mode" "${collector_args[@]}" --indices "$joined"
    )
  fi
  # Yield between batches, also when using the legacy lock.
  if ((first + batch_size < ${#indices[@]})); then
    sleep "$pause_seconds"
  fi
done
