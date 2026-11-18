# What every build with gothic-base-runtime adds after BaseImport.cmake: the runtime manifest reading and the cooked
# chunks (.wcb), the catalogue start, world cloth, and the MSVC flag for the base C sources. One list for the game
# (CMakeLists.txt), content-convert (scripts/convert) and the standalone storygraph (ai_bridge/storygraph): a source
# added to one copy only once broke the others' link. Outside BaseImport.cmake, since that file is a base format input
# (BaseFormat.cmake): a runtime source or a flag must not change the base fingerprint.
include(${CMAKE_CURRENT_LIST_DIR}/ClusterLod.cmake)
target_link_libraries(gothic-base-runtime PRIVATE gothic-cluster-lod)
# Keep contraction off in the chunk reader's position and attribute calculations.
set_property(SOURCE "${BASE_ROOT}/common/worldchunksruntime.cpp"
  APPEND PROPERTY COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/fp:precise,-ffp-contract=off>")
target_sources(gothic-base-runtime PRIVATE "${BASE_ROOT}/common/worldchunkmanifest.cpp"
  "${BASE_ROOT}/common/worldchunkbinary.cpp" "${BASE_ROOT}/common/worldchunkindex.cpp" "${BASE_ROOT}/common/worldchunkcook.cpp"
  "${BASE_ROOT}/common/worldchunkassemble.cpp" "${BASE_ROOT}/common/worldchunkcookedsource.cpp"
  "${BASE_ROOT}/common/worldchunkcatalogue.cpp")
include(${CMAKE_CURRENT_LIST_DIR}/WorldCloth.cmake)
if(MSVC)
  # musicnative.c (runtime) and basemusic.c/musicnativeimport.c (import) read dmusic's private structs
  # with C11 _Atomic counters; dmusic opts its own sources in the same way.
  target_compile_options(gothic-base-runtime PRIVATE $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
  target_compile_options(gothic-base-import PRIVATE $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
endif()
