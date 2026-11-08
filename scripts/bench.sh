#!/usr/bin/env bash
# Runs a test-bench scenario through the windowed game or the headless lockstep runner.
# See scripts/bench/README.md.
#
#   scripts/bench.sh <scenario file | name from scripts/bench/> [out dir]
#
# Completion requires [bench] done in game.log; interrupted runs exit 1 and still produce reports.
# The engine atomically refreshes heartbeat.json about every 10 s while the scenario runs.
# Progress is flushed to stdout. Stall/timeout shutdown sends SIGTERM, then SIGKILL after 10 s;
# the stall watchdog stops after completion, including with KEEP=1.
#
# env: GOTHIC_DIR    game data (default: /Volumes/DevSSD/Dev/games/Gothic II)
#      BENCH_HOST    auto (default): headless dev-ubuntu, Ryzen, local; windowed Ryzen, local
#      RYZEN_HOST    SSH host for Ryzen (default: ryzen)
#      DEV_UBUNTU_HOST SSH host for headless benches (default: dev-ubuntu)
#      RYZEN_LOCK_WAIT seconds to wait for the worktree lock and gq (default: auto 120, explicit host 900)
#      RYZEN_MAX     gq job time limit, including the build (default: 30m)
#      RYZEN_BENCH_CLASS quick (window default), batch (headless default), or quiet for timing
#      RYZEN_BENCH_ARCHIVE absolute local Ryzen task path to archive a result copy through gq
#      GAME_BIN      game executable (default: OpenGothic/build/opengothic/Gothic2Notr)
#      BENCH_HEADLESS 1: runner in lockstep, 0: window; default: scenario's # @headless marker
#      RUNNER_BIN    headless executable (default: gothic-body-runner beside GAME_BIN)
#      TEMPEST_WINDOW_SIZE client frame size (default: 800x600)
#      BENCH_VID_RES  [INTERNAL] vidResIndex of the run (default: 2 on macOS, 0 on Linux)
#      SAVE          save to start from (default: save_slot_1.sav in the main checkout)
#      WORLD         start a fresh game in this world instead (NEWWORLD.ZEN, OLDWORLD.ZEN, ADDONWORLD.ZEN, DRAGONISLAND.ZEN)
#      GOTHIC_INTRO=1 play the new game's intro and the scripts' videos (default: -nointro, none)
#      NO_BUILD=1    do not rebuild the engine first
#      TIMEOUT=300   seconds before the game is killed (a scenario's `# @timeout <s>` line sets its own default)
#      BENCH_PROGRESS seconds between progress lines (default: 300; 0 disables)
#      BENCH_STALL   seconds without output changes before killing a stalled game (default: 600; 0 disables)
#      KEEP=1        the window stays open after the scenario (incompatible with headless)
#      SOUND, MUSIC  volumes 0..1 (default 0); with TEMPEST_AUDIO_LOOPBACK=48000 the sound of a segment
#                    goes into <segment>.wav instead of the speakers
#      MOVABLE_VOBS  optional initial GAME movable_vobs setting (0 or 1), before world loading
#      ITEM_HULLS    optional initial GAME item_hulls setting (0 or 1), for new item bodies
#      ITEM_DEEPEST_CONTACT optional initial GAME item_deepest_contact setting (0 or 1)
#      RAGDOLL_BIRTH_GUARD optional initial GAME ragdoll_birth_guard setting (0 or 1)
#      FURNITURE_PARTS optional initial GAME furniture_parts setting (0 or 1)
#      ARROW_STICKING optional initial GAME arrow_sticking setting (0 or 1), before world loading
#      PHYSICS_NOISE optional initial GAME physics_noise setting (0 or 1), before world loading
#      DESTRUCTION optional initial GAME destruction setting (0 or 1), before world loading
#      BUOYANCY     optional initial GAME buoyancy setting (0 or 1), before world loading
#      CLOTH        optional initial GAME cloth setting (0 or 1), before world loading
#      ITEM_WAKE    optional initial GAME item_wake setting (0 or 1), before world loading
#      PLACED_ITEMS optional initial GAME placed_items setting (0 or 1), before world loading
#      BODY_IMPACT_DAMAGE optional initial GAME body_impact_damage setting (0 or 1)
#      BODY_TRACES  optional initial GAME body_traces setting (0 or 1)
#      BLOOD        optional initial GAME blood setting (0 or 1)
#      STATIC_COLLISION_SPLIT optional initial GAME static_collision_split setting (0 or 1), before world loading
#      CHARACTER_VIRTUAL optional initial GAME character_virtual setting (0 or 1), before world loading
#      NPC_CHARACTER_VIRTUAL optional initial GAME npc_character_virtual setting (0 or 1)
#      MONSTER_CHARACTER_VIRTUAL optional initial GAME monster_character_virtual setting (0 or 1)
#      MELEE_PHYSICS_HITS optional initial GAME melee_physics_hits setting (0 or 1)
#      MELEE_POSTURE optional initial GAME melee_posture setting (0 or 1)
#      MELEE_DOWN_TACTICS optional initial GAME melee_down_tactics setting (0 or 1)
#      MELEE_HITSTOP optional initial GAME melee_hitstop setting (0 or 1)
#      MELEE_STAMINA optional initial GAME melee_stamina setting (0 or 1)
#      MELEE_PARRY optional initial GAME melee_parry setting (0 or 1)
#      MELEE_PARRY_AI optional initial GAME melee_parry_ai setting (0 or 1)
#      MELEE_AI_DODGE optional initial GAME melee_ai_dodge setting (0 or 1)
#      MELEE_AI_RIPOSTE optional initial GAME melee_ai_riposte setting (0 or 1)
#      MELEE_AI_CHARGE optional initial GAME melee_ai_charge setting (0 or 1)
#      MELEE_AI_BARGE optional initial GAME melee_ai_barge setting (0 or 1)
#      MELEE_STAGGER optional initial GAME melee_stagger setting (0 or 1)
#      MELEE_BEAST_POISE optional initial GAME melee_beast_poise setting (0 or 1)
#      MELEE_BACKSTAB optional initial GAME melee_backstab setting (0 or 1)
#      MELEE_DODGE optional initial GAME melee_dodge setting (0 or 1)
#      MELEE_BARGE optional initial GAME melee_barge setting (0 or 1)
#      MELEE_CHARGE optional initial GAME melee_charge setting (0 or 1)
#      MELEE_RIPOSTE optional initial GAME melee_riposte setting (0 or 1)
#      MELEE_HEAL_INTERRUPT optional initial GAME melee_heal_interrupt setting (0 or 1)
#      MELEE_USE_IN_HAND optional initial GAME melee_use_in_hand setting (0 or 1)
#      FOOD_OVER_TIME optional initial GAME food_over_time setting (0 or 1)
#      MELEE_REACTIONS optional initial GAME melee_reactions setting (0 or 1)
#      MELEE_BLADE_SHADOW optional initial GAME melee_blade_shadow setting (0 or 1)
#      OPENGOTHIC_TEXTURES  sharper textures (default: those of scripts/hd-textures.sh, when made; empty: none)
#      OPENGOTHIC_CENSUS  session resource JSON (use an absolute path inside out dir for Ryzen)
#      OPENGOTHIC_FRACTURE  pre-cut meshes (default: ~/.local/share/gothic-ai/fracture, when made; empty: none)
#      OPENGOTHIC_CONVEX_PARTS furniture collision cache (default: ~/.local/share/gothic-ai/convex-parts; empty: none)
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
CONVEX_PARTS_CACHE="$HOME/.local/share/gothic-ai/convex-parts"
BENCH_HOST="${BENCH_HOST:-auto}"
case "$BENCH_HOST" in
  auto|ryzen|dev-ubuntu|local) ;;
  *) echo "bench: BENCH_HOST must be auto, ryzen, dev-ubuntu or local" >&2; exit 2 ;;
esac

SCEN="${1:?usage: scripts/bench.sh <scenario> [out dir]}"
[[ -f "$SCEN" ]] || SCEN="$ROOT/scripts/bench/$SCEN.bench"
[[ -f "$SCEN" ]] || { echo "no scenario: $1" >&2; exit 2; }
SCEN="$(cd "$(dirname "$SCEN")" && pwd)/$(basename "$SCEN")"
NAME="$(basename "$SCEN" .bench)"
if [[ -z "${TIMEOUT:-}" ]]; then
  SCEN_TIMEOUT="$(awk '$1 == "#" && $2 == "@timeout" { print $3; exit }' "$SCEN")"
  [[ -z "$SCEN_TIMEOUT" ]] || export TIMEOUT="$SCEN_TIMEOUT"
fi
if [[ -z "${BENCH_HEADLESS+x}" ]]; then
  BENCH_HEADLESS=0
  if grep -qx '# @headless' "$SCEN"; then BENCH_HEADLESS=1; fi
fi
case "$BENCH_HEADLESS" in
  0|1) export BENCH_HEADLESS ;;
  *) echo 'bench: BENCH_HEADLESS must be 0 or 1' >&2; exit 2 ;;
esac
if [[ "$BENCH_HOST" == dev-ubuntu ]]; then
  [[ "$BENCH_HEADLESS" == 1 ]] || { echo 'bench: окна на dev-ubuntu нет' >&2; exit 2; }
  [[ -z "${RYZEN_BENCH_ARCHIVE:-}" ]] || { echo 'bench: RYZEN_BENCH_ARCHIVE is not supported on dev-ubuntu (no shared store)' >&2; exit 2; }
fi
if [[ "$BENCH_HEADLESS" == 1 && "${KEEP:-}" == 1 ]]; then
  echo 'bench: KEEP=1 is incompatible with headless lockstep' >&2
  exit 2
fi
if [[ "$NAME" == routine-return || "$NAME" == npc-day-parity || "$NAME" == npc-tier-transitions ]]; then
  export WORLD="${WORLD:-NEWWORLD.ZEN}"
  export TIMEOUT="${TIMEOUT:-3600}"
  export RYZEN_MAX="${RYZEN_MAX:-75m}"
  if [[ "$NAME" == routine-return ]]; then
    [[ "$WORLD" == NEWWORLD.ZEN ]] || { echo "bench: routine-return requires WORLD=NEWWORLD.ZEN" >&2; exit 2; }
  fi
fi
if [[ "$NAME" == streaming-flythrough || "$NAME" == streaming-shots || "$NAME" == furniture-parts ]]; then
  export WORLD="${WORLD:-NEWWORLD.ZEN}"
  if [[ "$BENCH_HEADLESS" == 0 ]]; then
    export RYZEN_BENCH_CLASS="${RYZEN_BENCH_CLASS:-quiet}"
  fi
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
    if name in ("GOTHIC_DIR", "GOTHIC_AI_URL", "OPENGOTHIC_FRACTURE", "OPENGOTHIC_CONVEX_PARTS", "OPENGOTHIC_CONTENT", "OPENGOTHIC_BASE",
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
    if [[ "$BENCH_HOST" != auto ]]; then
      echo "bench: cannot run on $BENCH_HOST: $LOCAL_REASON" >&2
      exit 2
    fi
    echo "bench: falling back to Mac: $LOCAL_REASON" >&2
  else
    if [[ "$BENCH_HOST" == auto ]]; then
      LOCK_WAIT="${RYZEN_LOCK_WAIT:-120}"
    else
      LOCK_WAIT="${RYZEN_LOCK_WAIT:-900}"
    fi
    REMOTE_HOSTS=("$BENCH_HOST")
    if [[ "$BENCH_HOST" == auto ]]; then
      REMOTE_HOSTS=(ryzen)
      if [[ "$BENCH_HEADLESS" == 1 ]]; then REMOTE_HOSTS=(dev-ubuntu ryzen); fi
    fi
    for ((host_index = 0; host_index < ${#REMOTE_HOSTS[@]}; host_index++)); do
      HOST="${REMOTE_HOSTS[$host_index]}"
      SSH_HOST="${RYZEN_HOST:-ryzen}"
      if [[ "$HOST" == dev-ubuntu ]]; then SSH_HOST="${DEV_UBUNTU_HOST:-dev-ubuntu}"; fi
      rm -f -- "$OUT/game.log" "$OUT/host.txt"
      RYZEN_RC=0
      RYZEN_HOST="$SSH_HOST" RYZEN_LOCK_WAIT="$LOCK_WAIT" "$ROOT/scripts/ryzen.sh" bench "$SCEN" "$OUT" 2>&1 | tee "$OUT/$HOST.log" \
        || RYZEN_RC="${PIPESTATUS[0]}"
      if [[ -f "$OUT/game.log" ]]; then
        printf '%s\n' "$HOST" >"$OUT/host.txt"
        echo "bench: ran on $HOST"
        echo "$OUT"
        exit "$RYZEN_RC"
      fi
      if [[ "$BENCH_HOST" != auto ]]; then
        # A successful transport without a game log is still a failed launch.
        [[ "$RYZEN_RC" != 0 ]] || RYZEN_RC=1
        exit "$RYZEN_RC"
      fi
      LOCAL_REASON="$(awk 'NF && !/^Ryzen: (removed |queue guard |preparation check |window wait |bench [0-9]+s, exit )/ && !/^bench: output in / { line=$0 } END { print line }' "$OUT/$HOST.log")"
      NEXT_HOST=Mac
      if (( host_index + 1 < ${#REMOTE_HOSTS[@]} )); then NEXT_HOST="${REMOTE_HOSTS[$((host_index + 1))]}"; fi
      echo "bench: falling back to $NEXT_HOST: $HOST: ${LOCAL_REASON:-game did not start (exit $RYZEN_RC)}" >&2
    done
  fi
fi

if [[ -n "${WORLD:-}" ]]; then
  if [[ "$BENCH_HEADLESS" == 1 ]]; then
    START_ARGS=(-nomenu --world "$WORLD")
  else
    START_ARGS=(-nomenu -w "$WORLD")
  fi
else
  if [[ -z "${SAVE:-}" ]]; then
    # worktrees share the save of the main checkout
    COMMON="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)"
    SAVE="$(dirname "$COMMON")/save_slot_1.sav"
  fi
  [[ -f "$SAVE" ]] || { echo "no save: $SAVE (set SAVE=...)" >&2; exit 2; }
  if [[ "$BENCH_HEADLESS" == 1 ]]; then
    START_ARGS=(--save "$OUT/run/save_slot_1.sav")
  else
    START_ARGS=(-save 1)
  fi
fi

# No videos (`-nointro`), as in e2e: on a fresh WORLD the new game's INTRO.BIK otherwise plays for
# almost two minutes, with its music, before the first command. Marvin `playvideo` still plays.
if [[ "${GOTHIC_INTRO:-0}" != 1 ]]; then
  START_ARGS+=(-nointro)
fi

if [[ "${NO_BUILD:-}" != 1 ]]; then
  BUILD_TARGET=Gothic2Notr
  if [[ "$BENCH_HEADLESS" == 1 ]]; then BUILD_TARGET=gothic-body-runner; fi
  cmake --build "$ROOT/OpenGothic/build" --target "$BUILD_TARGET" >"$OUT/build.log" 2>&1 \
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
# BENCH_VID_RES=0 on a Mac: full resolution, a 960x540 window gives 1920x1080 frames (TAA clips)
VID_RES_INDEX="${BENCH_VID_RES:-$VID_RES_INDEX}"
cat >"$RUN/Gothic.ini" <<EOF
[INTERNAL]
vidResIndex=$VID_RES_INDEX
[SOUND]
soundVolume=${SOUND:-0}
musicVolume=${MUSIC:-0}
musicEnabled=$([[ "${MUSIC:-0}" == 0 || "${MUSIC:-0}" == 0.0 ]] && echo 0 || echo 1)
worldSounds=${WORLD_SOUNDS:-1}
EOF
if [[ -n "${MOVABLE_VOBS:-}${ITEM_HULLS:-}${ITEM_DEEPEST_CONTACT:-}${RAGDOLL_BIRTH_GUARD:-}${FURNITURE_PARTS:-}${ARROW_STICKING:-}${PHYSICS_NOISE:-}${DESTRUCTION:-}${BUOYANCY:-}${CLOTH:-}${ITEM_WAKE:-}${PLACED_ITEMS:-}${BODY_IMPACT_DAMAGE:-}${BODY_TRACES:-}${BLOOD:-}${STATIC_COLLISION_SPLIT:-}${CHARACTER_VIRTUAL:-}${NPC_CHARACTER_VIRTUAL:-}${MONSTER_CHARACTER_VIRTUAL:-}${MELEE_BLADE_SHADOW:-}${MELEE_PHYSICS_HITS:-}${MELEE_POSTURE:-}${MELEE_REACTIONS:-}${MELEE_DOWN_TACTICS:-}${MELEE_HITSTOP:-}${MELEE_STAMINA:-}${MELEE_PARRY:-}${MELEE_PARRY_AI:-}${MELEE_AI_DODGE:-}${MELEE_AI_RIPOSTE:-}${MELEE_AI_CHARGE:-}${MELEE_AI_BARGE:-}${MELEE_STAGGER:-}${MELEE_BEAST_POISE:-}${MELEE_BACKSTAB:-}${MELEE_DODGE:-}${MELEE_BARGE:-}${MELEE_CHARGE:-}${MELEE_RIPOSTE:-}${MELEE_HEAL_INTERRUPT:-}${MELEE_USE_IN_HAND:-}${FOOD_OVER_TIME:-}" ]]; then
  printf '\n[GAME]\n' >>"$RUN/Gothic.ini"
  if [[ -n "${MOVABLE_VOBS:-}" ]]; then
    [[ "$MOVABLE_VOBS" == 0 || "$MOVABLE_VOBS" == 1 ]] || { echo "MOVABLE_VOBS must be 0 or 1" >&2; exit 2; }
    printf 'movable_vobs=%s\n' "$MOVABLE_VOBS" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${ITEM_HULLS:-}" ]]; then
    [[ "$ITEM_HULLS" == 0 || "$ITEM_HULLS" == 1 ]] || { echo "ITEM_HULLS must be 0 or 1" >&2; exit 2; }
    printf 'item_hulls=%s\n' "$ITEM_HULLS" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${ITEM_DEEPEST_CONTACT:-}" ]]; then
    [[ "$ITEM_DEEPEST_CONTACT" == 0 || "$ITEM_DEEPEST_CONTACT" == 1 ]] || { echo "ITEM_DEEPEST_CONTACT must be 0 or 1" >&2; exit 2; }
    printf 'item_deepest_contact=%s\n' "$ITEM_DEEPEST_CONTACT" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${RAGDOLL_BIRTH_GUARD:-}" ]]; then
    [[ "$RAGDOLL_BIRTH_GUARD" == 0 || "$RAGDOLL_BIRTH_GUARD" == 1 ]] || { echo "RAGDOLL_BIRTH_GUARD must be 0 or 1" >&2; exit 2; }
    printf 'ragdoll_birth_guard=%s\n' "$RAGDOLL_BIRTH_GUARD" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${FURNITURE_PARTS:-}" ]]; then
    [[ "$FURNITURE_PARTS" == 0 || "$FURNITURE_PARTS" == 1 ]] || { echo "FURNITURE_PARTS must be 0 or 1" >&2; exit 2; }
    printf 'furniture_parts=%s\n' "$FURNITURE_PARTS" >>"$RUN/Gothic.ini"
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
  if [[ -n "${MELEE_STAMINA:-}" ]]; then
    [[ "$MELEE_STAMINA" == 0 || "$MELEE_STAMINA" == 1 ]] || { echo "MELEE_STAMINA must be 0 or 1" >&2; exit 2; }
    printf 'melee_stamina=%s\n' "$MELEE_STAMINA" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_PARRY:-}" ]]; then
    [[ "$MELEE_PARRY" == 0 || "$MELEE_PARRY" == 1 ]] || { echo "MELEE_PARRY must be 0 or 1" >&2; exit 2; }
    printf 'melee_parry=%s\n' "$MELEE_PARRY" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_PARRY_AI:-}" ]]; then
    [[ "$MELEE_PARRY_AI" == 0 || "$MELEE_PARRY_AI" == 1 ]] || { echo "MELEE_PARRY_AI must be 0 or 1" >&2; exit 2; }
    printf 'melee_parry_ai=%s\n' "$MELEE_PARRY_AI" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_AI_DODGE:-}" ]]; then
    [[ "$MELEE_AI_DODGE" == 0 || "$MELEE_AI_DODGE" == 1 ]] || { echo "MELEE_AI_DODGE must be 0 or 1" >&2; exit 2; }
    printf 'melee_ai_dodge=%s\n' "$MELEE_AI_DODGE" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_AI_RIPOSTE:-}" ]]; then
    [[ "$MELEE_AI_RIPOSTE" == 0 || "$MELEE_AI_RIPOSTE" == 1 ]] || { echo "MELEE_AI_RIPOSTE must be 0 or 1" >&2; exit 2; }
    printf 'melee_ai_riposte=%s\n' "$MELEE_AI_RIPOSTE" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_AI_CHARGE:-}" ]]; then
    [[ "$MELEE_AI_CHARGE" == 0 || "$MELEE_AI_CHARGE" == 1 ]] || { echo "MELEE_AI_CHARGE must be 0 or 1" >&2; exit 2; }
    printf 'melee_ai_charge=%s\n' "$MELEE_AI_CHARGE" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_AI_BARGE:-}" ]]; then
    [[ "$MELEE_AI_BARGE" == 0 || "$MELEE_AI_BARGE" == 1 ]] || { echo "MELEE_AI_BARGE must be 0 or 1" >&2; exit 2; }
    printf 'melee_ai_barge=%s\n' "$MELEE_AI_BARGE" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_STAGGER:-}" ]]; then
    [[ "$MELEE_STAGGER" == 0 || "$MELEE_STAGGER" == 1 ]] || { echo "MELEE_STAGGER must be 0 or 1" >&2; exit 2; }
    printf 'melee_stagger=%s\n' "$MELEE_STAGGER" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_BEAST_POISE:-}" ]]; then
    [[ "$MELEE_BEAST_POISE" == 0 || "$MELEE_BEAST_POISE" == 1 ]] || { echo "MELEE_BEAST_POISE must be 0 or 1" >&2; exit 2; }
    printf 'melee_beast_poise=%s\n' "$MELEE_BEAST_POISE" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_BACKSTAB:-}" ]]; then
    [[ "$MELEE_BACKSTAB" == 0 || "$MELEE_BACKSTAB" == 1 ]] || { echo "MELEE_BACKSTAB must be 0 or 1" >&2; exit 2; }
    printf 'melee_backstab=%s\n' "$MELEE_BACKSTAB" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_DODGE:-}" ]]; then
    [[ "$MELEE_DODGE" == 0 || "$MELEE_DODGE" == 1 ]] || { echo "MELEE_DODGE must be 0 or 1" >&2; exit 2; }
    printf 'melee_dodge=%s\n' "$MELEE_DODGE" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_BARGE:-}" ]]; then
    [[ "$MELEE_BARGE" == 0 || "$MELEE_BARGE" == 1 ]] || { echo "MELEE_BARGE must be 0 or 1" >&2; exit 2; }
    printf 'melee_barge=%s\n' "$MELEE_BARGE" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_CHARGE:-}" ]]; then
    [[ "$MELEE_CHARGE" == 0 || "$MELEE_CHARGE" == 1 ]] || { echo "MELEE_CHARGE must be 0 or 1" >&2; exit 2; }
    printf 'melee_charge=%s\n' "$MELEE_CHARGE" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_RIPOSTE:-}" ]]; then
    [[ "$MELEE_RIPOSTE" == 0 || "$MELEE_RIPOSTE" == 1 ]] || { echo "MELEE_RIPOSTE must be 0 or 1" >&2; exit 2; }
    printf 'melee_riposte=%s\n' "$MELEE_RIPOSTE" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_HEAL_INTERRUPT:-}" ]]; then
    [[ "$MELEE_HEAL_INTERRUPT" == 0 || "$MELEE_HEAL_INTERRUPT" == 1 ]] || { echo "MELEE_HEAL_INTERRUPT must be 0 or 1" >&2; exit 2; }
    printf 'melee_heal_interrupt=%s\n' "$MELEE_HEAL_INTERRUPT" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${MELEE_USE_IN_HAND:-}" ]]; then
    [[ "$MELEE_USE_IN_HAND" == 0 || "$MELEE_USE_IN_HAND" == 1 ]] || { echo "MELEE_USE_IN_HAND must be 0 or 1" >&2; exit 2; }
    printf 'melee_use_in_hand=%s\n' "$MELEE_USE_IN_HAND" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${FOOD_OVER_TIME:-}" ]]; then
    [[ "$FOOD_OVER_TIME" == 0 || "$FOOD_OVER_TIME" == 1 ]] || { echo "FOOD_OVER_TIME must be 0 or 1" >&2; exit 2; }
    printf 'food_over_time=%s\n' "$FOOD_OVER_TIME" >>"$RUN/Gothic.ini"
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
  if [[ -n "${PLACED_ITEMS:-}" ]]; then
    [[ "$PLACED_ITEMS" == 0 || "$PLACED_ITEMS" == 1 ]] || { echo "PLACED_ITEMS must be 0 or 1" >&2; exit 2; }
    printf 'placed_items=%s\n' "$PLACED_ITEMS" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${BODY_IMPACT_DAMAGE:-}" ]]; then
    [[ "$BODY_IMPACT_DAMAGE" == 0 || "$BODY_IMPACT_DAMAGE" == 1 ]] || { echo "BODY_IMPACT_DAMAGE must be 0 or 1" >&2; exit 2; }
    printf 'body_impact_damage=%s\n' "$BODY_IMPACT_DAMAGE" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${BODY_TRACES:-}" ]]; then
    [[ "$BODY_TRACES" == 0 || "$BODY_TRACES" == 1 ]] || { echo "BODY_TRACES must be 0 or 1" >&2; exit 2; }
    printf 'body_traces=%s\n' "$BODY_TRACES" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${BLOOD:-}" ]]; then
    [[ "$BLOOD" == 0 || "$BLOOD" == 1 ]] || { echo "BLOOD must be 0 or 1" >&2; exit 2; }
    printf 'blood=%s\n' "$BLOOD" >>"$RUN/Gothic.ini"
  fi
  if [[ -n "${STATIC_COLLISION_SPLIT:-}" ]]; then
    [[ "$STATIC_COLLISION_SPLIT" == 0 || "$STATIC_COLLISION_SPLIT" == 1 ]] || { echo "STATIC_COLLISION_SPLIT must be 0 or 1" >&2; exit 2; }
    printf 'static_collision_split=%s\n' "$STATIC_COLLISION_SPLIT" >>"$RUN/Gothic.ini"
  fi
fi

# the sharper textures the launcher gives the game too (scripts/hd-textures.sh)
if [[ -z "${OPENGOTHIC_TEXTURES+set}" && -d "$HD_TEXTURES" ]]; then
  export OPENGOTHIC_TEXTURES="$HD_TEXTURES"
fi
if [[ -z "${OPENGOTHIC_FRACTURE+set}" && -d "$FRACTURE_CACHE" ]]; then
  export OPENGOTHIC_FRACTURE="$FRACTURE_CACHE"
fi
if [[ -z "${OPENGOTHIC_CONVEX_PARTS+set}" && -d "$CONVEX_PARTS_CACHE" ]]; then
  export OPENGOTHIC_CONVEX_PARTS="$CONVEX_PARTS_CACHE"
fi

GAME_BIN="$GAME_BIN" bash "$ROOT/scripts/base-ensure.sh" "$GOTHIC_DIR"

BACKGROUND=1
if [[ "$NAME" == streaming-flythrough ]]; then
  export OPENGOTHIC_FRAME_PROFILE="$OUT/frameprofile.csv" OPENGOTHIC_BENCH_VISIBLE=1 OPENGOTHIC_LOADPROBE=1
  BACKGROUND=0
fi
GAME_RC=0
RUN_MODE=window
if [[ "$BENCH_HEADLESS" == 1 ]]; then
  RUN_MODE='headless lockstep'
  GAME_ARGS=("${RUNNER_BIN:-$(dirname "$GAME_BIN")/gothic-body-runner}" -g "$GOTHIC_DIR" -devmode "${START_ARGS[@]}")
  GAME_ARGS+=(--game-parity --scenario "$SCEN" --output "$OUT" --mode lockstep)
  WINDOW_ENV=()
else
  GAME_ARGS=("$GAME_BIN" -g "$GOTHIC_DIR" -window -devmode "${START_ARGS[@]}")
  WINDOW_ENV=(TEMPEST_BACKGROUND="$BACKGROUND" TEMPEST_WINDOW_SIZE="${TEMPEST_WINDOW_SIZE:-800x600}")
fi
env ${WINDOW_ENV[@]+"${WINDOW_ENV[@]}"} BENCH_RUN_MODE="$RUN_MODE" \
  OPENGOTHIC_BENCH="$SCEN" OPENGOTHIC_BENCH_OUT="$OUT" ${KEEP:+OPENGOTHIC_BENCH_KEEP=1} \
  python3 - "$OUT" "$RUN" "${TIMEOUT:-300}" "${BENCH_PROGRESS:-300}" "${BENCH_STALL:-600}" \
  "${GAME_ARGS[@]}" <<'PY' || GAME_RC=$?
import json
import os
from pathlib import Path
import subprocess
import sys
import time

out, run = map(Path, sys.argv[1:3])
timeout, progress, stall = map(int, sys.argv[3:6])
heartbeat = out / 'heartbeat.json'
# Never let a pulse from a previous run stand in for this game's progress.
heartbeat.unlink(missing_ok=True)
started = last_activity = time.monotonic()
next_progress = started + progress
activity = {}
complete = False
reason = ''
log_tail = ''
with (out / 'game.log').open('w') as log:
    log.write(f"[bench] run mode: {os.environ['BENCH_RUN_MODE']}\n")
    log.flush()
    game = subprocess.Popen(sys.argv[6:], cwd=run, stdout=log, stderr=subprocess.STDOUT)
    with (out / 'game.log').open(errors='replace') as reader:
        while True:
            now = time.monotonic()
            elapsed = int(now - started)
            # Read only newly appended log data, including the final lines after exit.
            exited = game.poll() is not None
            data = log_tail + reader.read()
            complete = complete or '[bench] done' in data
            log_tail = data[-len('[bench] done'):]
            if exited:
                break
            if not complete:
                changed = {}
                for directory, _, files in os.walk(out):
                    for name in files:
                        path = Path(directory) / name
                        try:
                            stat = path.stat()
                        except FileNotFoundError:
                            continue  # Atomic heartbeat replacement can race this scan.
                        changed[path] = (stat.st_mtime_ns, stat.st_size)
                if changed != activity:
                    activity = changed
                    last_activity = now
                if progress > 0 and now >= next_progress:
                    try:
                        pulse = json.loads(heartbeat.read_text())
                        line = (f"command {pulse['command_index']}/{pulse['command_count']} "
                                f"{pulse['command']}")
                        if 'npcday_elapsed_minutes' in pulse:
                            line += (f", npcday {int(pulse['npcday_elapsed_minutes'])}/"
                                     f"{int(pulse['npcday_target_minutes'])} game min")
                    except (OSError, ValueError, KeyError):
                        line = f"starting (game.log {(out / 'game.log').stat().st_size // 1024} KB)"
                    print(f'bench: {elapsed // 60} min, {line}', flush=True)
                    next_progress = now + progress
                if stall > 0 and now - last_activity >= stall:
                    reason = f'game stalled: no output or heartbeat for {stall} s, killed'
            if not reason and now - started >= timeout:
                reason = f'timeout after {timeout} s, game killed'
            if reason:
                game.terminate()
                try:
                    game.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    game.kill()
                    game.wait()
                complete = complete or '[bench] done' in log_tail + reader.read()
                break
            time.sleep(1)
elapsed = int(time.monotonic() - started)
with (out / 'game.log').open('a') as log:
    log.write(f'[bench] game wall seconds: {elapsed}\n')
print(f'bench: game ran {elapsed} s, output in {out}', flush=True)
if not complete:
    reason = reason or f'scenario did not finish: game exited after {elapsed} s without [bench] done'
    print(f'bench: {reason}', file=sys.stderr)
    raise SystemExit(1)
PY
grep -E "^\[bench\] (scenario|can not|unknown|marvin failed|goto failed|walkto: no|foreach|waitarrive)" "$OUT/game.log" || true

HOST=mac
[[ "${RYZEN_BENCH:-}" != 1 ]] || HOST="${RYZEN_HOST:-ryzen}"
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
routine_results = {}
routine_summaries = []
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
            elif kind == 'routine_return_result':
                key = (event.get('case'), event.get('attempt'))
                if key in routine_results:
                    failures.append(f'duplicate routine return result: {key}')
                routine_results[key] = event
            elif kind == 'routine_return_summary':
                routine_summaries.append(event)
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
if scenario.name == 'routine-return.bench':
    commands = [line.split() for line in scenario.read_text().splitlines()]
    labels = [command[2] for command in commands
              if len(command) == 8 and command[:2] == ['routine-return', 'case']]
    if not 10 <= len(labels) <= 12 or len(set(labels)) != len(labels):
        failures.append('routine return needs 10-12 unique case labels')
    if commands.count(['routine-return', 'retry-failures']) != 1:
        failures.append('routine return needs one retry-failures command')
    initial = {(label, 1) for label in labels}
    failed = {label for label in labels if (label, 1) in routine_results
              and routine_results[(label, 1)].get('pass') is not True}
    expected = initial | {(label, 2) for label in failed}
    if set(routine_results) != expected:
        failures.append(f'routine return result mismatch: missing {sorted(expected - set(routine_results))}, '
                        f'unexpected {sorted(set(routine_results) - expected)}')
    if any(result.get('preparation_valid') is not True for result in routine_results.values()):
        failures.append('routine return has invalid or missing preparation proof')
    if any(result.get('pass') is not True for result in routine_results.values()):
        failures.append('routine return has failed or incomplete cases')
    if len(routine_summaries) != 1:
        failures.append(f'routine return needs one summary, got {len(routine_summaries)}')
    else:
        summary = routine_summaries[0]
        if (summary.get('complete') is not True or summary.get('cases') != len(labels)
                or summary.get('failed') != len(failed) or summary.get('repeated') != len(failed)):
            failures.append('routine return summary is incomplete or inconsistent')
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
if [[ "$GAME_RC" != 0 ]]; then exit "$GAME_RC"; fi
exit "$REPORT_RC"
