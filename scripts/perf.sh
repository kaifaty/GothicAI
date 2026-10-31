#!/usr/bin/env bash
# Repeatable normal-clock frame profiling. See docs/performance.md.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/OpenGothic/build-perf"

fail() { echo "perf: $*" >&2; exit 2; }

if [[ "${PERF_REMOTE:-0}" == 1 ]]; then
  game_environment="$(python3 "$ROOT/scripts/ryzen_game.py")"
  eval "$game_environment"
fi

build_game() {
  local out="$1"
  mkdir -p "$out"
  local flags=(-DCMAKE_POLICY_VERSION_MINIMUM=3.5)
  if [[ "${PERF_REMOTE:-0}" == 1 ]]; then
    export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
    export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
    [[ -r "$VULKAN_SDK/include/vulkan/vulkan.h" && -r "$VULKAN_SDK/lib/libvulkan.so" ]] || fail 'incomplete Ryzen Vulkan SDK'
    flags+=("-DCMAKE_CXX_FLAGS=-I$VULKAN_SDK/include" "-DCMAKE_EXE_LINKER_FLAGS=-L$VULKAN_SDK/lib")
  fi
  cmake -S "$ROOT/OpenGothic" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DOPENGOTHIC_ENABLE_FRAME_PROFILE=ON -DOPENGOTHIC_ENABLE_APP_CADENCE=ON \
    "${flags[@]}" >"$out/configure.log" 2>&1 || { tail -60 "$out/configure.log"; return 1; }
  cmake --build "$BUILD" --target Gothic2Notr --parallel "${RYZEN_JOBS:-8}" \
    >"$out/build.log" 2>&1 || { tail -80 "$out/build.log"; return 1; }
  tail -5 "$out/build.log"
}

# These entry points run only inside the queue on Ryzen.
if [[ "${1:-}" == __build ]]; then shift; build_game "$1"; exit; fi
if [[ "${1:-}" == __run ]]; then shift; OUT="$1"; shift; REMOTE_RUN=1; else REMOTE_RUN=0; fi

PERF_WINDOW="${PERF_WINDOW:-1728x1117}"
PERF_REPEAT="${PERF_REPEAT:-1}"
PERF_WAIT="${PERF_WAIT:-600}"
[[ "$PERF_WINDOW" =~ ^[1-9][0-9]*x[1-9][0-9]*$ ]] || fail 'PERF_WINDOW must be WIDTHxHEIGHT'
[[ "$PERF_REPEAT" =~ ^[1-9][0-9]*$ ]] || fail 'PERF_REPEAT must be positive'
[[ "$PERF_WAIT" =~ ^[0-9]+$ ]] || fail 'PERF_WAIT must be seconds'
PERF_WAIT=$((10#$PERF_WAIT))
if [[ $# == 0 ]]; then PLACES=(market harbour forest interior combat); else PLACES=("$@"); fi
for place in "${PLACES[@]}"; do
  case "$place" in market|harbour|forest|interior|combat|shot) ;; *) fail "unknown place: $place (market harbour forest interior combat shot)" ;; esac
done
export PERF_WINDOW PERF_REPEAT PERF_WAIT
export PERF_ARGS="${PERF_ARGS:-}" PERF_ENV="${PERF_ENV:-}" PERF_VID_RES="${PERF_VID_RES:-0}" PERF_INI="${PERF_INI:-}"
if [[ "$REMOTE_RUN" == 0 ]]; then
  OUT="$ROOT/.bench/perf-$(date -u +%Y%m%dT%H%M%SZ)-$$"
  export PERF_COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
  export PERF_DIRTY="$(git -C "$ROOT" status --porcelain --untracked-files=normal | wc -l | tr -d ' ')"
  BENCH_HOST="${BENCH_HOST:-local}"
  case "$BENCH_HOST" in local|ryzen) ;; *) fail 'BENCH_HOST must be local or ryzen (no automatic fallback)' ;; esac
  if [[ "$BENCH_HOST" == ryzen ]]; then
    RYZEN_HOST="${RYZEN_HOST:-ryzen}"
    remote() {
      local command='' value
      for value in "$@"; do printf -v command '%s %q' "$command" "$value"; done
      ssh -o BatchMode=yes -o ConnectTimeout=10 "$RYZEN_HOST" "$command"
    }
    REMOTE_HOME="$(ssh -o BatchMode=yes -o ConnectTimeout=10 "$RYZEN_HOST" 'printf "%s" "$HOME"')"
    GAME_ENV=()
    for name in OPENGOTHIC_BASE OPENGOTHIC_BASE_FINGERPRINT OPENGOTHIC_VIDEOS_FINGERPRINT OPENGOTHIC_BASE_CACHE OPENGOTHIC_BASE_STORE; do
      if [[ -n "${!name+set}" ]]; then GAME_ENV+=("$name=${!name}"); fi
    done
    remote env ${GAME_ENV[@]+"${GAME_ENV[@]}"} python3 -c "$(cat "$ROOT/scripts/ryzen_game.py")" --check
    mkdir -p "$OUT"
    TASK="$REMOTE_HOME/gothic-ai/perf-stand"
    SOURCE="$TASK/source"
    REMOTE_OUT="$TASK/runs/${OUT##*/}"
    NAS_OUT="/mnt/shared/gothic-ai/perf-stand/runs/${OUT##*/}"
    remote mkdir -p "$SOURCE/OpenGothic" "$SOURCE/scripts/bench" "$REMOTE_OUT"
    remote df -h "$REMOTE_HOME"
    rsync -a --delete --exclude='/build*/' --exclude='.git' "$ROOT/OpenGothic/" "$RYZEN_HOST:$SOURCE/OpenGothic/"
    rsync -a "$ROOT/scripts/perf.sh" "$ROOT/scripts/perf_report.py" "$ROOT/scripts/base-ensure.sh" "$ROOT/scripts/ryzen_game.py" "$RYZEN_HOST:$SOURCE/scripts/"
    rsync -a --delete --exclude='/build*/' "$ROOT/scripts/convert/" "$RYZEN_HOST:$SOURCE/scripts/convert/"
    rsync -a "$ROOT"/scripts/bench/perf-*.bench "$RYZEN_HOST:$SOURCE/scripts/bench/"
    REMOTE_ENV=(PERF_REMOTE=1 "PERF_VID_RES=$PERF_VID_RES" "PERF_INI=$PERF_INI" "PERF_WINDOW=$PERF_WINDOW" "PERF_REPEAT=$PERF_REPEAT" "PERF_WAIT=$PERF_WAIT"
      "PERF_ARGS=$PERF_ARGS" "PERF_ENV=$PERF_ENV" "PERF_COMMIT=$PERF_COMMIT" "PERF_DIRTY=$PERF_DIRTY"
      ${GAME_ENV[@]+"${GAME_ENV[@]}"})
    if [[ "${NO_BUILD:-0}" != 1 ]]; then
      "$ROOT/scripts/gq" --host "$RYZEN_HOST" run --class quick --threads "${RYZEN_JOBS:-8}" --mem 12G \
        --max 30m --wait-max "${RYZEN_LOCK_WAIT:-3600}" --name 'build perf stand' \
        --archive "$REMOTE_OUT/build" -- env "${REMOTE_ENV[@]}" "RYZEN_JOBS=${RYZEN_JOBS:-8}" \
        bash "$SOURCE/scripts/perf.sh" __build "$REMOTE_OUT/build"
    fi
    rc=0
    "$ROOT/scripts/gq" --host "$RYZEN_HOST" run --class quiet --threads 4 --mem 5G --gpu-mem 4G \
      --display --max "$((5 + ${#PLACES[@]} * PERF_REPEAT * 5))m" --wait-max "${RYZEN_LOCK_WAIT:-3600}" --name 'perf stand' \
      --archive "$REMOTE_OUT" -- env "${REMOTE_ENV[@]}" NO_BUILD=1 \
      bash "$SOURCE/scripts/perf.sh" __run "$REMOTE_OUT" "${PLACES[@]}" \
      2>&1 | tee "$OUT/ryzen.log" || rc="${PIPESTATUS[0]}"
    # gq moves finished results to NAS with checksums. A failed archive leaves the source.
    if remote test -d "$NAS_OUT"; then
      rsync -a "$RYZEN_HOST:$NAS_OUT/" "$OUT/"
    else
      rsync -a "$RYZEN_HOST:$REMOTE_OUT/" "$OUT/"
    fi
    [[ "$rc" == 0 ]] || exit "$rc"
    python3 "$ROOT/scripts/perf_report.py" run "$OUT"
    echo "$OUT"
    exit
  fi
fi

mkdir -p "$OUT"
GAME_BIN="${GAME_BIN:-$BUILD/opengothic/Gothic2Notr}"
GOTHIC_DIR="${GOTHIC_DIR:-/Volumes/DevSSD/Dev/games/Gothic II}"
if [[ "${NO_BUILD:-0}" != 1 ]]; then build_game "$OUT/build"; fi
[[ -x "$GAME_BIN" ]] || fail "missing game executable: $GAME_BIN"
grep -qx 'CMAKE_BUILD_TYPE:STRING=RelWithDebInfo' "$BUILD/CMakeCache.txt" || fail 'build-perf must use RelWithDebInfo; rerun without NO_BUILD=1'
for setting in OPENGOTHIC_ENABLE_FRAME_PROFILE OPENGOTHIC_ENABLE_APP_CADENCE; do
  grep -qx "$setting:BOOL=ON" "$BUILD/CMakeCache.txt" || fail "build-perf lacks $setting=ON; rerun without NO_BUILD=1"
done
[[ "$REMOTE_RUN" == 1 || -d "$GOTHIC_DIR/Data" || -n "${OPENGOTHIC_BASE:-}${OPENGOTHIC_BASE_FINGERPRINT:-}" ]] || fail "missing game data: $GOTHIC_DIR/Data"

if [[ "$REMOTE_RUN" == 1 ]]; then
  export XDG_RUNTIME_DIR="/run/user/$(id -u)" ALSOFT_DRIVERS=null
  export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
  found=0
  for auth in "$XDG_RUNTIME_DIR"/.mutter-Xwaylandauth.* "$HOME/.Xauthority"; do
    [[ -r "$auth" ]] || continue
    for socket in /tmp/.X11-unix/X*; do
      [[ -S "$socket" ]] || continue
      if DISPLAY=":${socket##*/X}" XAUTHORITY="$auth" xdpyinfo >/dev/null 2>&1; then
        export DISPLAY=":${socket##*/X}" XAUTHORITY="$auth"
        found=1; break 2
      fi
    done
  done
  [[ "$found" == 1 ]] || fail 'no accessible Ryzen Xwayland desktop'
  export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json
fi
export GAME_BIN GOTHIC_DIR
python3 - "$OUT" "${PLACES[@]}" <<'PY'
import json
import os
from pathlib import Path
import platform
import shlex
import sys

out = Path(sys.argv[1])
# Parse shell-like quoting as data. PERF_ENV accepts assignments, never shell commands.
args = shlex.split(os.environ['PERF_ARGS'])
extra_env = {'GOTHIC_AI': '0'}
for item in shlex.split(os.environ['PERF_ENV']):
    key, sep, value = item.partition('=')
    if not sep or not key.isidentifier():
        raise SystemExit(f'perf: invalid PERF_ENV assignment: {item!r}')
    extra_env[key] = value
meta = dict(git_commit=os.environ.get('PERF_COMMIT'),
            dirty_files=int(os.environ.get('PERF_DIRTY', 0)),
            requested_window=os.environ['PERF_WINDOW'],
            backend='Metal' if platform.system() == 'Darwin' else 'Vulkan',
            host='ryzen' if os.environ.get('PERF_REMOTE') == '1' else platform.node(),
            repeats=int(os.environ['PERF_REPEAT']), places=sys.argv[2:],
            vid_res=(lambda v: v if v == 'game' else int(v))(os.environ.get('PERF_VID_RES', '0')), ini=os.environ.get('PERF_INI', ''),
            game_args=args, game_env=extra_env)
(out / 'metadata.json').write_text(json.dumps(meta, indent=2) + '\n')
PY

for place in "${PLACES[@]}"; do
  for ((repeat=1; repeat<=PERF_REPEAT; repeat++)); do
    if [[ "$(uname -s)" == Darwin ]]; then
      started=$SECONDS
      while pgrep -x Gothic2Notr >/dev/null; do
        (( SECONDS - started < PERF_WAIT )) || fail "another Gothic2Notr is still running after ${PERF_WAIT}s; close it and rerun"
        if (( SECONDS == started )); then echo 'perf: waiting for the other Mac game to close'; fi
        sleep 1
      done
    fi
    LEAF="$OUT/$place/repeat-$repeat"
    mkdir -p "$LEAF/run"
    echo "perf: $place, repeat $repeat/$PERF_REPEAT"
    python3 - "$ROOT/scripts/bench/perf-$place.bench" "$LEAF" "$OUT/metadata.json" "$place" "$repeat" <<'PY'
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys

scenario, leaf, metadata, place, repeat = sys.argv[1:]
leaf = Path(leaf)
meta = json.loads(Path(metadata).read_text())
env = os.environ.copy()
env.update(meta['game_env'])
# The timing contract wins over inherited bench/capture settings.
for key in ('OPENGOTHIC_BENCH_KEEP', 'OPENGOTHIC_BENCH_REALTIME', 'TEMPEST_AUDIO_LOOPBACK'):
    env.pop(key, None)
env.update(TEMPEST_BACKGROUND='0', TEMPEST_WINDOW_SIZE=meta['requested_window'],
           OPENGOTHIC_BENCH_VISIBLE='1',
           OPENGOTHIC_BENCH=scenario, OPENGOTHIC_BENCH_OUT=str(leaf),
           OPENGOTHIC_FRAME_PROFILE=str(leaf / 'frameprofile.csv'))
# PERF_VID_RES picks the internal render resolution: 0 full, 1 = 75 %, 2 = 50 % of the drawable,
# game = no key, the game's own default for this backend.
vid_res = env.get('PERF_VID_RES', '0')
if vid_res not in ('0', '1', '2', 'game'):
    raise SystemExit('perf: PERF_VID_RES must be 0, 1, 2 or game')
ini = {'ENGINE': {'zMaxFps': '0'}, 'INTERNAL': {} if vid_res == 'game' else {'vidResIndex': vid_res},
       'SOUND': {'soundVolume': '1', 'musicVolume': '1', 'musicEnabled': '1', 'worldSounds': '1'},
       'GAME': {}}
# The same initial GAME switches accepted by bench.sh, before world loading.
settings = ('MOVABLE_VOBS', 'ARROW_STICKING', 'PHYSICS_NOISE', 'DESTRUCTION', 'BUOYANCY',
            'CLOTH', 'CHARACTER_VIRTUAL', 'NPC_CHARACTER_VIRTUAL', 'MONSTER_CHARACTER_VIRTUAL',
            'MELEE_PHYSICS_HITS', 'MELEE_POSTURE', 'MELEE_REACTIONS', 'MELEE_BLADE_SHADOW')
for key in settings:
    if key in env:
        if env[key] not in ('0', '1'):
            raise SystemExit(f'perf: {key} must be 0 or 1')
        ini['GAME'][key.lower()] = env[key]
# PERF_INI='INTERNAL.halfResAoFog=0 ENGINE.zCloudShadowScale=1' sets any Gothic.ini key.
for item in shlex.split(env.get('PERF_INI', '')):
    name, sep, value = item.partition('=')
    section, dot, key = name.partition('.')
    if not sep or not dot or not section or not key:
        raise SystemExit(f'perf: PERF_INI expects SECTION.key=value, got {item!r}')
    ini.setdefault(section, {})[key] = value
lines = []
for section, values in ini.items():
    lines.append(f'[{section}]')
    lines += [f'{k}={v}' for k, v in values.items()]
(leaf / 'run/Gothic.ini').write_text('\n'.join(lines) + '\n')
subprocess.run(['bash', str(Path(scenario).parents[1] / 'base-ensure.sh'), env['GOTHIC_DIR']],
               env=env, check=True)
command = [env['GAME_BIN'], '-g', env['GOTHIC_DIR'], '-g2', '-window', '-devmode',
           '-nomenu', '-nointro', '-w', 'NEWWORLD.ZEN', *meta['game_args']]
meta.update(place=place, repeat=int(repeat), command=command)
with (leaf / 'game.log').open('w') as log:
    process = subprocess.Popen(command, cwd=leaf / 'run', env=env, stdout=log, stderr=subprocess.STDOUT)
    try:
        code = process.wait(timeout=300)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        raise SystemExit('perf: game timed out after 300s; stopped only this run')
meta['exit_code'] = code
(leaf / 'metadata.json').write_text(json.dumps(meta, indent=2) + '\n')
if code:
    raise SystemExit(f'perf: {place} exited {code}; see {leaf / "game.log"}')
PY
    python3 - "$LEAF" "$place" <<'PY'
import csv
import json
from pathlib import Path
import re
import sys

leaf, place = Path(sys.argv[1]), sys.argv[2]
try:
    timing = json.loads((leaf / 'appcadence.csv.json').read_text())
    if not timing['complete'] or timing['frames'] <= 0 or timing['overflow']:
        raise ValueError('incomplete/empty/overflowed app cadence')
    with (leaf / 'frameprofile.csv').open() as source:
        rows = list(csv.DictReader(source))
    if not rows or any(int(row.get('overflow', 0)) for row in rows):
        raise ValueError('empty or overflowed frame profile')
    for view in range(1, 5):
        samples = [row for row in rows if row['segment'] == f'perf-{place}/view-{view}']
        elapsed = (int(samples[-1]['begin_ns']) - int(samples[0]['begin_ns'])) / 1e9 if samples else 0
        if len(samples) < 2 or elapsed < 7.5:
            raise ValueError(f'view-{view}: only {len(samples)} attempts / {elapsed:.3f}s')
    events = [json.loads(line) for line in (leaf / 'events.jsonl').read_text().splitlines() if line]
    errors = [event for event in events if event.get('kind') == 'error' or event.get('kind', '').endswith('_error') or event.get('pass') is False]
    if errors:
        raise ValueError(f'scenario errors: {errors[:3]}')
    meta = json.loads((leaf / 'metadata.json').read_text())
    drawable = re.findall(r'\[bench\] drawable (\d+)x(\d+)', (leaf / 'game.log').read_text())
    if not drawable or min(map(int, drawable[-1])) <= 0:
        raise ValueError('missing actual drawable dimensions in game.log')
    meta['drawable_width'], meta['drawable_height'] = map(int, drawable[-1])
    segments = [event for event in events if event.get('kind') == 'perf_segment']
    if segments:
        meta['segments'] = segments
    (leaf / 'metadata.json').write_text(json.dumps(meta, indent=2) + '\n')
except (OSError, ValueError, KeyError) as error:
    raise SystemExit(f'perf: invalid {place} run: {error}; see {leaf}')
print(f'perf: validated {place}: 4 views, {len(rows)} attempts')
PY
  done
done
python3 "$ROOT/scripts/perf_report.py" run "$OUT"
echo "$OUT"
