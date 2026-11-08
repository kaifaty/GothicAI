#!/usr/bin/env bash
# Matched passive routine windows from the navigation baseline, one game at a time.
# NPC_CV_SPOTS="market harbour" selects four pairs; default is all eight pairs.
# NPC_CV_TIMEOUT is the wall timeout per process, including loading (default 600 s).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# RUNNER optionally uses the existing CPU-only body runner instead of a rendered game.
# Each window loads SAVE in its own process and run/Gothic.ini, preserving native routines.
# SAVE, GOTHIC_DIR and RUNNER must be absolute when this mode is selected.
OUT="${1:-$ROOT/.bench/npc-character-virtual/windows}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
[[ "${NPC_CV_MODES:-off on}" == *[![:space:]]* && "${NPC_CV_SPOTS:-market harbour onar lobart}" == *[![:space:]]* ]] || {
  echo 'NPC_CV_MODES and NPC_CV_SPOTS must not be blank' >&2; exit 2;
}
read -r -a modes <<< "${NPC_CV_MODES:-off on}"
read -r -a spots <<< "${NPC_CV_SPOTS:-market harbour onar lobart}"
limit="${NPC_CV_TIMEOUT:-${TIMEOUT:-600}}"
[[ "$limit" =~ ^[1-9][0-9]*$ ]] || { echo 'NPC_CV_TIMEOUT must be positive whole seconds' >&2; exit 2; }
for mode in "${modes[@]}"; do
  [[ "$mode" == off || "$mode" == on ]] || { echo 'NPC_CV_MODES must contain off and/or on' >&2; exit 2; }
done
for spot in "${spots[@]}"; do
  case "$spot" in
    market|harbour|onar|lobart) ;;
    *) echo 'NPC_CV_SPOTS must contain market, harbour, onar and/or lobart' >&2; exit 2 ;;
  esac
done
if [[ -n "${RUNNER:-}" ]]; then
  : "${SAVE:?Set SAVE to the absolute source save}"
  : "${GOTHIC_DIR:?Set GOTHIC_DIR to the absolute game assets}"
  [[ "$RUNNER" == /* && "$SAVE" == /* && "$GOTHIC_DIR" == /* ]] || { echo 'RUNNER, SAVE and GOTHIC_DIR must be absolute' >&2; exit 2; }
  [[ -x "$RUNNER" && -f "$SAVE" && -d "$GOTHIC_DIR" ]] || { echo 'Missing executable RUNNER, source SAVE or GOTHIC_DIR' >&2; exit 2; }
fi
check_window() {
  if [[ -n "${RUNNER:-}" ]]; then
    python3 "$ROOT/scripts/npc_character_virtual_compare.py" --check-window "$1" \
      --window-label "$2" --window-mode "$3" --require-runner-summary
  else
    python3 "$ROOT/scripts/npc_character_virtual_compare.py" --check-window "$1" \
      --window-label "$2" --window-mode "$3"
  fi
}
for mode in "${modes[@]}"; do
  value=0
  [[ "$mode" != on ]] || value=1
  for spot in "${spots[@]}"; do
    case "$spot" in
      market) anchor=NW_CITY_MERCHANT_PATH_20 ;;
      harbour) anchor=NW_CITY_HABOUR_06_B ;;
      onar) anchor=NW_BIGFARM_HOUSE_OUT_04 ;;
      lobart) anchor=NW_FARM1_ENTRANCE_01 ;;
    esac
    for hour in 7 20; do
      name="$spot-$hour"
      run="$OUT/$mode/$name"
      mkdir -p "$run"
      if [[ -e "$run/events.jsonl" || -e "$run/bench.log" || -e "$run/runner.log" ]]; then
        if check_window "$run" "$name" "$mode" >/dev/null; then
          echo "$mode $name already complete"
          continue
        fi
        echo "Incomplete earlier measurement: $run" >&2
        echo 'Preserved its evidence; choose a fresh output directory or move the incomplete window before retrying.' >&2
        exit 2
      fi
      scenario="$run/window.bench"
      cat >"$scenario" <<EOF
seed 7
cheat god
set time $((hour-1)) 59
goto $anchor
wait 4000
pos
stuck begin $name 3000
npccharacter observe begin $name
wait 248300
pos
npccharacter observe end
stuck end
label done
EOF
      while LC_ALL=C pgrep -f '^[^ ]*Gothic2Notr[^ /]*( |$)|^[^ ]*python[0-9.]* [^ ]*bin/pytest' >/dev/null; do sleep 5; done
      if [[ -n "${RUNNER:-}" ]]; then
        mkdir -p "$run/run"
        printf '[GAME]\nnpc_character_virtual=%s\n' "$value" >"$run/run/Gothic.ini"
        if ! (cd "$run/run" && GOTHIC_AI=0 OPENGOTHIC_STUCK_LOG="$run/stuck.jsonl" \
          python3 - "$limit" "$RUNNER" -g "$GOTHIC_DIR" -devmode --save "$SAVE" \
          --scenario "$scenario" --output "$run" --mode lockstep <<'PY'
import subprocess
import sys

try:
    result = subprocess.run(sys.argv[2:], timeout=int(sys.argv[1]))
except subprocess.TimeoutExpired:
    print(f'NPC CharacterVirtual window timed out after {sys.argv[1]} seconds.', file=sys.stderr)
    raise SystemExit(124)
raise SystemExit(result.returncode)
PY
        ) >"$run/runner.log" 2>&1; then
          echo "Window process failed or timed out; see $run/runner.log" >&2
          exit 1
        fi
      else
        GOTHIC_AI=0 NO_BUILD=1 KEEP= NPC_CHARACTER_VIRTUAL="$value" TIMEOUT="$limit" \
          OPENGOTHIC_STUCK_LOG="$run/stuck.jsonl" \
          "$ROOT/scripts/bench.sh" "$scenario" "$run" >"$run/runner.log" 2>&1
      fi
      if ! check_window "$run" "$name" "$mode"; then
        echo "Window failed: $run" >&2
        exit 1
      fi
      echo "$mode $name complete"
    done
  done
done
