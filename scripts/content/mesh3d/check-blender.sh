#!/bin/sh
# Pure geometry only; Blender and model weights are not installed on the Mac.
set -eu
mesh3d_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
exec uv run --no-project --with numpy==2.2.6 --with pytest==8.4.2 \
  python -m pytest "$mesh3d_root/scripts/content/mesh3d/blender/test_geometry.py" \
  "$mesh3d_root/scripts/content/mesh3d/blender/test_uv_quality.py" -q
