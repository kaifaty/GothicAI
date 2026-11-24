# What every build with gothic-base-runtime adds after BaseImport.cmake: the runtime manifest reading and the cooked
# chunks (.wcb), the catalogue start, world cloth, and the MSVC flag for the base C sources. One list for the game
# (CMakeLists.txt), content-convert (scripts/convert) and the standalone storygraph (ai_bridge/storygraph): a source
# added to one copy only once broke the others' link. Outside BaseImport.cmake, since that file is a base format input
# (BaseFormat.cmake): a runtime source or a flag must not change the base fingerprint.
include(${CMAKE_CURRENT_LIST_DIR}/ClusterLod.cmake)
target_link_libraries(gothic-base-runtime PRIVATE gothic-cluster-lod)
# Literal references in native game code are part of the derived runtime binding
# recipe. A change rebuilds the derived base without invalidating the source import.
file(GLOB_RECURSE RUNTIME_BINDING_CONSUMERS CONFIGURE_DEPENDS
  "${BASE_ROOT}/common/game/*.cpp" "${BASE_ROOT}/common/game/*.h"
  "${BASE_ROOT}/ai_bridge/*.cpp" "${BASE_ROOT}/ai_bridge/*.h")
set(RUNTIME_BINDING_CODEC "${BASE_ROOT}/common/game/definitions/runtimebindings.cpp")
set(RUNTIME_BINDING_COLLECTOR "${BASE_ROOT}/../scripts/convert/runtimebindingroots.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RUNTIME_BINDING_CONSUMERS}
  "${RUNTIME_BINDING_CODEC}" "${RUNTIME_BINDING_COLLECTOR}")
set(RUNTIME_BINDING_NAMES)
foreach(source IN LISTS RUNTIME_BINDING_CONSUMERS)
  file(READ "${source}" contents)
  string(REGEX MATCHALL "\"[$][0-9]+\"" matches "${contents}")
  list(APPEND RUNTIME_BINDING_NAMES ${matches})
endforeach()
list(REMOVE_DUPLICATES RUNTIME_BINDING_NAMES)
list(SORT RUNTIME_BINDING_NAMES)
list(LENGTH RUNTIME_BINDING_NAMES RUNTIME_BINDING_NAME_COUNT)
string(JOIN ",\n    " RUNTIME_BINDING_NAME_ROWS ${RUNTIME_BINDING_NAMES})
file(SHA256 "${RUNTIME_BINDING_CODEC}" RUNTIME_BINDING_CODEC_SHA)
file(SHA256 "${RUNTIME_BINDING_COLLECTOR}" RUNTIME_BINDING_COLLECTOR_SHA)
string(SHA256 RUNTIME_BINDING_ROOTS_SHA
  "${RUNTIME_BINDING_CODEC_SHA}\n${RUNTIME_BINDING_COLLECTOR_SHA}\n${RUNTIME_BINDING_NAME_ROWS}")
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
target_sources(gothic-base-runtime PRIVATE "${RUNTIME_BINDING_CODEC}")
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
