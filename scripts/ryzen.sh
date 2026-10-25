#!/usr/bin/env bash
# Run the current worktree on Ryzen. No system packages or training files are changed.
# Usage: scripts/ryzen.sh setup|e2e|pytest [pytest args...]|bench <scenario> <out>|clean|clean-cache
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RYZEN_HOST="${RYZEN_HOST:-ryzen}"
LOCK_WAIT="${RYZEN_LOCK_WAIT:-900}"
[[ "$LOCK_WAIT" =~ ^[0-9]+$ ]] || { echo 'RYZEN_LOCK_WAIT must be seconds' >&2; exit 2; }

toolchain() {
  export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
  export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
  local tool
  for tool in uv cmake ninja gcc g++ glslangValidator rsync flock python3; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
  done
  # Check BEFORE configure: a failed shader compiler lookup can remove build.ninja.
  [[ -r "$VULKAN_SDK/include/vulkan/vulkan.h" && -r "$VULKAN_SDK/lib/libvulkan.so" ]] || {
    echo "Incomplete user Vulkan SDK: $VULKAN_SDK" >&2; exit 1;
  }
}

take_lock() {
  mkdir -p "$(dirname "$WORKTREE_LOCK")"
  local started=$SECONDS monitor waiter status
  local wait_seconds=$((10#$LOCK_WAIT)) remaining
  echo "Ryzen: waiting for worktree lock $WORKTREE_LOCK (up to ${LOCK_WAIT}s)"
  # Without a PTY ssh disconnects do not send SIGHUP. EOF cancels our own waiter,
  # so an interrupted Mac command cannot later acquire the lock unattended.
  (IFS= read -r unused || kill -TERM "$$") <&0 &
  monitor=$!
  while :; do
    exec 9>"$WORKTREE_LOCK"
    remaining=$((wait_seconds - (SECONDS - started)))
    (( remaining >= 0 )) || remaining=0
    flock -w "$remaining" 9 &
    waiter=$!
    trap 'kill "$monitor" "$waiter" 2>/dev/null || true; exit 143' HUP INT TERM
    wait "$waiter" && status=0 || status=$?
    [[ "$status" == 0 ]] || break
    # The lock path may have been replaced while we waited on its old inode.
    # Never upload under that retired lock: reopen the current path instead.
    if python3 - "$WORKTREE_LOCK" <<'PY'
import os
import sys
try:
    current = os.stat(sys.argv[1])
except FileNotFoundError:
    sys.exit(1)
sys.exit(not os.path.samestat(current, os.fstat(9)))
PY
    then break; fi
    exec 9>&-
    if (( SECONDS - started >= wait_seconds )); then status=1; break; fi
  done
  kill "$monitor" 2>/dev/null || true
  wait "$monitor" 2>/dev/null || true
  trap - HUP INT TERM
  [[ "$status" == 0 ]] || { echo 'Ryzen занят' >&2; exit 75; }
  echo "Ryzen: lock acquired in $((SECONDS - started))s"
}

auto_clean() {
  python3 - "$WORKTREE_LOCK" "${CLEAN_IDLE_HOURS:-48}" <<'PY'
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import time

root = Path.home() / 'gothic-ai/e2e'
own = root / Path(sys.argv[1]).name.removesuffix('.lock')
idle_hours = int(sys.argv[2])
now = time.time()
cutoff = now - idle_hours * 3600
gib = 1024 ** 3

def available():
    return shutil.disk_usage(root).free

def lock_file(path):
    # A lock symlink must never redirect creation/truncation outside e2e.
    return os.fdopen(os.open(path, os.O_CREAT | os.O_WRONLY | os.O_NOFOLLOW, 0o600), 'w')

def try_lock(stream):
    return subprocess.run(['flock', '-n', str(stream.fileno())],
                          pass_fds=(stream.fileno(),), check=False).returncode == 0

def activity(directory):
    marker = directory / '.last-used'
    if marker.exists():
        return marker.lstat().st_mtime
    # Legacy worktrees have no marker. Inspect at most two levels, without
    # following links into game data, shared caches or other task directories.
    latest = directory.stat().st_mtime
    for entry in directory.iterdir():
        metadata = entry.lstat()
        latest = max(latest, metadata.st_mtime)
        if stat.S_ISDIR(metadata.st_mode):
            for child in entry.iterdir():
                latest = max(latest, child.lstat().st_mtime)
    return latest

if root.is_symlink() or own.is_symlink():
    raise SystemExit('Ryzen: refusing a symlink for the e2e/worktree directory')
own.mkdir(parents=True, exist_ok=True)
with lock_file(own / '.last-used') as marker:
    os.utime(marker.fileno(), None)

before = available()
removed, errors = [], []
if idle_hours:
    for directory in sorted(root.iterdir()):
        if (directory == own or directory.name.startswith('.')
                or directory.is_symlink() or not directory.is_dir()):
            continue
        try:
            if activity(directory) >= cutoff:
                continue
            path = root / (directory.name + '.lock')
            with lock_file(path) as lock:
                if not try_lock(lock):
                    continue
                # Skip a lock path replaced between open and flock.
                if path.stat().st_ino != os.fstat(lock.fileno()).st_ino:
                    continue
                if directory.is_symlink() or activity(directory) >= cutoff:
                    continue
                shutil.rmtree(directory)
                # Legacy wrappers may already have this inode open, but not
                # locked yet. Keep the named lock so they cannot later acquire
                # a detached inode alongside a new invocation of this worktree.
                removed.append(directory.name)
        except FileNotFoundError:
            continue  # A concurrent cleaner already removed this entry.
        except OSError as error:
            errors.append(f'{directory.name}: {error.strerror}')
freed = max(0, available() - before)
summary = ', '.join(removed) if removed else 'none'
suffix = f"; errors: {'; '.join(errors)}" if errors else ''
print(f'Ryzen: auto-clean removed {summary}; freed {freed / gib:.2f} GiB'
      + (' (disabled)' if not idle_hours else '') + suffix, flush=True)

if available() < 60 * gib:
    # Serialize the cooldown check across different worktree controllers. uv
    # itself owns the cache lock; we never remove cache files ourselves.
    with lock_file(root / '.uv-prune.lock') as lock:
        if try_lock(lock):
            marker = root / '.uv-prune'
            if not marker.exists() or now - marker.lstat().st_mtime >= 12 * 3600:
                before = available()
                # Stamp attempts too: a broken uv must not be retried by every
                # concurrent invocation while the disk is under pressure.
                with lock_file(marker) as stamp:
                    os.utime(stamp.fileno(), None)
                try:
                    result = subprocess.run([str(Path.home() / '.local/bin/uv'), 'cache', 'prune'],
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                    suffix = '' if result.returncode == 0 else f'; failed (exit {result.returncode})'
                except OSError as error:
                    suffix = f'; failed ({error.strerror})'
                freed = max(0, available() - before)
                print(f'Ryzen: uv cache prune freed {freed / gib:.2f} GiB{suffix}', flush=True)
PY
}

disk_usage() {
  local available directory size usage=''
  available="$(df -h --output=avail "$HOME" | awk 'NR == 2 {print $1}')" || available='?'
  for directory in "$HOME/gothic-ai/e2e/"*; do
    [[ -d "$directory" ]] || continue
    size="$(du -sh -- "$directory" 2>/dev/null | cut -f1)" || size='?'
    usage+="${usage:+, }${directory##*/}=$size"
  done
  echo "Ryzen: disk free $available; e2e worktrees: ${usage:-none}"
}

desktop() {
  export XDG_RUNTIME_DIR="/run/user/$(id -u)"
  export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
  local auth socket info
  command -v xdpyinfo >/dev/null || { echo 'Need xdpyinfo to find the Xwayland display' >&2; exit 1; }
  # Connecting also wakes GNOME's on-demand Xwayland. Never use xhost or change the session.
  for auth in "$XDG_RUNTIME_DIR"/.mutter-Xwaylandauth.* "$HOME/.Xauthority"; do
    [[ -r "$auth" ]] || continue
    for socket in /tmp/.X11-unix/X*; do
      [[ -S "$socket" ]] || continue
      if info="$(DISPLAY=":${socket##*/X}" XAUTHORITY="$auth" xdpyinfo 2>/dev/null)"; then
        export DISPLAY=":${socket##*/X}" XAUTHORITY="$auth"
        RYZEN_SCREEN_SIZE="$(printf '%s\n' "$info" | awk '/dimensions:/ {print $2; exit}')"
        export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json
        export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
        [[ -r "$VK_DRIVER_FILES" ]] || { echo 'Missing NVIDIA Vulkan ICD' >&2; exit 1; }
        export ALSOFT_DRIVERS=null TEMPEST_BACKGROUND=1
        echo "Ryzen: Xwayland $DISPLAY, NVIDIA Vulkan"
        return
      fi
    done
  done
  echo 'No accessible Xwayland display. Need a running user desktop or a GPU-capable headless compositor.' >&2
  exit 1
}

e2e_window_position() {
  [[ -z "${TEMPEST_WINDOW_POS+x}" ]] || return 0
  if [[ "$TEMPEST_WINDOW_SIZE" != 640x360 ]]; then
    # Nonstandard windows run exclusively, so preserve their full usable screen.
    export TEMPEST_WINDOW_POS=0,0
    return 0
  fi
  local slot="$1" width height screen_width screen_height columns x y
  [[ "$TEMPEST_WINDOW_SIZE" =~ ^([0-9]+)x([0-9]+)$ ]] || {
    echo 'Invalid TEMPEST_WINDOW_SIZE for e2e placement' >&2; return 2;
  }
  width=$((10#${BASH_REMATCH[1]})) height=$((10#${BASH_REMATCH[2]}))
  [[ "${RYZEN_SCREEN_SIZE:-}" =~ ^([0-9]+)x([0-9]+)$ ]] || {
    echo 'No screen dimensions from xdpyinfo for e2e placement' >&2; return 2;
  }
  screen_width=$((10#${BASH_REMATCH[1]})) screen_height=$((10#${BASH_REMATCH[2]}))
  (( width > 0 && height > 0 )) || return 2
  # Mutter's measured frames are 690x447 for a 640x360 client. Keep clear of
  # the dock/panel (which clamp a requested 0,0), decorations and shadows.
  columns=$(((screen_width - 200 + 60) / (width + 60)))
  (( columns > 0 )) || columns=1
  x=$((100 + (slot % columns) * (width + 60)))
  y=$((100 + (slot / columns) * (height + 100)))
  if (( x + width > screen_width || y + height > screen_height )); then
    echo "E2e window $TEMPEST_WINDOW_SIZE in slot $slot does not fit screen $RYZEN_SCREEN_SIZE" >&2
    return 2
  fi
  export TEMPEST_WINDOW_POS="$x,$y"
}

source_digest() {
  # Content and paths include additions/deletions and uncommitted files, not just HEAD.
  python3 - "$ROOT/OpenGothic" "${1:-engine}" <<'PY'
import hashlib
import sys
from pathlib import Path
root = Path(sys.argv[1])
digest = hashlib.sha256()
if sys.argv[2] == 'storygraph':
    inputs = ['ai_bridge/storygraph', 'ai_bridge/textcodec.cpp', 'ai_bridge/textcodec.h',
              'lib/ZenKit', 'lib/rapidjson/include']
    paths = [path for name in inputs for path in
             ((root / name).rglob('*') if (root / name).is_dir() else [root / name])]
else:
    paths = root.rglob('*')
for path in sorted(paths):
    if 'build' in path.relative_to(root).parts:
        continue
    if path.is_file():
        digest.update(path.relative_to(root).as_posix().encode() + b'\0')
        digest.update(hashlib.sha256(path.read_bytes()).digest())
print(digest.hexdigest())
PY
}

storygraph_digest() {
  source_digest storygraph
}

cache_artifact() {
  # Copy without the lock, then publish the complete runtime directory with one
  # rename. Readers, publishers and cleaners share the same short-lived lock.
  python3 - "$HOME/gothic-ai/engine-cache" "$@" <<'PY'
import fcntl
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import time
import uuid

root = Path(sys.argv[1])
action, kind, digest = sys.argv[2:5]
assert action in ('get', 'publish', 'prune', 'clean')
assert kind in ('engine', 'storygraph')
if action in ('get', 'publish'):
    assert re.fullmatch('[0-9a-f]{64}', digest), 'invalid cache digest'
root.mkdir(parents=True, exist_ok=True)
parent = root if kind == 'engine' else root / 'storygraph'
binary = 'Gothic2Notr' if kind == 'engine' else 'storygraph'
entry = parent / digest
staging = None
garbage = []

def ready(path, name):
    return (path / name).is_file() and os.access(path / name, os.X_OK)

def retire(path):
    # Deletion may take time. Detach under the lock and unlink outside it.
    trash = root / ('.trash-' + uuid.uuid4().hex)
    path.rename(trash)
    garbage.append(trash)

try:
    if action == 'publish':
        parent.mkdir(exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix='.publish-', dir=parent))
        source = Path(sys.argv[5])
        if kind == 'engine':
            shutil.copytree(source, staging, dirs_exist_ok=True)
        else:
            shutil.copy2(source / binary, staging / binary)
        assert ready(staging, binary), 'build did not produce an executable'
    with root.with_suffix('.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if action == 'get' and not ready(entry, binary):
            sys.exit(1)
        if action == 'publish' and not ready(entry, binary):
            if entry.exists():
                retire(entry)
            staging.rename(entry)
        if action in ('get', 'publish'):
            os.utime(entry, None)
        removed = protected = 0
        cutoff = time.time() - 2 * 60 * 60
        for namespace in (root, root / 'storygraph'):
            if not namespace.exists():
                continue
            entries = sorted((p for p in namespace.iterdir()
                              if re.fullmatch('[0-9a-f]{64}', p.name) and p.is_dir()),
                             key=lambda p: (p.stat().st_mtime, p.name))
            count = len(entries)
            for old in entries:
                if action != 'clean' and count <= 8:
                    break
                if old.stat().st_mtime > cutoff:
                    protected += 1
                    continue
                retire(old)
                count -= 1
                removed += 1
        if action in ('get', 'publish'):
            print(entry)
        elif action == 'clean':
            print(f'Ryzen: cache removed {removed} entries; kept {protected} younger than 2h')
finally:
    if staging is not None and staging.exists():
        shutil.rmtree(staging)
    for trash in garbage:
        shutil.rmtree(trash)
PY
}

link_storygraph() {
  # Both the server and its native-tool tests look for this conventional path.
  mkdir -p "$ROOT/OpenGothic/ai_bridge/storygraph/build"
  ln -sfn "$RYZEN_STORYGRAPH_BIN" "$ROOT/OpenGothic/ai_bridge/storygraph/build/storygraph"
}

build_storygraph() {
  local state="$1" digest="$2" started=$SECONDS
  local build="$state/storygraph-build"
  if storygraph_ready "$state" "$digest" && [[ "$RYZEN_STORYGRAPH_BIN" != "$build/storygraph" ]]; then
    return
  fi
  if [[ ! -x "$build/storygraph" || ! -f "$build/source.sha256" ]] || \
     [[ "$(cat "$build/source.sha256")" != "$digest" ]]; then
    mkdir -p "$build"
    # Standalone execcheck.cpp uses std::find without <algorithm>. Supply the header
    # in the build flags so the wrapper works on GCC 15 without editing engine sources.
    cmake -S "$ROOT/OpenGothic/ai_bridge/storygraph" -B "$build" -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_CXX_FLAGS='-include algorithm' \
      >"$build/configure.log" 2>&1 || { local rc=$?; cat "$build/configure.log"; return "$rc"; }
    cmake --build "$build" --target storygraph --parallel "${RYZEN_JOBS:-12}" \
      >"$build/build.log" 2>&1 || { local rc=$?; tail -80 "$build/build.log"; return "$rc"; }
    tail -3 "$build/build.log"
    printf '%s\n' "$digest" >"$build/source.sha256"
    echo "Ryzen: storygraph configure + build $((SECONDS - started))s"
  fi
  local cached
  cached="$(cache_artifact publish storygraph "$digest" "$build")"
  export RYZEN_STORYGRAPH_BIN="$cached/storygraph"
  link_storygraph
}

engine_ready() {
  local state="$1" digest="$2" cached
  if cached="$(cache_artifact get engine "$digest")"; then
    export RYZEN_GAME_BIN="$cached/Gothic2Notr"
    return
  fi
  [[ -x "$state/build/opengothic/Gothic2Notr" && -f "$state/build/source.sha256" && -f "$state/build/CMakeCache.txt" ]] && \
     [[ "$(cat "$state/build/source.sha256")" == "$digest" ]] && \
     grep -qx 'OPENGOTHIC_ENABLE_APP_CADENCE:BOOL=ON' "$state/build/CMakeCache.txt" || return 1
  export RYZEN_GAME_BIN="$state/build/opengothic/Gothic2Notr"
}

storygraph_ready() {
  local state="$1" digest="$2" build="$1/storygraph-build" cached
  if cached="$(cache_artifact get storygraph "$digest")"; then
    export RYZEN_STORYGRAPH_BIN="$cached/storygraph"
    link_storygraph
    return
  fi
  [[ -x "$build/storygraph" && -f "$build/source.sha256" ]] && \
    [[ "$(cat "$build/source.sha256")" == "$digest" ]] || return 1
  export RYZEN_STORYGRAPH_BIN="$build/storygraph"
  link_storygraph
}

dependency_projects() {
  echo server
  case "$1" in
    tts|combat_barks|gossip|day|group|interject|programs|errand*|fight_reactions_*) echo tts-service ;;
  esac
  case "$1" in
    m3|m5e) echo asr-service ;;
  esac
}

check_protocol() {
  python3 - "$ROOT" <<'PY'
import re
import sys
from pathlib import Path
root = Path(sys.argv[1])
server = int(re.search(r'^PROTOCOL_VERSION\s*=\s*(\d+)',
                      (root / 'server/src/gothic_ai/protocol.py').read_text(), re.M)[1])
engine = int(re.search(r'\bProtocolVersion\s*=\s*(\d+)\s*;',
                      (root / 'OpenGothic/ai_bridge/aibridge.cpp').read_text())[1])
if server != engine:
    raise SystemExit(f'Protocol mismatch: server={server}, engine={engine}')
print(f'Ryzen: protocol {engine} (engine = server)', flush=True)
PY
}

remote_probe() {
  local action="$1" state="$2" run="$3" scenario="$4" digest project
  toolchain
  digest="$(source_digest)"
  printf '%s\n' "$digest" >"$state/inputs/$run/source.sha256"
  if [[ "$action" == e2e ]]; then check_protocol || return 2; fi
  engine_ready "$state" "$digest" || return 1
  # A ready worktree build still needs publication in the no-display preparation
  # job. Never copy the runtime directory in this probe or the game request.
  [[ "$RYZEN_GAME_BIN" == "$HOME/gothic-ai/engine-cache/$digest/Gothic2Notr" ]] || return 1
  if [[ "$action" == e2e ]]; then
    digest="$(storygraph_digest)"
    printf '%s\n' "$digest" >"$state/inputs/$run/storygraph.sha256"
    storygraph_ready "$state" "$digest" || return 1
    [[ "$RYZEN_STORYGRAPH_BIN" == "$HOME/gothic-ai/engine-cache/storygraph/$digest/storygraph" ]] || return 1
    while IFS= read -r project; do
      (cd "$ROOT/$project" && uv sync --check --frozen) || return 1
    done < <(dependency_projects "$scenario")
  fi
}

remote_prepare() {
  local action="$1" state="$2" run="$3" scenario="$4" digest project started=$SECONDS
  toolchain
  digest="$(cat "$state/inputs/$run/source.sha256")"
  if [[ "$action" == e2e ]]; then check_protocol; fi
  build_engine "$state" "$digest"
  if [[ "$action" == e2e ]]; then
    # Probe can stop at the missing engine before scanning the smaller tool tree.
    digest="$(storygraph_digest)"
    printf '%s\n' "$digest" >"$state/inputs/$run/storygraph.sha256"
    build_storygraph "$state" "$digest"
    while IFS= read -r project; do
      (cd "$ROOT/$project" && uv sync --frozen)
    done < <(dependency_projects "$scenario")
  fi
  echo "Ryzen: build preparation $((SECONDS - started))s"
}

require_prepared() {
  local action="$1" state="$2" run="$3" scenario="$4" digest
  digest="$(cat "$state/inputs/$run/source.sha256")" || return 1
  if [[ -z "$digest" ]] || ! engine_ready "$state" "$digest"; then
    echo 'Ryzen: engine does not match snapshot source.sha256; refusing to build with the display reserved. Run preparation again.' >&2
    return 1
  fi
  if [[ "$action" == e2e ]]; then
    local story_digest
    story_digest="$(cat "$state/inputs/$run/storygraph.sha256")" || return 1
    if ! storygraph_ready "$state" "$story_digest"; then
      echo 'Ryzen: storygraph does not match snapshot storygraph.sha256; refusing to build with the display reserved. Run preparation again.' >&2
      return 1
    fi
  fi
  if [[ "$RYZEN_GAME_BIN" == "$HOME/gothic-ai/engine-cache/$digest/Gothic2Notr" ]]; then
    echo "Ryzen: engine from cache ${digest:0:12} (0s)"
  fi
}

build_engine() {
  local state="$1" digest="$2" started=$SECONDS
  if engine_ready "$state" "$digest"; then
    if [[ "$RYZEN_GAME_BIN" != "$state/build/opengothic/Gothic2Notr" ]]; then
      echo "Ryzen: engine from cache ${digest:0:12} (0s)"
      return
    fi
    echo "Ryzen: engine unchanged, build skipped ($((SECONDS - started))s)"
  else
    local jobs="${RYZEN_JOBS:-12}"
    [[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'RYZEN_JOBS must be positive' >&2; exit 2; }
    mkdir -p "$state/build"
    # Reconfigure on tree changes: OpenGothic's source globs need this for new .cpp files.
    cmake -S "$ROOT/OpenGothic" -B "$state/build" -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DOPENGOTHIC_ENABLE_APP_CADENCE=ON \
      -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib" \
      >"$state/build/configure.log" 2>&1 || { local rc=$?; cat "$state/build/configure.log"; return "$rc"; }
    cmake --build "$state/build" --target Gothic2Notr --parallel "$jobs" \
      >"$state/build/build.log" 2>&1 || { local rc=$?; tail -80 "$state/build/build.log"; return "$rc"; }
    tail -5 "$state/build/build.log"
    printf '%s\n' "$digest" >"$state/build/source.sha256"
    echo "Ryzen: engine configure + build $((SECONDS - started))s"
  fi
  local cached
  cached="$(cache_artifact publish engine "$digest" "$state/build/opengothic")"
  export RYZEN_GAME_BIN="$cached/Gothic2Notr"
}

remote_bench() {
  local state="$1" run="$2" scenario="$3" rc started=$SECONDS
  echo 'Ryzen: game admitted'
  if [[ "${RYZEN_AI_TUNNEL:-0}" == 1 ]]; then
    # This command runs only after gq grants the window. The Mac opens the
    # reverse forwarding on its lock-holding SSH connection, then acknowledges.
    echo 'Ryzen: waiting for AI tunnel'
    until [[ -f "$state/inputs/$run/tunnel-ready" ]]; do sleep 0.2; done
    GOTHIC_AI_URL="$(python3 - "$GOTHIC_AI_URL" "$state/inputs/$run/tunnel-ready" <<'PY'
from pathlib import Path
import sys
from urllib.parse import urlsplit, urlunsplit
url = urlsplit(sys.argv[1])
port = int(Path(sys.argv[2]).read_text())
assert 1 <= port <= 65535
credentials, separator, _ = url.netloc.rpartition('@')
netloc = (credentials + separator) if separator else ''
print(urlunsplit(url._replace(netloc=netloc + f'{url.hostname}:{port}')))
PY
)"
    export GOTHIC_AI_URL
  fi
  toolchain
  export GOTHIC_DIR="$HOME/gothic-ai/game"
  [[ -d "$GOTHIC_DIR/Data" ]] || { echo 'Run scripts/ryzen.sh setup first' >&2; return 1; }
  command -v ffmpeg >/dev/null || { echo 'Missing tool: ffmpeg' >&2; return 1; }
  desktop
  require_prepared bench "$state" "$run" "$scenario"
  local out="$state/runs/$run"
  mkdir -p "$out"
  cat >"$out/game.sh" <<'WRAPPER'
#!/usr/bin/env bash
exec "$RYZEN_GAME_BIN" "$@" -g2 -window
WRAPPER
  chmod +x "$out/game.sh"
  export GAME_BIN="$out/game.sh"
  echo "Ryzen: prepare/start overhead $((SECONDS - started))s"
  started=$SECONDS
  # bench.sh produces the report here, before the complete output is collected.
  BENCH_HOST=local NO_BUILD=1 RYZEN_BENCH=1 bash "$ROOT/scripts/bench.sh" "$scenario" "$out" && rc=0 || rc=$?
  echo "Ryzen: bench $((SECONDS - started))s, exit $rc"
  return "$rc"
}

remote_run() {
  local action="$1" state="$2" run="$3"
  shift 3
  toolchain
  export GOTHIC_DIR="$HOME/gothic-ai/game"
  export GOTHIC_AI_DATA="$state/data"
  local started=$SECONDS rc digest
  [[ "${RYZEN_JOBS:-12}" =~ ^[1-9][0-9]*$ ]] || { echo 'RYZEN_JOBS must be positive' >&2; return 2; }
  if [[ "$action" == pytest ]]; then
    digest="$(storygraph_digest)"
    build_storygraph "$state" "$digest"
    cd "$ROOT/server"
    uv run pytest "$@" && rc=0 || rc=$?
    echo "Ryzen: pytest $((SECONDS - started))s, exit $rc"
    return "$rc"
  fi
  echo 'Ryzen: game admitted'
  export LOG_DIR="$state/runs/$run"
  # The worktree lock spans preparation and the game; retain converted graphs.
  mkdir -p "$LOG_DIR" "$GOTHIC_AI_DATA"
  # gq assigns the slot when this request starts, after the upload/preparation.
  # Keep explicit caller ports; defaults must not collide with the other slots.
  local display_slot="${GQ_DISPLAY_SLOT:-0}"
  [[ "$display_slot" =~ ^[0-9]+$ ]] || { echo 'Invalid GQ_DISPLAY_SLOT' >&2; return 2; }
  display_slot=$((10#$display_slot))
  export PORT="${PORT-$((47827 + 100 * display_slot))}"
  export ASR_PORT="${ASR_PORT-$((47829 + 100 * display_slot))}"
  export TTS_PORT="${TTS_PORT-$((47840 + 100 * display_slot))}"
  export TEMPEST_WINDOW_SIZE="${TEMPEST_WINDOW_SIZE-640x360}"
  [[ -d "$GOTHIC_DIR/Data" ]] || { echo 'Run scripts/ryzen.sh setup first' >&2; return 1; }
  started=$SECONDS
  desktop
  e2e_window_position "$display_slot"
  echo "Ryzen: e2e slot $display_slot; ports $PORT/$ASR_PORT/$TTS_PORT; window $TEMPEST_WINDOW_SIZE at $TEMPEST_WINDOW_POS; screen $RYZEN_SCREEN_SIZE"
  require_prepared "$action" "$state" "$run" "${SCENARIO:-m0}"
  # Dependencies were synchronized without the display; uv run must not rebuild them here.
  export UV_NO_SYNC=1
  # Windowed on the logged-in desktop; never take it fullscreen or use a software display.
  cat >"$LOG_DIR/game.sh" <<'WRAPPER'
#!/usr/bin/env bash
exec "$RYZEN_GAME_BIN" "$@" -g2 -window
WRAPPER
  chmod +x "$LOG_DIR/game.sh"
  export GAME_BIN="$LOG_DIR/game.sh"
  echo "Ryzen: prepare/start overhead $((SECONDS - started))s"
  started=$SECONDS
  local out="$LOG_DIR" run_number killed_at
  local killed_times=()
  for run_number in 1 2 3 4; do
    if (( run_number > 1 )); then
      export LOG_DIR="$out/run-$run_number"
      mkdir -p "$LOG_DIR"
    fi
    bash "$ROOT/scripts/e2e.sh" 2>&1 | tee "$LOG_DIR/e2e.log" && rc=0 || rc=$?
    grep -E 'GPU = ' "$LOG_DIR/game.log" || true
    killed_at="$(sed -n 's/^game killed before checks at //p' "$LOG_DIR/e2e.log")"
    [[ -n "$killed_at" ]] || break
    killed_times+=("$killed_at")
    echo "Ryzen: game killed before checks at $killed_at, run $run_number/4"
    if (( run_number == 4 )); then
      echo "Ryzen: game killed before checks 4 times; times: ${killed_times[*]}"
      rc=76
    fi
  done
  echo "Ryzen: e2e $((SECONDS - started))s, exit $rc"
  return "$rc"
}

# Internal entry point runs inside one gq job; the control SSH holds the worktree lock.
if [[ "${1:-}" == __remote ]]; then
  shift
  case "${1:-}" in
    probe) shift; remote_probe "$@"; exit $? ;;
    prepare) shift; remote_prepare "$@"; exit $? ;;
  esac
  if [[ "${1:-}" == bench ]]; then
    shift
    remote_bench "$@"
    exit $?
  fi
  remote_run "$@"
  exit $?
fi

ACTION="${1:-}"
case "$ACTION" in setup|e2e|pytest|bench|clean|clean-cache) shift ;; *) echo 'Usage: scripts/ryzen.sh setup|e2e|pytest [args...]|bench <scenario> <out>|clean|clean-cache' >&2; exit 2 ;; esac
CLEAN_IDLE_HOURS="${RYZEN_CLEAN_IDLE_HOURS:-48}"
[[ "$CLEAN_IDLE_HOURS" =~ ^[0-9]+$ ]] || { echo 'RYZEN_CLEAN_IDLE_HOURS must be nonnegative hours' >&2; exit 2; }
if [[ "$ACTION" == bench ]]; then
  BENCH_CLASS="${RYZEN_BENCH_CLASS:-quick}"
  case "$BENCH_CLASS" in
    quick|quiet) ;;
    *) echo 'RYZEN_BENCH_CLASS must be quick or quiet' >&2; exit 2 ;;
  esac
  [[ $# == 2 ]] || { echo 'Usage: scripts/ryzen.sh bench <scenario> <out>' >&2; exit 2; }
  BENCH_SCENARIO="$1"
  [[ -f "$BENCH_SCENARIO" ]] || BENCH_SCENARIO="$ROOT/scripts/bench/$1.bench"
  [[ -f "$BENCH_SCENARIO" ]] || { echo "No scenario: $1" >&2; exit 2; }
  BENCH_SCENARIO="$(cd "$(dirname "$BENCH_SCENARIO")" && pwd)/$(basename "$BENCH_SCENARIO")"
  LOG_DIR="$2"
  mkdir -p "$LOG_DIR"
  LOG_DIR="$(cd "$LOG_DIR" && pwd)"
  if [[ -z "${WORLD:-}" && -z "${SAVE:-}" ]]; then
    COMMON="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)"
    SAVE="$(dirname "$COMMON")/save_slot_1.sav"
  fi
elif [[ "$ACTION" != pytest && $# != 0 ]]; then
  echo 'Only pytest and bench accept arguments' >&2
  exit 2
fi

remote() {
  local command='' arg
  for arg in "$@"; do printf -v command '%s %q' "$command" "$arg"; done
  ssh -o BatchMode=yes -o ConnectTimeout=10 "$RYZEN_HOST" "$command"
}

if [[ "$ACTION" == clean-cache ]]; then
  { declare -f cache_artifact; echo 'set -euo pipefail; cache_artifact clean engine unused'; } | remote bash -s
  exit
fi

sync_hd_textures() {
  # Upload only into this worktree: another admitted bench may still be reading
  # its own texture files while we prepare a queued job.
  remote mkdir -p "$STATE/hd-textures/textures"
  rsync -a --delete "$HD_TEXTURES/" "$RYZEN_HOST:$REMOTE/hd-textures/textures/"
  ENV_ARGS+=("OPENGOTHIC_TEXTURES=$STATE/hd-textures/textures")
}

sync_content() {
  # An unset variable keeps the engine's previous default. Each explicit upload
  # belongs to this run, so a later invocation cannot replace its input files.
  [[ -n "${OPENGOTHIC_CONTENT+set}" ]] || return 0
  if [[ -z "$OPENGOTHIC_CONTENT" ]]; then
    ENV_ARGS+=("OPENGOTHIC_CONTENT=")
    return 0
  fi
  local content
  content="$(python3 - "$OPENGOTHIC_CONTENT" <<'PY'
import os
import sys
print(os.path.realpath(os.path.expanduser(sys.argv[1])))
PY
)"
  [[ -d "$content" ]] || { echo "Missing OPENGOTHIC_CONTENT directory: $content" >&2; return 2; }
  remote mkdir -p "$STATE/inputs/$RUN/content"
  rsync -a --delete "$content/" "$RYZEN_HOST:$REMOTE/inputs/$RUN/content/"
  ENV_ARGS+=("OPENGOTHIC_CONTENT=$STATE/inputs/$RUN/content")
}

if [[ "$ACTION" == setup ]]; then
  remote bash -s <<'REMOTE'
set -euo pipefail
mkdir -p "$HOME/gothic-ai/game"
REMOTE
  # Run the same preflight without needing a checkout or configuring a build.
  { declare -f toolchain; echo 'set -euo pipefail; toolchain; echo "Ryzen: toolchain OK"'; } | remote bash -s
  GOTHIC_DIR="${GOTHIC_DIR:-/Volumes/DevSSD/Dev/games/Gothic II}"
  [[ -d "$GOTHIC_DIR/Data" ]] || { echo "No game Data directory: $GOTHIC_DIR" >&2; exit 1; }
  started=$SECONDS
  # rsync writes temporary files and renames them; no machine lock is needed.
  rsync -a --exclude=.DS_Store "$GOTHIC_DIR/" "$RYZEN_HOST:gothic-ai/game/"
  echo "Ryzen: game data ready, sync $((SECONDS - started))s"
  exit
fi

# Codex worktrees are named .../worktrees/<name>/<repo>; ordinary worktrees use their basename.
WORKTREE="$(basename "$ROOT")"
if [[ "$ROOT" == */worktrees/*/* ]]; then WORKTREE="$(basename "$(dirname "$ROOT")")"; fi
WORKTREE="${RYZEN_WORKTREE:-$WORKTREE}"
[[ "$WORKTREE" =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]] || { echo 'Unsafe worktree name' >&2; exit 2; }
REMOTE="gothic-ai/e2e/$WORKTREE"
REMOTE_HOME="$(remote printenv HOME)"
STATE="$REMOTE_HOME/$REMOTE"

finish_run() {
  local found
  # A lost connection or interrupted caller may leave the remote task running.
  if [[ "$RUN_STARTED" == 1 && "$RUN_FINISHED" == 0 ]] || [[ "$RUN_RC" == 255 ]]; then
    echo "Ryzen: remote task status unknown; keeping artifacts for $RUN on Ryzen" >&2
  elif [[ "$ACTION" == pytest ]]; then
    remote rm -rf -- "$STATE/inputs/$RUN" || echo "Ryzen: could not remove $STATE/inputs/$RUN" >&2
  else
    remote test -d "$STATE/runs/$RUN" && found=0 || found=$?
    case "$found" in
      0)
        if rsync -a "$RYZEN_HOST:$REMOTE/runs/$RUN/" "$LOG_DIR/"; then
          if [[ "$ACTION" == bench ]]; then
            # Reports are built on Ryzen; collected links must point at the local output.
            if ! python3 - "$STATE/runs/$RUN" "$LOG_DIR" <<'PY'
import json
import sys
from pathlib import Path

remote_out, local_out = sys.argv[1:]
out = Path(local_out)
report = out / 'report.md'
if report.is_file():
    text = report.read_text().replace(remote_out, local_out)
    text = text.replace(f'# Bench report: {Path(remote_out).name}\n',
                        f'# Bench report: {out.name}\n', 1)
    report.write_text(text)

def localize(value):
    if isinstance(value, str):
        return value.replace(remote_out, local_out)
    if isinstance(value, list):
        return [localize(item) for item in value]
    if isinstance(value, dict):
        return {key: localize(item) for key, item in value.items()}
    return value

report = out / 'report.json'
if report.is_file():
    report.write_text(json.dumps(localize(json.loads(report.read_text())), indent=2) + '\n')
PY
            then
              echo "Ryzen: could not localize reports; keeping remote artifacts for $RUN" >&2
              return 1
            fi
          fi
          if remote rm -rf -- "$STATE/runs/$RUN" "$STATE/inputs/$RUN"; then
            echo "Ryzen: removed runs/$RUN and inputs/$RUN"
          else
            echo "Ryzen: could not remove remote artifacts for $RUN" >&2
          fi
        else
          echo "Ryzen: could not collect all logs; keeping $STATE/runs/$RUN and $STATE/inputs/$RUN on Ryzen" >&2
        fi
        ;;
      1)
        # Preparation failed before a log directory was created.
        remote rm -rf -- "$STATE/inputs/$RUN" || echo "Ryzen: could not remove $STATE/inputs/$RUN" >&2
        ;;
      *)
        echo "Ryzen: could not check remote logs; keeping artifacts for $RUN on Ryzen" >&2
        ;;
    esac
  fi
  [[ "$ACTION" != e2e ]] || echo "logs: $LOG_DIR"
  [[ "$ACTION" != bench ]] || echo "bench: output in $LOG_DIR"
}

# Hold one SSH session open across upload and execution. This protects the snapshot
# even from concurrent invocations in the same worktree (including clean).
SESSION="$(mktemp -d "${TMPDIR:-/tmp}/gothic-ryzen-control.XXXXXX")"
SSH_PID=''
GQ_PID=''
TUNNEL_OPEN=0
RUN_CREATED=0
RUN_ARMED=0
RUN_STARTED=0
RUN_FINISHED=0
RUN_RC=0
guard_ack() {
  local message="$1"
  until grep -qx "$message" "$OUTPUT"; do
    kill -0 "$SSH_PID" 2>/dev/null || return 1
    sleep 0.1
  done
}

cleanup() {
  local status=$?
  trap '' HUP INT TERM
  if [[ -n "$GQ_PID" ]]; then kill -TERM "$GQ_PID" 2>/dev/null || true; wait "$GQ_PID" 2>/dev/null || true; fi
  if [[ "${RUN_ARMED:-0}" == 1 && -n "$SSH_PID" ]]; then
    # Local SSH exit only starts cancellation. The remote lock owner confirms
    # the unit's terminal queue state before collection or releasing fd 9.
    # No submission was launched if interruption happened while arming.
    if [[ "$RUN_STARTED" == 0 ]]; then printf 'complete %s\n' "${QUEUE_RUN:-$RUN}" >&8 || true; fi
    if printf 'drain %s\n' "${QUEUE_RUN:-$RUN}" >&8 && guard_ack "Ryzen: queue guard complete ${QUEUE_RUN:-$RUN}"; then
      [[ "$RUN_FINISHED" == 1 ]] || RUN_RC="$status"
      RUN_FINISHED=1
    else
      RUN_RC=255
    fi
  fi
  if [[ "$RUN_CREATED" == 1 ]]; then finish_run || true; fi
  exec 8>&- || true
  if [[ -n "$SSH_PID" ]]; then wait "$SSH_PID" 2>/dev/null || true; fi
  rm -rf -- "$SESSION" || true
  return "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mkfifo "$SESSION/input"
exec 8<>"$SESSION/input"
OUTPUT="$SESSION/output"
if [[ "$ACTION" == e2e ]]; then
  LOG_DIR="${LOG_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/gothic-ryzen.XXXXXX")}"
  mkdir -p "$LOG_DIR"
  LOG_DIR="$(cd "$LOG_DIR" && pwd)"
  OUTPUT="$LOG_DIR/launcher.log"
fi
bootstrap="$(declare -f take_lock auto_clean disk_usage)
set -euo pipefail
LOCK_WAIT=$LOCK_WAIT
$(printf 'WORKTREE_LOCK=%q' "$STATE.lock")
take_lock
$(if [[ "$ACTION" != clean ]]; then printf 'CLEAN_IDLE_HOURS=%q\nauto_clean\n' "$CLEAN_IDLE_HOURS"; fi)
disk_usage
$(if [[ -n "${GQ_HOME+set}" ]]; then printf 'export GQ_HOME=%q\n' "$GQ_HOME"; fi)
$(printf 'exec python3 -u -c %q %q %q' "$(cat "$ROOT/scripts/ryzen-lock.py")" "$STATE" "$REMOTE_HOME/gothic-ai/bin/gq")"
printf -v bootstrap_command 'bash -c %q' "$bootstrap"
: >"$OUTPUT"
ssh -o BatchMode=yes -o ConnectTimeout=10 \
  -o ControlMaster=yes -o ControlPath="$SESSION/control" "$RYZEN_HOST" "$bootstrap_command" \
  8>&- <"$SESSION/input" > >(exec 8>&-; tee -a "$OUTPUT") 2>&1 &
SSH_PID=$!
until [[ -f "$OUTPUT" ]] && grep -qx 'Ryzen: queue guard ready' "$OUTPUT"; do
  if ! kill -0 "$SSH_PID" 2>/dev/null; then
    wait "$SSH_PID" && rc=0 || rc=$?
    SSH_PID=''
    exit "$rc"
  fi
  sleep 0.1
done

AI_PORT=''
if [[ "$ACTION" == bench && -n "${GOTHIC_AI_URL:-}" ]]; then
  # Only loopback endpoints need forwarding. Bind remotely to loopback as well.
  AI_PORT="$(python3 - "$GOTHIC_AI_URL" <<'PY'
import sys
from urllib.parse import urlsplit
url = urlsplit(sys.argv[1])
if url.hostname in ('127.0.0.1', 'localhost'):
    print(url.port or (443 if url.scheme == 'wss' else 80))
PY
)"
fi

start_tunnel() {
  # Forward only after the admitted job asks for it. Reuse the control SSH so
  # forwarding also ends when the worktree-lock connection closes. A private
  # port avoids conflicts even while collecting logs after gq releases display.
  local port attempt
  for attempt in {1..10}; do
    if ! ssh -S "$SESSION/control" -O forward -o ExitOnForwardFailure=yes \
      -R "127.0.0.1:0:127.0.0.1:$AI_PORT" "$RYZEN_HOST" \
      8>&- >"$SESSION/tunnel.port" 2>"$SESSION/tunnel.log"; then
      cat "$SESSION/tunnel.log" >&2
      echo "Ryzen: could not open AI tunnel to port $AI_PORT" >&2
      return 1
    fi
    port="$(cat "$SESSION/tunnel.port")"
    [[ "$port" =~ ^[0-9]+$ && "$port" -ge 1 && "$port" -le 65535 ]] || {
      echo 'Ryzen: invalid allocated tunnel port' >&2; return 1;
    }
    case "$port" in
      47827|47829|47840|47927|47929|47940|48027|48029|48040)
        ssh -S "$SESSION/control" -O cancel \
          -R "127.0.0.1:0:127.0.0.1:$AI_PORT" "$RYZEN_HOST" 8>&- || return 1
        continue ;;
    esac
    remote bash -c 'printf "%s\n" "$1" > "$2.tmp" && mv -- "$2.tmp" "$2"' -- \
      "$port" "$STATE/inputs/$RUN/tunnel-ready" || return 1
    TUNNEL_OPEN=1
    echo "Ryzen: AI tunnel ready on private port $port"
    return 0
  done
  echo 'Ryzen: could not allocate a tunnel outside e2e ports' >&2
  return 1
}

run_quick() {
  local rc control_lost=0 tunnel_failed=0 requested=$SECONDS window_wait=''
  QUEUE_RUN="${QUEUE_RUN:-$RUN}"
  kill -0 "$SSH_PID" 2>/dev/null || { echo 'Ryzen: worktree lock connection lost' >&2; return 255; }
  # Arm the remote guard before submission so EOF cannot race late registration.
  RUN_ARMED=1
  RUN_STARTED=0 RUN_FINISHED=0
  printf 'arm %s\n' "$QUEUE_RUN" >&8
  guard_ack "Ryzen: queue guard armed $QUEUE_RUN" || return 255
  RUN_STARTED=1
  # The Mac wrapper owns gq's stdin pipe; running gq inside the control SSH
  # would close that pipe on command completion and cancel the job at once.
  # Keep fd 8 in the wrapper: even SIGKILL of this shell must not release the
  # worktree lock while its gq job is still alive. tee must not retain that fd.
  "$ROOT/scripts/gq" --host "$RYZEN_HOST" run "${GQ_ARGS[@]}" -- env "RYZEN_RUN_ID=$QUEUE_RUN" "$@" \
    > >(exec 8>&-; tee -a "$OUTPUT") 2>&1 &
  GQ_PID=$!
  # A dropped control connection releases the snapshot lock. Stop our job too,
  # before another invocation can upload over its source/build directories.
  while kill -0 "$GQ_PID" 2>/dev/null; do
    if [[ -z "$window_wait" && " ${GQ_ARGS[*]} " == *' --display '* ]] && grep -qx 'Ryzen: game admitted' "$OUTPUT"; then
      window_wait=$((SECONDS - requested))
    fi
    if ! kill -0 "$SSH_PID" 2>/dev/null; then
      echo 'Ryzen: worktree lock connection lost; cancelling our job' >&2
      control_lost=1
      kill -TERM "$GQ_PID" 2>/dev/null || true
      break
    fi
    if [[ -n "$AI_PORT" && "$TUNNEL_OPEN" == 0 ]] && grep -qx 'Ryzen: waiting for AI tunnel' "$OUTPUT"; then
      if ! start_tunnel; then
        tunnel_failed=1
        kill -TERM "$GQ_PID" 2>/dev/null || true
        break
      fi
    fi
    sleep 1
  done
  wait "$GQ_PID" && rc=0 || rc=$?
  GQ_PID=''
  if [[ " ${GQ_ARGS[*]} " == *' --display '* ]]; then
    echo "Ryzen: window wait ${window_wait:-$((SECONDS - requested))}s" | tee -a "$OUTPUT"
  fi
  if [[ "$rc" -lt 128 && "$control_lost" == 0 && "$tunnel_failed" == 0 ]]; then
    printf 'complete %s\n' "$QUEUE_RUN" >&8
  fi
  [[ "$control_lost" == 0 ]] || rc=255
  [[ "$tunnel_failed" == 0 ]] || rc=1
  RUN_FINISHED=1
  RUN_RC="$rc"
  printf 'drain %s\n' "$QUEUE_RUN" >&8
  guard_ack "Ryzen: queue guard complete $QUEUE_RUN" || { RUN_RC=255; return 255; }
  RUN_ARMED=0
  return "$rc"
}

preparation_needed() {
  local request rc
  request="$(python3 - "$ACTION" "$RUN" "${SCENARIO:-m0}" <<'PY'
import json
import sys
print(json.dumps(dict(zip(('action', 'run', 'scenario'), sys.argv[1:]))))
PY
)"
  # The lock owner runs this cheap read-only probe on the same SSH connection.
  printf 'probe %s\n' "$request" >&8
  until grep -q "^Ryzen: preparation check $RUN exit " "$OUTPUT"; do
    kill -0 "$SSH_PID" 2>/dev/null || return 255
    sleep 0.1
  done
  rc="$(sed -n "s/^Ryzen: preparation check $RUN exit //p" "$OUTPUT" | tail -1)"
  case "$rc" in
    0) return 1 ;;  # Ready: do not submit a build request.
    1) return 0 ;;
    *) return "$rc" ;;
  esac
}

e2e_display_exclusive() {
  # Keep timing-sensitive exceptions and their measured reason in this one list.
  # RYZEN_DISPLAY_EXCLUSIVE=1 forces it for any scenario: rerun a suspicious
  # failure from a shared slot this way before chasing it. =0 shares a listed
  # scenario: only for a three-game measurement (scripts/ryzen-round.sh).
  [[ "${RYZEN_DISPLAY_EXCLUSIVE:-}" != 1 ]] || return 0
  [[ "${RYZEN_DISPLAY_EXCLUSIVE:-}" != 0 ]] || return 1
  case "$1" in
    # Measured 19.10 (reports/compute/2026-10-19-ryzen-three-windows): after fixing
    # arena geometry and the stop race, two three-game rounds passed 6/6 runs,
    # each with 6/6 checks, at 72.1-76.2 FPS; alone 6/6 checks at 101.0 FPS.
    # With main's E2E_MAX_FPS=30 cap one more three-game round passed 3/3 at
    # 26.9-28.0 FPS, and fight_reactions_flee alone at 27.8.
    fight_reactions_bandits) return 1 ;;
    # Same fight, companion and movement timing, not measured yet: kept exclusive
    # rather than risking false failures. Move one out only after it passes
    # every round of a three-game measurement (scripts/ryzen-round.sh), as orders
    # and calm did on 19.10: orders 4/4 mixed rounds at 88-108 FPS once #258
    # placed its wolf on visible ground (6/6 more as three copies), calm 4/4 at
    # 79-81 FPS (4/4 more earlier that day), both also passing alone.
    fight_reactions_*|calm_*|neural*|wolf|wolf_load|rescue*|companion|crime_ally) return 0 ;;
    *) return 1 ;;
  esac
}

game_queue_args() {
  GQ_ARGS=(--class "${BENCH_CLASS:-quick}" --threads 4 --mem 5G --display --max "${RYZEN_MAX:-30m}" --wait-max "$LOCK_WAIT")
  if [[ "$ACTION" == bench ]]; then
    # Arbitrary benchmark windows can cover neighbours and affect their cadence.
    # Keep the legacy exclusive display reservation even with a private tunnel.
    GQ_ARGS+=(--gpu-mem 4G --name "bench $(basename "$BENCH_SCENARIO" .bench)")
  else
    GQ_ARGS+=(--gpu-mem 2G --name "e2e ${SCENARIO:-m0}")
    # A custom size or position cannot share the default slot layout safely.
    if [[ "${TEMPEST_WINDOW_SIZE:-640x360}" != 640x360 || -n "${TEMPEST_WINDOW_POS+x}" ]] || \
       e2e_display_exclusive "${SCENARIO:-m0}"; then
      GQ_ARGS+=(--display-exclusive)
    else
      GQ_ARGS+=(--display-shared)
    fi
  fi
}

run_game() {
  local rc name
  if preparation_needed; then
    if [[ "$ACTION" == bench ]]; then name="bench $(basename "$BENCH_SCENARIO" .bench)"; else name="e2e ${SCENARIO:-m0}"; fi
    GQ_ARGS=(--class quick --threads "$JOBS" --mem 12G --max "${RYZEN_MAX:-30m}" --name "build $name")
    QUEUE_RUN="$RUN-build"
    run_quick env "${ENV_ARGS[@]}" bash "$STATE/source/scripts/ryzen.sh" __remote prepare \
      "$ACTION" "$STATE" "$RUN" "${SCENARIO:-m0}" && rc=0 || rc=$?
    [[ "$rc" == 0 ]] || return "$rc"
  else
    rc=$?
    [[ "$rc" == 1 ]] || return "$rc"
    echo 'Ryzen: build preparation 0s (unchanged, request skipped)' | tee -a "$OUTPUT"
  fi
  game_queue_args
  QUEUE_RUN="$RUN-game"
  run_quick "$@"
}

if [[ "$ACTION" == clean ]]; then
  remote rm -rf -- "$STATE"
  echo "Ryzen: removed $REMOTE"
  exit
fi

RUN="$(date -u +%Y%m%dT%H%M%SZ)-$$"
RUN_CREATED=1
remote mkdir -p "$STATE/source" "$STATE/inputs/$RUN"
started=$SECONDS
rsync -a --checksum --delete \
  --exclude=.git --exclude=build/ --exclude='build-*/' --exclude=cmake-build-\*/ \
  --exclude=.venv/ --exclude=__pycache__/ --exclude=.pytest_cache/ --exclude=.ruff_cache/ --exclude=.mypy_cache/ \
  --exclude=.claude/ --exclude=.codex/ --exclude=.DS_Store --exclude=node_modules/ \
  --exclude='.env*' --exclude=secrets/ --exclude='*.local.toml' --exclude='*.sqlite*' --exclude='*.log' \
  --exclude='*.sav' --exclude='*.vdf' --exclude='*.VDF' --exclude='*.o' --exclude='*.a' \
  --exclude=/.bench/ --exclude=/.shorts/ --exclude=/.ryzen/ --exclude=/game/ --exclude=/data/ \
  --exclude=/tts-samples/ --exclude=/site/_build/ \
  "$ROOT/" "$RYZEN_HOST:$REMOTE/source/"

if [[ "$ACTION" == pytest ]]; then
  GQ_ARGS=(--class quick --threads 4 --mem 6G --max "${RYZEN_MAX:-20m}" --name pytest)
  JOBS=4
else
  JOBS="${RYZEN_JOBS:-12}"
  [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo 'RYZEN_JOBS must be positive' >&2; exit 2; }
fi
if [[ "$ACTION" == pytest ]]; then GQ_ARGS+=(--wait-max "$LOCK_WAIT"); fi
ENV_ARGS=("RYZEN_LOCK_WAIT=$LOCK_WAIT" "RYZEN_JOBS=$JOBS")
sync_content
if [[ "$ACTION" == bench ]]; then
  [[ -z "$AI_PORT" ]] || ENV_ARGS+=("RYZEN_AI_TUNNEL=1")
  REMOTE_OUT="$STATE/runs/$RUN"
  bench_path() {
    python3 - "$1" "$LOG_DIR" "$REMOTE_OUT" <<'PY'
import os
import sys
value, local, remote = sys.argv[1:]
if value.startswith(('/', '~/')):
    path = os.path.realpath(os.path.expanduser(value))
    local = os.path.realpath(local)
    if os.path.commonpath((path, local)) == local:
        value = os.path.join(remote, os.path.relpath(path, local))
print(value, end='')
PY
  }
  # Preserve game options, translating paths into this run's output directory.
  while IFS= read -r name; do
    case "$name" in
      GOTHIC_DIR|OPENGOTHIC_TEXTURES|OPENGOTHIC_FRACTURE|OPENGOTHIC_CONTENT) continue ;;
      OPENGOTHIC_*|GOTHIC_*|TEMPEST_*|WORLD|TIMEOUT|SOUND|MUSIC|MOVABLE_VOBS|ARROW_STICKING|PHYSICS_NOISE|DESTRUCTION|BUOYANCY|CLOTH|CHARACTER_VIRTUAL|NPC_CHARACTER_VIRTUAL|MONSTER_CHARACTER_VIRTUAL|MELEE_BLADE_SHADOW)
        value="$(bench_path "${!name}")"
        ENV_ARGS+=("$name=$value") ;;
    esac
  done < <(compgen -e)
  HD_TEXTURES="$HOME/.local/share/gothic-ai/hd-textures/textures"
  HD_TEXTURES="$(python3 - "$HD_TEXTURES" <<'PY'
import os
import sys
print(os.path.realpath(sys.argv[1]))
PY
)"
  TEXTURES="${OPENGOTHIC_TEXTURES-$HD_TEXTURES}"
  if [[ -n "$TEXTURES" ]]; then
    TEXTURES="$(python3 - "$TEXTURES" <<'PY'
import os
import sys
print(os.path.realpath(os.path.expanduser(sys.argv[1])))
PY
)"
  fi
  if [[ "$TEXTURES" == "$HD_TEXTURES" ]]; then
    if [[ -d "$HD_TEXTURES" ]]; then
      sync_hd_textures
    else
      ENV_ARGS+=("OPENGOTHIC_TEXTURES=")
    fi
  else
    # An explicit empty value disables HD textures, including on the remote host.
    ENV_ARGS+=("OPENGOTHIC_TEXTURES=$(bench_path "${OPENGOTHIC_TEXTURES}")")
  fi
  FRACTURE_CACHE="${OPENGOTHIC_FRACTURE-$HOME/.local/share/gothic-ai/fracture}"
  if [[ -n "$FRACTURE_CACHE" ]]; then
    FRACTURE_CACHE="$(python3 - "$FRACTURE_CACHE" <<'PY'
import os
import sys
print(os.path.realpath(os.path.expanduser(sys.argv[1])))
PY
)"
  fi
  if [[ -n "$FRACTURE_CACHE" && -d "$FRACTURE_CACHE" ]]; then
    # Keep this worktree's cache beside its source, away from other sessions.
    remote mkdir -p "$STATE/fracture"
    rsync -a --delete "$FRACTURE_CACHE/" "$RYZEN_HOST:$REMOTE/fracture/"
    ENV_ARGS+=("OPENGOTHIC_FRACTURE=$STATE/fracture")
  else
    # Missing and explicitly disabled local caches must not select a remote default.
    ENV_ARGS+=("OPENGOTHIC_FRACTURE=")
  fi
  rsync -a -- "$BENCH_SCENARIO" "$RYZEN_HOST:$REMOTE/inputs/$RUN/"
  if [[ -z "${WORLD:-}" ]]; then
    [[ -f "$SAVE" ]] || { echo "Missing SAVE: $SAVE" >&2; exit 2; }
    rsync -a -- "$SAVE" "$RYZEN_HOST:$REMOTE/inputs/$RUN/SAVE"
    ENV_ARGS+=("SAVE=$STATE/inputs/$RUN/SAVE")
  fi
  echo "Ryzen: upload $((SECONDS - started))s ($WORKTREE)"
  run_game env "${ENV_ARGS[@]}" bash "$STATE/source/scripts/ryzen.sh" __remote bench \
    "$STATE" "$RUN" "$STATE/inputs/$RUN/$(basename "$BENCH_SCENARIO")" && rc=0 || rc=$?
  exit "$rc"
fi
# Carry e2e options, including scenario-specific GOTHIC_* settings; paths are remapped below.
while IFS= read -r name; do
  case "$name" in
    GOTHIC_DIR|GOTHIC_AI_URL|OPENGOTHIC_FRACTURE|OPENGOTHIC_CONTENT) ;;
    SCENARIO|LLM|ASR|PAID_ASR|TTS|PAID_TTS|TIMEOUT|PORT|ASR_PORT|TTS_PORT|DECIDER_URL|QUICK_URL|E2E_MAX_FPS|MEMPROBE_*|GAME_EXTRA_ENV|CHARACTER_VIRTUAL|NPC_CHARACTER_VIRTUAL|MONSTER_CHARACTER_VIRTUAL|MELEE_BLADE_SHADOW|GOTHIC_*|OPENGOTHIC_*|TEMPEST_*|CHATGPT_*|OPENAI_*)
      ENV_ARGS+=("$name=${!name}") ;;
  esac
done < <(compgen -e)
if [[ "$ACTION" == e2e && ( "${SCENARIO:-m0}" == broken || "${SCENARIO:-m0}" == trace_crime ) ]]; then
  FRACTURE_CACHE="${OPENGOTHIC_FRACTURE-$HOME/.local/share/gothic-ai/fracture}"
  if [[ -n "$FRACTURE_CACHE" && -d "$FRACTURE_CACHE" ]]; then
    remote mkdir -p "$STATE/fracture"
    rsync -a --delete "$FRACTURE_CACHE/" "$RYZEN_HOST:$REMOTE/fracture/"
    ENV_ARGS+=("OPENGOTHIC_FRACTURE=$STATE/fracture")
  else
    ENV_ARGS+=("OPENGOTHIC_FRACTURE=")
  fi
elif [[ -n "${OPENGOTHIC_FRACTURE+set}" ]]; then
  ENV_ARGS+=("OPENGOTHIC_FRACTURE=$OPENGOTHIC_FRACTURE")
fi
for name in SAVE VOICE_FILE; do
  if [[ -n "${!name:-}" ]]; then
    [[ -f "${!name}" ]] || { echo "Missing $name: ${!name}" >&2; exit 2; }
    rsync -a -- "${!name}" "$RYZEN_HOST:$REMOTE/inputs/$RUN/$name"
    ENV_ARGS+=("$name=$STATE/inputs/$RUN/$name")
  fi
done
echo "Ryzen: upload $((SECONDS - started))s ($WORKTREE)"

if [[ "$ACTION" == e2e ]]; then
  run_game env "${ENV_ARGS[@]}" bash "$STATE/source/scripts/ryzen.sh" __remote e2e "$STATE" "$RUN" && rc=0 || rc=$?
  exit "$rc"
fi
run_quick env "${ENV_ARGS[@]}" bash "$STATE/source/scripts/ryzen.sh" __remote pytest "$STATE" "$RUN" "$@"
