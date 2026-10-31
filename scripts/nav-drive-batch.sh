#!/usr/bin/env bash
# Submit each small policy-drive batch to gq; retain the common lock without gq.
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
[[ "$(uname -s)" == Linux ]] || { echo 'Run nav-drive-batch.sh on Ryzen/Linux' >&2; exit 2; }
batch_size="${NAV_DRIVE_BATCH_SIZE:-2}"
lock_wait="${RYZEN_LOCK_WAIT:-900}"
pause_seconds="${NAV_DRIVE_PAUSE_SECONDS:-1}"
watchdog_seconds=900
teacher_beta=0
drive_kind=drive
collector_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --batch-size) batch_size="$2"; shift 2 ;;
    --lock-wait) lock_wait="$2"; shift 2 ;;
    --pause-seconds) pause_seconds="$2"; shift 2 ;;
    --watchdog-seconds) watchdog_seconds="$2"; collector_args+=("$1" "$2"); shift 2 ;;
    --watchdog-seconds=*) watchdog_seconds="${1#*=}"; collector_args+=("$1"); shift ;;
    --teacher-beta) teacher_beta="$2"; collector_args+=("$1" "$2"); shift 2 ;;
    --teacher-beta=*) teacher_beta="${1#*=}"; collector_args+=("$1"); shift ;;
    --dagger-collection) drive_kind=dagger; collector_args+=("$1"); shift ;;
    *) collector_args+=("$1"); shift ;;
  esac
done
[[ "$batch_size" =~ ^[23]$ && "$lock_wait" =~ ^[0-9]+$ && "$pause_seconds" =~ ^[0-9]+$ ]] || {
  echo 'batch-size must be 2 or 3; lock-wait/pause-seconds nonnegative seconds' >&2; exit 2;
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
pending="$(python3 -I "$ROOT/scripts/nav_drive.py" collect --list-indices "${collector_args[@]}")"
read -r -a indices <<< "$pending"
if [[ ${#indices[@]} == 0 ]]; then
  echo 'All selected episodes are complete.'
  exit 0
fi
for ((first = 0; first < ${#indices[@]}; first += batch_size)); do
  batch=("${indices[@]:first:batch_size}")
  joined="$(IFS=,; echo "${batch[*]}")"
  if gq_available; then
    # One runner plus driver, ~0.7 GiB for the runner plus the Python/base budget.
    # Include navmesh preparation (720 s), episode watchdogs, and driver overhead.
    max_seconds=$(python3 -c 'import math,sys; print(min(7200, math.ceil(840 + int(sys.argv[1])*float(sys.argv[2]))))' \
      "${#batch[@]}" "$watchdog_seconds")
    df -h "$HOME"
    gq_job --class batch --threads 2 --mem 3G --max "${max_seconds}s" \
      --name "nav $drive_kind beta=$teacher_beta indices=$joined" -- \
      python3 -I "$ROOT/scripts/nav_job.py" --max-seconds "$max_seconds" -- \
      python3 -X faulthandler -I "$ROOT/scripts/nav_drive.py" collect "${collector_args[@]}" --indices "$joined"
  else
    (
      exec 9>"$HOME/gothic-ai/body-simulation.lock"
      echo "nav-drive: waiting for body-simulation.lock; indices=$joined"
      flock -w "$lock_wait" 9 || { echo 'Ryzen занят' >&2; exit 75; }
      df -h "$HOME"
      python3 -I "$ROOT/scripts/nav_drive.py" collect "${collector_args[@]}" --indices "$joined"
    )
  fi
  if ((first + batch_size < ${#indices[@]})); then
    sleep "$pause_seconds"
  fi
done
