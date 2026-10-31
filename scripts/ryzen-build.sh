#!/usr/bin/env bash
# Source on Ryzen before configuring CMake. Keep address-to-line debug information
# and GCC's -O2: -O3 triggers false-positive -Werror=stringop-overflow diagnostics.
RYZEN_CMAKE_FLAGS=(
  '-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -g1 -DNDEBUG'
  '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -g1 -DNDEBUG'
)
