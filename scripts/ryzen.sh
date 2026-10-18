#!/usr/bin/env bash
# Run the current worktree on Ryzen. No system packages or training files are changed.
# Usage: scripts/ryzen.sh setup|e2e|pytest [pytest args...]|bench <scenario> <out>|clean
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
  exec 9>"$WORKTREE_LOCK"
  local started=$SECONDS monitor waiter status
  echo "Ryzen: waiting for worktree lock $WORKTREE_LOCK (up to ${LOCK_WAIT}s)"
  # Without a PTY ssh disconnects do not send SIGHUP. EOF cancels our own waiter,
  # so an interrupted Mac command cannot later acquire the lock unattended.
  (IFS= read -r unused || kill -TERM "$$") <&0 &
  monitor=$!
  flock -w "$LOCK_WAIT" 9 &
  waiter=$!
  trap 'kill "$monitor" "$waiter" 2>/dev/null || true; exit 143' HUP INT TERM
  wait "$waiter" && status=0 || status=$?
  kill "$monitor" 2>/dev/null || true
  wait "$monitor" 2>/dev/null || true
  trap - HUP INT TERM
  [[ "$status" == 0 ]] || { echo 'Ryzen занят' >&2; exit 75; }
  echo "Ryzen: lock acquired in $((SECONDS - started))s"
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
  local auth socket
  command -v xdpyinfo >/dev/null || { echo 'Need xdpyinfo to find the Xwayland display' >&2; exit 1; }
  # Connecting also wakes GNOME's on-demand Xwayland. Never use xhost or change the session.
  for auth in "$XDG_RUNTIME_DIR"/.mutter-Xwaylandauth.* "$HOME/.Xauthority"; do
    [[ -r "$auth" ]] || continue
    for socket in /tmp/.X11-unix/X*; do
      [[ -S "$socket" ]] || continue
      if DISPLAY=":${socket##*/X}" XAUTHORITY="$auth" xdpyinfo >/dev/null 2>&1; then
        export DISPLAY=":${socket##*/X}" XAUTHORITY="$auth"
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

source_digest() {
  # Content and paths include additions/deletions and uncommitted files, not just HEAD.
  python3 - "$ROOT/OpenGothic" <<'PY'
import hashlib
import sys
from pathlib import Path
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
}

build_storygraph() {
  local state="$1" digest="$2" started=$SECONDS
  local build="$state/storygraph-build"
  if [[ ! -x "$build/storygraph" || ! -f "$build/source.sha256" ]] || \
     [[ "$(cat "$build/source.sha256")" != "$digest" ]]; then
    mkdir -p "$build"
    # Standalone execcheck.cpp uses std::find without <algorithm>. Supply the header
    # in the build flags so the wrapper works on GCC 15 without editing engine sources.
    cmake -S "$ROOT/OpenGothic/ai_bridge/storygraph" -B "$build" -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_CXX_FLAGS='-include algorithm' \
      >"$build/configure.log" 2>&1 || { cat "$build/configure.log"; return 1; }
    cmake --build "$build" --target storygraph --parallel "${RYZEN_JOBS:-12}" \
      >"$build/build.log" 2>&1 || { tail -80 "$build/build.log"; return 1; }
    tail -3 "$build/build.log"
    printf '%s\n' "$digest" >"$build/source.sha256"
    echo "Ryzen: storygraph configure + build $((SECONDS - started))s"
  fi
  # Both the server and its native-tool tests look for this conventional path.
  mkdir -p "$ROOT/OpenGothic/ai_bridge/storygraph/build"
  ln -sfn "$build/storygraph" "$ROOT/OpenGothic/ai_bridge/storygraph/build/storygraph"
}

build_engine() {
  local state="$1" digest="$2" started=$SECONDS
  if [[ -x "$state/build/opengothic/Gothic2Notr" && -f "$state/build/source.sha256" && -f "$state/build/CMakeCache.txt" ]] && \
     [[ "$(cat "$state/build/source.sha256")" == "$digest" ]] && \
     grep -qx 'OPENGOTHIC_ENABLE_APP_CADENCE:BOOL=ON' "$state/build/CMakeCache.txt"; then
    echo "Ryzen: engine unchanged, build skipped ($((SECONDS - started))s)"
    return
  fi
  local jobs="${RYZEN_JOBS:-12}"
  [[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'RYZEN_JOBS must be positive' >&2; exit 2; }
  mkdir -p "$state/build"
  # Reconfigure on tree changes: OpenGothic's source globs need this for new .cpp files.
  cmake -S "$ROOT/OpenGothic" -B "$state/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DOPENGOTHIC_ENABLE_APP_CADENCE=ON \
    -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib" \
    >"$state/build/configure.log" 2>&1 || { cat "$state/build/configure.log"; return 1; }
  cmake --build "$state/build" --target Gothic2Notr --parallel "$jobs" \
    >"$state/build/build.log" 2>&1 || { tail -80 "$state/build/build.log"; return 1; }
  tail -5 "$state/build/build.log"
  printf '%s\n' "$digest" >"$state/build/source.sha256"
  echo "Ryzen: engine configure + build $((SECONDS - started))s"
}

remote_bench() {
  local state="$1" run="$2" scenario="$3" digest rc started=$SECONDS
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
  digest="$(source_digest)"
  build_engine "$state" "$digest"
  desktop
  local out="$state/runs/$run"
  mkdir -p "$out"
  export RYZEN_GAME_BIN="$state/build/opengothic/Gothic2Notr"
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
  digest="$(source_digest)"
  if [[ "$action" == pytest ]]; then
    build_storygraph "$state" "$digest"
    cd "$ROOT/server"
    uv run pytest "$@" && rc=0 || rc=$?
    echo "Ryzen: pytest $((SECONDS - started))s, exit $rc"
    return "$rc"
  fi
  export LOG_DIR="$state/runs/$run"
  mkdir -p "$LOG_DIR"
  [[ -d "$GOTHIC_DIR/Data" ]] || { echo 'Run scripts/ryzen.sh setup first' >&2; return 1; }
  # This version is compiled into the only binary we ever launch, from this snapshot.
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
  build_engine "$state" "$digest"
  build_storygraph "$state" "$digest"
  started=$SECONDS
  desktop
  # Resolve dependencies inside the quick job, before the e2e server's two-second startup wait.
  (cd "$ROOT/server" && uv sync --frozen)
  case "${SCENARIO:-m0}" in
    tts|gossip|day|group|interject|programs) (cd "$ROOT/tts-service" && uv sync --frozen) ;;
  esac
  case "${SCENARIO:-m0}" in
    m3|m5e) (cd "$ROOT/asr-service" && uv sync --frozen) ;;
  esac
  export RYZEN_GAME_BIN="$state/build/opengothic/Gothic2Notr"
  # Windowed on the logged-in desktop; never take it fullscreen or use a software display.
  cat >"$LOG_DIR/game.sh" <<'WRAPPER'
#!/usr/bin/env bash
exec "$RYZEN_GAME_BIN" "$@" -g2 -window
WRAPPER
  chmod +x "$LOG_DIR/game.sh"
  export GAME_BIN="$LOG_DIR/game.sh"
  echo "Ryzen: prepare/start overhead $((SECONDS - started))s"
  started=$SECONDS
  bash "$ROOT/scripts/e2e.sh" && rc=0 || rc=$?
  echo "Ryzen: e2e $((SECONDS - started))s, exit $rc"
  grep -E 'GPU = ' "$LOG_DIR/game.log" || true
  return "$rc"
}

# Internal entry point runs inside one gq job; the control SSH holds the worktree lock.
if [[ "${1:-}" == __remote ]]; then
  shift
  if [[ "${1:-}" == bench ]]; then
    shift
    remote_bench "$@"
    exit $?
  fi
  remote_run "$@"
  exit $?
fi

ACTION="${1:-}"
case "$ACTION" in setup|e2e|pytest|bench|clean) shift ;; *) echo 'Usage: scripts/ryzen.sh setup|e2e|pytest [args...]|bench <scenario> <out>|clean' >&2; exit 2 ;; esac
if [[ "$ACTION" == bench ]]; then
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

sync_hd_textures() {
  # Upload only into this worktree: another admitted bench may still be reading
  # its own texture files while we prepare a queued job.
  remote mkdir -p "$STATE/hd-textures/textures"
  rsync -a --delete "$HD_TEXTURES/" "$RYZEN_HOST:$REMOTE/hd-textures/textures/"
  ENV_ARGS+=("OPENGOTHIC_TEXTURES=$STATE/hd-textures/textures")
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
    if [[ "$RUN_STARTED" == 0 ]]; then printf 'complete %s\n' "$RUN" >&8 || true; fi
    if printf 'drain %s\n' "$RUN" >&8 && guard_ack "Ryzen: queue guard complete $RUN"; then
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
bootstrap="$(declare -f take_lock disk_usage)
set -euo pipefail
LOCK_WAIT=$LOCK_WAIT
$(printf 'WORKTREE_LOCK=%q' "$STATE.lock")
take_lock
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
      47827|47829|47840)
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
  local rc control_lost=0 tunnel_failed=0
  kill -0 "$SSH_PID" 2>/dev/null || { echo 'Ryzen: worktree lock connection lost' >&2; return 255; }
  # Arm the remote guard before submission so EOF cannot race late registration.
  RUN_ARMED=1
  printf 'arm %s\n' "$RUN" >&8
  guard_ack "Ryzen: queue guard armed $RUN" || return 255
  RUN_STARTED=1
  # The Mac wrapper owns gq's stdin pipe; running gq inside the control SSH
  # would close that pipe on command completion and cancel the job at once.
  # Keep fd 8 in the wrapper: even SIGKILL of this shell must not release the
  # worktree lock while its gq job is still alive. tee must not retain that fd.
  "$ROOT/scripts/gq" --host "$RYZEN_HOST" run "${GQ_ARGS[@]}" -- "$@" \
    > >(exec 8>&-; tee -a "$OUTPUT") 2>&1 &
  GQ_PID=$!
  # A dropped control connection releases the snapshot lock. Stop our job too,
  # before another invocation can upload over its source/build directories.
  while kill -0 "$GQ_PID" 2>/dev/null; do
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
  if [[ "$rc" -lt 128 && "$control_lost" == 0 && "$tunnel_failed" == 0 ]]; then
    printf 'complete %s\n' "$RUN" >&8
  fi
  [[ "$control_lost" == 0 ]] || rc=255
  [[ "$tunnel_failed" == 0 ]] || rc=1
  RUN_FINISHED=1
  RUN_RC="$rc"
  return "$rc"
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
  GQ_ARGS=(--class quick --threads "$JOBS" --mem 8G --display --max "${RYZEN_MAX:-30m}")
  if [[ "$ACTION" == bench ]]; then
    GQ_ARGS+=(--gpu-mem 4G --name "bench $(basename "$BENCH_SCENARIO" .bench)")
  else
    GQ_ARGS+=(--gpu-mem 2G --name "e2e ${SCENARIO:-m0}")
  fi
fi
GQ_ARGS+=(--wait-max "$LOCK_WAIT")
ENV_ARGS=("RYZEN_LOCK_WAIT=$LOCK_WAIT" "RYZEN_JOBS=$JOBS" "RYZEN_RUN_ID=$RUN")
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
      GOTHIC_DIR|OPENGOTHIC_TEXTURES|OPENGOTHIC_FRACTURE) continue ;;
      OPENGOTHIC_*|GOTHIC_*|TEMPEST_*|WORLD|TIMEOUT|SOUND|MUSIC|MOVABLE_VOBS|ARROW_STICKING|PHYSICS_NOISE|DESTRUCTION|BUOYANCY|CHARACTER_VIRTUAL|NPC_CHARACTER_VIRTUAL)
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
  run_quick env "${ENV_ARGS[@]}" bash "$STATE/source/scripts/ryzen.sh" __remote bench \
    "$STATE" "$RUN" "$STATE/inputs/$RUN/$(basename "$BENCH_SCENARIO")" && rc=0 || rc=$?
  exit "$rc"
fi
# Carry e2e options, including scenario-specific GOTHIC_* settings; paths are remapped below.
while IFS= read -r name; do
  case "$name" in
    GOTHIC_DIR|GOTHIC_AI_URL|OPENGOTHIC_FRACTURE) ;;
    SCENARIO|LLM|ASR|TTS|TIMEOUT|PORT|ASR_PORT|TTS_PORT|DECIDER_URL|QUICK_URL|CHARACTER_VIRTUAL|NPC_CHARACTER_VIRTUAL|GOTHIC_*|OPENGOTHIC_*|CHATGPT_*|OPENAI_*)
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
  run_quick env "${ENV_ARGS[@]}" bash "$STATE/source/scripts/ryzen.sh" __remote e2e "$STATE" "$RUN" && rc=0 || rc=$?
  exit "$rc"
fi
run_quick env "${ENV_ARGS[@]}" bash "$STATE/source/scripts/ryzen.sh" __remote pytest "$STATE" "$RUN" "$@"
