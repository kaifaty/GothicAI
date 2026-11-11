#!/usr/bin/env bash
# Run from the Mac worktree after setup-dev-ubuntu.sh; one window for both models.
# Usage: run-mesh3d.sh --batch [--model trellis2 --images-dir DIR] --max-downtime 3600 --authorization LABEL
#        run-mesh3d.sh --model pixal3d (--image RGBA.png [--fov RAD] | --views DIR) --seed N ...  (setup-pixal3d.sh)
set -euo pipefail

image=
views=
fov=0.3490658503988659
seed=
model=both
batch=0
images_dir="$HOME/.local/share/gothic-ai/content-sources/mesh3d-pilot"
max_downtime=
authorization=
min_free=16000
while (( $# > 0 )); do
  case "$1" in
    --batch) batch=1; shift ;;
    --image|--views|--fov|--seed|--model|--images-dir|--max-downtime|--authorization|--min-free-mib)
      (( $# >= 2 )) || exit 2
      case "$1" in
        --image) image="$2" ;;
        --views) views="$2" ;;
        --fov) fov="$2" ;;
        --seed) seed="$2" ;;
        --model) model="$2" ;;
        --images-dir) images_dir="$2" ;;
        --max-downtime) max_downtime="$2" ;;
        --authorization) authorization="$2" ;;
        --min-free-mib) min_free="$2" ;;
      esac
      shift 2 ;;
    --help|-h)
      echo 'Usage: run-mesh3d.sh (--batch [--images-dir DIR] | --image RGBA.png --seed N | --views DIR --seed N) [--model both|trellis2|trellis|pixal3d] [--fov RAD] [--min-free-mib N] --max-downtime SECONDS --authorization LABEL'
      exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ "$max_downtime" =~ ^[0-9]+$ && "$authorization" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || {
  echo 'Explicit --max-downtime and --authorization are required' >&2; exit 2;
}
(( max_downtime > 600 && max_downtime <= 3600 )) || { echo 'Downtime must be 601..3600 seconds (600 reserved for recovery)' >&2; exit 2; }
if (( batch )); then
  # A whole furniture set goes through TRELLIS.2 alone in one window (2026-11-03); both models stay the default.
  [[ -z "$image" && -z "$views" && -z "$seed" && ( "$model" == both || "$model" == trellis2 ) && -d "$images_dir" ]] || exit 2
  shopt -s nullglob
  images=("$images_dir"/*.png)
  (( ${#images[@]} > 0 )) || { echo 'No PNG inputs' >&2; exit 2; }
elif [[ -n "$views" ]]; then
  # Posed views for Pixal3D's multi-view mode: RGBA PNGs and the transforms.json that names them.
  [[ "$model" == pixal3d && -z "$image" && -f "$views/transforms.json" && "$seed" =~ ^[0-9]+$ ]] || exit 2
  shopt -s nullglob
  images=("$views"/*.png "$views/transforms.json")
  (( ${#images[@]} > 1 )) || { echo 'No PNG views' >&2; exit 2; }
else
  [[ -f "$image" && "$seed" =~ ^[0-9]+$ ]] || { echo 'An RGBA PNG and a nonnegative seed are required' >&2; exit 2; }
  images=("$image")
fi
case "$model" in both|trellis2|trellis|pixal3d) ;; *) exit 2 ;; esac
[[ "$fov" =~ ^[0-9]*\.?[0-9]+$ ]] || exit 2
# GPU memory the window must free before it runs (Gemma first, then the Decider); Pixal3D's NAF stage peaks
# above 16 GB next to the always-on services (the hero head, 2026-11-05)
[[ "$min_free" =~ ^[0-9]+$ ]] && (( min_free >= 16000 && min_free <= 23000 )) || exit 2
script_dir="$(cd "$(dirname "$0")" && pwd)"
run_id="$(date -u +%Y%m%dT%H%M%SZ)-${authorization}-$$"

check_mac_game() {
  local game_status
  set +e
  # Exact process name excludes an agent whose prompt mentions the game binary.
  LC_ALL=C pgrep -lx Gothic2Notr
  game_status=$?
  set -e
  [[ "$game_status" != 1 ]] || echo no game
  [[ "$game_status" == 1 ]] || { echo "Mac game preflight failed (pgrep exit $game_status); no services stopped" >&2; exit 75; }
}
check_mac_game
ssh -T dev-ubuntu mkdir -p "gothic-ai/mesh3d/inputs/$run_id"
scp "$script_dir/generate_trellis.py" "$script_dir/generate_pixal3d.py" "$script_dir/adapter.py" dev-ubuntu:gothic-ai/mesh3d/
scp "${images[@]}" "dev-ubuntu:gothic-ai/mesh3d/inputs/$run_id/"
# Repeat after transfer, immediately before allowing service downtime.
check_mac_game

# ssh joins the arguments into one command line, where an empty one vanishes: the input kind is always a word.
input_kind=image
[[ -z "$views" ]] || input_kind=views
ssh -T dev-ubuntu bash -s -- "$run_id" "${seed:-1}" "$model" "$batch" "$max_downtime" "$authorization" \
  "$input_kind" "$fov" "$min_free" <<'REMOTE'
set -euo pipefail
run_id="$1"
seed="$2"
model="$3"
batch="$4"
max_downtime="$5"
authorization="$6"
views=
[[ "${7:-image}" != views ]] || views=1
fov="${8:-0.3490658503988659}"
min_free_mib="${9:-16000}"
cd "$HOME/gothic-ai/mesh3d"
export HF_HOME="$PWD/hf" HF_HUB_DISABLE_IMPLICIT_TOKEN=1
export HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1
export TORCH_HOME="$PWD/cache/torch" XDG_CACHE_HOME="$PWD/cache" TMPDIR="$PWD/tmp"
export CUDA_HOME="$PWD/toolchain" TORCH_CUDA_ARCH_LIST=8.6 ATTN_BACKEND=xformers
export CC="$CUDA_HOME/bin/x86_64-conda-linux-gnu-gcc" CXX="$CUDA_HOME/bin/x86_64-conda-linux-gnu-g++"
export CUDACXX="$CUDA_HOME/bin/nvcc" FLEX_GEMM_AUTOSAVE_AUTOTUNE_CACHE=0
export PATH="$CUDA_HOME/bin:$PATH"
export CPATH="$CUDA_HOME/targets/x86_64-linux/include${CPATH:+:$CPATH}"
export LIBRARY_PATH="$CUDA_HOME/lib:$CUDA_HOME/targets/x86_64-linux/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
export LD_LIBRARY_PATH="$CUDA_HOME/lib:$CUDA_HOME/targets/x86_64-linux/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p "$TMPDIR" "runs/$run_id"

models=(trellis trellis2)
[[ "$model" == both ]] || models=("$model")
for current in "${models[@]}"; do
  [[ -x ".venv-$current/bin/python" ]] || { echo "Missing environment for $current; run setup first" >&2; exit 1; }
done
if [[ "$model" != trellis ]]; then
  [[ -s models/dinov3/config.json && ( -s models/dinov3/model.safetensors || -s models/dinov3/model.safetensors.index.json ) ]] || {
    echo 'DINOv3 weights are unavailable; finish the authorized download before opening the GPU window' >&2
    exit 1
  }
fi
# Reject opaque or empty inputs before stopping any service. This uses no GPU.
".venv-${models[0]}/bin/python" - "$PWD/inputs/$run_id" <<'PY'
import sys
from pathlib import Path
from PIL import Image
paths = sorted(Path(sys.argv[1]).glob("*.png"))
if not paths:
    raise SystemExit("No PNG inputs")
for path in paths:
    with Image.open(path) as image:
        if image.format != "PNG" or image.mode != "RGBA":
            raise SystemExit("Input must be an RGBA PNG")
        alpha_min, alpha_max = image.getchannel("A").getextrema()
        if alpha_min == 255 or alpha_max <= 204:
            raise SystemExit("Input must have opaque foreground and a transparent background")
PY

exec 9>"$HOME/gothic-ai/materials/gpu.lock"
flock -n 9 || { echo 'The shared materials/mesh3d GPU window is occupied' >&2; exit 75; }
log_event() {
  printf '%s\t%s\t%s\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$run_id" "$1" "$2" >> events.tsv
}
listener_url() {
  local port="$1" path="$2" endpoint host
  endpoint=$(timeout 3 ss -ltnH "sport = :$port" 2>/dev/null | awk '{print $4}' | sort -u) || return 1
  [[ -n "$endpoint" && "$endpoint" != *$'\n'* && "$endpoint" == *":$port" ]] || return 1
  host="${endpoint%:*}"
  case "$host" in
    '*'|0.0.0.0|'[::]'|'::') host=127.0.0.1 ;;
    \[*\]) ;;
    *:*) host="[$host]" ;;
  esac
  [[ -n "$host" ]] || return 1
  printf 'http://%s:%s%s' "$host" "$port" "$path"
}
unit_responds() {
  local unit="$1" url="$2" code
  if [[ "$unit" == gothic-gemma ]]; then
    curl --fail --silent --max-time 2 "$url" >/dev/null
  else
    # An HTTP upgrade rejection also proves that the WebSocket service responds.
    code=$(curl --silent --output /dev/null --max-time 2 --write-out '%{http_code}' "$url" || true)
    [[ -n "$code" && "$code" != 000 ]]
  fi
}
free_memory() {
  local value
  value=$(timeout 5 nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits)
  [[ "$value" =~ ^[0-9]+$ ]] || { echo 'Expected one GPU with numeric free memory' >&2; return 1; }
  printf '%s' "$value"
}

# Each label represents one separately authorized window. Never include the
# historical smoke downtime.tsv, and never reopen an already consumed label.
mkdir -p authorizations
authorization_log="$PWD/authorizations/$authorization.tsv"
[[ ! -e "$authorization_log" ]] || { echo 'Authorization already consumed; no services stopped' >&2; exit 75; }
generation_budget=$(( max_downtime - 600 ))
deadline=0
stopped=()
health_urls=()
stop_epochs=()
stop_epoch=0
run_pid=
run_scope=
monitor_pid=
restore() {
  local rc=$? index unit healthy attempt elapsed now service_elapsed pid
  local -a health_pids=()
  # Background launch/monitor shells must never restore the parent window.
  (( BASH_SUBSHELL == 0 )) || exit "$rc"
  trap - EXIT HUP INT TERM
  set +e
  if [[ -n "$run_pid" ]]; then
    # setsid below isolates only this run's process group, including its children.
    kill -TERM -- "-$run_pid" 2>/dev/null
    for attempt in {1..10}; do
      kill -0 "$run_pid" 2>/dev/null || break
      sleep 1
    done
    kill -KILL -- "-$run_pid" 2>/dev/null
    wait "$run_pid" 2>/dev/null
  fi
  if [[ -n "$run_scope" ]]; then
    timeout 15 systemctl --user stop "$run_scope" 2>/dev/null
  fi
  if [[ -n "$monitor_pid" ]]; then
    kill "$monitor_pid" 2>/dev/null
    wait "$monitor_pid" 2>/dev/null
  fi
  # Start every stopped unit before waiting for either model to become ready.
  for unit in ${stopped[@]+"${stopped[@]}"}; do
    timeout 10 systemctl --user start --no-block "$unit" || rc=1
    log_event start "$unit"
  done
  for index in "${!stopped[@]}"; do
    (
      trap - EXIT HUP INT TERM
      unit="${stopped[$index]}"
      healthy=0
      # Each service gets 30 bounded probes, in parallel within the 600s reserve.
      for attempt in {1..30}; do
        if timeout 3 systemctl --user is-active --quiet "$unit" && unit_responds "$unit" "${health_urls[$index]}"; then
          healthy=1
          break
        fi
        sleep 2
      done
      now=$(date +%s)
      service_elapsed=$(( now - stop_epochs[index] ))
      printf 'service\t%s\t%s\t%s\t%s\t%s\n' "$run_id" "$unit" "${stop_epochs[$index]}" "$now" "$service_elapsed" >> "$authorization_log"
      log_event health "$unit:$healthy downtime_seconds=$service_elapsed"
      if [[ "$healthy" != 1 ]]; then
        echo "SERVICE RESTORE FAILED: $unit" >&2
        exit 1
      fi
    ) &
    health_pids+=("$!")
  done
  for pid in ${health_pids[@]+"${health_pids[@]}"}; do
    wait "$pid" || rc=1
  done
  if (( stop_epoch > 0 )); then
    elapsed=$(( $(date +%s) - stop_epoch ))
    printf 'window\t%s\t%s\t%s\t%s\n' "$run_id" "$stop_epoch" "$(date +%s)" "$elapsed" >> "$authorization_log"
    log_event downtime "authorization=$authorization window_seconds=$elapsed max_seconds=$max_downtime"
    (( elapsed <= max_downtime )) || { echo 'AUTHORIZED SERVICE DOWNTIME LIMIT EXCEEDED' >&2; rc=1; }
  fi
  timeout 5 nvidia-smi > "runs/$run_id/nvidia-restored.txt" || rc=1
  log_event window-end "exit=$rc"
  exit "$rc"
}
trap restore EXIT
trap 'exit 130' HUP INT TERM

# Verify the actual process cgroup before any service is stopped. These limits
# apply only to this pilot's scopes; no user-manager or system limits are changed.
cat > "runs/$run_id/limited-exec.sh" <<'LIMITED'
#!/usr/bin/env bash
set -euo pipefail
cg=$(awk -F: '$1 == "0" {print $3}' /proc/self/cgroup)
cgdir="/sys/fs/cgroup$cg"
[[ "$(cat "$cgdir/memory.max")" == 21474836480 ]] || exit 1
[[ "$(cat "$cgdir/memory.swap.max")" == 0 ]] || exit 1
# This systemd version lacks MemoryOOMGroup; the delegated scope file is writable.
echo 1 > "$cgdir/memory.oom.group"
[[ "$(cat "$cgdir/memory.oom.group")" == 1 ]] || exit 1
printf '%s\n' "$cgdir" > "$1"
shift
exec "$@"
LIMITED
scope_properties=(--user --scope --quiet -p MemoryMax=20G -p MemorySwapMax=0)
manager_cg=$(systemctl --user show --property=ControlGroup --value)
grep -qw memory "/sys/fs/cgroup$manager_cg/cgroup.controllers"
grep -qw memory "/sys/fs/cgroup$manager_cg/cgroup.subtree_control"
systemd-run "${scope_properties[@]}" --unit="mesh3d-preflight-$run_id" \
  bash "$PWD/runs/$run_id/limited-exec.sh" "$PWD/runs/$run_id/preflight-cgroup.txt" true
log_event window-start "$model batch=$batch authorization=$authorization max_downtime=$max_downtime"
timeout 5 nvidia-smi > "runs/$run_id/nvidia-before.txt"
free_mib=$(free_memory)
# Require the memory available after Gemma/Decider (--min-free-mib, 16000 by default).
# These are the only services this window may stop. Voxtral/VoxCPM stay running.
for unit in gothic-gemma gothic-decider; do
  (( free_mib >= min_free_mib )) && break
  timeout 3 systemctl --user is-active --quiet "$unit" || continue
  if [[ "$unit" == gothic-gemma ]]; then
    url=$(listener_url 47834 /v1/models) || { echo "Cannot identify $unit listener; refusing to stop it" >&2; exit 1; }
  else
    url=$(listener_url 47832 /) || { echo "Cannot identify $unit listener; refusing to stop it" >&2; exit 1; }
  fi
  unit_responds "$unit" "$url" || { echo "$unit does not respond before stop; refusing to stop it" >&2; exit 1; }
  health_urls+=("$url")
  stopped+=("$unit")
  now=$(date +%s)
  stop_epochs+=("$now")
  if (( stop_epoch == 0 )); then
    stop_epoch=$now
    # Persist consumption before the first stop, including interrupted windows.
    printf 'start\t%s\t%s\t%s\t%s\n' "$run_id" "$now" "$max_downtime" "$generation_budget" > "$authorization_log"
    deadline=$(( stop_epoch + generation_budget - 30 ))
  fi
  log_event stop-request "$unit"
  timeout 35 systemctl --user stop "$unit"
  log_event stop "$unit"
  free_mib=$(free_memory)
done
timeout 5 nvidia-smi > "runs/$run_id/nvidia-ready.txt"
(( free_mib >= min_free_mib )) || { echo "Insufficient GPU memory: $free_mib MiB free, require $min_free_mib MiB" >&2; exit 75; }
# RAM is checked after the stops (user, 2026-11-02): Gemma alone holds about 12 GB, so before them dev-ubuntu sat at
# 23 GiB and the window never opened. Short of 24 GiB the trap starts the stopped services again.
available_kib=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
(( available_kib >= 24 * 1024 * 1024 )) || {
  echo "MemAvailable below 24 GiB after the stops: $available_kib KiB; services restored" >&2
  exit 75
}
log_event memory-preflight "available_kib=$available_kib memory_max=21474836480 swap_max=0 oom_group=1"

monitor_scope() {
  local path_file="$1" journal="$2" cgdir value peak events
  while kill -0 "$run_pid" 2>/dev/null; do
    if [[ -s "$path_file" ]]; then
      cgdir=$(cat "$path_file")
      if [[ -r "$cgdir/memory.current" ]]; then
        value=$(cat "$cgdir/memory.current" 2>/dev/null) || value=unavailable
        peak=$(cat "$cgdir/memory.peak" 2>/dev/null) || peak=unavailable
        events=$(tr '\n' ',' < "$cgdir/memory.events" 2>/dev/null) || events=unavailable
        printf '%s\t%s\t%s\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$value" "$peak" "$events" >> "$journal"
      fi
    fi
    sleep 1
  done
}

# v1 first, then the corrected 512 loader. The earlier cascade decoder reached
# 29103 MiB RSS; omitting its unused 2465 MiB texture weights still exceeds the
# 20480 MiB scope limit. Do not admit that known oversized variant in this pilot.
variants=()
for current in "${models[@]}"; do
  case "$current" in
    trellis) variants+=(trellis) ;;
    pixal3d) variants+=(pixal3d-1024) ;;  # bf16 flow models one at a time: within the 20 GiB scope
    *) variants+=(trellis2-512) ;;
  esac
done
generation_rc=0
(( deadline > 0 )) || deadline=$(( $(date +%s) + generation_budget - 30 ))
for variant in "${variants[@]}"; do
  budget=$(( deadline - $(date +%s) ))
  (( budget > 0 )) || { echo 'Generation deadline reached' >&2; exit 124; }
  current=trellis
  pipeline_args=()
  model_args=(--model trellis)
  generator=generate_trellis.py
  if [[ "$variant" == pixal3d-* ]]; then
    current=pixal3d
    model_args=()
    generator=generate_pixal3d.py
  elif [[ "$variant" == trellis2-* ]]; then
    current=trellis2
    model_args=(--model trellis2)
    pipeline_args=(--pipeline-type "${variant#trellis2-}")
  fi
  output="$PWD/runs/$run_id/$variant"
  # Remote shells do not have the Mac array; single mode has exactly one PNG.
  if (( batch )); then
    output="$PWD/runs/$run_id/batch"
    input_args=(--images-dir "$PWD/inputs/$run_id" --seeds 1 2 --deadline "$deadline")
  elif [[ "$current" == pixal3d && -n "$views" ]]; then
    input_args=(--views "$PWD/inputs/$run_id" --seed "$seed")
  elif [[ "$current" == pixal3d ]]; then
    inputs=("$PWD/inputs/$run_id/"*.png)
    input_args=(--image "${inputs[0]}" --fov "$fov" --seed "$seed")
  else
    inputs=("$PWD/inputs/$run_id/"*.png)
    input_args=(--image "${inputs[0]}" --seed "$seed")
  fi
  run_scope="mesh3d-$run_id-$variant.scope"
  path_file="$PWD/runs/$run_id/$variant.cgroup"
  log_event generation-start "$variant budget_seconds=$budget scope=$run_scope memory_max=21474836480"
  setsid timeout --signal=TERM --kill-after=10 "$budget" systemd-run "${scope_properties[@]}" --unit="$run_scope" \
    bash "$PWD/runs/$run_id/limited-exec.sh" "$path_file" \
    "$PWD/.venv-$current/bin/python" -u "$PWD/$generator" ${model_args[@]+"${model_args[@]}"} \
    --root "$PWD" "${input_args[@]}" \
    --output "$output" ${pipeline_args[@]+"${pipeline_args[@]}"} > "runs/$run_id/$variant.log" 2>&1 &
  run_pid=$!
  monitor_scope "$path_file" "$PWD/runs/$run_id/$variant.cgroup-memory.tsv" &
  monitor_pid=$!
  set +e
  wait "$run_pid"
  rc=$?
  timeout 15 systemctl --user stop "$run_scope" 2>/dev/null
  kill "$monitor_pid" 2>/dev/null
  wait "$monitor_pid" 2>/dev/null
  monitor_pid=
  run_scope=
  run_pid=
  set -e
  log_event generation-end "$variant exit=$rc"
  if (( rc != 0 )); then
    cat "runs/$run_id/$variant.log" >&2
    # Exit 1 means individual failures are durably recorded; still try the next
    # model. Repeated generator errors (86), deadlines and process kills stop.
    if (( batch == 0 || rc != 1 )); then exit "$rc"; fi
    generation_rc=1
  fi
done
if [[ "$model" == trellis2 || "$model" == both ]]; then
  log_event generation-skip 'trellis2-1024_cascade estimated_rss_mib=26638 memory_max_mib=20480; prior measured decode exceeds cap even after unused weights removed'
fi
printf 'Results: ~/gothic-ai/mesh3d/runs/%s\n' "$run_id"
exit "$generation_rc"
REMOTE
