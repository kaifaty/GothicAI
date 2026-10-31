#!/usr/bin/env bash
# Build Gothic2Notr with the opt-in frame profiler (CMake option only, no source change).
set -euo pipefail
R="$HOME/gothic-ai/render-features"
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
mkdir -p "$R/build"
cmake -S "$R/src/OpenGothic" -B "$R/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DOPENGOTHIC_ENABLE_FRAME_PROFILE=ON \
  -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib" \
  >"$R/build/configure.log" 2>&1 || { tail -40 "$R/build/configure.log"; exit 1; }
start=$SECONDS
cmake --build "$R/build" --target Gothic2Notr --parallel 12 >"$R/build/build.log" 2>&1 || { tail -60 "$R/build/build.log"; exit 1; }
tail -3 "$R/build/build.log"
echo "build $((SECONDS-start))s"
ls -la "$R/build/opengothic/Gothic2Notr"
