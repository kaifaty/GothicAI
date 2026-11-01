#!/usr/bin/env bash
# Source on Ryzen before configuring CMake. Keep address-to-line debug information
# and GCC's -O2: -O3 triggers false-positive -Werror=stringop-overflow diagnostics.
RYZEN_CMAKE_FLAGS=(
  '-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -g1 -DNDEBUG'
  '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -g1 -DNDEBUG'
)

ryzen_ccache=
if [[ "${RYZEN_CCACHE:-1}" != 0 ]] && ryzen_ccache=$(command -v ccache); then
  export CCACHE_DIR="${CCACHE_DIR-$HOME/.cache/gothic-ai/ccache}"
  export CCACHE_MAXSIZE=20G
  export CCACHE_BASEDIR="$HOME/gothic-ai"
  export CCACHE_NOHASHDIR=true
  mkdir -p "$CCACHE_DIR"
else
  ryzen_ccache=
fi
RYZEN_CMAKE_FLAGS+=(
  "-DCMAKE_C_COMPILER_LAUNCHER=$ryzen_ccache"
  "-DCMAKE_CXX_COMPILER_LAUNCHER=$ryzen_ccache"
)
unset ryzen_ccache
