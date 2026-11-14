#!/usr/bin/env bash
# One Linux acceptance run, including build, clean launch, evidence and verification.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
OUT="${1:?usage: scripts/island.sh RESULT_DIRECTORY}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd -P)"
[[ -z "$(ls -A "$OUT")" ]] || { echo 'island: result directory must be empty' >&2; exit 76; }
started=$SECONDS
# Match ryzen.sh so clean and the janitor own the island along with this worktree.
WORKTREE="$(basename "$ROOT")"
if [[ "$ROOT" == */worktrees/*/* ]]; then WORKTREE="$(basename "$(dirname "$ROOT")")"; fi
WORKTREE="${RYZEN_WORKTREE:-$WORKTREE}"
[[ "$WORKTREE" =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]] || { echo 'Unsafe worktree name' >&2; exit 76; }
REMOTE="gothic-ai/e2e/$WORKTREE"
remote_parent="$(ssh -T -o BatchMode=yes -o ConnectTimeout=10 ryzen 'set -e; mkdir -p ~/gothic-ai/e2e; cd ~/gothic-ai/e2e; pwd -P')" || exit 76
worktree_state="$remote_parent/$WORKTREE"
state="$worktree_state/island"
run="$state/run-$(date -u +%Y%m%dT%H%M%S)-$$"
locked=0
# The build cache belongs to this worktree. Concurrent invocations must not overwrite its source.
# A nonblocking lock is held by this SSH connection through synchronization, build and capture.
lockdir=$(mktemp -d)
cleanup() {
  if [[ -n "${lockpid:-}" ]]; then
    if [[ "$locked" == 1 ]]; then ssh -T ryzen "rm -rf -- '$run'" || true; fi
    exec 8>&-
    wait "$lockpid" || true
  fi
  rm -rf -- "$lockdir"
}
trap cleanup EXIT
trap 'exit 76' INT TERM HUP
mkfifo "$lockdir/input"
ssh -T -o BatchMode=yes -o ConnectTimeout=10 ryzen "command -v flock >/dev/null || exit 76; exec 9>'$worktree_state.lock' || exit 76; flock -n 9 || exit 75; mkdir -p '$state/source' || exit 76; echo locked; cat >/dev/null" \
  <"$lockdir/input" >"$lockdir/lock.log" 2>&1 &
lockpid=$!
exec 8>"$lockdir/input"
for ((n=0; n<100; n++)); do
  if [[ -s "$lockdir/lock.log" ]]; then break; fi
  sleep 0.1
done
if ! grep -qx locked "$lockdir/lock.log"; then
  cat "$lockdir/lock.log"
  exec 8>&-
  rc=0
  wait "$lockpid" || rc=$?
  lockpid=''
  [[ "$rc" == 75 ]] && exit 75
  exit 76
fi
locked=1
rsync -a --checksum --no-times --delete \
  --exclude=.git --exclude=build/ --exclude='build-*/' --exclude='cmake-build-*/' \
  --exclude=.venv/ --exclude=venv/ --exclude=__pycache__/ --exclude=.pytest_cache/ \
  --exclude=/.claude/worktrees/ --exclude=/.bench/ --exclude=/.shorts/ --exclude=/.ryzen/ \
  --exclude=/game/ --exclude=/data/ --exclude=/tts-samples/ --exclude=/site/_build/ \
  "$ROOT/" "ryzen:$state/source/" || exit 76
queue=(--class quick --wait-max "${RYZEN_LOCK_WAIT:-900}" --max 30m)
export GQ_OWNER="$WORKTREE"
rc=0
"$ROOT/scripts/gq" run "${queue[@]}" --threads 12 --mem 10G --name island-build -- \
  bash "$state/source/scripts/island-remote.sh" build "$state" "$run" || rc=$?
if [[ "$rc" != 0 ]]; then
  case "$rc" in
    1|75|76) exit "$rc" ;;
    *) exit 76 ;;
  esac
fi
rc=0
"$ROOT/scripts/gq" run "${queue[@]}" --threads 4 --mem 5G --gpu-mem 3G \
  --display --display-shared --archive "$run/output" --name island-acceptance -- \
  bash "$state/source/scripts/island-remote.sh" run "$state" "$run" || rc=$?
[[ "$rc" != 75 ]] || exit 75
# gq archives only output; the large PNG sequence and the temporary HOME are removed by cleanup.
remote_output="$run/output"
if ! ssh -T ryzen "test -d '$remote_output'"; then
  remote_output="/mnt/shared/$REMOTE/island/${run##*/}/output"
fi
rsync -a "ryzen:$remote_output/" "$OUT/" || exit 76
[[ "$rc" == 0 || "$rc" == 1 ]] || exit 76
python3 - "$OUT/report.json" "$((SECONDS-started))" <<'PY'
import json, sys
from pathlib import Path
p = Path(sys.argv[1])
report = json.loads(p.read_text())
report['total_seconds'] = int(sys.argv[2])
p.write_text(json.dumps(report, indent=2) + '\n')
PY
python3 "$ROOT/scripts/island_check.py" "$OUT"
