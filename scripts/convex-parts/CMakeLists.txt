cmake_minimum_required(VERSION 3.20)
project(gothic_convex_parts LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Threads REQUIRED)
add_executable(convex-parts-vhacd vhacd.cpp)
target_link_libraries(convex-parts-vhacd PRIVATE Threads::Threads)
add_executable(convex-parts-extract extract.cpp
  ../../OpenGothic/common/gltfmesh.cpp
  ../../OpenGothic/common/gothicglb.cpp
  ../../OpenGothic/common/basemesh.cpp)
target_include_directories(convex-parts-extract PRIVATE
  ../../OpenGothic/lib/ZenKit/include ../../OpenGothic/lib/rapidjson/include)
