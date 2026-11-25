# What every build with gothic-base-runtime adds after BaseImport.cmake: the runtime manifest reading and the cooked
# chunks (.wcb), the catalogue start, world cloth, and the MSVC flag for the base C sources. One list for the game
# (CMakeLists.txt), content-convert (scripts/convert) and the standalone storygraph (ai_bridge/storygraph): a source
# added to one copy only once broke the others' link. Outside BaseImport.cmake, since that file is a base format input
# (BaseFormat.cmake): a runtime source or a flag must not change the base fingerprint.
include(${CMAKE_CURRENT_LIST_DIR}/ClusterLod.cmake)
target_link_libraries(gothic-base-runtime PRIVATE gothic-cluster-lod)
include(${CMAKE_CURRENT_LIST_DIR}/RuntimeBindingRecipe.cmake)
set(RUNTIME_BINDING_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/runtime-bindings-generated")
file(CONFIGURE OUTPUT "${RUNTIME_BINDING_GENERATED_DIR}/runtimebindingroots.h" CONTENT [=[
#pragma once

#include <array>
#include <string_view>

inline constexpr std::string_view RuntimeBindingRootsSha256 = "@RUNTIME_BINDING_ROOTS_SHA@";
inline constexpr std::array<std::string_view,@RUNTIME_BINDING_NAME_COUNT@> RuntimeBindingLiteralNames = {
    @RUNTIME_BINDING_NAME_ROWS@
    };
]=] @ONLY)
add_library(gothic-offline-bindings STATIC EXCLUDE_FROM_ALL "${RUNTIME_BINDING_CODEC}"
  "${BASE_ROOT}/common/game/definitions/definitiontypes.cpp")
target_include_directories(gothic-offline-bindings SYSTEM PRIVATE "${BASE_ROOT}/lib/rapidjson/include")
target_link_libraries(gothic-offline-bindings PRIVATE gothic-base-runtime gothic-script-data)
if(NOT MSVC)
  target_compile_options(gothic-offline-bindings PRIVATE -Wall -Wconversion)
endif()
target_include_directories(gothic-base-runtime PUBLIC "${RUNTIME_BINDING_GENERATED_DIR}")
# Keep contraction off in the chunk reader's position and attribute calculations.
set_property(SOURCE "${BASE_ROOT}/common/worldchunksruntime.cpp"
  APPEND PROPERTY COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/fp:precise,-ffp-contract=off>")
target_sources(gothic-base-runtime PRIVATE "${BASE_ROOT}/common/worldchunkmanifest.cpp"
  "${BASE_ROOT}/common/worldchunkbinary.cpp" "${BASE_ROOT}/common/worldchunkindex.cpp" "${BASE_ROOT}/common/worldchunkcook.cpp"
  "${BASE_ROOT}/common/worldchunkassemble.cpp" "${BASE_ROOT}/common/worldchunkcookedsource.cpp"
  "${BASE_ROOT}/common/worldchunkcatalogue.cpp" "${BASE_ROOT}/common/meshclusterlod.cpp")
include(${CMAKE_CURRENT_LIST_DIR}/WorldCloth.cmake)
if(MSVC)
  # musicnative.c (runtime) and basemusic.c/musicnativeimport.c (import) read dmusic's private structs
  # with C11 _Atomic counters; dmusic opts its own sources in the same way.
  target_compile_options(gothic-base-runtime PRIVATE $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
  target_compile_options(gothic-base-import PRIVATE $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
endif()
