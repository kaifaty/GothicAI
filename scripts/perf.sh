#!/usr/bin/env bash
# Repeatable normal-clock frame profiling. See docs/performance.md.
set -euo pipefail

fail() { echo "perf: $*" >&2; exit 2; }

perf_checkout() {
  python3 - "$1" <<'PY'
import hashlib
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
# Codex worktrees use .../worktrees/<name>/<repo>, like ryzen.sh.
name = root.parent.name if '/worktrees/' in str(root.parent) else root.name
print(name + '-' + hashlib.sha256(str(root).encode()).hexdigest()[:12])
PY
}

source_fingerprint() {
  python3 - "$ROOT/OpenGothic" <<'PY'
import hashlib
import os
from pathlib import Path
import sys

root = Path(sys.argv[1])
digest = hashlib.sha256()
for directory, dirs, files in os.walk(root):
    dirs[:] = sorted(name for name in dirs if name != '.git'
                     and not name.startswith(('build', 'cmake-build-')))
    for name in sorted(files):
        if name == '.git':
            continue
        path = Path(directory) / name
        digest.update(path.relative_to(root).as_posix().encode() + b'\0')
        digest.update(hashlib.sha256(path.read_bytes()).digest())
print(digest.hexdigest())
PY
}

check_build_fingerprint() {
  local built="$1"
  [[ -n "${PERF_SOURCE_FINGERPRINT:-}" && "$built" == "$PERF_SOURCE_FINGERPRINT" ]] ||
    fail "build source_fingerprint ${built:-missing} does not match run ${PERF_SOURCE_FINGERPRINT:-missing}; rerun without NO_BUILD=1"
}

build_game() {
  local out="$1"
  mkdir -p "$out"
  rm -f "$BUILD/source_fingerprint"
  local flags=(-DCMAKE_POLICY_VERSION_MINIMUM=3.5)
  if [[ "${PERF_REMOTE:-0}" == 1 ]]; then
    local actual_fingerprint
    actual_fingerprint="$(source_fingerprint)"
    [[ "$actual_fingerprint" == "$PERF_SOURCE_FINGERPRINT" ]] ||
      fail "uploaded source_fingerprint $actual_fingerprint does not match run $PERF_SOURCE_FINGERPRINT; rerun perf.sh to upload current sources"
    export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
    export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
    [[ -r "$VULKAN_SDK/include/vulkan/vulkan.h" && -r "$VULKAN_SDK/lib/libvulkan.so" ]] || fail 'incomplete Ryzen Vulkan SDK'
    source "$ROOT/scripts/ryzen-build.sh"
    flags+=("${RYZEN_CMAKE_FLAGS[@]}")
    flags+=("-DCMAKE_CXX_FLAGS=-I$VULKAN_SDK/include" "-DCMAKE_EXE_LINKER_FLAGS=-L$VULKAN_SDK/lib")
  fi
  cmake -S "$ROOT/OpenGothic" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DOPENGOTHIC_ENABLE_FRAME_PROFILE=ON -DOPENGOTHIC_ENABLE_APP_CADENCE=ON \
    "${flags[@]}" >"$out/configure.log" 2>&1 || { tail -60 "$out/configure.log"; return 1; }
  cmake --build "$BUILD" --target Gothic2Notr --parallel "${RYZEN_JOBS:-8}" \
    >"$out/build.log" 2>&1 || { tail -80 "$out/build.log"; return 1; }
  printf '%s\n' "$PERF_SOURCE_FINGERPRINT" >"$BUILD/source_fingerprint"
  tail -5 "$out/build.log"
}

# ryzen.sh clean and shell tests reuse the checkout and fingerprint helpers.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then return; fi
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/OpenGothic/build-perf"
# One Ryzen run per checkout at a time: the stand keeps one source tree and one build per checkout,
# and a second upload during the first run's queue wait would replace what that run builds and
# measures. The second run waits here, before it reads its sources. See docs/performance.md.
if [[ "${1:-}" != __build && "${1:-}" != __run && "${BENCH_HOST:-local}" == ryzen && "${PERF_STAND_LOCKED:-}" != "$ROOT" ]]; then
  mkdir -p "$ROOT/.bench"
  PERF_STAND_LOCKED="$ROOT" exec python3 "$ROOT/scripts/perf_lock.py" "$ROOT/.bench/perf-ryzen.lock" bash "$0" "$@"
fi
# The GPU stamp's age counts from entering the stand, before hashing the sources (see run_game warmup).
export PERF_RUN_STARTED_AT="$(date +%s)"
# perf-content.sh also calls the queue entry points directly in its own source tree.
PERF_SOURCE_FINGERPRINT="${PERF_SOURCE_FINGERPRINT:-$(source_fingerprint)}"
export PERF_SOURCE_FINGERPRINT

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
# GPU warm-up before the first place: auto (default where the GPU clock journal runs), 0 or seconds.
PERF_WARMUP="${PERF_WARMUP:-}"
PERF_WARMUP_MAX="${PERF_WARMUP_MAX:-300}"
[[ -z "$PERF_WARMUP" || "$PERF_WARMUP" == auto || "$PERF_WARMUP" =~ ^[0-9]+$ ]] || fail 'PERF_WARMUP must be auto, 0 or seconds'
[[ "$PERF_WARMUP" != [0-9]* ]] || PERF_WARMUP=$((10#$PERF_WARMUP))
[[ "$PERF_WARMUP_MAX" =~ ^[0-9]*[1-9][0-9]*$ ]] || fail 'PERF_WARMUP_MAX must be positive seconds'
PERF_WARMUP_MAX=$((10#$PERF_WARMUP_MAX))
if [[ $# == 0 ]]; then PLACES=(market harbour forest interior combat); else PLACES=("$@"); fi
for place in "${PLACES[@]}"; do
  case "$place" in
    market|market-walk|harbour|harbour-night|house-night|forest|interior|combat|shot|face|face-far|rock|meadow|grass|grass-track|grass-fields|grass-pack|grass-fields-pack|grass-ground|grass-ground-fields|trees|xardas|temple|fires|firerain|crops|crops-onar|crops-game|itemfx) ;;
    *) fail "unknown place: $place (market market-walk harbour harbour-night house-night forest interior combat shot face face-far rock meadow grass grass-track grass-fields grass-pack grass-fields-pack grass-ground grass-ground-fields trees xardas temple fires firerain crops crops-onar crops-game itemfx)" ;;
  esac
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
    SOURCE="$TASK/$(perf_checkout "$ROOT")/source"
    if [[ "${NO_BUILD:-0}" == 1 ]]; then
      REMOTE_BUILT_FINGERPRINT="$(remote bash -c 'cat "$1" 2>/dev/null || true' -- "$SOURCE/OpenGothic/build-perf/source_fingerprint")"
      check_build_fingerprint "$REMOTE_BUILT_FINGERPRINT"
    fi
    REMOTE_OUT="$TASK/runs/${OUT##*/}"
    NAS_OUT="/mnt/shared/gothic-ai/perf-stand/runs/${OUT##*/}"
    remote mkdir -p "$SOURCE/OpenGothic" "$SOURCE/scripts/bench" "$REMOTE_OUT"
    remote df -h "$REMOTE_HOME"
    # Stamp changed sources now: old source mtimes could otherwise skip rebuilding newer objects.
    rsync -a --checksum --no-times --delete --exclude='build*/' --exclude='cmake-build-*/' --exclude='.git' "$ROOT/OpenGothic/" "$RYZEN_HOST:$SOURCE/OpenGothic/"
    rsync -a "$ROOT/scripts/ryzen-build.sh" "$ROOT/scripts/perf.sh" "$ROOT/scripts/perf_report.py" "$ROOT/scripts/perf_gpu_log.py" "$ROOT/scripts/base-ensure.sh" "$ROOT/scripts/ryzen_game.py" "$RYZEN_HOST:$SOURCE/scripts/"
    rsync -a --delete --exclude='/build*/' "$ROOT/scripts/convert/" "$RYZEN_HOST:$SOURCE/scripts/convert/"
    rsync -a "$ROOT"/scripts/bench/perf-*.bench "$RYZEN_HOST:$SOURCE/scripts/bench/"
    REMOTE_ENV=(PERF_REMOTE=1 "PERF_VID_RES=$PERF_VID_RES" "PERF_INI=$PERF_INI" "PERF_WINDOW=$PERF_WINDOW" "PERF_REPEAT=$PERF_REPEAT" "PERF_WAIT=$PERF_WAIT"
      "PERF_WARMUP=$PERF_WARMUP" "PERF_WARMUP_MAX=$PERF_WARMUP_MAX" "PERF_GPU_STAMP=${PERF_GPU_STAMP:-}"
      "PERF_ARGS=$PERF_ARGS" "PERF_ENV=$PERF_ENV" "PERF_COMMIT=$PERF_COMMIT" "PERF_DIRTY=$PERF_DIRTY"
      "PERF_SOURCE_FINGERPRINT=$PERF_SOURCE_FINGERPRINT"
      ${GAME_ENV[@]+"${GAME_ENV[@]}"})
    if [[ "${NO_BUILD:-0}" != 1 ]]; then
      "$ROOT/scripts/gq" --host "$RYZEN_HOST" run --class quick --threads "${RYZEN_JOBS:-8}" --mem 12G \
        --max 30m --wait-max "${RYZEN_LOCK_WAIT:-3600}" --name 'build perf stand' \
        --archive "$REMOTE_OUT/build" -- env "${REMOTE_ENV[@]}" "RYZEN_JOBS=${RYZEN_JOBS:-8}" \
        bash "$SOURCE/scripts/perf.sh" __build "$REMOTE_OUT/build"
    fi
    rc=0
    "$ROOT/scripts/gq" --host "$RYZEN_HOST" run --class quiet --threads 4 --mem 5G --gpu-mem 4G \
      --display --max "$((10 + ${#PLACES[@]} * PERF_REPEAT * 5))m" --wait-max "${RYZEN_LOCK_WAIT:-3600}" --name 'perf stand' \
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
if [[ "$REMOTE_RUN" == 1 ]]; then
  check_build_fingerprint "$(cat "$BUILD/source_fingerprint" 2>/dev/null || true)"
fi
GAME_BIN="${GAME_BIN:-$BUILD/opengothic/Gothic2Notr}"
GOTHIC_DIR="${GOTHIC_DIR:-/Volumes/DevSSD/Dev/games/Gothic II}"
# A local build takes minutes: the stamp's age then counts from its end.
if [[ "${NO_BUILD:-0}" != 1 ]]; then build_game "$OUT/build"; PERF_RUN_STARTED_AT="$(date +%s)"; fi
[[ -x "$GAME_BIN" ]] || fail "missing game executable: $GAME_BIN"
grep -qx 'CMAKE_BUILD_TYPE:STRING=RelWithDebInfo' "$BUILD/CMakeCache.txt" || fail 'build-perf must use RelWithDebInfo; rerun without NO_BUILD=1'
for setting in OPENGOTHIC_ENABLE_FRAME_PROFILE OPENGOTHIC_ENABLE_APP_CADENCE; do
  grep -qx "$setting:BOOL=ON" "$BUILD/CMakeCache.txt" || fail "build-perf lacks $setting=ON; rerun without NO_BUILD=1"
done
[[ "$REMOTE_RUN" == 1 || -d "$GOTHIC_DIR/Data" || -n "${OPENGOTHIC_BASE:-}${OPENGOTHIC_BASE_FINGERPRINT:-}" ]] || fail "missing game data: $GOTHIC_DIR/Data"

if [[ "$REMOTE_RUN" == 1 ]]; then
  # game-base.json names the base of whichever version last ran ryzen.sh setup. The stand's own
  # content-convert reports this engine's importer format, so a ready base of that format is chosen.
  CONTENT_CONVERT="$(dirname "$GAME_BIN")/content-convert"
  [[ -x "$CONTENT_CONVERT" ]] || fail "missing $CONTENT_CONVERT; rerun without NO_BUILD=1"
  game_environment="$(python3 "$ROOT/scripts/ryzen_game.py" --converter "$CONTENT_CONVERT")" || fail 'no game base for this engine'
  eval "$game_environment"
  echo "perf: base ${OPENGOTHIC_BASE_FINGERPRINT:-${OPENGOTHIC_BASE:-}}"
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
import hashlib
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
meta = dict(binary_sha256=hashlib.sha256(Path(os.environ['GAME_BIN']).read_bytes()).hexdigest(),
            source_fingerprint=os.environ['PERF_SOURCE_FINGERPRINT'],
            git_commit=os.environ.get('PERF_COMMIT'),
            dirty_files=int(os.environ.get('PERF_DIRTY', 0)),
            requested_window=os.environ['PERF_WINDOW'],
            backend='Metal' if platform.system() == 'Darwin' else 'Vulkan',
            host='ryzen' if os.environ.get('PERF_REMOTE') == '1' else platform.node(),
            repeats=int(os.environ['PERF_REPEAT']), places=sys.argv[2:],
            vid_res=(lambda v: v if v == 'game' else int(v))(os.environ.get('PERF_VID_RES', '0')), ini=os.environ.get('PERF_INI', ''),
            game_args=args, game_env=extra_env)
(out / 'metadata.json').write_text(json.dumps(meta, indent=2) + '\n')
PY
export PERF_GPU_STAMP="${PERF_GPU_STAMP:-$HOME/.cache/gothic-ai/perf-last-game}"
# One launch path for the warm-up and the measured places: the same environment, Gothic.ini and command.
#   run_game measure SCENARIO LEAF PLACE REPEAT
#   run_game warmup SCENARIO OUT PLACE GPU_LOG
# Every game that ran, measured or not, refreshes PERF_GPU_STAMP when it ends.
run_game() {
  python3 - "$OUT/metadata.json" "$@" <<'PY'
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import time

# A cancelled job (SIGTERM to the group) still stops the game and refreshes the stamp in launch().
signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
metadata, mode, scenario, leaf, place, extra = sys.argv[1:]
scripts = Path(scenario).parents[1]
meta = json.loads(Path(metadata).read_text())
stamp = Path(os.environ['PERF_GPU_STAMP'])
# The meadow ground scenario has 354 s of scripted waits.
timeout = 420 if place == 'grass-ground' else 300


def prepare(leaf, profile):
    (leaf / 'run').mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(meta['game_env'])
    # The timing contract wins over inherited bench/capture settings.
    for key in ('OPENGOTHIC_BENCH_KEEP', 'OPENGOTHIC_BENCH_REALTIME', 'TEMPEST_AUDIO_LOOPBACK', 'OPENGOTHIC_FRAME_PROFILE'):
        env.pop(key, None)
    env.update(TEMPEST_BACKGROUND='0', TEMPEST_WINDOW_SIZE=meta['requested_window'],
               OPENGOTHIC_BENCH_VISIBLE='1',
               OPENGOTHIC_BENCH=scenario, OPENGOTHIC_BENCH_OUT=str(leaf))
    if profile:
        env['OPENGOTHIC_FRAME_PROFILE'] = str(leaf / 'frameprofile.csv')
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
    subprocess.run(['bash', str(scripts / 'base-ensure.sh'), env['GOTHIC_DIR']], env=env, check=True)
    command = [env['GAME_BIN'], '-g', env['GOTHIC_DIR'], '-window', '-devmode',
               '-nomenu', '-nointro', '-w', 'NEWWORLD.ZEN', *meta['game_args']]
    return env, command


def stop(process):
    process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def stall_guard(leaf):
    previous = None
    slow_intervals = 0

    def stalled():
        nonlocal previous, slow_intervals
        try:
            current = json.loads((leaf / 'heartbeat.json').read_text())
        except FileNotFoundError:
            return False
        if previous is not None:
            seconds = current['elapsed_seconds'] - previous['elapsed_seconds']
            if seconds <= 0:
                return False
            frames = current['frame'] - previous['frame']
            slow_intervals = slow_intervals + 1 if frames <= 2 * seconds else 0
        previous = current
        return slow_intervals >= 2

    return stalled


launched = []  # monotonic_ns of each game start


def launch(leaf, profile, done=None):
    """Exit code of one game; None when done() stopped it. RuntimeError on the timeout."""
    env, command = prepare(leaf, profile)
    deadline = time.monotonic() + timeout
    with (leaf / 'game.log').open('w') as log:
        process = subprocess.Popen(command, cwd=leaf / 'run', env=env, stdout=log, stderr=subprocess.STDOUT)
        launched.append(time.monotonic_ns())
        # Only a game that started refreshes the stamp, whatever way it ends.
        try:
            while True:
                try:
                    return process.wait(timeout=0.2 if done else timeout), command
                except subprocess.TimeoutExpired:
                    if done is not None and done():
                        stop(process)
                        return None, command
                    if time.monotonic() >= deadline:
                        stop(process)
                        raise RuntimeError(f'perf: game timed out after {timeout}s; stopped only this run')
        finally:
            stamp.parent.mkdir(parents=True, exist_ok=True)
            stamp.touch()


if mode == 'measure':
    leaf = Path(leaf)
    # The stall is the stand's, not the game's (the window iconified under Xwayland, issue #1686), and a new game
    # has so far run at full speed: one restart instead of losing the job. The stalled game's files stay beside it.
    restarts = 0
    try:
        while True:
            code, command = launch(leaf, True, stall_guard(leaf))
            if code is not None:
                break
            failure = f'{place} stayed at or below 2 FPS for two heartbeat intervals'
            if restarts == 1:
                raise RuntimeError(f'perf: stand failure: {failure}')
            restarts += 1
            kept = leaf / f'stalled-{restarts}'
            kept.mkdir()
            for path in list(leaf.iterdir()):
                if path != kept:
                    path.rename(kept / path.name)
            print(f'perf: stand stall: {failure}; restarting the game once, its files are in {kept}',
                  file=sys.stderr, flush=True)
    except RuntimeError as error:
        raise SystemExit(str(error))
    meta.update(place=place, repeat=int(extra), command=command, exit_code=code, stall_restarts=restarts)
    (leaf / 'metadata.json').write_text(json.dumps(meta, indent=2) + '\n')
    if code:
        raise SystemExit(f'perf: {place} exited {code}; see {leaf / "game.log"}')
    sys.exit()

# Warm-up: run the first place's scenario until the GPU temperature holds still, see docs/performance.md.
out = Path(leaf)
setting, limit = os.environ['PERF_WARMUP'], int(os.environ['PERF_WARMUP_MAX'])
result = dict(mode=setting, reason=None, seconds=0.0, runs=0, temp_start_c=None, temp_end_c=None,
              sm_start_mhz=None, sm_end_mhz=None, last_game_age_s=None)
# The stamp's age when the run phase began, before the journal's own start-up.
age = int(os.environ['PERF_RUN_STARTED_AT']) - stamp.stat().st_mtime if stamp.exists() else None
journal = out / 'gpu.csv' if extra == '1' else None
if setting == '0':
    result['reason'] = 'off'
elif setting == 'auto' and journal is None:
    result['reason'] = 'no-gpu-log'
elif setting == 'auto' and age is not None and age < 120:
    result.update(reason='skipped-recent-game', last_game_age_s=round(age, 1))
    print(f'perf: GPU warm: last stand game ended {age:.0f} s ago, no warm-up', flush=True)
else:
    if journal is not None:
        sys.path.insert(0, str(scripts))
        import perf_gpu_log as gpu
        # Only the stand's own tests shorten these; Ryzen runs keep perf_gpu_log's defaults.
        warm = dict(min_s=int(os.environ.get('PERF_WARMUP_MIN', gpu.WARM_MIN_S)),
                    window_s=int(os.environ.get('PERF_WARMUP_WINDOW', gpu.WARM_WINDOW_S)))
    fixed = None if setting == 'auto' else int(setting)

    def done():
        # Warm-up time counts from the first game's start, not from preparing the base.
        now, started = time.monotonic_ns(), launched[0]
        if fixed is not None:
            result['reason'] = 'fixed' if now - started >= fixed * 1e9 else None
        else:
            result['reason'] = gpu.warm_reason(gpu.read_log(journal), started, now, limit, **warm)
        return result['reason'] is not None

    print(f'perf: GPU warm-up ({setting}) on {place}', flush=True)
    while True:
        result['runs'] += 1
        run = out / 'warmup' / f'run-{result["runs"]}'
        try:
            code, _ = launch(run, False, done)
        except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
            code = str(error)
        if code is None or (code == 0 and done()):
            break
        if code != 0:
            result['reason'] = 'failed'
            print(f'perf: warning: warm-up game failed ({code}); measuring anyway, see {run / "game.log"}', flush=True)
            break
    ended = time.monotonic_ns()
    started = launched[0] if launched else ended
    result['seconds'] = round((ended - started) / 1e9, 1)
    rows = [] if journal is None else [row for row in gpu.read_log(journal) if started <= row['mono_ns'] <= ended]
    if rows:
        result.update(temp_start_c=rows[0]['temp_c'], temp_end_c=rows[-1]['temp_c'],
                      sm_start_mhz=rows[0]['sm_mhz'], sm_end_mhz=rows[-1]['sm_mhz'])
    print(f'perf: GPU warm-up {result["reason"]} after {result["seconds"]:.0f} s, {result["runs"]} run(s), '
          f'{result["temp_start_c"]} → {result["temp_end_c"]} °C, SM {result["sm_start_mhz"]} → {result["sm_end_mhz"]} MHz',
          flush=True)
(out / 'warmup.json').write_text(json.dumps(result, indent=2) + '\n')
PY
}

# The GPU clock journal runs over the whole run phase where nvidia-smi works (Ryzen); Mac has none.
# It never stops a measurement: without samples the run goes on as without a journal.
GPU_SMI=0 GPU_LOG=0 GPU_LOG_PID=''
if command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi -L >/dev/null 2>&1; then GPU_SMI=1; fi
gpu_samples() { [[ -f "$OUT/gpu.csv" ]] && (( $(wc -l <"$OUT/gpu.csv") >= 2 )); }
stop_gpu_log() {
  [[ -n "$GPU_LOG_PID" ]] || return 0
  local alive=0 code=0
  if kill -0 "$GPU_LOG_PID" 2>/dev/null; then alive=1; kill -TERM "$GPU_LOG_PID" 2>/dev/null || true; fi
  wait "$GPU_LOG_PID" 2>/dev/null || code=$?
  GPU_LOG_PID=''
  [[ "$alive" == 1 && "$code" == 0 ]] || echo "perf: warning: GPU clock journal stopped early (exit $code)" >&2
}
if [[ "$GPU_SMI" == 1 && ! -r "$ROOT/scripts/perf_gpu_log.py" ]]; then
  echo "perf: warning: missing $ROOT/scripts/perf_gpu_log.py; no journal" >&2
elif [[ "$GPU_SMI" == 1 ]]; then
  trap stop_gpu_log EXIT
  trap 'exit 143' TERM
  python3 "$ROOT/scripts/perf_gpu_log.py" "$OUT/gpu.csv" &
  GPU_LOG_PID=$!
  for ((tries = 0; tries < 50; tries++)); do
    ! gpu_samples || break
    kill -0 "$GPU_LOG_PID" 2>/dev/null || break
    sleep 0.1
  done
  if gpu_samples; then
    GPU_LOG=1
  else
    stop_gpu_log
    rm -f "$OUT/gpu.csv"
    echo 'perf: warning: no GPU clock sample in 5 s; no journal' >&2
  fi
fi
[[ -n "$PERF_WARMUP" ]] || PERF_WARMUP=$([[ "$GPU_SMI" == 1 ]] && echo auto || echo 0)
export PERF_WARMUP PERF_WARMUP_MAX
wait_mac_game() {
  [[ "$(uname -s)" == Darwin ]] || return 0
  local started=$SECONDS
  while pgrep -x Gothic2Notr >/dev/null; do
    (( SECONDS - started < PERF_WAIT )) || fail "another Gothic2Notr is still running after ${PERF_WAIT}s; close it and rerun"
    if (( SECONDS == started )); then echo 'perf: waiting for the other Mac game to close'; fi
    sleep 1
  done
}
if [[ "$PERF_WARMUP" != 0 || "$GPU_LOG" == 1 ]]; then
  wait_mac_game
  run_game warmup "$ROOT/scripts/bench/perf-${PLACES[0]}.bench" "$OUT" "${PLACES[0]}" "$GPU_LOG"
fi

for place in "${PLACES[@]}"; do
  for ((repeat=1; repeat<=PERF_REPEAT; repeat++)); do
    wait_mac_game
    LEAF="$OUT/$place/repeat-$repeat"
    mkdir -p "$LEAF/run"
    echo "perf: $place, repeat $repeat/$PERF_REPEAT"
    run_game measure "$ROOT/scripts/bench/perf-$place.bench" "$LEAF" "$place" "$repeat"
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
    if place == 'grass-track':
        track_samples = ('', '-noroad', '-nowander', '-nofade', '-noragged', '-noorm', '-nocolor',
                         '-nospots', '-notone', '-noribbonnormal', '-nolayernormal')
        segments_expected = [f'perf-{place}/view-{view}{suffix}'
                             for view in (5, 1) for suffix in track_samples]
    elif place in ('grass-pack', 'grass-fields-pack'):
        segments_expected = [f'perf-{place}/view-{view}-pack{pack}'
                             for view in range(1, 5) for pack in (1, 4, 16, 64)]
    elif place in ('grass-ground', 'grass-ground-fields'):
        ground_samples = ('', '-nolayers', '-nopainted', '-noborder', '-nobare', '-nobetween',
                          '-nosward', '-nolayermaps', '-nogrounddetail', '-notone')
        views = (3, 4, 5) if place == 'grass-ground' else (1, 4)
        segments_expected = [f'perf-{place}/view-{view}{suffix}'
                             for view in views for suffix in ground_samples]
    else:
        segments_expected = [f'perf-{place}/view-{view}' for view in range(1, 5)]
    for segment in segments_expected:
        samples = [row for row in rows if row['segment'] == segment]
        elapsed = (int(samples[-1]['begin_ns']) - int(samples[0]['begin_ns'])) / 1e9 if samples else 0
        if len(samples) < 2 or elapsed < 7.5:
            raise ValueError(f'{segment}: only {len(samples)} attempts / {elapsed:.3f}s')
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
print(f'perf: validated {place}: {len(segments_expected)} samples, {len(rows)} attempts')
PY
  done
done
stop_gpu_log
python3 "$ROOT/scripts/perf_report.py" run "$OUT"
echo "$OUT"
