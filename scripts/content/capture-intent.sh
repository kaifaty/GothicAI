#!/usr/bin/env bash
# Capture original city texture locations on Ryzen through gq, without a build.
# Usage: capture-intent.sh --scenario FILE --output OUTSIDE_GIT [--expected-shots N]
# Check: capture-intent.sh --check CAPTURE_DIR SCENARIO [EXPECTED_SHOTS=71]
# A private, read-only input snapshot is prepared before waiting in the queue.
set -euo pipefail

if [[ "${1:-}" == --check ]]; then
  python3 - "${2:?capture directory}" "${3:?scenario}" "${4:-71}" <<'PY'
import json
from pathlib import Path
import re
import struct
import sys

directory, scenario = map(Path, sys.argv[1:3])
def require(ok, reason):
    if not ok:
        raise SystemExit(f'capture-intent: {reason}')
def read(path):
    require(path.is_file(), f'missing {path.name}')
    return path.read_text(errors='replace')
try:
    expected = int(sys.argv[3])
except ValueError:
    raise SystemExit('capture-intent: expected shot count must be a positive integer')
require(expected > 0, 'expected shot count must be a positive integer')
commands = [line.split('#', 1)[0].strip() for line in read(scenario).splitlines()]
commands = [line for line in commands if line]
shots = [line[5:] for line in commands if line.startswith('shot ')]
require(len(shots) == expected, f'scenario has {len(shots)} shots, expected {expected}')
require(len(set(shots)) == len(shots), 'duplicate shot names')
require(all(re.fullmatch(r'[A-Za-z0-9_-]+', name) for name in shots), 'unsafe shot name')
require(read(directory / 'exit.txt').strip() == '0', 'game exit was not zero')
log = read(directory / 'game.log')
require('[bench] done' in log, 'scenario did not finish')
# Consume commands in order: an earlier duplicate log line cannot prove that a
# later location was visited. Position and capture commands are never optional.
cursor = 0
for command in commands:
    if command.startswith(('goto ', 'shot ')):
        match = re.search(r'^\[bench\] ' + re.escape(command) + r'\r?$', log[cursor:], re.MULTILINE)
        require(match is not None, f'command not reached in order: {command}')
        cursor += match.end()
for name in shots:
    path = directory / (name + '.png')
    require(path.is_file() and path.stat().st_size > 24, f'missing or empty shot: {path.name}')
    with path.open('rb') as stream:
        header = stream.read(24)
    require(header[:8] == b'\x89PNG\r\n\x1a\n' and header[12:16] == b'IHDR'
            and all(struct.unpack('>II', header[16:24])), f'invalid PNG: {path.name}')
require(not re.search(r'materials: (invalid|unsupported|unable)|\[bench\] (can not|unknown|.*failed|.*error)', log,
                      re.IGNORECASE), 'material or benchmark error in game.log')
require(re.search(r'materials: 0 entries from ', log), 'explicit empty material index was not loaded')
events = directory / 'events.jsonl'
if events.is_file():
    for number, line in enumerate(events.read_text().splitlines(), 1):
        try:
            event = json.loads(line)
        except ValueError:
            raise SystemExit(f'capture-intent: invalid events.jsonl line {number}')
        require(isinstance(event, dict), f'invalid event at line {number}')
        kind = event.get('kind', '')
        require(not (kind == 'error' or str(kind).endswith('_error') or event.get('pass') is False),
                f'benchmark error at events.jsonl line {number}: {event}')
print(f'capture-intent: shots={len(shots)} gotos={sum(c.startswith("goto ") for c in commands)} exit=0 done=true')
PY
  exit 0
fi

if [[ "${1:-}" == --remote ]]; then
  TASK="${2:?task directory}"; RUN="${3:?run id}"; EXPECTED="${4:?shot count}"
  [[ "$TASK" == "$HOME/gothic-ai/texture-intent" && "$RUN" =~ ^[a-zA-Z0-9_-]+$ && "$EXPECTED" =~ ^[0-9]+$ ]] || exit 2
  INPUT="$TASK/inputs/$RUN"
  export INTENT_CAPTURE_ROOT="$INPUT/runtime"
  game_environment="$(python3 "$(dirname "$0")/ryzen_game.py")"
  eval "$game_environment"
  GAME_BIN="$INTENT_CAPTURE_ROOT/build/opengothic/Gothic2Notr" \
    bash "$(dirname "$0")/base-ensure.sh" "$GOTHIC_DIR"
  OUT="$TASK/runs/$RUN"
  [[ ! -e "$OUT" ]] || { echo "capture output already exists: $OUT" >&2; exit 2; }
  mkdir -p "$OUT"
  # The cgroup records the admitted gq job ID even when it starts before the
  # queue's first waiting-status message. Keep this with the captured evidence.
  { cat /proc/self/cgroup; printf 'owner=texture-intent class=quick threads=4 mem=5G gpu_mem=4G display=shared max=20m\n'; } \
    | tee "$OUT/gq-job.txt"
  # This environment is explicit even if the submitting session uses generated materials.
  OPENGOTHIC_CONTENT="$INPUT/content" GAME_TIMEOUT=1150 \
    bash "$INTENT_CAPTURE_ROOT/tools/run-ryzen.sh" "$OUT" "$INTENT_CAPTURE_ROOT/tools/city-intent.bench" "$INPUT/config.tsv"
  bash "$INTENT_CAPTURE_ROOT/tools/capture-intent.sh" --check "$OUT/original" \
    "$INTENT_CAPTURE_ROOT/tools/city-intent.bench" "$EXPECTED"
  exit 0
fi

ROOT="$(cd "$(dirname "$0")/../.." && pwd -P)"
HOST="${RYZEN_HOST:-ryzen}"
SCENARIO=''; OUT=''; EXPECTED=71
while (( $# )); do
  case "$1" in
    --scenario) SCENARIO="${2:?--scenario needs a file}"; shift 2 ;;
    --output) OUT="${2:?--output needs a directory}"; shift 2 ;;
    --expected-shots) EXPECTED="${2:?--expected-shots needs a count}"; shift 2 ;;
    -h|--help) sed -n '2,5p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ -r "$SCENARIO" && -n "$OUT" && "$EXPECTED" =~ ^[0-9]+$ && "$EXPECTED" -gt 0 ]] || {
  echo 'need --scenario FILE --output OUTSIDE_GIT and a positive expected shot count' >&2; exit 2;
}
GAME_ENV=()
for name in OPENGOTHIC_BASE OPENGOTHIC_BASE_FINGERPRINT OPENGOTHIC_VIDEOS_FINGERPRINT OPENGOTHIC_BASE_CACHE OPENGOTHIC_BASE_STORE; do
  [[ ! ${!name+x} ]] || GAME_ENV+=("$name=${!name}")
done
printf -v GAME_CHECK '%q ' env ${GAME_ENV[@]+"${GAME_ENV[@]}"} python3 -c "$(cat "$ROOT/scripts/ryzen_game.py")" --check
ssh -T -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "$GAME_CHECK"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd -P)"
case "$OUT" in "$ROOT"|"$ROOT"/*) echo '--output must be outside git' >&2; exit 2 ;; esac
STAGING="$(mktemp -d "$OUT/$(date -u +%Y%m%dT%H%M%SZ)-XXXXXXXX")"
RUN="${STAGING##*/}"
mkdir -p "$STAGING/tools" "$STAGING/content/materials"
printf '{}\n' >"$STAGING/content/materials/materials.json"
printf 'original\t\t\n' >"$STAGING/config.tsv"
cp "$SCENARIO" "$STAGING/tools/city-intent.bench"
# Fail locally before preparing a remote snapshot for a wrong/incomplete scenario.
python3 - "$STAGING/tools/city-intent.bench" "$EXPECTED" <<'PY'
from pathlib import Path
import re
import sys
commands = [line.split('#', 1)[0].strip() for line in Path(sys.argv[1]).read_text().splitlines()]
shots = [line[5:] for line in commands if line.startswith('shot ')]
if len(shots) != int(sys.argv[2]) or len(set(shots)) != len(shots) or not all(re.fullmatch(r'[A-Za-z0-9_-]+', s) for s in shots):
    raise SystemExit(f'capture-intent: need {sys.argv[2]} uniquely named safe shots; got {len(shots)}')
if 'set time 12 0' not in commands or any(line.startswith('set time ') and line != 'set time 12 0' for line in commands):
    raise SystemExit('capture-intent: scenario must explicitly use daytime: set time 12 0')
PY
if [[ -z "${SAVE:-}" ]]; then
  COMMON="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)"
  SAVE="$(dirname "$COMMON")/save_slot_1.sav"
fi
[[ -r "$SAVE" ]] || { echo "missing standard bench save: $SAVE" >&2; exit 2; }
cp "$SAVE" "$STAGING/save_slot_1.sav"
TOOLS="$ROOT/reports/render-features/2026-10-18/tools"
cp "$ROOT/scripts/content/capture-intent.sh" "$ROOT/scripts/base-ensure.sh" "$ROOT/scripts/ryzen_game.py" "$STAGING/tools/"
cp "$TOOLS/gpu-nvidia.py" "$TOOLS/discard-ffmpeg.sh" "$STAGING/tools/"
python3 - "$TOOLS/run-ryzen.sh" "$STAGING/tools/run-ryzen.sh" <<'PY'
from pathlib import Path
import sys
import re
source, output = map(Path, sys.argv[1:])
text = source.read_text()
replacements = {
    'R="$HOME/gothic-ai/render-features"': 'R="${INTENT_CAPTURE_ROOT:?}"',
    'OPENGOTHIC_FRAME_PROFILE="$D/frames.csv" ': '',
}
for old, new in replacements.items():
    if text.count(old) != 1:
        raise SystemExit(f'render-feature Ryzen runner changed: {old}')
    text = text.replace(old, new)
text, count = re.subn(r'(?<= -g )"[^"]+"', '"${GOTHIC_DIR:?}"', text, count=1)
if count != 1:
    raise SystemExit("render-feature runner game argument changed")
output.write_text(text)
PY
DIGEST="$(python3 - "$ROOT/OpenGothic" <<'PY'
import hashlib
from pathlib import Path
import sys
root = Path(sys.argv[1])
digest = hashlib.sha256()
for path in sorted(root.rglob('*')):
    if 'build' in path.relative_to(root).parts:
        continue
    if path.is_file():
        digest.update(path.relative_to(root).as_posix().encode() + b'\0')
        digest.update(hashlib.sha256(path.read_bytes()).digest())
print(digest.hexdigest())
PY
)"
printf '%s\n' "$DIGEST" >"$STAGING/source.sha256"
TASK="$(ssh -T -o BatchMode=yes -o ConnectTimeout=10 "$HOST" 'printf "%s" "$HOME/gothic-ai/texture-intent"')"
printf -v REMOTE_ARGS '%q %q %q' "$TASK" "$DIGEST" "$RUN"
ssh -T "$HOST" "bash -s -- $REMOTE_ARGS" <<'SH'
set -euo pipefail
task="$1"; digest="$2"; run="$3"
[[ "$task" == "$HOME/gothic-ai/texture-intent" && "$digest" =~ ^[a-f0-9]{64}$ && "$run" =~ ^[a-zA-Z0-9_-]+$ ]]
df -h "$HOME"
bin="$HOME/gothic-ai/engine-cache/$digest/Gothic2Notr"
[[ -x "$bin" ]] || { echo "no source-matching engine cache for $digest" >&2; exit 2; }
grep -aq OPENGOTHIC_CONTENT "$bin" || { echo 'cached engine lacks OPENGOTHIC_CONTENT' >&2; exit 2; }
# 71 1080p PNGs plus the binary/save/runtime snapshot need room before submission.
available=$(df -Pk "$HOME" | awk 'NR==2 {print $4}')
(( available >= 2097152 )) || { echo 'less than 2 GiB available on Ryzen' >&2; exit 2; }
mkdir -p "$task/inputs"
mkdir "$task/inputs/$run"
runtime="$task/inputs/$run/runtime"
mkdir -p "$runtime/build/opengothic" "$runtime/tools" "$runtime/hd-textures/textures" "$task/inputs/$run/content"
cp -p "$bin" "$runtime/build/opengothic/Gothic2Notr"
cp -p "$(dirname "$bin")/content-convert" "$runtime/build/opengothic/content-convert"
printf '%s\n' "$digest" >"$runtime/build/source.sha256"
SH
RUNTIME="$TASK/inputs/$RUN/runtime"
rsync -a -- "$STAGING/tools/" "$HOST:$RUNTIME/tools/"
rsync -a -- "$STAGING/tools/gpu-nvidia.py" "$STAGING/tools/discard-ffmpeg.sh" "$HOST:$RUNTIME/"
rsync -a -- "$STAGING/content/" "$HOST:$TASK/inputs/$RUN/content/"
rsync -a -- "$STAGING/config.tsv" "$HOST:$TASK/inputs/$RUN/config.tsv"
rsync -a -- "$STAGING/save_slot_1.sav" "$HOST:$RUNTIME/save_slot_1.sav"
printf -v REMOTE_INPUT '%q' "$TASK/inputs/$RUN"
ssh -T "$HOST" "chmod -R a-w -- $REMOTE_INPUT"
printf 'Capture input: %s:%s\nLocal output: %s\n' "$HOST" "$TASK/inputs/$RUN" "$STAGING" | tee "$STAGING/locations.txt"
set +e
GQ_OWNER=texture-intent "$ROOT/scripts/gq" --host "$HOST" run \
  --class quick --threads 4 --mem 5G --gpu-mem 4G --display --display-shared \
  --max 20m --name 'city texture intent daytime captures' -- \
  env ${GAME_ENV[@]+"${GAME_ENV[@]}"} bash "$RUNTIME/tools/capture-intent.sh" --remote "$TASK" "$RUN" "$EXPECTED" 2>&1 | tee "$STAGING/capture.log"
rc="${PIPESTATUS[0]}"
set -e
rsync -a -- "$HOST:$TASK/runs/$RUN/" "$STAGING/captures/" || { [[ "$rc" != 0 ]] || rc=1; }
if [[ "$rc" == 0 ]]; then
  bash "$STAGING/tools/capture-intent.sh" --check "$STAGING/captures/original" "$STAGING/tools/city-intent.bench" "$EXPECTED"
fi
printf 'City intent capture output: %s\n' "$STAGING"
exit "$rc"
