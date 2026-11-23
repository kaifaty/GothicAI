#!/usr/bin/env bash
# Installs NPC memory embeddings as a systemd user service on the GPU server.
# Run scripts/deploy-gpu.sh embed from the repository; uv must be in ~/.local/bin.
# The first start downloads FRIDA and warms it up. Logs: journalctl --user -u gothic-embed -f
set -euo pipefail
cd "$(dirname "$0")/.."
UV="${UV:-$HOME/.local/bin/uv}"

"$UV" sync --locked --extra gpu --no-dev --python 3.12

mkdir -p ~/.config/systemd/user
cp deploy/gothic-embed.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable gothic-embed.service
systemctl --user restart gothic-embed.service
echo "started; ready when the log says 'listening on'"
