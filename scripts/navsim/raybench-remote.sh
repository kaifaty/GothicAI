#!/usr/bin/env bash
# Build and measure only the standalone libraries, never the game.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
stage="${1:?Usage: raybench-remote.sh selftest|run|scaling}"
case "$stage" in selftest|run|scaling) ;; *) echo "Unknown phase: $stage" >&2; exit 2;; esac
task="$(ssh -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-sim-research/raybench" "$HOME"')"
ssh ryzen "mkdir -p '$task/source/OpenGothic/lib' '$task/source/scripts/navsim'"
rsync -a --exclude=.git --exclude=__pycache__ "$root/OpenGothic/lib/JoltPhysics" "ryzen:$task/source/OpenGothic/lib/"
rsync -a --exclude=__pycache__ "$root/scripts/navsim/raybench" "$root/scripts/navsim/formats.py" "ryzen:$task/source/scripts/navsim/"
export GQ_OWNER=navsim-raybench
# This helper lives on the Mac; it cancels only this invocation's queue job.
deadline_helper="$HOME/.codex/checks/gothic/gq-deadline.sh"
# A direct manual invocation also has a bounded queue wait (30 minutes).
export CODEX_TASK_CHECK_DEADLINE="${CODEX_TASK_CHECK_DEADLINE:-$(( $(date +%s) + 2400 ))}"
threads=4;memory=4G;gpu=0G;job_class=quick;max_time=2h
if [[ "$stage" != selftest ]]; then
  threads=28;memory=5G
  [[ "$stage" != run ]] || gpu=3G
  # quiet must fit in ~30 minutes; otherwise preserve the competing-job snapshot.
  status="$("$root/scripts/gq" status)"
  if ! printf '%s\n' "$status" | rg -q 'running|queued'; then job_class=quiet; fi
fi
if [[ "$stage" == scaling ]]; then
  max_time=30m
  quiet_log="$(mktemp "${TMPDIR:-/tmp}/raybench-quiet.XXXXXX")"
  trap 'rm -f "$quiet_log"' EXIT
  # The deadline helper rejects a promised queue wait over 30 minutes immediately.
  quiet_deadline=$(( $(date +%s) + 2400 ))
  if (( CODEX_TASK_CHECK_DEADLINE < quiet_deadline )); then quiet_deadline=$CODEX_TASK_CHECK_DEADLINE; fi
  if CODEX_TASK_CHECK_DEADLINE="$quiet_deadline" CHECK_MIN_WORK=600 \
      bash "$deadline_helper" "$root/scripts/gq" run --class quiet --threads "$threads" \
      --mem "$memory" --max 30m --name 'navsim raybench scaling quiet' \
      -- bash "$task/source/scripts/navsim/raybench/phase.sh" "$stage" "$threads" quiet 2>"$quiet_log"; then
    job_class=quiet
  else
    rc=$?;cat "$quiet_log" >&2
    [[ "$rc" == 75 ]] || exit "$rc"
    ssh ryzen 'mkdir -p /mnt/shared/gothic-ai/nav-sim-research/raybench'
    rsync -a "$quiet_log" ryzen:/mnt/shared/gothic-ai/nav-sim-research/raybench/quiet-attempt.txt
    job_class=quick
  fi
fi
if [[ "$stage" != scaling || "$job_class" != quiet ]]; then
  bash "$deadline_helper" "$root/scripts/gq" run --class "$job_class" --threads "$threads" \
    --mem "$memory" --gpu-mem "$gpu" --max "$max_time" --name "navsim raybench $stage" \
    -- bash "$task/source/scripts/navsim/raybench/phase.sh" "$stage" "$threads" "$job_class"
fi
if [[ "$stage" != selftest ]]; then
  mkdir -p "$root/reports/navigation/2026-11-04-nav-sim-research/data/rays"
  rsync -a 'ryzen:/mnt/shared/gothic-ai/nav-sim-research/raybench/standalone.json' \
    "$root/reports/navigation/2026-11-04-nav-sim-research/data/rays/standalone.json"
fi
