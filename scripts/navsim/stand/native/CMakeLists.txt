# nav-sim stand library (docs/nav-sim.md, S1). Standalone project, like raybench:
#   cmake -S scripts/navsim/stand/native -B BUILD -DCMAKE_PREFIX_PATH=<embree-4.4.0> -DCMAKE_BUILD_TYPE=Release
# Ryzen: scripts/navsim/stand/stand-remote.sh builds it with Embree 4.4.0 from its release archive.
cmake_minimum_required(VERSION 3.22)
project(navsim_stand LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(OPENGOTHIC "${CMAKE_CURRENT_SOURCE_DIR}/../../../../OpenGothic")
find_package(embree 4.4 REQUIRED)
find_package(Threads REQUIRED)

# The eye core is the engine's own file, never a copy; it needs only Tempest math.
add_library(navsim_stand SHARED navsim.cpp "${OPENGOTHIC}/common/game/navigationeye.cpp")
target_include_directories(navsim_stand PRIVATE
  "${OPENGOTHIC}/common" "${OPENGOTHIC}/lib/Tempest/Engine/include")
target_link_libraries(navsim_stand PRIVATE embree Threads::Threads)
target_compile_options(navsim_stand PRIVATE -Wall -Wextra)
set_target_properties(navsim_stand PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
