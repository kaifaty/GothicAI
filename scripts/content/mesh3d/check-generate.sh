#!/bin/sh
# Cached CPU dependencies only; uv is forbidden from contacting the network.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export UV_OFFLINE=1 HF_HUB_OFFLINE=1 HF_HUB_DISABLE_IMPLICIT_TOKEN=1
export PYTHONDONTWRITEBYTECODE=1
exec uv run --offline --no-project --with numpy --with scipy --with pillow --with pytest \
  python -m pytest -q -p no:cacheprovider "$HERE/test_generate_trellis.py" "$HERE/test_run_mesh3d.py"
