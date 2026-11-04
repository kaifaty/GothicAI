#!/usr/bin/env bash
# Build and measure only the standalone libraries, never the game.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
stage="${1:?Usage: raybench-remote.sh selftest|run}"
case "$stage" in selftest|run) ;; *) echo "Unknown phase: $stage" >&2; exit 2;; esac
task="$(ssh -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-sim-research/raybench" "$HOME"')"
ssh ryzen "mkdir -p '$task/source/OpenGothic/lib' '$task/source/scripts/navsim'"
rsync -a --exclude=.git --exclude=__pycache__ "$root/OpenGothic/lib/JoltPhysics" "ryzen:$task/source/OpenGothic/lib/"
rsync -a --exclude=__pycache__ "$root/scripts/navsim/raybench" "$root/scripts/navsim/formats.py" "ryzen:$task/source/scripts/navsim/"
export GQ_OWNER=navsim-raybench
# This helper lives on the Mac; it cancels only this invocation's queue job.
deadline_helper="$HOME/.codex/checks/gothic/gq-deadline.sh"
# A direct manual invocation also has a bounded queue wait (30 minutes).
export CODEX_TASK_CHECK_DEADLINE="${CODEX_TASK_CHECK_DEADLINE:-$(( $(date +%s) + 2400 ))}"
threads=4;memory=4G;gpu=0G;job_class=quick
if [[ "$stage" == run ]]; then
  threads=28;memory=5G;gpu=3G
  # quiet must fit in ~30 minutes; otherwise preserve the competing-job snapshot.
  status="$("$root/scripts/gq" status)"
  if ! printf '%s\n' "$status" | rg -q 'running|queued'; then job_class=quiet; fi
fi
bash "$deadline_helper" "$root/scripts/gq" run --class "$job_class" --threads "$threads" \
  --mem "$memory" --gpu-mem "$gpu" --max 2h --name "navsim raybench $stage" \
  -- bash "$task/source/scripts/navsim/raybench/phase.sh" "$stage" "$threads" "$job_class"
if [[ "$stage" == run ]]; then
  mkdir -p "$root/reports/navigation/2026-11-04-nav-sim-research/data/rays"
  rsync -a 'ryzen:/mnt/shared/gothic-ai/nav-sim-research/raybench/standalone.json' \
    "$root/reports/navigation/2026-11-04-nav-sim-research/data/rays/standalone.json"
fi
