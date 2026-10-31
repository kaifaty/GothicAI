#!/usr/bin/env bash
# Build only the scalar decoder outside OpenGothic and measure all 71 originals.
# No original pixels or decoded images are written by this tool.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
if [ "$#" -lt 1 ]; then
  echo "usage: scripts/content/measure-originals.sh <Gothic II directory> [measure_luminance.py options, e.g. --write]" >&2
  exit 2
fi
game="$1"
shift
build="${GOTHIC_MEASURE_BUILD:-${TMPDIR:-/tmp}/gothic-original-scalar-measures}"
build="$(python3 -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).resolve())' "$build")"
case "$build/" in "$ROOT/"*) echo "measurement build must stay outside the repository" >&2; exit 2;; esac
cmake -S "$ROOT/OpenGothic/lib/ZenKit" -B "$build/zenkit" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DZK_BUILD_TESTS=OFF \
  -DZK_BUILD_EXAMPLES=OFF -DZK_BUILD_SHARED=OFF -DZK_ENABLE_ASAN=OFF \
  -DZK_ENABLE_DEPRECATION=OFF -DZK_ENABLE_MMAP=ON -DZK_ENABLE_ZIPPED_VDF=OFF \
  -DZK_ENABLE_INSTALL=OFF -DBUILD_SQUISH_WITH_OPENMP=OFF -DBUILD_SQUISH_WITH_SSE2=OFF >&2
cmake --build "$build/zenkit" --target zenkit --parallel 2 >&2
"${CXX:-c++}" -std=c++20 -O2 -DZKNO_REM=1 -D_ZK_WITH_MMAP=1 \
  -I"$ROOT/OpenGothic/lib/ZenKit/include" -I"$build/zenkit/include" \
  "$ROOT/scripts/content/texture-luminance.cpp" "$build/zenkit/libzenkit.a" \
  "$build/zenkit/vendor/libsquish/libsquish.a" -o "$build/texture-luminance" >&2
exec python3 "$ROOT/scripts/content/measure_luminance.py" --extractor "$build/texture-luminance" \
  --game "$game" --materials "$@"
