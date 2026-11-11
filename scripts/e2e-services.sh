#!/usr/bin/env bash
# Shared scenario requirements for service startup and dependency preparation.

e2e_needs_tts() {
  case "$1" in
    tts|svm_lines|combat_barks|mercy|hero_yield|humiliate|surrender_live|live_npc|live_director|gossip|day|group|interject|programs|errand*|fight_reactions_*|face_cues_demo|face_eyes_demo) return 0 ;;
    *) return 1 ;;
  esac
}

e2e_needs_asr() {
  case "$1" in
    m3|m5e) return 0 ;;
    *) return 1 ;;
  esac
}

# Fill TIMEOUT and SAVE from the scenario's run settings in RUN
# (server/src/gothic_ai/scenarios/groups.py), as scripts/e2e-regression.sh does.
# An explicit TIMEOUT, SAVE or empty SAVE= (a new game) wins; a scenario without
# a RUN entry (the LIVE group and others) keeps both untouched.
e2e_run_defaults() {
  local declared timeout save
  [[ -z "${TIMEOUT:-}" || -z "${SAVE+set}" ]] || return 0
  declared="$(cd "$ROOT/server" && uv run python - "$1" <<'PY'
import sys

from gothic_ai.scenarios.groups import RUN

run = RUN.get(sys.argv[1])
if run is not None:
    print(run["timeout"], int(bool(run["save"])))
PY
)" || { echo "e2e: cannot read the run settings of $1 (server/src/gothic_ai/scenarios/groups.py)" >&2; return 2; }
  [[ -n "$declared" ]] || return 0
  read -r timeout save <<<"$declared"
  TIMEOUT="${TIMEOUT:-$timeout}"
  if [[ "$save" == 1 && -z "${SAVE+set}" ]]; then
    SAVE="$(dirname "$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)")/save_slot_1.sav"
  fi
}
