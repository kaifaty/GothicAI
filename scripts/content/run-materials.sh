#!/usr/bin/env bash
# Run one GPU stage on dev-ubuntu. Invoke from the Mac worktree, never remotely.
# Usage: scripts/content/run-materials.sh flux|sdxl|normals|appearance|stablematerials [--base flux|sdxl]
set -euo pipefail
STAGE="${1:?stage required}"
shift
case "$STAGE" in flux|sdxl|normals|appearance|stablematerials) ;; *) exit 2 ;; esac
BASE=sdxl
if (( $# > 0 )); then
  [[ "$STAGE" == normals || "$STAGE" == appearance ]] || exit 2
  [[ $# == 2 && "$1" == --base ]] || exit 2
  BASE="$2"
fi
case "$BASE" in flux|sdxl) ;; *) exit 2 ;; esac
MIN_FREE_MIB=8500
[[ "$STAGE" != flux ]] || MIN_FREE_MIB=10000
MIN_FREE_MIB="${GPU_MIN_FREE_MIB:-$MIN_FREE_MIB}"
[[ "$MIN_FREE_MIB" =~ ^[1-9][0-9]*$ ]] || exit 2
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
started=$SECONDS
while true; do
  set +e
  # Exact process name excludes other agents whose prompt mentions the binary.
  LC_ALL=C pgrep -lx Gothic2Notr
  game_status=$?
  set -e
  [[ "$game_status" == 1 ]] && break
  [[ "$game_status" == 0 ]] || { echo "Mac game preflight failed: pgrep exit $game_status" >&2; exit 1; }
  (( SECONDS - started < 3600 )) || { echo 'Mac game still running after 60 minutes; no units stopped' >&2; exit 75; }
  echo 'Waiting for the Mac game before temporarily stopping a model service'
  sleep 30
done
scp "$ROOT/scripts/content/bakeoff.py" "$ROOT/scripts/content/stable_materials_adapter.py" dev-ubuntu:gothic-ai/materials/
# registry.jsonl is copied once at preparation. Preserve remote stage progress.
ssh -T dev-ubuntu bash -s -- "$STAGE" "$BASE" "$MIN_FREE_MIB" <<'REMOTE'
set -euo pipefail
stage="$1"
base="$2"
min_free_mib="$3"
cd "$HOME/gothic-ai/materials"
export HF_HOME="$PWD/hf" HF_HUB_DISABLE_IMPLICIT_TOKEN=1
export XDG_CACHE_HOME="$PWD/cache" TMPDIR="$PWD/tmp"
mkdir -p "$TMPDIR"
exec 9>gpu.lock
flock -n 9 || { echo 'our material generation is already running' >&2; exit 75; }
# No file or weight may be installed outside the task directory.
bytes=$(du -sb . | cut -f1)
(( bytes < 80000000000 )) || { echo '80 GB task limit reached' >&2; exit 1; }
nvidia-smi
stopped=()
health_urls=()
stop_epoch=0
log_event() {
  printf '%s\t%s\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$1" "$2" >> units.tsv
}
listener_url() {
  local port="$1" path="$2" endpoint host
  endpoint=$(ss -ltnH "sport = :$port" 2>/dev/null | awk '{print $4}' | sort -u) || return 1
  # An unresolved/ambiguous bind is not permission to stop a service.
  [[ -n "$endpoint" && "$endpoint" != *$'\n'* ]] || return 1
  [[ "$endpoint" == *":$port" ]] || return 1
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
    # HTTP upgrade rejection is also a response from the WS server.
    code=$(curl --silent --output /dev/null --max-time 2 --write-out '%{http_code}' "$url" || true)
    [[ "$code" != 000 && -n "$code" ]]
  fi
}
restore() {
  rc=$?
  trap - EXIT HUP INT TERM
  for unit in ${stopped[@]+"${stopped[@]}"}; do
    timeout 10 systemctl --user start --no-block "$unit" || rc=1
    log_event start "$unit"
  done
  for index in "${!stopped[@]}"; do
    unit="${stopped[$index]}"
    healthy=0
    for attempt in {1..45}; do
      if systemctl --user is-active --quiet "$unit"; then
        unit_responds "$unit" "${health_urls[$index]}" && healthy=1 || true
      fi
      [[ "$healthy" == 1 ]] && break
      sleep 2
    done
    systemctl --user is-active "$unit" || true
    log_event health "$unit:$healthy"
    if [[ "$healthy" != 1 ]]; then
      echo "SERVICE RESTORE FAILED: $unit" >&2
      rc=1
    fi
  done
  if (( stop_epoch > 0 )); then
    elapsed=$(( $(date +%s) - stop_epoch ))
    printf '%s\t%s\n' "$stage" "$elapsed" >> downtime.tsv
    (( elapsed <= 3600 )) || { echo 'SERVICE DOWNTIME LIMIT EXCEEDED' >&2; rc=1; }
  fi
  exit "$rc"
}
trap restore EXIT
trap 'exit 130' HUP INT TERM
free_mib=$(nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits)
# FLUX loads quantized T5 and transformer in separate phases; others need 8.5 GiB.
for unit in gothic-gemma gothic-decider; do
  (( free_mib >= min_free_mib )) && break
  systemctl --user is-active --quiet "$unit" || continue
  if [[ "$unit" == gothic-gemma ]]; then
    url=$(listener_url 47834 /v1/models) || { echo "Cannot identify listener for $unit; refusing to stop it" >&2; exit 1; }
  else
    url=$(listener_url 47832 /) || { echo "Cannot identify listener for $unit; refusing to stop it" >&2; exit 1; }
  fi
  unit_responds "$unit" "$url" || { echo "$unit does not respond before stop; refusing to stop it" >&2; exit 1; }
  health_urls+=("$url")
  # The Mac preflight immediately above authorized this GPU window.
  (( stop_epoch > 0 )) || stop_epoch=$(date +%s)
  stopped+=("$unit")
  log_event stop-request "$unit"
  timeout 35 systemctl --user stop "$unit"
  log_event stop "$unit"
  free_mib=$(nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits)
done
nvidia-smi
(( free_mib >= min_free_mib )) || { echo "Insufficient GPU memory: ${free_mib} MiB free, require ${min_free_mib} MiB" >&2; exit 75; }
prior=0
if [[ -f downtime.tsv ]]; then prior=$(awk '{s+=$2} END {print s+0}' downtime.tsv); fi
budget=$(( 3000 - prior ))
if (( stop_epoch > 0 )); then budget=$(( budget - $(date +%s) + stop_epoch )); fi
(( budget > 60 )) || { echo 'Cumulative stopped-unit budget exhausted' >&2; exit 75; }
# Hard wall deadline reserves at least 600 seconds for bounded recovery.
args=("$stage" --registry registry.jsonl --root "$PWD")
[[ "$stage" != normals && "$stage" != appearance ]] || args+=(--base "$base")
log="$stage"
[[ "$stage" != normals && "$stage" != appearance ]] || log="$stage-$base"
timeout -k 10 "$budget" .venv/bin/python -u bakeoff.py "${args[@]}" > "$log.log" 2>&1
REMOTE
