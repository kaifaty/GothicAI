#!/usr/bin/env bash
# Builds the game of the Linux x64 release archive (.github/workflows/release.yml, docs/development.md):
#   OpenGothic/build/opengothic/Gothic2Notr               the game with the AI bridge
#   OpenGothic/build/opengothic/content-convert           the base and video converter (a dependency of the game)
#   OpenGothic/ai_bridge/storygraph/build/storygraph      the story graph converter (STORYGRAPH_TOOL)
#
# For the players' archive, not for development: scripts/build-engine.sh stays the everyday build. The
# differences are only flags given here, so the engine's CMakeLists.txt is the same for every system:
#   - Release at -O2 instead of CMake's -O3: GCC's -O3 raises false -Werror=stringop-overflow
#     diagnostics in the engine (scripts/ryzen-build.sh keeps -O2 for the same reason);
#   - libstdc++ and libgcc linked statically, so the binaries need only glibc (2.35 on Ubuntu 22.04, where CI
#     builds), the Vulkan loader and the X11 libraries of the player's system, whatever their GCC is. Their
#     symbols stay out of the dynamic table that the engine's -rdynamic exports: a Vulkan driver loaded into
#     the game binds to its own libstdc++, not to this copy.
#
# Needs CMake 3.21+, Ninja, a C++20 GCC (CC and CXX; CI uses gcc-13 and g++-13), the X11, Xcursor and
# ALSA/PulseAudio/PipeWire headers, and the Vulkan SDK: VULKAN_SDK with include/, the loader's libvulkan.so
# where the linker finds it (LunarG's Linux SDK keeps it in lib/VulkanLoader/lib, so CI puts that folder on
# LIBRARY_PATH) and glslangValidator on PATH. The first configure fetches IXWebSocket from GitHub.
#
#   scripts/linux/build.sh            JOBS=<n> limits the parallel compilers
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_TYPE=Release
: "${VULKAN_SDK:?set VULKAN_SDK to the Vulkan SDK folder with include/ and lib/}"
for tool in cmake ninja glslangValidator; do
  command -v "$tool" > /dev/null || { echo "$tool is not on PATH" >&2; exit 2; }
done
[[ -f "$VULKAN_SDK/include/vulkan/vulkan.h" ]] || { echo "no Vulkan headers in $VULKAN_SDK/include" >&2; exit 2; }

static_runtime='-static-libstdc++ -static-libgcc -Wl,--exclude-libs,libstdc++.a:libgcc.a:libgcc_eh.a'
flags=(
  -G Ninja
  "-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
  '-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG'
  '-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG'
  "-DCMAKE_EXE_LINKER_FLAGS=$static_runtime"
  "-DCMAKE_SHARED_LINKER_FLAGS=$static_runtime"
)
parallel=()
if [[ -n "${JOBS:-}" ]]; then parallel=(--parallel "$JOBS"); fi

engine="$ROOT/OpenGothic"
graph="$engine/ai_bridge/storygraph"
# Ninja keeps compiling the other objects after an error (-k 0): one failed build reports every broken file.
cmake -S "$engine" -B "$engine/build" "${flags[@]}"
cmake --build "$engine/build" --target Gothic2Notr ${parallel[@]+"${parallel[@]}"} -- -k 0
cmake -S "$graph" -B "$graph/build" "${flags[@]}"
cmake --build "$graph/build" --target storygraph ${parallel[@]+"${parallel[@]}"} -- -k 0

out="$engine/build/opengothic"
built=("$out/Gothic2Notr" "$out/content-convert" "$graph/build/storygraph")
for file in "${built[@]}"; do
  [[ -x "$file" ]] || { echo "the build finished without $file" >&2; exit 1; }
done
# This fork, not a stock OpenGothic: the AI bridge reads its server address from GOTHIC_AI_URL.
if ! grep -q GOTHIC_AI_URL "$out/Gothic2Notr"; then
  echo "$out/Gothic2Notr has no AI bridge (GOTHIC_AI_URL not found)" >&2
  exit 1
fi
echo "built ($BUILD_TYPE):"
printf '  %s\n' "${built[@]}"
