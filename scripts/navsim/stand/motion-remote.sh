#!/usr/bin/env bash
# Build and check the nav-sim stand motion on Ryzen through gq (docs/nav-sim.md, S2).
#   motion-remote.sh selftest           # Embree 4.4.0 + Jolt, the library, the stand's pytest (motion and eye)
#   motion-remote.sh accept [THREADS]   # plus acceptance 1 on the held-out corpus episodes (8 threads)
#   motion-remote.sh cost [LIST]        # cost of one decision of 2 048 agents (1,8,28 threads; 28 declared)
#   motion-remote.sh summary RAW...     # motion.json from NAS raw folders (NAS seen from the Mac)
#   motion-remote.sh clean              # remove the build, venv and cache in ~/gothic-ai/nav-sim-stand-motion
# Each accept or cost run writes ~/gothic-ai/nav-sim-research/stand/motion/<UTC>-<stage>/ on Ryzen;
# gq --archive moves it to /mnt/shared/gothic-ai/nav-sim-research/stand/motion/ with a sha256 check.
# MOTION_SUBMIT=1 submits the job and prints its id instead of waiting (then: scripts/gq wait ID).
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
stage="${1:?Usage: motion-remote.sh selftest|accept [THREADS]|cost [LIST]|summary RAW...|clean}"
case "$stage" in selftest|accept|cost|summary|clean) ;; *) echo "Unknown stage: $stage" >&2; exit 2;; esac
out="$root/reports/navigation/2026-11-04-nav-sim-research/data/stand/motion.json"
if [[ "$stage" == summary ]]; then
  shift
  local_nas=/Volumes/shared/gothic-ai
  raws=()
  for r in "$@"; do raws+=("$local_nas/${r#/mnt/shared/gothic-ai/}"); done
  exec python3 "$root/scripts/navsim/stand/motion_accept.py" summary "$out" "${raws[@]}"
fi
home="$(ssh -o ConnectTimeout=10 ryzen 'printf "%s" "$HOME"')"
task="$home/gothic-ai/nav-sim-stand-motion"
if [[ "$stage" == clean ]]; then ssh ryzen "rm -rf '$task'"; exit 0; fi
src="$task/source"
ssh ryzen "rm -rf '$src' && mkdir -p '$src'"
# Only what the library and its checks read: the eye core, Tempest math, Jolt, the stand, the corpus readers.
(cd "$root" && rsync -aR --exclude=__pycache__ --exclude=build \
  OpenGothic/common/game/navigationeye.h OpenGothic/common/game/navigationeye.cpp \
  OpenGothic/lib/Tempest/Engine/include/Tempest/Vec OpenGothic/lib/Tempest/Engine/utility/utility.h \
  OpenGothic/lib/JoltPhysics scripts/nav_demos.py scripts/navsim/motion_table.py scripts/navsim/formats.py \
  scripts/navsim/stand scripts/navsim/gpumem \
  scripts/tests/test_navsim_stand_eye.py scripts/tests/test_navsim_stand_motion.py "ryzen:$src/")
export GQ_OWNER=nav-sim-stand-motion
threads=4;memory=4G;max_time=30m;raw="";archive=();extra=()
case "$stage" in
  accept) threads="${2:-8}";memory=5G;max_time=1h ;;
  cost) threads=28;memory=4G;max_time=40m;extra=("${2:-1,8,28}") ;;
esac
if [[ "$stage" != selftest ]]; then
  raw="$home/gothic-ai/nav-sim-research/stand/motion/$(date -u +%Y%m%dT%H%M%SZ)-$stage"
  archive=(--archive "$raw")
fi
queue=(run --wait-max "${MOTION_WAIT_MAX:-1800}")
[[ -z "${MOTION_SUBMIT:-}" ]] || queue=(submit)
"$root/scripts/gq" "${queue[@]}" --class quick --threads "$threads" --mem "$memory" \
  --max "$max_time" --name "navsim motion $stage" ${archive[@]+"${archive[@]}"} \
  -- bash "$src/scripts/navsim/stand/motion-phase.sh" "$stage" "$threads" "$raw" ${extra[@]+"${extra[@]}"}
[[ -z "$raw" ]] || echo "raw: /mnt/shared/gothic-ai/${raw#"$home"/gothic-ai/}"
