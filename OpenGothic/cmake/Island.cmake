# The empty island (reports/engine/2026-11-13-empty-island/island-content.md): layer `island` of
# scripts/engine_layers.py, which may take the platform only. Its content is made by code at build time and
# lives in the build folder alone:
#
#   island-content   the generator (island/content/)
#   island-data      runs it into <build>/island/; not in ALL, rerun only when the generator itself changed
#   gothic-island-content-test   the contract of what it wrote, and frames of it through Renderer::draw
#
# EngineLayerGuard.cmake has already set guard_rules. Without the layer rules the layer cannot be checked, so
# its targets are not made (the same condition as engine-platform in EnginePlatform.cmake).
if(NOT EXISTS "${guard_rules}")
  message(WARNING "Island targets skipped: no layer rules at ${guard_rules}")
  return()
endif()

set(island_source "${CMAKE_CURRENT_SOURCE_DIR}/island/content")
# The world chunk and GLB writers are the platform's own (gothic-base-runtime); squish is the copy Tempest reads
# DDS with. Nothing of the game: the generator relinks, and the data is redone, only when these change.
add_library(island-content-lib STATIC EXCLUDE_FROM_ALL
  "${island_source}/islandcontent.cpp" "${island_source}/islandimage.cpp" "${island_source}/islandprops.cpp"
  "${island_source}/islandterrain.cpp" "${island_source}/islandtextures.cpp")
target_include_directories(island-content-lib PUBLIC "${island_source}")
target_include_directories(island-content-lib SYSTEM PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/rapidjson/include" "${CMAKE_CURRENT_SOURCE_DIR}/lib/Tempest/Engine/thirdparty/squish")
target_link_libraries(island-content-lib PUBLIC gothic-base-runtime PRIVATE squish-tempest)
add_executable(island-content EXCLUDE_FROM_ALL "${island_source}/main.cpp")
target_link_libraries(island-content PRIVATE island-content-lib)
if(NOT MSVC)
  target_compile_options(island-content-lib PRIVATE -Wall -Wconversion -Werror)
  target_compile_options(island-content PRIVATE -Wall -Wconversion -Werror)
endif()

set(ISLAND_DATA_DIR "${CMAKE_CURRENT_BINARY_DIR}/island")
set(island_stamp "${ISLAND_DATA_DIR}/island.stamp")
add_custom_command(OUTPUT "${island_stamp}"
  COMMAND island-content "${ISLAND_DATA_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${island_stamp}"
  DEPENDS island-content
  COMMENT "Generating the empty island into ${ISLAND_DATA_DIR}"
  VERBATIM)
add_custom_target(island-data DEPENDS "${island_stamp}")

if(UNIX)
  # No Gothic, no base, the stubs of tests/platform_services.h; reads <build>/island/ by the path compiled in.
  add_executable(gothic-island-content-test EXCLUDE_FROM_ALL tests/island_content_test.cpp)
  target_link_libraries(gothic-island-content-test PRIVATE OpenGothic island-content-lib)
  target_include_directories(gothic-island-content-test SYSTEM BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/rapidjson/include")
  target_compile_definitions(gothic-island-content-test PRIVATE ISLAND_DATA_DIR="${ISLAND_DATA_DIR}")
  target_compile_options(gothic-island-content-test PRIVATE -Wall -Wconversion -Werror)
  add_dependencies(gothic-island-content-test island-data)
endif()
