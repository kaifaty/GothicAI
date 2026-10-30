#!/usr/bin/env bash
# Runs a test-bench scenario inside the game: video with a debug overlay, per-frame metrics, a report.
# See scripts/bench/README.md.
#
#   scripts/bench.sh <scenario file | name from scripts/bench/> [out dir]
#
# env: GOTHIC_DIR    game data (default: /Volumes/DevSSD/Dev/games/Gothic II)
#      BENCH_HOST    auto (default): Ryzen first, ryzen: require Ryzen, local: this machine
#      RYZEN_HOST    SSH host for Ryzen (default: ryzen)
#      RYZEN_LOCK_WAIT seconds to wait for the worktree lock and gq (default: auto 120, ryzen 900)
#      RYZEN_MAX     gq job time limit, including the build (default: 30m)
#      RYZEN_BENCH_CLASS quick (default) or quiet for timing without other jobs
#      GAME_BIN      game executable (default: OpenGothic/build/opengothic/Gothic2Notr)
#      TEMPEST_WINDOW_SIZE client frame size (default: 800x600)
#      SAVE          save to start from (default: save_slot_1.sav in the main checkout)
#      WORLD         start a fresh game in this world instead (NEWWORLD.ZEN, OLDWORLD.ZEN, ADDONWORLD.ZEN, DRAGONISLAND.ZEN)
#      NO_BUILD=1    do not rebuild the engine first
#      TIMEOUT=300   seconds before the game is killed
#      KEEP=1        the game stays open after the scenario
#      SOUND, MUSIC  volumes 0..1 (default 0); with TEMPEST_AUDIO_LOOPBACK=48000 the sound of a segment
#                    goes into <segment>.wav instead of the speakers
#      MOVABLE_VOBS  optional initial GAME movable_vobs setting (0 or 1), before world loading
#      ITEM_HULLS    optional initial GAME item_hulls setting (0 or 1), for new item bodies
#      ARROW_STICKING optional initial GAME arrow_sticking setting (0 or 1), before world loading
#      PHYSICS_NOISE optional initial GAME physics_noise setting (0 or 1), before world loading
#      DESTRUCTION optional initial GAME destruction setting (0 or 1), before world loading
#      BUOYANCY     optional initial GAME buoyancy setting (0 or 1), before world loading
#      CLOTH        optional initial GAME cloth setting (0 or 1), before world loading
#      ITEM_WAKE    optional initial GAME item_wake setting (0 or 1), before world loading
#      CHARACTER_VIRTUAL optional initial GAME character_virtual setting (0 or 1), before world loading
#      NPC_CHARACTER_VIRTUAL optional initial GAME npc_character_virtual setting (0 or 1)
#      MONSTER_CHARACTER_VIRTUAL optional initial GAME monster_character_virtual setting (0 or 1)
#      MELEE_PHYSICS_HITS optional initial GAME melee_physics_hits setting (0 or 1)
#      MELEE_POSTURE optional initial GAME melee_posture setting (0 or 1)
#      MELEE_DOWN_TACTICS optional initial GAME melee_down_tactics setting (0 or 1)
#      MELEE_HITSTOP optional initial GAME melee_hitstop setting (0 or 1)
#      MELEE_REACTIONS optional initial GAME melee_reactions setting (0 or 1)
#      MELEE_BLADE_SHADOW optional initial GAME melee_blade_shadow setting (0 or 1)
#      OPENGOTHIC_TEXTURES  sharper textures (default: those of scripts/hd-textures.sh, when made; empty: none)
#      OPENGOTHIC_CENSUS  session resource JSON (use an absolute path inside out dir for Ryzen)
#      OPENGOTHIC_FRACTURE  pre-cut meshes (default: ~/.local/share/gothic-ai/fracture, when made; empty: none)
#      OPENGOTHIC_CONTENT  material/mesh overrides (Ryzen: sync this directory; empty: none; unset: engine default)
#      OPENGOTHIC_BASE_CACHE, OPENGOTHIC_BASE_STORE  base cache/store on the selected host
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEFAULT_GOTHIC_DIR="/Volumes/DevSSD/Dev/games/Gothic II"
GOTHIC_DIR="${GOTHIC_DIR:-$DEFAULT_GOTHIC_DIR}"
BIN="$ROOT/OpenGothic/build/opengothic/Gothic2Notr"
GAME_BIN="${GAME_BIN:-$BIN}"
HD_TEXTURES="$HOME/.local/share/gothic-ai/hd-textures/textures"
FRACTURE_CACHE="$HOME/.local/share/gothic-ai/fracture"
BENCH_HOST="${BENCH_HOST:-auto}"
case "$BENCH_HOST" in
  auto|ryzen|local) ;;
  *) echo "bench: BENCH_HOST must be auto, ryzen or local" >&2; exit 2 ;;
esac

SCEN="${1:?usage: scripts/bench.sh <scenario> [out dir]}"
[[ -f "$SCEN" ]] || SCEN="$ROOT/scripts/bench/$SCEN.bench"
[[ -f "$SCEN" ]] || { echo "no scenario: $1" >&2; exit 2; }
SCEN="$(cd "$(dirname "$SCEN")" && pwd)/$(basename "$SCEN")"
NAME="$(basename "$SCEN" .bench)"
if [[ "$NAME" == streaming-flythrough || "$NAME" == streaming-shots ]]; then
  export WORLD="${WORLD:-NEWWORLD.ZEN}"
  export RYZEN_BENCH_CLASS="${RYZEN_BENCH_CLASS:-quiet}"
fi
NEEDS_CADENCE=0
if awk '$1 == "cadence" { found=1 } END { exit !found }' "$SCEN"; then NEEDS_CADENCE=1; fi
OUT="${2:-$ROOT/.bench/$NAME-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

if [[ "$BENCH_HOST" != local ]]; then
  LOCAL_REASON=""
  if [[ "${KEEP:-}" == 1 ]]; then
    LOCAL_REASON="KEEP=1 needs the local game"
  elif [[ "${OPENGOTHIC_BENCH_ENCODER:-}" == *videotoolbox* ]]; then
    LOCAL_REASON="the videotoolbox encoder needs macOS"
  elif [[ "$GOTHIC_DIR" != "$DEFAULT_GOTHIC_DIR" ]]; then
    LOCAL_REASON="GOTHIC_DIR uses local game data"
  else
    LOCAL_REASON="$(python3 - "$OUT" "$HD_TEXTURES" <<'PY'
import os
import sys

def path(value):
    return os.path.realpath(os.path.expanduser(value))

out, textures = map(path, sys.argv[1:])
for name, value in sorted(os.environ.items()):
    if not name.startswith(("OPENGOTHIC_", "GOTHIC_", "TEMPEST_")):
        continue
    if name in ("GOTHIC_DIR", "GOTHIC_AI_URL", "OPENGOTHIC_FRACTURE", "OPENGOTHIC_CONTENT", "OPENGOTHIC_BASE",
                "OPENGOTHIC_BASE_CACHE", "OPENGOTHIC_BASE_STORE"):
        continue
    if not value.startswith(("/", "~/")):
        continue
    value = path(value)
    if name == "OPENGOTHIC_TEXTURES" and value == textures:
        continue
    if os.path.commonpath((out, value)) == out:
        continue
    print(f"{name} points to a local path outside OUT")
    break
PY
)"
  fi
  if [[ -n "$LOCAL_REASON" ]]; then
    if [[ "$BENCH_HOST" == ryzen ]]; then
      echo "bench: cannot run on Ryzen: $LOCAL_REASON" >&2
      exit 2
    fi
    echo "bench: falling back to Mac: $LOCAL_REASON" >&2
  else
    if [[ "$BENCH_HOST" == auto ]]; then
      LOCK_WAIT="${RYZEN_LOCK_WAIT:-120}"
    else
      LOCK_WAIT="${RYZEN_LOCK_WAIT:-900}"
    fi
    rm -f -- "$OUT/game.log" "$OUT/host.txt"
    RYZEN_RC=0
    RYZEN_LOCK_WAIT="$LOCK_WAIT" "$ROOT/scripts/ryzen.sh" bench "$SCEN" "$OUT" 2>&1 | tee "$OUT/ryzen.log" \
      || RYZEN_RC="${PIPESTATUS[0]}"
    if [[ -f "$OUT/game.log" ]]; then
      printf 'ryzen\n' >"$OUT/host.txt"
      echo "bench: ran on ryzen"
      echo "$OUT"
      exit "$RYZEN_RC"
    fi
    if [[ "$BENCH_HOST" == ryzen ]]; then
      # A successful transport without a game log is still a failed launch.
      [[ "$RYZEN_RC" != 0 ]] || RYZEN_RC=1
      exit "$RYZEN_RC"
    fi
    LOCAL_REASON="$(awk 'NF && !/^Ryzen: removed / && !/^bench: output in / { line=$0 } END { print line }' "$OUT/ryzen.log")"
    echo "bench: falling back to Mac: ${LOCAL_REASON:-Ryzen did not start the game (exit $RYZEN_RC)}" >&2
  fi
fi

if [[ -n "${WORLD:-}" ]]; then
  START_ARGS=(-nomenu -w "$WORLD")
else
  if [[ -z "${SAVE:-}" ]]; then
    # worktrees share the save of the main checkout
    COMMON="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)"
    SAVE="$(dirname "$COMMON")/save_slot_1.sav"
  fi
  [[ -f "$SAVE" ]] || { echo "no save: $SAVE (set SAVE=...)" >&2; exit 2; }
  START_ARGS=(-save 1)
fi

# These fixtures use a prepared world and clock; skip the new-game chapter video
# before their first command on NEWWORLD.
if [[ "$NAME" == item-wake || "$NAME" == cloth* || "$NAME" == monster-character-virtual* || "$NAME" == own-mesh-* || "$NAME" == movable-census || "$NAME" == melee-physics || "$NAME" == melee-blade-shadow || "$NAME" == melee-posture-* || "$NAME" == streaming-flythrough || "$NAME" == streaming-shots || "$NAME" == loadprobe-idle || "$NAME" == loadprobe-chain ]]; then
  START_ARGS+=(-nointro)
fi

if [[ "${NO_BUILD:-}" != 1 ]]; then
  cmake --build "$ROOT/OpenGothic/build" --target Gothic2Notr >"$OUT/build.log" 2>&1 \
    || { tail -30 "$OUT/build.log"; exit 1; }
fi

# Normal-clock timing is compiled out of ordinary local builds. Refuse an
# unsupported fallback before opening a game instead of producing empty timings.
if [[ "${RYZEN_BENCH:-}" != 1 && "$NEEDS_CADENCE" == 1 ]]; then
  CADENCE_BUILD="$(dirname "$GAME_BIN")"
  CADENCE_SUPPORTED=0
  for ((i = 0; i < 4; i++)); do
    if [[ -f "$CADENCE_BUILD/CMakeCache.txt" ]]; then
      if grep -qx 'OPENGOTHIC_ENABLE_APP_CADENCE:BOOL=ON' "$CADENCE_BUILD/CMakeCache.txt"; then
        CADENCE_SUPPORTED=1
      fi
      break
    fi
    CADENCE_BUILD="$(dirname "$CADENCE_BUILD")"
  done
  if [[ "$CADENCE_SUPPORTED" != 1 ]]; then
    echo 'bench: cadence requires -DOPENGOTHIC_ENABLE_APP_CADENCE=ON; use BENCH_HOST=ryzen to wait for Ryzen' >&2
    exit 2
  fi
fi

# Chunk publication measurements need frame profiling as well as normal cadence.
if [[ "${RYZEN_BENCH:-}" != 1 && "$NAME" == streaming-flythrough ]]; then
  PROFILE_BUILD="$(dirname "$GAME_BIN")"
  PROFILE_SUPPORTED=0
  for ((i = 0; i < 4; i++)); do
    if [[ -f "$PROFILE_BUILD/CMakeCache.txt" ]]; then
      if grep -qx 'OPENGOTHIC_ENABLE_FRAME_PROFILE:BOOL=ON' "$PROFILE_BUILD/CMakeCache.txt"; then
        PROFILE_SUPPORTED=1
      fi
      break
    fi
    PROFILE_BUILD="$(dirname "$PROFILE_BUILD")"
  done
  if [[ "$PROFILE_SUPPORTED" != 1 ]]; then
    echo 'bench: streaming-flythrough requires -DOPENGOTHIC_ENABLE_FRAME_PROFILE=ON; use BENCH_HOST=ryzen to wait for Ryzen' >&2
    exit 2
  fi
fi

# the game reads Gothic.ini and saves from its working directory
RUN="$OUT/run"
mkdir -p "$RUN"
[[ -n "${WORLD:-}" ]] || cp "$SAVE" "$RUN/save_slot_1.sav"
# macOS captures half of its Retina backing size; X11 client sizes are already pixels.
VID_RES_INDEX=2
if [[ "$(uname -s)" == Linux ]]; then VID_RES_INDEX=0; fi
cat >"$RUN/Gothic.ini" <<EOF
[INTERNAL]
vidResIndex=$VID_RES_INDEX
[SOUND]
soundVolume=${SOUND:-0}
musicVolume=${MUSIC:-0}
musicEnabled=$([[ "${MUSIC:-0}" == 0 || "${MUSIC:-0}" == 0.0 ]] && echo 0 || echo 1)
worldSounds=${WORLD_SOUNDS:-1}
EOF
if [[ -n "${MOVABLE_VOBS:-}${ITEM_HULLS:-}${ARROW_STICKING:-}${PHYSICS_NOISE:-}${DESTRUCTION:-}${BUOYANCY:-}${CLOTH:-}${ITEM_WAKE:-}${CHARACTER_VIRTUAL:-}${NPC_CHARACTER_VIRTUAL:-}${MONSTER_CHARACTER_VIRTUAL:-}${MELEE_BLADE_SHADOW:-}${MELEE_PHYSICS_HITS:-}${MELEE_POSTURE:-}${MELEE_REACTIONS:-}${MELEE_DOWN_TACTICS:-}${MELEE_HITSTOP:-}" ]]; then
  printf '\n[GAME]\n' >>"$RUN/Gothic.ini"
  if [[ -n "${MOVABLE_VOBS:-}" ]]; then
    [[ "$MOVABLE_VOBS" == 0 || "$MOVABLE_VOBS" == 1 ]] || { echo "MOVABLE_VOBS must be 0 or 1" >&2; exit 2; }
    printf 'movable_vobs=%s\n' "$MOVABLE_VOBS" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${ITEM_HULLS:-}" ]]; then
    [[ "$ITEM_HULLS" == 0 || "$ITEM_HULLS" == 1 ]] || { echo "ITEM_HULLS must be 0 or 1" >&2; exit 2; }
    printf 'item_hulls=%s\n' "$ITEM_HULLS" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${ARROW_STICKING:-}" ]]; then
    [[ "$ARROW_STICKING" == 0 || "$ARROW_STICKING" == 1 ]] || { echo "ARROW_STICKING must be 0 or 1" >&2; exit 2; }
    printf 'arrow_sticking=%s\n' "$ARROW_STICKING" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${CHARACTER_VIRTUAL:-}" ]]; then
    [[ "$CHARACTER_VIRTUAL" == 0 || "$CHARACTER_VIRTUAL" == 1 ]] || { echo "CHARACTER_VIRTUAL must be 0 or 1" >&2; exit 2; }
    printf 'character_virtual=%s\n' "$CHARACTER_VIRTUAL" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${NPC_CHARACTER_VIRTUAL:-}" ]]; then
    [[ "$NPC_CHARACTER_VIRTUAL" == 0 || "$NPC_CHARACTER_VIRTUAL" == 1 ]] || { echo "NPC_CHARACTER_VIRTUAL must be 0 or 1" >&2; exit 2; }
    printf 'npc_character_virtual=%s\n' "$NPC_CHARACTER_VIRTUAL" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MONSTER_CHARACTER_VIRTUAL:-}" ]]; then
    [[ "$MONSTER_CHARACTER_VIRTUAL" == 0 || "$MONSTER_CHARACTER_VIRTUAL" == 1 ]] || { echo "MONSTER_CHARACTER_VIRTUAL must be 0 or 1" >&2; exit 2; }
    printf 'monster_character_virtual=%s\n' "$MONSTER_CHARACTER_VIRTUAL" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_PHYSICS_HITS:-}" ]]; then
    [[ "$MELEE_PHYSICS_HITS" == 0 || "$MELEE_PHYSICS_HITS" == 1 ]] || { echo "MELEE_PHYSICS_HITS must be 0 or 1" >&2; exit 2; }
    printf 'melee_physics_hits=%s\n' "$MELEE_PHYSICS_HITS" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_POSTURE:-}${MELEE_REACTIONS:-}" ]]; then
    [[ "$MELEE_POSTURE" == 0 || "$MELEE_POSTURE" == 1 ]] || { echo "MELEE_POSTURE must be 0 or 1" >&2; exit 2; }
    printf 'melee_posture=%s\n' "$MELEE_POSTURE" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_DOWN_TACTICS:-}" ]]; then
    [[ "$MELEE_DOWN_TACTICS" == 0 || "$MELEE_DOWN_TACTICS" == 1 ]] || { echo "MELEE_DOWN_TACTICS must be 0 or 1" >&2; exit 2; }
    printf 'melee_down_tactics=%s\n' "$MELEE_DOWN_TACTICS" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_HITSTOP:-}" ]]; then
    [[ "$MELEE_HITSTOP" == 0 || "$MELEE_HITSTOP" == 1 ]] || { echo "MELEE_HITSTOP must be 0 or 1" >&2; exit 2; }
    printf 'melee_hitstop=%s\n' "$MELEE_HITSTOP" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_REACTIONS:-}" ]]; then
    [[ "$MELEE_REACTIONS" == 0 || "$MELEE_REACTIONS" == 1 ]] || { echo "MELEE_REACTIONS must be 0 or 1" >&2; exit 2; }
    printf 'melee_reactions=%s\n' "$MELEE_REACTIONS" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_BLADE_SHADOW:-}" ]]; then
    [[ "$MELEE_BLADE_SHADOW" == 0 || "$MELEE_BLADE_SHADOW" == 1 ]] || { echo "MELEE_BLADE_SHADOW must be 0 or 1" >&2; exit 2; }
    printf 'melee_blade_shadow=%s\n' "$MELEE_BLADE_SHADOW" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${PHYSICS_NOISE:-}" ]]; then
    [[ "$PHYSICS_NOISE" == 0 || "$PHYSICS_NOISE" == 1 ]] || { echo "PHYSICS_NOISE must be 0 or 1" >&2; exit 2; }
    printf 'physics_noise=%s\n' "$PHYSICS_NOISE" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${DESTRUCTION:-}" ]]; then
    [[ "$DESTRUCTION" == 0 || "$DESTRUCTION" == 1 ]] || { echo "DESTRUCTION must be 0 or 1" >&2; exit 2; }
    printf 'destruction=%s\n' "$DESTRUCTION" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${BUOYANCY:-}" ]]; then
    [[ "$BUOYANCY" == 0 || "$BUOYANCY" == 1 ]] || { echo "BUOYANCY must be 0 or 1" >&2; exit 2; }
    printf 'buoyancy=%s\n' "$BUOYANCY" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${CLOTH:-}" ]]; then
    [[ "$CLOTH" == 0 || "$CLOTH" == 1 ]] || { echo "CLOTH must be 0 or 1" >&2; exit 2; }
    printf 'cloth=%s\n' "$CLOTH" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${ITEM_WAKE:-}" ]]; then
    [[ "$ITEM_WAKE" == 0 || "$ITEM_WAKE" == 1 ]] || { echo "ITEM_WAKE must be 0 or 1" >&2; exit 2; }
    printf 'item_wake=%s\n' "$ITEM_WAKE" >>"$RUN/Gothic.ini"
  fi
fi

# the sharper textures the launcher gives the game too (scripts/hd-textures.sh)
if [[ -z "${OPENGOTHIC_TEXTURES+set}" && -d "$HD_TEXTURES" ]]; then
  export OPENGOTHIC_TEXTURES="$HD_TEXTURES"
fi
if [[ -z "${OPENGOTHIC_FRACTURE+set}" && -d "$FRACTURE_CACHE" ]]; then
  export OPENGOTHIC_FRACTURE="$FRACTURE_CACHE"
fi

GAME_BIN="$GAME_BIN" bash "$ROOT/scripts/base-ensure.sh" "$GOTHIC_DIR"

BACKGROUND=1
if [[ "$NAME" == streaming-flythrough ]]; then
  export OPENGOTHIC_FRAME_PROFILE="$OUT/frameprofile.csv" OPENGOTHIC_BENCH_VISIBLE=1 OPENGOTHIC_LOADPROBE=1
  BACKGROUND=0
fi
START=$(date +%s)
(cd "$RUN" && exec env TEMPEST_BACKGROUND="$BACKGROUND" TEMPEST_WINDOW_SIZE="${TEMPEST_WINDOW_SIZE:-800x600}" \
  OPENGOTHIC_BENCH="$SCEN" OPENGOTHIC_BENCH_OUT="$OUT" ${KEEP:+OPENGOTHIC_BENCH_KEEP=1} \
  "$GAME_BIN" -g "$GOTHIC_DIR" -window -devmode "${START_ARGS[@]}") >"$OUT/game.log" 2>&1 &
PID=$!
for ((i = 0; i < ${TIMEOUT:-300} * 4; i++)); do
  kill -0 "$PID" 2>/dev/null || break
  sleep 0.25
done
if kill -0 "$PID" 2>/dev/null; then
  echo "bench: timeout, game killed" >&2
  kill "$PID"
fi
wait "$PID" || true
echo "bench: game ran $(( $(date +%s) - START )) s, output in $OUT"
grep -E "^\[bench\] (scenario|can not|unknown|marvin failed|goto failed|walkto: no|foreach|waitarrive)" "$OUT/game.log" || true

HOST=mac
[[ "${RYZEN_BENCH:-}" != 1 ]] || HOST=ryzen
printf '%s\n' "$HOST" >"$OUT/host.txt"
REPORT_RC=0
python3 "$ROOT/scripts/bench_report.py" "$OUT" || REPORT_RC=$?
python3 - "$OUT/events.jsonl" "$SCEN" <<'PY' || REPORT_RC=1
import json
import sys
from collections import Counter
from pathlib import Path
path = Path(sys.argv[1])
failures = []
observed = Counter()
if path.exists():
    with path.open() as events:
        for line_number, line in enumerate(events, 1):
            try:
                event = json.loads(line)
            except ValueError:
                failures.append(f'events.jsonl:{line_number}: invalid JSON')
                continue
            kind = event.get('kind', '')
            if kind == 'cloth_verify' and event.get('pass'):
                observed[('verify', event.get('check'))] += 1
            elif kind == 'cloth_checkpoint':
                observed[('checkpoint', event.get('label'))] += 1
            elif kind == 'cloth_session_initial' and event.get('rest'):
                observed[('initial', event.get('label'))] += 1
            if kind == 'error' or kind.endswith('_error') or event.get('pass') is False:
                failures.append(f"{kind}: {event.get('reason', event.get('check', 'check failed'))}")
scenario = Path(sys.argv[2])
if scenario.name.startswith('cloth'):
    expected = Counter()
    for line in scenario.read_text().splitlines():
        command = line.split()
        if len(command) == 3 and command[0] == 'cloth' and command[1] in ('verify', 'checkpoint', 'initial'):
            expected[(command[1], command[2])] += 1
    for (command, label), count in (expected - observed).items():
        failures.append(f'missing cloth {command} {label}: {count} result(s)')
if failures:
    for failure in failures[:10]:
        print(f'bench: failed: {failure}', file=sys.stderr)
    raise SystemExit(1)
PY
if [[ "$NEEDS_CADENCE" == 1 ]]; then
  python3 - "$OUT" <<'PY' || REPORT_RC=1
import json
import sys
from pathlib import Path
out = Path(sys.argv[1])
try:
    timing = json.loads((out / 'appcadence.csv.json').read_text())
    valid = timing['complete'] and timing['frames'] > 0 and timing['overflow'] == 0
except (OSError, ValueError, KeyError):
    valid = False
if not valid:
    raise SystemExit('bench: normal-clock timing did not complete; no valid cadence measurement')
PY
fi
if [[ "$NAME" == streaming-flythrough ]]; then
  python3 "$ROOT/scripts/perf_report.py" run "$OUT" || REPORT_RC=1
fi
echo "bench: ran on $HOST"
echo "$OUT"
exit "$REPORT_RC"
