#!/usr/bin/env bash
# Installs the face track service as a systemd user service on the GPU server.
# Run scripts/deploy-gpu.sh a2f from the repository; uv must be in ~/.local/bin.
# Needs the built Audio2Face-3D SDK and TensorRT engine of audio2face.md ("Как поставлено"):
#   A2F_SDK  the SDK with _build/release (default ~/gothic-ai/a2f/sdk)
#   A2F_GEN  the generated v3.0 model with network.trt (default ~/gothic-ai/a2f/gen/v3), used read-only
# Logs: journalctl --user -u gothic-a2f -f
set -euo pipefail
cd "$(dirname "$0")/.."
UV="${UV:-$HOME/.local/bin/uv}"
GEN="${A2F_GEN:-$HOME/gothic-ai/a2f/gen/v3}"
MODEL="$HOME/gothic-ai/a2f/model/v3.0"

native/build.sh
"$UV" sync --locked --no-dev --python 3.12

# the unit's model directory: links to the generated model, so a shape file can replace its link later
if [ ! -e "$MODEL/model.json" ]; then
  [ -f "$GEN/network.trt" ] || { echo "no TensorRT engine at $GEN/network.trt" >&2; exit 1; }
  mkdir -p "$MODEL"
  for f in "$GEN"/*; do ln -sfn "$f" "$MODEL/$(basename "$f")"; done
fi

mkdir -p ~/.config/systemd/user
cp deploy/gothic-a2f.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable gothic-a2f.service
systemctl --user restart gothic-a2f.service
echo "started; ready when the log says 'listening on'"
