add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../lib/meshoptimizer" meshoptimizer EXCLUDE_FROM_ALL)

add_library(gothic-cluster-lod STATIC "${CMAKE_CURRENT_LIST_DIR}/../common/graphics/mesh/clusterlod.cpp")
target_include_directories(gothic-cluster-lod PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../common")
target_compile_features(gothic-cluster-lod PUBLIC cxx_std_20)
target_link_libraries(gothic-cluster-lod PRIVATE meshoptimizer)
if(NOT MSVC)
  target_compile_options(gothic-cluster-lod PRIVATE -Wall -Wconversion -Werror)
endif()
