#!/usr/bin/env bash
# Manage the user janitor on Ryzen from Mac, without sudo.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SSH=(ssh -T -o BatchMode=yes -o ConnectTimeout=10 "${RYZEN_HOST:-ryzen}")

usage() {
  echo 'Usage: scripts/ryzen-janitor.sh install|uninstall|status|run [--dry-run]|du' >&2
  exit 2
}

[[ $# -ge 1 ]] || usage
action=$1
shift
if [[ "$action" == run ]]; then
  [[ $# == 0 || ( $# == 1 && "$1" == --dry-run ) ]] || usage
else
  [[ $# == 0 ]] || usage
fi

preview() {
  # Execute this checkout, keeping installed scripts and service state untouched.
  local option=$1 remote
  remote=$(cat <<'REMOTE'
set -eu
stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM
tar -xf - -C "$stage"
/usr/bin/python3 -B "$stage/ryzen_janitor.py" __JANITOR_PREVIEW_OPTION__
REMOTE
)
  remote=${remote/__JANITOR_PREVIEW_OPTION__/$option}
  tar -C "$ROOT/scripts" -cf - ryzen_janitor.py nas_archive.py | "${SSH[@]}" "$remote"
}

case "$action" in
  install)
    remote=$(cat <<'REMOTE'
set -eu
umask 022
mkdir -p "$HOME/gothic-ai/lib" "$HOME/.config/systemd/user"
stage=$(mktemp -d "$HOME/gothic-ai/lib/.janitor-install.XXXXXX")
service=$(mktemp "$HOME/.config/systemd/user/.gothic-janitor.service.XXXXXX")
timer=$(mktemp "$HOME/.config/systemd/user/.gothic-janitor.timer.XXXXXX")
trap 'rm -rf -- "$stage"; rm -f -- "$service" "$timer"' EXIT
tar -xf - -C "$stage"
chmod 644 "$stage/ryzen_janitor.py" "$stage/nas_archive.py"
cat > "$service" <<EOF
[Unit]
Description=Gothic Ryzen disk janitor

[Service]
Type=oneshot
TimeoutStartSec=infinity
ExecStart=/usr/bin/python3 %h/gothic-ai/lib/ryzen_janitor.py
Environment=PATH=%h/.local/bin:/usr/local/bin:/usr/bin:/bin
Nice=19
IOSchedulingClass=idle
EOF
cat > "$timer" <<EOF
[Unit]
Description=Check the Gothic disks every hour

[Timer]
OnCalendar=hourly
Persistent=true

[Install]
WantedBy=timers.target
EOF
chmod 644 "$service" "$timer"
mv -f -- "$stage/nas_archive.py" "$HOME/gothic-ai/lib/ryzen_janitor_archive.py"
mv -f -- "$stage/ryzen_janitor.py" "$HOME/gothic-ai/lib/ryzen_janitor.py"
mv -f -- "$service" "$HOME/.config/systemd/user/gothic-janitor.service"
mv -f -- "$timer" "$HOME/.config/systemd/user/gothic-janitor.timer"
systemctl --user daemon-reload
systemctl --user enable --now gothic-janitor.timer
REMOTE
)
    tar -C "$ROOT/scripts" -cf - ryzen_janitor.py nas_archive.py | "${SSH[@]}" "$remote"
    ;;
  uninstall)
    "${SSH[@]}" 'set -eu
systemctl --user disable --now gothic-janitor.timer
systemctl --user stop gothic-janitor.service
rm -f -- "$HOME/.config/systemd/user/gothic-janitor.service" "$HOME/.config/systemd/user/gothic-janitor.timer" "$HOME/gothic-ai/lib/ryzen_janitor.py" "$HOME/gothic-ai/lib/ryzen_janitor_archive.py"
systemctl --user daemon-reload'
    ;;
  status)
    "${SSH[@]}" 'result=0
systemctl --user status gothic-janitor.timer --no-pager || result=$?
systemctl --user list-timers gothic-janitor.timer --all --no-pager
if [ -f "$HOME/gothic-ai/.janitor/last.json" ]; then
  cat "$HOME/gothic-ai/.janitor/last.json"
else
  echo "No janitor summary yet."
fi
exit "$result"'
    ;;
  du)
    preview --du
    ;;
  run)
    if [[ $# == 1 ]]; then
      preview --dry-run
    else
      "${SSH[@]}" 'set -eu
result=0
systemctl --user start gothic-janitor.service || result=$?
cat "$HOME/gothic-ai/.janitor/last.json"
exit "$result"'
    fi
    ;;
  *) usage ;;
esac
