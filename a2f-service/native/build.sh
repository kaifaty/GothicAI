#!/usr/bin/env bash
# Builds liba2f_engine.so next to this script against a built Audio2Face-3D SDK (audio2face.md, "Как поставлено").
#   A2F_SDK   the SDK checkout with _build/release (default ~/gothic-ai/a2f/sdk)
#   CUDA_HOME the CUDA toolkit the SDK was built with (default /usr/local/cuda)
set -euo pipefail
cd "$(dirname "$0")"
SDK="${A2F_SDK:-$HOME/gothic-ai/a2f/sdk}"
CUDA="${CUDA_HOME:-/usr/local/cuda}"
BUILD="$SDK/_build/release"
LIB="$BUILD/audio2x-sdk/lib"
[ -f "$LIB/libaudio2x.so" ] || { echo "no built SDK at $BUILD (libaudio2x.so missing)" >&2; exit 1; }
mkdir -p build
# -include cstdint: the SDK headers need it with gcc 15, as in the SDK's own build
g++ -std=c++17 -O2 -fPIC -shared -Wall -include cstdint \
  -I "$BUILD/audio2x-sdk/include" -I "$CUDA/include" \
  a2f_engine.cpp -o build/liba2f_engine.so.tmp \
  -L "$LIB" -laudio2x -L "$CUDA/lib64" -lcudart -Wl,-rpath,"$LIB:$CUDA/lib64"
mv build/liba2f_engine.so.tmp build/liba2f_engine.so
echo "built $(pwd)/build/liba2f_engine.so"
