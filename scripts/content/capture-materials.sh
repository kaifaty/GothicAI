#!/usr/bin/env bash
# Capture eligible pavement/wall variants on Ryzen, using the render-feature runner.
# Usage: capture-materials.sh --content-root <registry content root> --output <local binary output>
# Optional: --registry <jsonl> --id <resource id> (repeatable).
# All writes on Ryzen stay in ~/gothic-ai/material-bakeoff; the engine cache and
# shared settings are only read. The exact source-matching cached engine is copied
# to our runtime before entering gq. No engine build or GPU model is started.
set -euo pipefail

if [[ "${1:-}" == --remote ]]; then
  TASK="${2:?remote task directory required}"
  RUN="${3:?run id required}"
  [[ "$TASK" == "$HOME/gothic-ai/material-bakeoff" ]] || { echo 'unexpected remote task directory' >&2; exit 2; }
  [[ "$RUN" =~ ^[a-zA-Z0-9_-]+$ ]] || { echo 'invalid capture run id' >&2; exit 2; }
  CAPTURE="$TASK/inputs/$RUN"
  export MATERIAL_CAPTURE_ROOT="$CAPTURE/runtime"
  game_environment="$(python3 "$(dirname "$0")/ryzen_game.py")"
  eval "$game_environment"
  GAME_BIN="$MATERIAL_CAPTURE_ROOT/build/opengothic/Gothic2Notr" \
    bash "$(dirname "$0")/base-ensure.sh" "$GOTHIC_DIR"
  OUT="$TASK/runs/$RUN"
  mkdir -p "$OUT"
  count=0
  for content in "$CAPTURE"/content/*; do
    [[ -f "$content/materials/materials.json" ]] || continue
    id="${content##*/}"
    [[ "$id" =~ ^[a-z0-9][a-z0-9_-]*$ ]] || { echo "invalid resource id: $id" >&2; exit 2; }
    printf '%s\t\t\n' "$id" >"$CAPTURE/config.tsv"
    # run-ryzen.sh is the original stand runner, with only its task root changed.
    OPENGOTHIC_CONTENT="$content" GAME_TIMEOUT="${GAME_TIMEOUT:-180}" \
      bash "$MATERIAL_CAPTURE_ROOT/tools/run-ryzen.sh" "$OUT" "$MATERIAL_CAPTURE_ROOT/tools/city.bench" "$CAPTURE/config.tsv"
    D="$OUT/$id"
    [[ "$(cat "$D/exit.txt")" == 0 && -s "$D/city.png" ]] || {
      echo "capture failed: $id (exit $(cat "$D/exit.txt"))" >&2; exit 1;
    }
    # A successful game can silently fall back when it cannot read the maps.
    grep -q 'materials: 1 entries from' "$D/game.log" || {
      echo "material index was not loaded: $id" >&2; exit 1;
    }
    grep -q '^\[bench\] goto NW_CITY_MERCHANT_SHOP01_FRONT_03$' "$D/game.log" &&
      grep -q '^\[bench\] shot city$' "$D/game.log" || {
      echo "city position/shot was not reached: $id" >&2; exit 1;
    }
    if grep -Eq 'materials: (invalid|unsupported|unable)|\[bench\] (can not|unknown|marvin failed|goto failed)' "$D/game.log"; then
      echo "material/bench error in $D/game.log" >&2; exit 1
    fi
    python3 - "$D/events.jsonl" <<'PY'
import json
from pathlib import Path
import sys
path = Path(sys.argv[1])
# This simple shot scenario need not emit structured events. The mandatory
# city/goto/shot and material-load checks above validate its actual work.
if not path.is_file():
    raise SystemExit(0)
for line in path.read_text().splitlines():
    event = json.loads(line)
    kind = event.get('kind', '')
    if kind == 'error' or kind.endswith('_error') or event.get('pass') is False:
        raise SystemExit(f'capture: benchmark error: {event}')
PY
    count=$((count + 1))
  done
  (( count > 0 )) || { echo 'no eligible packed materials to capture' >&2; exit 2; }
  echo "Captured $count city/noon screenshots in $OUT"
  exit 0
fi

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HOST="${RYZEN_HOST:-ryzen}"
REGISTRY="$ROOT/data/content/registry.jsonl"
CONTENT=''
OUT=''
PACK_ARGS=()
while (( $# )); do
  case "$1" in
    --registry) REGISTRY="${2:?--registry needs a file}"; shift 2 ;;
    --content-root) CONTENT="${2:?--content-root needs a directory}"; shift 2 ;;
    --output) OUT="${2:?--output needs a directory}"; shift 2 ;;
    --id) PACK_ARGS+=(--id "${2:?--id needs a resource id}"); shift 2 ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$CONTENT" && -n "$OUT" ]] || { echo 'need --content-root and --output (see --help)' >&2; exit 2; }
GAME_ENV=()
for name in OPENGOTHIC_BASE OPENGOTHIC_BASE_FINGERPRINT OPENGOTHIC_VIDEOS_FINGERPRINT OPENGOTHIC_BASE_CACHE OPENGOTHIC_BASE_STORE; do
  [[ ! ${!name+x} ]] || GAME_ENV+=("$name=${!name}")
done
printf -v GAME_CHECK '%q ' env ${GAME_ENV[@]+"${GAME_ENV[@]}"} python3 -c "$(cat "$ROOT/scripts/ryzen_game.py")" --check
ssh -T -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "$GAME_CHECK"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
case "$OUT" in "$ROOT"|"$ROOT"/*) echo '--output must be outside git (binary captures)' >&2; exit 2 ;; esac
STAGING="$(mktemp -d "$OUT/$(date -u +%Y%m%dT%H%M%SZ)-XXXXXXXX")"
RUN="${STAGING##*/}"
mkdir -p "$STAGING/content" "$STAGING/tools"
uv run --no-project --with numpy --with pillow python3 "$ROOT/scripts/content/pack_materials.py" \
  --registry "$REGISTRY" --content-root "$CONTENT" --output "$STAGING/content" ${PACK_ARGS[@]+"${PACK_ARGS[@]}"}

# Mirror scripts/ryzen.sh's content hash exactly. The engine is reused only if
# its entire current source tree, including uncommitted inputs, matches cache.
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
if [[ -z "${SAVE:-}" ]]; then
  COMMON="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)"
  SAVE="$(dirname "$COMMON")/save_slot_1.sav"
fi
[[ -r "$SAVE" ]] || { echo "missing standard bench save: $SAVE" >&2; exit 2; }
TOOLS="$ROOT/reports/render-features/2026-10-18/tools"
python3 - "$TOOLS/run-ryzen.sh" "$STAGING/tools/run-ryzen.sh" "$TOOLS/rf.bench" "$STAGING/tools/city.bench" <<'PY'
from pathlib import Path
import sys
import re
runner, output, bench, scenario = map(Path, sys.argv[1:])
source = runner.read_text()
old = 'R="$HOME/gothic-ai/render-features"'
if source.count(old) != 1:
    raise SystemExit('render-feature runner task-root assignment changed')
source = source.replace(old, 'R="${MATERIAL_CAPTURE_ROOT:?}"')
source, count = re.subn(r'(?<= -g )"[^"]+"', '"${GOTHIC_DIR:?}"', source, count=1)
if count != 1:
    raise SystemExit("render-feature runner game argument changed")
output.write_text(source)
lines = []
for line in bench.read_text().splitlines():
    # Preserve the original position, camera and 26 s settling interval.
    # Stop just after its day shot, before recording, night and farm sections.
    lines.append(line)
    if line.strip() == 'shot city':
        lines.append('wait 500')
        break
else:
    raise SystemExit('rf.bench no longer contains shot city')
scenario.write_text('\n'.join(lines) + '\n')
PY
cp "$TOOLS/gpu-nvidia.py" "$TOOLS/discard-ffmpeg.sh" "$STAGING/tools/"
cp "$ROOT/scripts/content/capture-materials.sh" "$ROOT/scripts/base-ensure.sh" "$ROOT/scripts/ryzen_game.py" "$STAGING/tools/"
cp "$SAVE" "$STAGING/save_slot_1.sav"
TASK="$(ssh -T -o BatchMode=yes -o ConnectTimeout=10 "$HOST" 'printf "%s" "$HOME/gothic-ai/material-bakeoff"')"
# Quoting is done by bash printf %q, never by interpolating a JSON string.
printf -v REMOTE_ARGS '%q %q %q' "$TASK" "$DIGEST" "$RUN"
ssh -T "$HOST" "bash -s -- $REMOTE_ARGS" <<'SH'
set -euo pipefail
task="$1"; digest="$2"; run="$3"
[[ "$task" == "$HOME/gothic-ai/material-bakeoff" && "$digest" =~ ^[a-f0-9]{64}$ ]]
df -h "$HOME"
bin="$HOME/gothic-ai/engine-cache/$digest/Gothic2Notr"
[[ -x "$bin" ]] || { echo "no engine cache for source $digest; prepare this revision through gq first" >&2; exit 2; }
grep -aq 'OPENGOTHIC_CONTENT' "$bin" || { echo 'cached engine lacks PBR content support' >&2; exit 2; }
mkdir -p "$task/inputs"
# A run owns every mutable input, including the runner and its root helpers.
# Reserve without -p so a duplicate run id cannot overwrite a queued capture.
mkdir "$task/inputs/$run"
runtime="$task/inputs/$run/runtime"
mkdir -p "$runtime/build/opengothic" "$runtime/tools" "$runtime/hd-textures/textures" "$task/inputs/$run/content"
cp -p "$bin" "$runtime/build/opengothic/Gothic2Notr"
cp -p "$(dirname "$bin")/content-convert" "$runtime/build/opengothic/content-convert"
printf '%s\n' "$digest" >"$runtime/build/source.sha256"
SH
RUNTIME="$TASK/inputs/$RUN/runtime"
rsync -a -- "$STAGING/tools/" "$HOST:$RUNTIME/tools/"
# The original stand runner reads these helpers directly beneath its task root.
rsync -a -- "$STAGING/tools/gpu-nvidia.py" "$STAGING/tools/discard-ffmpeg.sh" "$HOST:$RUNTIME/"
rsync -a -- "$STAGING/content/" "$HOST:$TASK/inputs/$RUN/content/"
rsync -a -- "$STAGING/save_slot_1.sav" "$HOST:$RUNTIME/save_slot_1.sav"
set +e
GQ_OWNER=material-bakeoff "$ROOT/scripts/gq" --host "$HOST" run \
  --class quick --threads 4 --mem 5G --gpu-mem 4G --display --display-exclusive \
  --max "${MATERIAL_CAPTURE_MAX:-20m}" --wait-max "${RYZEN_LOCK_WAIT:-900}" \
  --name 'material bakeoff city captures' -- \
  env ${GAME_ENV[@]+"${GAME_ENV[@]}"} bash "$RUNTIME/tools/capture-materials.sh" --remote "$TASK" "$RUN" 2>&1 | tee "$STAGING/capture.log"
rc="${PIPESTATUS[0]}"
set -e
rsync -a -- "$HOST:$TASK/runs/$RUN/" "$STAGING/captures/" || { [[ "$rc" != 0 ]] || rc=1; }
printf 'Capture output: %s\n' "$STAGING"
exit "$rc"
