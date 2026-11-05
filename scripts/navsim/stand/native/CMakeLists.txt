# nav-sim stand library (docs/nav-sim.md, S1 and S2). Standalone project, like raybench:
#   cmake -S scripts/navsim/stand/native -B BUILD -DCMAKE_PREFIX_PATH=<embree-4.4.0> -DCMAKE_BUILD_TYPE=Release
# Ryzen: scripts/navsim/stand/stand-remote.sh and motion-remote.sh build it with Embree 4.4.0
# from its release archive and Jolt from OpenGothic/lib/JoltPhysics.
cmake_minimum_required(VERSION 3.22)
project(navsim_stand LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # Jolt is linked statically into the shared library
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(OPENGOTHIC "${CMAKE_CURRENT_SOURCE_DIR}/../../../../OpenGothic")
find_package(embree 4.4 REQUIRED)
find_package(Threads REQUIRED)

# Jolt as the engine configures it (OpenGothic/CMakeLists.txt): single precision,
# cross-platform deterministic, the x86-64-v3 instruction sets of the Ryzen runner.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(JOLT_X86_ENABLED SSE4_1 SSE4_2 AVX AVX2 LZCNT TZCNT F16C)
else()
  set(JOLT_X86_ENABLED "")
endif()
set(JPH_BUILD_SHARED_LIBS                OFF CACHE INTERNAL "")
set(DOUBLE_PRECISION                     OFF CACHE INTERNAL "")
set(CROSS_PLATFORM_DETERMINISTIC          ON CACHE INTERNAL "")
set(OVERRIDE_CXX_FLAGS                    OFF CACHE INTERNAL "")
set(INTERPROCEDURAL_OPTIMIZATION          OFF CACHE INTERNAL "")
set(ENABLE_ALL_WARNINGS                   OFF CACHE INTERNAL "")
set(ENABLE_INSTALL                       OFF CACHE INTERNAL "")
set(GENERATE_DEBUG_SYMBOLS               OFF CACHE INTERNAL "")
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE   OFF CACHE INTERNAL "")
set(DEBUG_RENDERER_IN_DISTRIBUTION        OFF CACHE INTERNAL "")
set(PROFILER_IN_DEBUG_AND_RELEASE         OFF CACHE INTERNAL "")
set(PROFILER_IN_DISTRIBUTION              OFF CACHE INTERNAL "")
set(ENABLE_OBJECT_STREAM                 OFF CACHE INTERNAL "")
set(JPH_USE_DX12                         OFF CACHE INTERNAL "")
set(JPH_USE_VK                           OFF CACHE INTERNAL "")
set(JPH_USE_MTL                          OFF CACHE INTERNAL "")
set(JPH_USE_CPU_COMPUTE                  OFF CACHE INTERNAL "")
foreach(JOLT_X86_FEATURE SSE4_1 SSE4_2 AVX AVX2 AVX512 LZCNT TZCNT F16C FMADD)
  if(JOLT_X86_FEATURE IN_LIST JOLT_X86_ENABLED)
    set(USE_${JOLT_X86_FEATURE} ON CACHE INTERNAL "")
  else()
    set(USE_${JOLT_X86_FEATURE} OFF CACHE INTERNAL "")
  endif()
endforeach()
add_subdirectory("${OPENGOTHIC}/lib/JoltPhysics/Build" jolt)
target_compile_options(Jolt PUBLIC -ffp-contract=off)

# The eye core is the engine's own file, never a copy; it needs only Tempest math.
add_library(navsim_stand SHARED navsim.cpp motion.cpp "${OPENGOTHIC}/common/game/navigationeye.cpp")
target_include_directories(navsim_stand PRIVATE
  "${OPENGOTHIC}/common" "${OPENGOTHIC}/lib/Tempest/Engine/include")
target_link_libraries(navsim_stand PRIVATE embree Jolt Threads::Threads)
target_compile_options(navsim_stand PRIVATE -Wall -Wextra)
set_target_properties(navsim_stand PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
