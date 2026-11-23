#!/usr/bin/env bash
# Paired real-engine test using the existing offline TTS scenario on Ryzen.
# Usage: scripts/check-voice-stem.sh [absolute-output-directory]
# For a Mac fallback: VOICE_STEM_HOST=local scripts/check-voice-stem.sh <out>
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/.bench/voice-stem-$(date -u +%Y%m%dT%H%M%SZ)}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
RUNNER=(bash "$ROOT/scripts/ryzen.sh" e2e)
if [[ "${VOICE_STEM_HOST:-ryzen}" == local ]]; then
  RUNNER=(bash "$ROOT/scripts/e2e.sh")
fi

for take in control stem; do
  mkdir -p "$OUT/$take"
  OPTIONS=(-u OPENGOTHIC_BENCH_VOICE_STEM SCENARIO=tts LLM=off TTS=fake GOTHIC_TTS_RECORD=1
    DECIDER_URL= QUICK_URL=
    "LOG_DIR=$OUT/$take")
  if [[ "$take" == stem ]]; then OPTIONS+=(OPENGOTHIC_BENCH_VOICE_STEM=1); fi
  env "${OPTIONS[@]}" "${RUNNER[@]}"
done
python3 "$ROOT/scripts/check-voice-stem.py" "$OUT/control" "$OUT/stem"
