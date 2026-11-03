#!/usr/bin/env bash
# Deploys the self-hosted models to the GPU server and keeps its settings in sync with the repository's .env.
#
#   scripts/deploy-gpu.sh show                 print the server settings that would be sent
#   scripts/deploy-gpu.sh env [--no-restart]   sync ~/gothic-ai/gothic-ai.env; running services whose
#                                              settings changed are restarted
#   scripts/deploy-gpu.sh voxtral|voxcpm       sync the settings, copy deploy/<name> and run its install.sh
#   scripts/deploy-gpu.sh decider              the same for the npc decision service (all of decider-service/)
#   scripts/deploy-gpu.sh embed                the same for NPC memory embeddings (all of embed-service/)
#   scripts/deploy-gpu.sh gemma                the same for the quick model of fights (server/deploy/gemma)
#   scripts/deploy-gpu.sh aligner              copy asr-service/deploy/aligner and run its install.sh: the
#                                              forced aligner, a command per job (no service, no restart)
#   scripts/deploy-gpu.sh dub                  copy shorts/deploy/dub and run its install.sh: voice cloning
#                                              for gothic-shorts dub through gothic-voxcpm
#
# Settings come from the nearest .env at or above the repository (real environment variables win):
#   GOTHIC_GPU_SSH       ssh destination of the GPU server (required)
#   GOTHIC_VOXTRAL_URL   -> VOXTRAL_HOST, VOXTRAL_PORT: the server listens where the ASR service connects
#   GOTHIC_VOXCPM_URL    -> VOXCPM_HOST, VOXCPM_PORT: the same for the TTS service
#   GOTHIC_DECIDER_URL   -> DECIDER_HOST, DECIDER_PORT: the same for the AI server's npc planner
#   GOTHIC_EMBED_URL     -> EMBED_HOST, EMBED_PORT: the same for NPC memory embeddings
#   GOTHIC_QUICK_LLM_URL -> GEMMA_HOST, GEMMA_PORT: the same for the AI server's quick model of fights
#   GPU_<NAME>=value     -> <NAME>=value, for anything else the server needs (e.g. GPU_HF_TOKEN)
# Nothing else is sent: API keys of the services on this machine stay here.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REMOTE_DIR=gothic-ai
REMOTE_ENV="$REMOTE_DIR/gothic-ai.env"

ENV_FILE=""
d="$ROOT"
while [ "$d" != "/" ]; do
  if [ -f "$d/.env" ]; then ENV_FILE="$d/.env"; break; fi
  d="$(dirname "$d")"
done

# value of a setting: the environment first, then the first KEY=value line of .env
setting() {
  if printenv "$1"; then return; fi
  [ -n "$ENV_FILE" ] || return 0
  sed -n -E "s/^[[:space:]]*(export[[:space:]]+)?$1=(.*)$/\2/p" "$ENV_FILE" | head -1 \
    | sed -E "s/^[[:space:]]*//; s/[[:space:]]*$//; s/^[\"']//; s/[\"']$//"
}

# ws://host:port/path -> HOST= and PORT= lines for the service prefix
listen_lines() {
  local url rest
  url="$(setting "$2")"
  [ -n "$url" ] || return 0
  rest="${url#*://}"
  rest="${rest%%/*}"
  if [[ "$rest" == *:* ]]; then
    printf '%s_HOST=%s\n%s_PORT=%s\n' "$1" "${rest%:*}" "$1" "${rest##*:}"
  else
    printf '%s_HOST=%s\n' "$1" "$rest"
  fi
}

render_env() {
  echo "# Managed by scripts/deploy-gpu.sh from the repository's .env: edits here are overwritten."
  listen_lines VOXTRAL GOTHIC_VOXTRAL_URL
  listen_lines VOXCPM GOTHIC_VOXCPM_URL
  listen_lines DECIDER GOTHIC_DECIDER_URL
  listen_lines EMBED GOTHIC_EMBED_URL
  listen_lines GEMMA GOTHIC_QUICK_LLM_URL
  local name value
  { env; [ -z "$ENV_FILE" ] || sed -E 's/^[[:space:]]*export[[:space:]]+//' "$ENV_FILE"; } \
    | sed -n -E 's/^GPU_([A-Za-z0-9_]+)=.*/\1/p' | awk '!seen[$0]++' | while read -r name; do
      value="$(setting "GPU_$name")"
      # quoted unless plain: the file is read by systemd (EnvironmentFile) and by `.` in bash
      if [[ "$value" =~ ^[A-Za-z0-9_./:@+=,-]*$ ]]; then
        printf '%s=%s\n' "$name" "$value"
      else
        printf "%s='%s'\n" "$name" "${value//\'/}"
      fi
    done
}

case "${1:-}" in
  show)
    render_env
    exit 0
    ;;
  env | voxtral | voxcpm | decider | embed | gemma | aligner | dub) ;;
  *)
    sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac

SSH_DEST="$(setting GOTHIC_GPU_SSH)"
if [ -z "$SSH_DEST" ]; then
  echo "GOTHIC_GPU_SSH is not set: add the GPU server's ssh destination to ${ENV_FILE:-.env}" >&2
  exit 1
fi
remote() { ssh -o BatchMode=yes "$SSH_DEST" "$@"; }

# uploads the settings; prints the setting names that changed
sync_env() {
  local content current
  content="$(render_env)"
  current="$(remote "cat ~/$REMOTE_ENV 2>/dev/null" || true)"
  if [ "$content" = "$current" ]; then
    echo "~/$REMOTE_ENV on $SSH_DEST is up to date" >&2
    return
  fi
  printf '%s\n' "$content" | remote "mkdir -p ~/$REMOTE_DIR && umask 077 && cat > ~/$REMOTE_ENV.tmp && mv ~/$REMOTE_ENV.tmp ~/$REMOTE_ENV"
  echo "~/$REMOTE_ENV on $SSH_DEST updated:" >&2
  printf '%s\n' "$content" | grep -v '^#' | sed -E 's/^([A-Za-z0-9_]+)=.*/  \1/' >&2
  diff <(printf '%s\n' "$current" | grep -v '^#' || true) <(printf '%s\n' "$content" | grep -v '^#') \
    | sed -n -E 's/^[<>] ([A-Za-z0-9_]+)=.*/\1/p' | sort -u || true
}

# a service restarts when its own settings or a GPU_ passthrough changed
needs_restart() {
  local prefix="$1" changed="$2"
  printf '%s\n' "$changed" | grep -q -E "^$prefix" && return 0
  printf '%s\n' "$changed" | grep -v -E '^(VOXTRAL|VOXCPM|DECIDER|EMBED|GEMMA)_' | grep -q .
}

case "${1:-}" in
  env)
    changed="$(sync_env)"
    [ -n "$changed" ] || exit 0
    if [ "${2:-}" = "--no-restart" ]; then
      echo "not restarting; running services pick the settings up on their next restart" >&2
      exit 0
    fi
    for svc in voxtral voxcpm decider embed gemma; do
      prefix="$(echo "$svc" | tr '[:lower:]' '[:upper:]')_"
      if needs_restart "$prefix" "$changed" && remote "systemctl --user is-active --quiet gothic-$svc"; then
        echo "restarting gothic-$svc" >&2
        remote "systemctl --user restart gothic-$svc"
      fi
    done
    ;;
  voxtral | voxcpm | gemma)
    case "$1" in
      voxtral) src="$ROOT/asr-service/deploy/voxtral" ;;
      voxcpm) src="$ROOT/tts-service/deploy/voxcpm" ;;
      gemma) src="$ROOT/server/deploy/gemma" ;;
    esac
    sync_env >/dev/null
    remote "mkdir -p ~/$REMOTE_DIR"
    scp -q -r "$src" "$SSH_DEST:$REMOTE_DIR/"
    # install.sh (re)starts the service, which reads the settings itself
    remote "set -a; . ~/$REMOTE_ENV; set +a; ~/$REMOTE_DIR/$1/install.sh"
    ;;
  aligner)
    remote "mkdir -p ~/$REMOTE_DIR"
    scp -q -r "$ROOT/asr-service/deploy/aligner" "$SSH_DEST:$REMOTE_DIR/"
    # its own audio.cpp build next to the scripts; nothing runs until gothic-shorts align calls align.sh
    remote "~/$REMOTE_DIR/aligner/install.sh"
    ;;
  dub)
    remote "mkdir -p ~/$REMOTE_DIR/dub"
    # the scripts only; the server's own .venv stays
    rsync -a --exclude .venv --exclude __pycache__ "$ROOT/shorts/deploy/dub/" "$SSH_DEST:$REMOTE_DIR/dub/"
    remote "~/$REMOTE_DIR/dub/install.sh"
    ;;
  decider)
    sync_env >/dev/null
    remote "mkdir -p ~/$REMOTE_DIR/decider"
    # the whole package; the server's own .venv stays
    rsync -a --delete --exclude .venv --exclude __pycache__ --exclude .pytest_cache --exclude .ruff_cache \
      "$ROOT/decider-service/" "$SSH_DEST:$REMOTE_DIR/decider/"
    remote "set -a; . ~/$REMOTE_ENV; set +a; ~/$REMOTE_DIR/decider/deploy/install.sh"
    ;;
  embed)
    sync_env >/dev/null
    remote "mkdir -p ~/$REMOTE_DIR/embed"
    # the whole package; the server's own .venv stays
    rsync -a --delete --exclude .venv --exclude __pycache__ --exclude .pytest_cache --exclude .ruff_cache \
      "$ROOT/embed-service/" "$SSH_DEST:$REMOTE_DIR/embed/"
    remote "set -a; . ~/$REMOTE_ENV; set +a; ~/$REMOTE_DIR/embed/deploy/install.sh"
    ;;
esac
