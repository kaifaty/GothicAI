#!/usr/bin/env bash
# Builds the converted base (docs/asset-migration.md): four worlds, meshless .ZEN files, every texture and
# every static mesh of the installed Gothic II, each checked against its source.
#
#   scripts/content/build-base.sh [out dir]      # default ~/.local/share/gothic-ai/base/v1
#   GOTHIC_DIR=... scripts/content/build-base.sh
#
# The output is Gothic content: it stays outside git. On Ryzen pass an explicit folder of
# your task; automatic engine imports use a fingerprinted cache instead.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GOTHIC_DIR="${GOTHIC_DIR:-/Volumes/DevSSD/Dev/games/Gothic II}"
OUT="${1:-$HOME/.local/share/gothic-ai/base/v1}"
BUILD="${CONVERT_BUILD:-$HOME/.cache/gothic-ai/content-convert-build}"
generator=(); command -v ninja >/dev/null && generator=(-G Ninja)
cmake -S "$ROOT/scripts/convert" -B "$BUILD" "${generator[@]}" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" --parallel 4 >/dev/null
tool="$BUILD/content-convert"
"$tool" --self-test
"$tool" base "$GOTHIC_DIR" "$OUT"
# The independent video part is a sibling, never included in base.json or its fingerprint.
"$tool" videos "$GOTHIC_DIR" "${OUT}-videos"
du -sh "$OUT" "${OUT}-videos"
