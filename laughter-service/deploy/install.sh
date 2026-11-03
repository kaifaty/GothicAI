#!/usr/bin/env bash
# Installs laughter detection as a systemd user service on the GPU server.
# Run scripts/deploy-gpu.sh laughter from the repository; uv must be in ~/.local/bin.
# The first start downloads and warms up the model. Logs: journalctl --user -u gothic-laughter -f
set -euo pipefail
cd "$(dirname "$0")/.."
UV="${UV:-$HOME/.local/bin/uv}"

"$UV" sync --locked --extra gpu --no-dev --python 3.12

mkdir -p ~/.config/systemd/user
cp deploy/gothic-laughter.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable gothic-laughter.service
systemctl --user restart gothic-laughter.service
echo "started; ready when the log says 'listening on'"
