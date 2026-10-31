#!/usr/bin/env bash
# Offline area/UV inventory of the four Gothic II worlds, including the addon.
# The triangle mode exports NEWWORLD geometry; no game runs or texture pixels are read.
#
#   scripts/content-textures.sh [output-directory]
#   scripts/content-textures.sh --self-test
#   scripts/content-textures.sh --intent /absolute/path/outside/repository/intent.json
#   scripts/content-textures.sh --triangles outside.json --textures names.txt [--bounds xmin zmin xmax zmax]
#
# env: GOTHIC_DIR  game data (default: /Volumes/DevSSD/Dev/games/Gothic II)
# The C++ tool mounts geometry VDF archives with addon world priority.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GOTHIC_DIR="${GOTHIC_DIR:-/Volumes/DevSSD/Dev/games/Gothic II}"

usage() {
  echo "usage: scripts/content-textures.sh [output-directory|--self-test|--intent outside-json|--triangles outside-json --textures names-file [--bounds xmin zmin xmax zmax]]"
}
case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  --self-test)
    if [ "$#" -ne 1 ]; then usage >&2; exit 2; fi ;;
  --intent|--triangles)
    if [ "$1" = --intent ] && [ "$#" -ne 2 ]; then usage >&2; exit 2; fi
    if [ "$1" = --triangles ]; then
      if { [ "$#" -ne 4 ] && [ "$#" -ne 9 ]; } || [ "${3:-}" != --textures ] \
          || { [ "$#" -eq 9 ] && [ "${5:-}" != --bounds ]; }; then usage >&2; exit 2; fi
    fi
    python3 - "$ROOT" "$2" <<'PYGUARD'
import pathlib, sys
root, target = map(lambda p: pathlib.Path(p).resolve(), sys.argv[1:])
if target.is_relative_to(root):
    raise SystemExit("raw geometry must stay outside the repository")
PYGUARD
    ;;
  -*) echo "unknown option: $1" >&2; exit 2 ;;
  *) if [ "$#" -gt 1 ]; then usage >&2; exit 2; fi ;;
esac

tool() {
  local build="$ROOT/OpenGothic/build" src="$ROOT/scripts/content/world-textures.cpp"
  local library="$build/lib/ZenKit/libzenkit.a" squish="$build/lib/ZenKit/vendor/libsquish/libsquish.a"
  local bin="$build/content/world-textures" cmake_build intent_src="$ROOT/scripts/content/world-intent.inc"

  if [ ! -f "$library" ] || [ ! -f "$squish" ]; then
    if [ -f "$build/CMakeCache.txt" ]; then
      # Reuse an existing engine configuration, building only the ZenKit target and its dependencies.
      cmake_build="$build"
    else
      # A standalone ZenKit configuration preserves the engine's usual library paths without
      # configuring or compiling the renderer, game, or other engine dependencies.
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

  mkdir -p "$build/content"
  if [ ! -x "$bin" ] || [ "$src" -nt "$bin" ] || [ "$0" -nt "$bin" ] \
      || [ "$intent_src" -nt "$bin" ] || [ "$library" -nt "$bin" ] || [ "$squish" -nt "$bin" ]; then
    local libraries=("$library" "$squish")
    # Existing engine builds can also provide miniz (standalone ZenKit uses ordinary VDFs).
    if [ -f "$build/libminiz.a" ]; then libraries+=("$build/libminiz.a"); fi
    "${CXX:-c++}" -std=c++20 -O2 -DZKNO_REM=1 -D_ZK_WITH_MMAP=1 \
      -I"$ROOT/OpenGothic/lib/ZenKit/include" -I"$build/lib/ZenKit/include" \
      -I"$ROOT/OpenGothic/lib/ZenKit/vendor/libsquish" -I"$build/lib/ZenKit/vendor/libsquish" \
      "$src" "${libraries[@]}" -o "$bin" >&2 || return 1
  fi
  echo "$bin"
}

bin="$(tool)"
if [ "${1:-}" = --triangles ]; then
  exec "$bin" --triangles "$GOTHIC_DIR" "${@:2}"
fi
if [ "${1:-}" = --intent ]; then
  exec "$bin" --intent "$GOTHIC_DIR" "$2"
fi
if [ "${1:-}" = --self-test ]; then
  exec "$bin" --self-test
fi
exec "$bin" "$GOTHIC_DIR" "${1:-$ROOT/reports/content/2026-10-world-textures}"
