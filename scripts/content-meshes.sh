#!/usr/bin/env bash
# Offline inventory of static vob visuals in the four Gothic II worlds, including the addon.
# Only resource names and aggregate geometry statistics are written; no game or mesh export.
#
#   scripts/content-meshes.sh [output-directory]
#   scripts/content-meshes.sh --self-test
#
# env: GOTHIC_DIR  game data (default: /Volumes/DevSSD/Dev/games/Gothic II)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GOTHIC_DIR="${GOTHIC_DIR:-/Volumes/DevSSD/Dev/games/Gothic II}"

if [ "$#" -gt 1 ]; then
  echo "usage: scripts/content-meshes.sh [output-directory|--self-test]" >&2
  exit 2
fi
case "${1:-}" in
  -h|--help)
    echo "usage: scripts/content-meshes.sh [output-directory|--self-test]"
    exit 0 ;;
  --self-test) ;;
  -*) echo "unknown option: $1" >&2; exit 2 ;;
esac

tool() {
  local build="$ROOT/OpenGothic/build" src="$ROOT/scripts/meshes/world-meshes.cpp"
  local library="$build/lib/ZenKit/libzenkit.a" squish="$build/lib/ZenKit/vendor/libsquish/libsquish.a"
  local bin="$build/meshes/world-meshes" cmake_build

  if [ ! -f "$library" ] || [ ! -f "$squish" ]; then
    if [ -f "$build/CMakeCache.txt" ]; then
      # Build only ZenKit and its dependencies in an existing engine configuration.
      cmake_build="$build"
    else
      # Configure ZenKit alone, without the renderer or game.
      cmake_build="$build/lib/ZenKit"
      cmake -S "$ROOT/OpenGothic/lib/ZenKit" -B "$cmake_build" \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
        -DZK_BUILD_TESTS=OFF -DZK_BUILD_EXAMPLES=OFF -DZK_BUILD_SHARED=OFF \
        -DZK_ENABLE_ASAN=OFF -DZK_ENABLE_DEPRECATION=OFF -DZK_ENABLE_MMAP=ON \
        -DZK_ENABLE_ZIPPED_VDF=OFF -DZK_ENABLE_INSTALL=OFF \
        -DBUILD_SQUISH_WITH_OPENMP=OFF -DBUILD_SQUISH_WITH_SSE2=OFF >&2 || return 1
    fi
    cmake --build "$cmake_build" --target zenkit --parallel 2 >&2 || return 1
  fi

  mkdir -p "$build/meshes"
  if [ ! -x "$bin" ] || [ "$src" -nt "$bin" ] || [ "$0" -nt "$bin" ] \
      || [ "$library" -nt "$bin" ] || [ "$squish" -nt "$bin" ]; then
    local libraries=("$library" "$squish")
    if [ -f "$build/libminiz.a" ]; then libraries+=("$build/libminiz.a"); fi
    "${CXX:-c++}" -std=c++20 -O2 -DZKNO_REM=1 -D_ZK_WITH_MMAP=1 \
      -I"$ROOT/OpenGothic/lib/ZenKit/include" -I"$build/lib/ZenKit/include" \
      -I"$ROOT/OpenGothic/lib/ZenKit/vendor/libsquish" -I"$build/lib/ZenKit/vendor/libsquish" \
      "$src" "${libraries[@]}" -o "$bin" >&2 || return 1
  fi
  echo "$bin"
}

bin="$(tool)"
if [ "${1:-}" = --self-test ]; then
  exec "$bin" --self-test
fi
exec "$bin" "$GOTHIC_DIR" "${1:-$ROOT/reports/content/2026-10-world-meshes}" "$ROOT"
