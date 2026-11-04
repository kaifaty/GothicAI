#!/usr/bin/env bash
# Build and check the nav-sim stand library on Ryzen through gq (docs/nav-sim.md, S1).
#   stand-remote.sh selftest  # Embree 4.4.0, the library, test_navsim_stand_eye.py
#   stand-remote.sh run       # plus acceptance 2 on the engine's rays.bin and the cost on 1 and 8 threads
#   stand-remote.sh cost 28   # the cost on 28 threads (its own job: the whole free part of Ryzen)
#   stand-remote.sh clean     # remove ~/gothic-ai/nav-sim-stand on Ryzen
# Results accumulate in /mnt/shared/gothic-ai/nav-sim-research/stand/eye/ (sha256-checked); run and cost
# refresh eye.json -> reports/navigation/2026-11-04-nav-sim-research/data/stand/.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
stage="${1:?Usage: stand-remote.sh selftest|run|cost THREADS|clean}"
case "$stage" in selftest|run|cost|clean) ;; *) echo "Unknown stage: $stage" >&2; exit 2;; esac
task="$(ssh -o ConnectTimeout=10 ryzen 'printf "%s/gothic-ai/nav-sim-stand" "$HOME"')"
if [[ "$stage" == clean ]]; then ssh ryzen "rm -rf '$task'"; exit 0; fi
src="$task/source"
ssh ryzen "rm -rf '$src' && mkdir -p '$src'"
# Only what the library and its checks read: the eye core, Tempest math, the stand, formats, gpumem layout.
(cd "$root" && rsync -aR --exclude=__pycache__ --exclude=build \
  OpenGothic/common/game/navigationeye.h OpenGothic/common/game/navigationeye.cpp \
  OpenGothic/lib/Tempest/Engine/include/Tempest/Vec OpenGothic/lib/Tempest/Engine/utility/utility.h \
  scripts/navsim/formats.py scripts/navsim/stand scripts/navsim/gpumem \
  scripts/tests/test_navsim_stand_eye.py "ryzen:$src/")
export GQ_OWNER=nav-sim-stand
# The queue wait is bounded: 30 minutes (STAND_WAIT_MAX), or a codex-worker check's deadline
# through its helper on the Mac, which cancels only this invocation's job.
queue=("$root/scripts/gq" run --wait-max "${STAND_WAIT_MAX:-1800}")
if [[ -n "${CODEX_TASK_CHECK_DEADLINE:-}" ]]; then
  queue=(bash "$HOME/.codex/checks/gothic/gq-deadline.sh" "$root/scripts/gq" run)
fi
threads=4;memory=3G;job_class=quick;max_time=30m
if [[ "$stage" != selftest ]]; then
  threads=8;memory=6G;max_time=1h
  [[ "$stage" != cost ]] || threads="${2:?cost THREADS}"
  # A quiet machine for the cost when nobody else is queued; otherwise quick, with the snapshot kept.
  if ! "$root/scripts/gq" status | grep -Eq 'running|queued'; then job_class=quiet; fi
fi
"${queue[@]}" --class "$job_class" --threads "$threads" \
  --mem "$memory" --max "$max_time" --name "navsim stand $stage" \
  -- bash "$src/scripts/navsim/stand/phase.sh" "$stage" "$threads" "$job_class"
if [[ "$stage" != selftest ]]; then
  out="$root/reports/navigation/2026-11-04-nav-sim-research/data/stand"
  mkdir -p "$out"
  rsync -a "ryzen:/mnt/shared/gothic-ai/nav-sim-research/stand/eye/eye.json" "$out/eye.json"
fi
