#!/usr/bin/env bash
# Capture the complete packed city set against an explicitly empty content root.
# Usage: capture-city.sh --content-root <packed staging root> --output <outside-git directory>
# Optional: --set city-1|city-2|city-2-imagegen|city-3; imagegen needs --flux-root <packed city-2 root>.
# --mac-city-fps uses profiled BIN and GOTHIC_DIR for city-1/2/3 (waits <=40 minutes).
# Ryzen: source-matching cached engine, private ~/gothic-ai/city-set-N runtime, gq.
# No build, default content installation, model service or engine change is performed.
set -euo pipefail

if [[ "${1:-}" == --check-fps ]]; then
  python3 - "${2:?capture directory}" "${@:3}" <<'PY'
import csv
import json
import math
from pathlib import Path
import sys

root = Path(sys.argv[1])
spots = sys.argv[2:] or ['city']
def require(ok, reason):
    if not ok:
        raise SystemExit(f'capture-city FPS: {reason}')
summary = root / 'summary.json'
require(summary.is_file(), 'missing summary.json')
rows = json.loads(summary.read_text())
require(isinstance(rows, list), 'summary.json must contain an array')
for variant in ('before', 'after'):
    matches = [row for row in rows if isinstance(row, dict) and row.get('label') == variant]
    require(len(matches) == 1, f'{variant}: missing or duplicate analysis')
    result = matches[0]
    require(result.get('exit') == '0' and result.get('done') is True, f'{variant}: game did not finish successfully')
    profile = root / variant / 'frames.csv'
    require(profile.is_file() and profile.stat().st_size > 0,
            f'{variant}: missing frames.csv; build with OPENGOTHIC_ENABLE_FRAME_PROFILE=ON')
    with profile.open(newline='') as stream:
        reader = csv.DictReader(stream)
        require({'flags', 'outcome', 'begin_ns', 'end_ns', 'dt'} <= set(reader.fieldnames or []),
                f'{variant}: invalid frame profile header')
        profile_rows = list(reader)
    require(len(profile_rows) > 2, f'{variant}: frame profile has no measurement window')
    require(result.get('overflow') == 0, f'{variant}: frame profile overflowed or overflow count is missing')
    # Match analyze.py: singleton capture groups are shots; multi-frame groups are recordings.
    capture_groups = []
    for index, row in enumerate(profile_rows):
        if int(row['flags']) & 4:
            if capture_groups and index - capture_groups[-1][-1] <= 2:
                capture_groups[-1].append(index)
            else:
                capture_groups.append([index])
    shots = [group[0] for group in capture_groups if len(group) == 1]
    for number, spot in enumerate(spots):
        metric = result.get('spots', {}).get(spot)
        require(isinstance(metric, dict), f'{variant}/{spot}: missing FPS window')
        require(3 * number + 2 < len(shots), f'{variant}/{spot}: missing shot markers')
        start, end = shots[3 * number:3 * number + 2]
        window = profile_rows[start + 1:end]
        game_ms = sum(int(row['dt']) for row in window if int(row['dt']) > 0)
        require(game_ms >= 19000, f'{variant}/{spot}: missing or incomplete 20-second game-time window')
        presented = sum(int(row['outcome']) == 1 for row in window)
        measured_wall = (int(profile_rows[end]['begin_ns']) - int(profile_rows[start]['end_ns'])) / 1e6
        frames, wall, fps = (metric.get(key) for key in ('frames', 'wall_ms', 'fps'))
        require(type(frames) is int and frames > 0 and frames == presented,
                f'{variant}/{spot}: missing or invalid presented frame count')
        require(type(wall) in (int, float) and math.isfinite(wall) and wall > 0
                and math.isclose(wall, measured_wall, rel_tol=1e-6),
                f'{variant}/{spot}: missing or invalid wall-time window')
        require(type(fps) in (int, float) and math.isfinite(fps) and fps > 0,
                f'{variant}/{spot}: missing or invalid FPS')
        require(math.isclose(fps, frames * 1000 / wall, rel_tol=1e-6),
                f'{variant}/{spot}: FPS does not match its frames and window')
        print(f'{variant}/{spot}: fps={fps:.3f} frames={frames} wall_ms={wall:.3f} game_ms={game_ms}')
PY
  exit 0
fi

if [[ "${1:-}" == --check ]]; then
  python3 - "${2:?capture directory}" "${3:?scenario}" "${4:?material index}" "${5:?before, flux or after}" "${6:-all}" <<'PY'
import json
from pathlib import Path
import re
import sys

directory, scenario, index = map(Path, sys.argv[1:4])
variant = sys.argv[4]
coverage = sys.argv[5]
def require(ok, reason):
    if not ok:
        raise SystemExit(f'capture-city: {directory.name}: {reason}')
require(variant in ('before', 'flux', 'after'), 'unknown variant')
require(coverage in ('all', 'seen'), 'unknown target coverage')
require((directory / 'exit.txt').read_text().strip() == '0', 'game exit was not zero')
log = (directory / 'game.log').read_text(errors='replace')
require('[bench] done' in log, 'scenario did not finish')
commands = [line.split('#', 1)[0].strip() for line in scenario.read_text().splitlines()]
for command in commands:
    if command.startswith(('goto ', 'shot ')):
        require(f'[bench] {command}\n' in log, f'command not reached: {command}')
    if command.startswith('shot '):
        image = directory / (command[5:] + '.png')
        require(image.is_file() and image.stat().st_size > 0, f'missing shot: {image.name}')
require(not re.search(r'materials: (invalid|unsupported|unable)|\[bench\] (can not|unknown|marvin failed|goto failed)', log),
        'material or benchmark error in game.log')
expected = json.loads(index.read_text())
count = len(expected) if variant != 'before' else 0
require(re.search(rf'materials: {count} entries from ', log), f'material index did not load {count} entries')
events = directory / 'events.jsonl'
if events.is_file():
    for line in events.read_text().splitlines():
        event = json.loads(line)
        kind = event.get('kind', '')
        require(not (kind == 'error' or kind.endswith('_error') or event.get('pass') is False),
                f'benchmark error: {event}')
census = json.loads((directory / 'census.json').read_text())
def texture_key(name):
    name = name.replace('\\', '/').rsplit('/', 1)[-1].upper()
    if name.endswith('.TGA'):
        return name[:-4]
    if name.endswith('.TEX'):
        name = name[:-4]
        return name[:-2] if name.endswith('-C') else name
    return name
for section, source in (('files', 'source'), ('replacements', 'replacement')):
    require(isinstance(census.get(section), list), f'census.{section} is missing')
    for row in census[section]:
        require(isinstance(row, dict) and all(isinstance(row.get(k), str) and row[k] for k in ('name', 'kind', source))
                and type(row.get('opens')) is int and row['opens'] > 0,
                f'invalid census row: {row}')
targets = {texture_key(name) for name in expected}
albedos = {texture_key(name): 'materials/' + entry['albedo'].replace('\\', '/').casefold()
           for name, entry in expected.items()}
opened = {texture_key(row['name']) for row in census['files'] if row['kind'] == 'texture'}
opened.update(texture_key(row['name']) for row in census['replacements'] if row['kind'] == 'basetexture')
replaced = {texture_key(row['name']) for row in census['replacements'] if row['kind'] == 'texture'}
if variant != 'before':
    require(not (targets & opened), 'original target textures opened: ' + ', '.join(sorted(targets & opened)))
    require(bool(targets & replaced), 'no target textures were replaced')
    if coverage == 'all':
        require(not (targets - replaced), 'target textures not replaced: ' + ', '.join(sorted(targets - replaced)))
    for row in census['replacements']:
        key = texture_key(row['name'])
        if row['kind'] == 'texture' and key in targets:
            replacement = row['replacement'].replace('\\', '/').casefold()
            require(replacement == albedos[key] or replacement.endswith('/' + albedos[key]),
                    'replacement did not use the packed set: ' + row['name'])
else:
    require(not (targets & replaced), 'empty baseline unexpectedly replaced target textures')
    require(bool(targets & opened), 'baseline did not open any target textures')
print(f'{variant}: shots={sum(c.startswith("shot ") for c in commands)} '
      f'targets={len(targets)} replaced={len(targets & replaced)} originals_opened={len(targets & opened)}')
PY
  exit 0
fi

if [[ "${1:-}" == --compose ]]; then
  uv run --no-project --with pillow python3 - "${2:?capture directory}" "${3:?output directory}" "${4:?set name}" <<'PY'
from pathlib import Path
import sys
from PIL import Image, ImageDraw, ImageFont

captures, output = map(Path, sys.argv[1:3])
set_name = sys.argv[3]
if set_name not in ('city-1', 'city-2', 'city-2-imagegen', 'city-3'):
    raise SystemExit('unknown material set')
output.mkdir()
try:
    font = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf', 22)
except OSError:
    font = ImageFont.load_default()
triple = set_name == 'city-2-imagegen'
variants = ('before', 'flux', 'after') if triple else ('before', 'after')
labels = ('original', 'city-2 (FLUX)', 'imagegen') if triple else variants
if set_name == 'city-3':
    labels = ('original', 'city-3')
    shots = [(spot, '', '12:00') for spot in (
        'city', 'market', 'harbour', 'upper', 'house',
        'facade_close', 'facade_corner', 'facade_full',
        'upper_modules', 'harbour_modules', 'interior_modules',
        'alpha_flag_close', 'alpha_flag_far',
        'alpha_ivy_border_close', 'alpha_ivy_border_far',
        'alpha_ivy_full_close', 'alpha_ivy_full_far')]
    shots += [(spot, '_night', '23:00') for spot in ('facade_close', 'market')]
elif triple:
    shots = [(spot, suffix, hour) for spot in ('facade_close', 'market', 'facade_corner', 'harbour')
             for suffix, hour in ((('', '12:00'), ('_night', '23:00'))
                                  if spot == 'facade_close' else (('', '12:00'),))]
else:
    spots = ('city', 'market', 'harbour', 'upper', 'house') if set_name == 'city-1' else (
        'city', 'harbour', 'market', 'facade_close', 'facade_corner')
    shots = [(spot, suffix, hour) for spot in spots
             for suffix, hour in ((('', '12:00'),) if set_name == 'city-2' and spot in ('city', 'harbour')
                                  else (('', '12:00'), ('_night', '23:00')))]
for spot, suffix, hour in shots:
    images = []
    for variant in variants:
        with Image.open(captures / variant / f'{spot}{suffix}.png') as source:
            image = source.convert('RGB')
            image.thumbnail((768, 432), Image.Resampling.LANCZOS)
            images.append(image)
    width, height = max(i.width for i in images), max(i.height for i in images)
    board = Image.new('RGB', (len(variants) * width, height + 40), '#17191b')
    draw = ImageDraw.Draw(board)
    for position, (label, image) in enumerate(zip(labels, images)):
        board.paste(image, (position * width, 40))
        draw.text((position * width + 12, 8), f'{spot} {hour} | {label}', font=font, fill='white')
    board.save(output / f'{spot}{suffix}.jpg', quality=88, optimize=True)
print(f'{len(shots)} {"original/FLUX/imagegen JPG triples" if triple else "before/after JPG pairs"}: {output}')
PY
  exit 0
fi

if [[ "${1:-}" == --append-census ]]; then
  python3 - "${2:?staged scenario}" "${3:?inspection scenario}" "${4:?material index}" <<'PY'
import json
from pathlib import Path
import re
import sys

scenario, inspection, index = map(Path, sys.argv[1:])
targets = json.loads(index.read_text())
if not isinstance(targets, dict) or not targets:
    raise SystemExit('census tour requires a nonempty packed material index')
blocks = {}
current = None
for line in inspection.read_text().splitlines():
    match = re.match(r'^# ([A-Z0-9_]+\.TGA) \|', line)
    if match:
        current = match[1]
        blocks[current] = []
    command = line.split('#', 1)[0].strip()
    if current is not None and command.startswith(('view ', 'cam ', 'goto ', 'wait ')):
        blocks[current].append(command)
missing = sorted(set(targets) - blocks.keys())
if missing:
    raise SystemExit('missing census camera: ' + ', '.join(missing))
for name in targets:
    commands = blocks[name]
    if not any(c.startswith('goto ') for c in commands) or 'cam hold' not in commands:
        raise SystemExit('incomplete census camera: ' + name)
lines = ['# Census-only surface tour: visit every packed target without additional boards.',
         'set time 12 0', 'view hide animated morph pfx', 'cam lift off']
for name in targets:
    lines += ['', '# ' + name] + blocks[name]
scenario.write_text(scenario.read_text().rstrip() + '\n\n' + '\n'.join(lines) + '\n')
print(f'Census tour: {len(targets)} packed materials; no additional shots')
PY
  exit 0
fi

if [[ "${1:-}" == --remote ]]; then
  TASK="${2:?remote task directory}"; RUN="${3:?run id}"; SET="${4:-city-1}"
  [[ "$SET" == city-1 || "$SET" == city-2 || "$SET" == city-2-imagegen || "$SET" == city-3 ]] || exit 2
  [[ "$TASK" == "$HOME/gothic-ai/city-set-${SET#city-}" && "$RUN" =~ ^[a-zA-Z0-9_-]+$ ]] || exit 2
  CAPTURE="$TASK/inputs/$RUN"
  export CITY_CAPTURE_ROOT="$CAPTURE/runtime"
  game_environment="$(python3 "$(dirname "$0")/ryzen_game.py")"
  eval "$game_environment"
  GAME_BIN="$CITY_CAPTURE_ROOT/build/opengothic/Gothic2Notr" \
    bash "$(dirname "$0")/base-ensure.sh" "$GOTHIC_DIR"
  OUT="$TASK/runs/$RUN"
  mkdir -p "$OUT"
  VARIANTS=(before after)
  [[ "$SET" != city-2-imagegen ]] || VARIANTS=(before flux after)
  for variant in "${VARIANTS[@]}"; do
    printf '%s\t\t\n' "$variant" >"$CAPTURE/config.tsv"
    OPENGOTHIC_CONTENT="$CAPTURE/content/$variant" OPENGOTHIC_CENSUS="$OUT/$variant/census.json" \
      GAME_TIMEOUT="${GAME_TIMEOUT:-420}" \
      bash "$CITY_CAPTURE_ROOT/tools/run-ryzen.sh" "$OUT" "$CITY_CAPTURE_ROOT/tools/city.bench" "$CAPTURE/config.tsv"
    index_variant="$variant"
    [[ "$variant" != before ]] || index_variant=after
    bash "$CITY_CAPTURE_ROOT/tools/capture-city.sh" --check "$OUT/$variant" \
      "$CITY_CAPTURE_ROOT/tools/city.bench" "$CAPTURE/content/$index_variant/materials/materials.json" "$variant"
  done
  exit 0
fi

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HOST="${RYZEN_HOST:-ryzen}"
CONTENT=''; FLUX=''; OUT=''; MAC=0; SET=city-1
while (( $# )); do
  case "$1" in
    --content-root) CONTENT="${2:?--content-root needs a directory}"; shift 2 ;;
    --flux-root) FLUX="${2:?--flux-root needs a directory}"; shift 2 ;;
    --output) OUT="${2:?--output needs a directory}"; shift 2 ;;
    --set) SET="${2:?--set needs city-1, city-2, city-2-imagegen or city-3}"; shift 2 ;;
    --mac-city-fps) MAC=1; shift ;;
    -h|--help) sed -n '2,6p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ "$SET" == city-1 || "$SET" == city-2 || "$SET" == city-2-imagegen || "$SET" == city-3 ]] || { echo '--set must be city-1 or city-2 or city-2-imagegen or city-3' >&2; exit 2; }
if [[ "$SET" == city-2-imagegen ]]; then
  [[ -n "$FLUX" && -d "$FLUX" ]] || { echo 'city-2-imagegen requires --flux-root with packed city-2 content' >&2; exit 2; }
  [[ "$MAC" == 0 ]] || { echo 'city-2-imagegen capture runs on Ryzen; omit --mac-city-fps' >&2; exit 2; }
  FLUX="$(cd "$FLUX" && pwd -P)"
else
  [[ -z "$FLUX" ]] || { echo '--flux-root requires --set city-2-imagegen' >&2; exit 2; }
fi
[[ -n "$CONTENT" && -n "$OUT" ]] || { echo 'need --content-root and --output' >&2; exit 2; }
[[ -d "$CONTENT" ]] || { echo 'content root does not exist' >&2; exit 2; }
CONTENT="$(cd "$CONTENT" && pwd -P)"
if [[ "$MAC" == 0 ]]; then
  GAME_ENV=()
  for name in OPENGOTHIC_BASE OPENGOTHIC_BASE_FINGERPRINT OPENGOTHIC_VIDEOS_FINGERPRINT OPENGOTHIC_BASE_CACHE OPENGOTHIC_BASE_STORE; do
    [[ ! ${!name+x} ]] || GAME_ENV+=("$name=${!name}")
  done
  printf -v GAME_CHECK '%q ' env ${GAME_ENV[@]+"${GAME_ENV[@]}"} python3 -c "$(cat "$ROOT/scripts/ryzen_game.py")" --check
  ssh -T -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "$GAME_CHECK"
fi
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd -P)"
case "$OUT" in "$ROOT"|"$ROOT"/*) echo '--output must be outside git' >&2; exit 2 ;; esac
STAGING="$(mktemp -d "$OUT/$(date -u +%Y%m%dT%H%M%SZ)-XXXXXXXX")"
RUN="${STAGING##*/}"
mkdir -p "$STAGING/content/before/materials" "$STAGING/content/after/materials" "$STAGING/tools"
printf '{}\n' >"$STAGING/content/before/materials/materials.json"
# Copy only maps named by already-packed indices; never read original game images.
COPY_ARGS=("$CONTENT/materials" "$STAGING/content/after/materials")
if [[ "$SET" == city-2-imagegen ]]; then
  mkdir -p "$STAGING/content/flux/materials"
  COPY_ARGS+=("$FLUX/materials" "$STAGING/content/flux/materials")
fi
python3 - "${COPY_ARGS[@]}" <<'PY'
import json
from pathlib import Path
import shutil
import sys
arguments = list(map(Path, sys.argv[1:]))
indices = []
for source, target in zip(arguments[::2], arguments[1::2]):
    index = json.loads((source / 'materials.json').read_text())
    if not isinstance(index, dict) or not index:
        raise SystemExit('packed materials.json must be a nonempty object')
    if indices and index.keys() != indices[0].keys():
        raise SystemExit('FLUX and imagegen sets must replace the same textures')
    indices.append(index)
    for name, entry in index.items():
        if not isinstance(name, str) or not name.upper().endswith('.TGA') or not isinstance(entry, dict):
            raise SystemExit(f'invalid packed material: {name}')
        for channel in ('albedo', 'normal', 'orm'):
            value = entry.get(channel)
            if not isinstance(value, str) or not value:
                raise SystemExit(f'{name}: missing {channel}')
            relative = Path(value)
            path = (source / relative).resolve()
            if relative.is_absolute() or '..' in relative.parts or not path.is_relative_to(source.resolve()) or path.suffix.lower() != '.dds':
                raise SystemExit(f'unsafe DDS path: {value}')
            destination = target / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
    shutil.copy2(source / 'materials.json', target / 'materials.json')
    print(f'Packed city set: {len(index)} materials ({source.parent.name})')
PY
if [[ -z "${SAVE:-}" ]]; then
  COMMON="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)"
  SAVE="$(dirname "$COMMON")/save_slot_1.sav"
fi
[[ -r "$SAVE" ]] || { echo "missing standard bench save: $SAVE" >&2; exit 2; }
cp "$SAVE" "$STAGING/save_slot_1.sav"
TOOLS="$ROOT/reports/render-features/2026-10-18/tools"
SCENARIO="$ROOT/scripts/content/city.bench"
if [[ "$MAC" == 0 ]]; then
  case "$SET" in
    city-2|city-2-imagegen) SCENARIO="$ROOT/scripts/content/city-2.bench" ;;
    city-3) SCENARIO="$ROOT/scripts/content/city-3.bench" ;;
  esac
fi
cp "$SCENARIO" "$STAGING/tools/city.bench"
if [[ "$SET" == city-3 && "$MAC" == 0 ]]; then
  bash "$ROOT/scripts/content/capture-city.sh" --append-census "$STAGING/tools/city.bench" \
    "$ROOT/scripts/content/city-intent.bench" "$STAGING/content/after/materials/materials.json"
fi
cp "$ROOT/scripts/content/capture-city.sh" "$ROOT/scripts/base-ensure.sh" "$ROOT/scripts/ryzen_game.py" "$STAGING/tools/"
cp "$TOOLS/analyze.py" "$TOOLS/discard-ffmpeg.sh" "$STAGING/tools/"

if [[ "$MAC" == 1 ]]; then
  : "${BIN:?--mac-city-fps requires BIN pointing to the existing Mac game binary}"
  : "${GOTHIC_DIR:?--mac-city-fps requires GOTHIC_DIR}"
  [[ -x "$BIN" && -d "$GOTHIC_DIR" ]] || { echo 'missing Mac binary or game directory' >&2; exit 2; }
  [[ "$(uname)" == Darwin ]] || { echo '--mac-city-fps requires macOS' >&2; exit 2; }
  cp -p "$BIN" "$STAGING/Gothic2Notr"
  cp -p "${CONTENT_CONVERT:-$(dirname "$BIN")/content-convert}" "$STAGING/content-convert"
  export BIN="$STAGING/Gothic2Notr" SAVE="$STAGING/save_slot_1.sav"
  cp "$TOOLS/gpu-apple.py" "$STAGING/tools/"
  mkdir "$STAGING/tools/empty-textures"
  python3 - "$TOOLS/run-mac.sh" "$STAGING/tools/run-mac.sh" "$STAGING/tools/city.bench" <<'PY'
from pathlib import Path
import sys
source, output, scenario = map(Path, sys.argv[1:])
text = source.read_text()
old = 'TEX="$HOME/.local/share/gothic-ai/hd-textures/textures"'
if text.count(old) != 1:
    raise SystemExit('render-feature Mac runner texture-root assignment changed')
output.write_text(text.replace(old, 'TEX="$S/empty-textures"'))
lines = []
for line in scenario.read_text().splitlines():
    lines.append(line)
    if line == 'shot city_night':
        lines.append('wait 500')
        break
scenario.write_text('\n'.join(lines) + '\n')
PY
  wait_limit="${MAC_WAIT_SECONDS:-2400}"
  [[ "$wait_limit" =~ ^[0-9]+$ && "$wait_limit" -le 2400 ]] || { echo 'MAC_WAIT_SECONDS must be 0..2400' >&2; exit 2; }
  wait_start=$SECONDS
  while pgrep -x Gothic2Notr >/dev/null; do
    (( SECONDS - wait_start < wait_limit )) || { echo "Mac occupied for ${wait_limit}s; no Mac game started" >&2; exit 75; }
    sleep 5
  done
  GAME_BIN="$BIN" bash "$ROOT/scripts/base-ensure.sh" "$GOTHIC_DIR"
  mkdir -p "$STAGING/captures"
  for variant in before after; do
    printf '%s\t\t\n' "$variant" >"$STAGING/config.tsv"
    OPENGOTHIC_CONTENT="$STAGING/content/$variant" OPENGOTHIC_CENSUS="$STAGING/captures/$variant/census.json" \
      bash "$STAGING/tools/run-mac.sh" "$STAGING/captures" "$STAGING/tools/city.bench" "$STAGING/config.tsv"
    # City-only FPS checks the observed target textures; the Ryzen tour checks the full set.
    bash "$STAGING/tools/capture-city.sh" --check "$STAGING/captures/$variant" \
      "$STAGING/tools/city.bench" "$STAGING/content/after/materials/materials.json" "$variant" seen
  done
  python3 "$STAGING/tools/analyze.py" "$STAGING/captures" city
  bash "$STAGING/tools/capture-city.sh" --check-fps "$STAGING/captures" city
  printf 'Mac city FPS output: %s\n' "$STAGING"
  exit 0
fi

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
python3 - "$TOOLS/run-ryzen.sh" "$STAGING/tools/run-ryzen.sh" <<'PY'
from pathlib import Path
import sys
import re
source, output = map(Path, sys.argv[1:])
text = source.read_text()
old = 'R="$HOME/gothic-ai/render-features"'
if text.count(old) != 1:
    raise SystemExit('render-feature Ryzen runner task-root assignment changed')
text = text.replace(old, 'R="${CITY_CAPTURE_ROOT:?}"')
text, count = re.subn(r'(?<= -g )"[^"]+"', '"${GOTHIC_DIR:?}"', text, count=1)
if count != 1:
    raise SystemExit("render-feature runner game argument changed")
output.write_text(text)
PY
cp "$TOOLS/gpu-nvidia.py" "$STAGING/tools/"
TASK_NAME="city-set-${SET#city-}"
printf -v TASK_QUERY 'printf "%%s" "$HOME/gothic-ai/%s"' "$TASK_NAME"
TASK="$(ssh -T -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "$TASK_QUERY")"
printf -v REMOTE_ARGS '%q %q %q %q' "$TASK" "$DIGEST" "$RUN" "$SET"
ssh -T "$HOST" "bash -s -- $REMOTE_ARGS" <<'SH'
set -euo pipefail
task="$1"; digest="$2"; run="$3"; set_name="$4"
[[ "$set_name" == city-1 || "$set_name" == city-2 || "$set_name" == city-2-imagegen || "$set_name" == city-3 ]]
[[ "$task" == "$HOME/gothic-ai/city-set-${set_name#city-}" && "$digest" =~ ^[a-f0-9]{64}$ && "$run" =~ ^[a-zA-Z0-9_-]+$ ]]
df -h "$HOME"
bin="$HOME/gothic-ai/engine-cache/$digest/Gothic2Notr"
[[ -x "$bin" ]] || { echo "no source-matching engine cache for $digest" >&2; exit 2; }
for feature in OPENGOTHIC_CONTENT OPENGOTHIC_CENSUS; do
  grep -aq "$feature" "$bin" || { echo "cached engine lacks $feature" >&2; exit 2; }
done
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
rsync -a -- "$STAGING/save_slot_1.sav" "$HOST:$RUNTIME/save_slot_1.sav"
set +e
GQ_OWNER="$TASK_NAME" "$ROOT/scripts/gq" --host "$HOST" run \
  --class quick --threads 4 --mem 5G --gpu-mem 4G --display --display-exclusive \
  --max "${CITY_CAPTURE_MAX:-20m}" --wait-max "${RYZEN_LOCK_WAIT:-900}" \
  --name "$SET captures" -- \
  env ${GAME_ENV[@]+"${GAME_ENV[@]}"} bash "$RUNTIME/tools/capture-city.sh" --remote "$TASK" "$RUN" "$SET" 2>&1 | tee "$STAGING/capture.log"
rc="${PIPESTATUS[0]}"
set -e
rsync -a -- "$HOST:$TASK/runs/$RUN/" "$STAGING/captures/" || { [[ "$rc" != 0 ]] || rc=1; }
if [[ "$rc" == 0 ]]; then
  SPOTS=(city market harbour upper house)
  [[ "$SET" == city-1 ]] || SPOTS=(city)
  python3 "$STAGING/tools/analyze.py" "$STAGING/captures" "${SPOTS[@]}"
  BOARDS=pairs
  [[ "$SET" != city-2-imagegen ]] || BOARDS=triples
  bash "$STAGING/tools/capture-city.sh" --compose "$STAGING/captures" "$STAGING/$BOARDS" "$SET"
fi
printf 'City capture output: %s\n' "$STAGING"
exit "$rc"
