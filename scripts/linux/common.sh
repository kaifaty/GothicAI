# shellcheck shell=bash
# Shared by Setup-GothicAI.sh and Start-GothicAI.sh in the repository root (the Linux counterpart of
# scripts/windows/common.ps1). The caller sets GOTHIC_ROOT, the folder the entry points are in.

# the launcher's Linux log folder (gothic_launcher.commands.default_log_root), for the entry points' messages
# shellcheck disable=SC2034
LOG_ROOT="$HOME/.local/state/gothic-ai/logs"
# one interpreter for the launcher and the services (theirs is pinned by .python-version): uv downloads it
# once, on the first setup
export UV_PYTHON=3.12
UV=''

say() { printf '%s\n' "$*"; }

# The release archive's own pinned uv first (package.sh), then an installed one. Sets UV and puts its
# folder on PATH: the launcher finds it there too when it starts the services with `uv run`.
find_uv() {
  local candidate
  for candidate in "$GOTHIC_ROOT/tools/uv/uv" "$(command -v uv || true)" "$HOME/.local/bin/uv" \
                   "$HOME/.cargo/bin/uv"; do
    if [[ -n "$candidate" && -f "$candidate" && -x "$candidate" ]]; then
      UV="$candidate"
      case ":$PATH:" in
        *":$(dirname "$candidate"):"*) ;;
        *) PATH="$(dirname "$candidate"):$PATH"; export PATH ;;
      esac
      return 0
    fi
  done
  return 1
}

launcher() { "$UV" run --locked --project "$GOTHIC_ROOT/launcher/core" gothic-launcher "$@"; }

# Whether the dynamic loader finds a 64-bit library by its soname: its cache first (ldconfig is in /sbin,
# which a user's PATH may lack), then the usual folders when there is no cache.
have_library() {
  local ldconfig folder
  ldconfig="$(command -v ldconfig || true)"
  for folder in /sbin /usr/sbin; do
    if [[ -z "$ldconfig" && -x "$folder/ldconfig" ]]; then ldconfig="$folder/ldconfig"; fi
  done
  # The cache is read whole first: grep -q would stop at the first match, ldconfig would die of SIGPIPE and
  # the pipeline would fail under the callers' pipefail although the library is there.
  local cache=''
  if [[ -n "$ldconfig" ]]; then cache="$("$ldconfig" -p 2> /dev/null || true)"; fi
  if grep -E "^[[:space:]]*$1 \(.*x86-64" > /dev/null <<< "$cache"; then
    return 0
  fi
  for folder in /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib64 /lib64 /usr/lib; do
    if [[ -e "$folder/$1" ]]; then return 0; fi
  done
  return 1
}
