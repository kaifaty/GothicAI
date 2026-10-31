#!/usr/bin/env bash
# Record independent, identically seeded OFF/ON processes, then make review pairs.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/.bench/monster-character-virtual-videos}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
export LC_ALL=C
export GOTHIC_AI=0
HOST="${BENCH_HOST:-ryzen}"
SCENES=(wolf_terrain wolf_stairs wolf_pack troll_props cliff wolf_water fieldraider_bloodfly)
if [[ -n "${SCENE:-}" ]]; then SCENES=("$SCENE"); fi
python3 - "$ROOT/scripts/bench/monster-character-virtual-videos.bench" "$OUT" <<'PY'
from pathlib import Path
import sys
source, output = map(Path, sys.argv[1:])
prelude, *scenes = source.read_text().split('# scene ')
(output / 'scenarios').mkdir(exist_ok=True)
for scene in scenes:
    name, commands = scene.split('\n', 1)
    (output / 'scenarios' / f'monster-character-virtual-{name}.bench').write_text(prelude + commands)
PY
wait_mac() {
  if [[ "${NO_BUILD:-1}" != 0 && ! -x "${GAME_BIN:-$ROOT/OpenGothic/build/opengothic/Gothic2Notr}" ]]; then
    echo 'Build Gothic2Notr before recording (or set NO_BUILD=0).' >&2
    return 2
  fi
  until ! pgrep -f '^[^ ]*python[0-9.]* [^ ]*bin/pytest|^[^ ]*Gothic2Notr' >/dev/null; do sleep 5; done
  pgrep -fl Gothic2Notr >"$OUT/mac-process-check.log" || echo 'no game' >"$OUT/mac-process-check.log"
}
for scene in "${SCENES[@]}"; do
  scenario="$OUT/scenarios/monster-character-virtual-$scene.bench"
  [[ -f "$scenario" ]] || { echo "unknown scene: $scene" >&2; exit 2; }
  for take in off on; do
    enabled=0
    [[ "$take" != on ]] || enabled=1
    destination="$OUT/$scene-$take"
    # An auto launcher may fall back to Mac, so check its window beforehand too.
    [[ "$HOST" == ryzen ]] || wait_mac
    rc=0
    env BENCH_HOST="$HOST" RYZEN_LOCK_WAIT="${RYZEN_LOCK_WAIT:-120}" WORLD=NEWWORLD.ZEN \
      MOVABLE_VOBS=1 MONSTER_CHARACTER_VIRTUAL="$enabled" OPENGOTHIC_BENCH_TAKE="$take" \
      NO_BUILD="${NO_BUILD:-1}" TEMPEST_WINDOW_SIZE="${TEMPEST_WINDOW_SIZE:-1280x720}" \
      TIMEOUT="${TIMEOUT:-600}" "$ROOT/scripts/bench.sh" "$scenario" "$destination" || rc=$?
    if [[ "$rc" == 75 ]]; then
      HOST=local
      wait_mac
      env BENCH_HOST=local WORLD=NEWWORLD.ZEN MOVABLE_VOBS=1 MONSTER_CHARACTER_VIRTUAL="$enabled" \
        OPENGOTHIC_BENCH_TAKE="$take" NO_BUILD="${NO_BUILD:-1}" \
        TEMPEST_WINDOW_SIZE="${TEMPEST_WINDOW_SIZE:-1280x720}" TIMEOUT="${TIMEOUT:-600}" \
        "$ROOT/scripts/bench.sh" "$scenario" "$destination"
    elif [[ "$rc" != 0 ]]; then
      exit "$rc"
    fi
    [[ -s "$destination/$scene.mp4" && -s "$destination/events.jsonl" ]] || {
      echo "incomplete native recording: $destination" >&2; exit 1;
    }
  done
  for take in off on; do
    ffmpeg -v error -y -i "$OUT/$scene-$take/$scene.mp4" -frames:v 1 "$OUT/$scene-$take/first-frame.png"
  done
  mkdir -p "$OUT/pairs"
  python3 "$ROOT/scripts/monster_character_virtual_videos.py" metrics \
    --off "$OUT/$scene-off" --on "$OUT/$scene-on" \
    >"$OUT/pairs/$scene-metrics.json"
  uv run "$ROOT/scripts/monster_character_virtual_videos.py" compose \
    --off "$OUT/$scene-off/$scene.mp4" --on "$OUT/$scene-on/$scene.mp4" \
    --out "$OUT/pairs/$scene.mp4"
done
