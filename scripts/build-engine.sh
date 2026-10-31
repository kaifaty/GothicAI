#!/usr/bin/env bash
# Builds the game and adjacent content-convert (OpenGothic/build/opengothic/). Everything it needs is in the repository,
# including the engine's libraries (OpenGothic/lib, see OpenGothic/lib/VENDORED.md).
#
#   scripts/build-engine.sh                 # RelWithDebInfo
#   BUILD_TYPE=Debug scripts/build-engine.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cmake -S "$ROOT/OpenGothic" -B "$ROOT/OpenGothic/build" -G Ninja -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
cmake --build "$ROOT/OpenGothic/build" --target Gothic2Notr
