add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../lib/meshoptimizer" meshoptimizer EXCLUDE_FROM_ALL)

add_library(gothic-cluster-lod STATIC "${CMAKE_CURRENT_LIST_DIR}/../common/graphics/mesh/clusterlod.cpp")
target_include_directories(gothic-cluster-lod PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../common")
target_compile_features(gothic-cluster-lod PUBLIC cxx_std_20)
target_link_libraries(gothic-cluster-lod PRIVATE meshoptimizer)
if(MSVC)
  # Supported MSVC versions (VS 2022+) disable contraction in precise mode.
  target_compile_options(meshoptimizer PRIVATE /fp:precise)
  target_compile_options(gothic-cluster-lod PRIVATE /fp:precise)
else()
  # Match x86 and ARM rounding when scoring and ordering edge collapses.
  target_compile_options(meshoptimizer PRIVATE -ffp-contract=off)
  target_compile_options(gothic-cluster-lod PRIVATE -Wall -Wconversion -Werror -ffp-contract=off)
endif()
