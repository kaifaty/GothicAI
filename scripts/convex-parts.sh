#!/usr/bin/env bash
# Offline deterministic V-HACD 4 furniture collision. No game or GPU is started.
# scripts/convex-parts.sh [all|verify]
# scripts/convex-parts.sh content <content-dir> [--verify]
# OPENGOTHIC_BASE selects a prepared converted base; otherwise use the engine's
# fingerprint resolver (OPENGOTHIC_BASE_CACHE/OPENGOTHIC_BASE_STORE also apply).
# OPENGOTHIC_CONVEX_PARTS overrides the stock cache, which never belongs in git.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CACHE="${OPENGOTHIC_CONVEX_PARTS:-$HOME/.local/share/gothic-ai/convex-parts}"
BASE="${OPENGOTHIC_BASE:-}"
MODE="${1:-all}"
if [ "$#" -gt 0 ]; then shift; fi
if [ "$MODE" != content ] && [ -z "$BASE" ]; then
  CONVERT_BUILD="${CONVERT_BUILD:-$HOME/.cache/gothic-ai/content-convert-build}"
  cmake -S "$ROOT/scripts/convert" -B "$CONVERT_BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  cmake --build "$CONVERT_BUILD" --parallel 2 --target content-convert
  BASE="$("$CONVERT_BUILD/content-convert" resolve-base "${GOTHIC_DIR:-/Volumes/DevSSD/Dev/games/Gothic II}")"
fi
ARGS=(--cache "$CACHE" --base "$BASE" --rules "$ROOT/OpenGothic/data/movable-vobs.csv")
case "$MODE" in
  all) ;;
  verify) ARGS+=(--verify) ;;
  content) ARGS+=(--content "${1:?usage: scripts/convex-parts.sh content <content-dir> [--verify]}"); shift ;;
  *) echo "usage: scripts/convex-parts.sh [all|verify] | content <content-dir> [--verify]" >&2; exit 2 ;;
esac
cmake -S "$ROOT/scripts/convex-parts" -B "$CACHE/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$CACHE/build" --parallel 2
uv run --quiet --no-project --with numpy==2.3.5 --with scipy==1.16.3 \
  python "$ROOT/scripts/convex-parts/parts.py" "${ARGS[@]}" \
  --extractor "$CACHE/build/convex-parts-extract" --decomposer "$CACHE/build/convex-parts-vhacd" "$@"
