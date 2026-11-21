# hand-motion-bridge: Joy-Con over SDL3 to the game's "hand motion" frames (README.md). A project of its own, not
# part of the game's build: the game does not link SDL3.
cmake_minimum_required(VERSION 3.16)
project(hand-motion-bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL3 3.4.0 CONFIG REQUIRED)

add_executable(hand-motion-bridge
  src/main.cpp
  src/motion.cpp
  src/record.cpp
  src/sinks.cpp
  src/run.cpp
  src/live.cpp
  src/selftest.cpp)

# the frame format comes from the engine: OpenGothic/common/input/handmotionframe.h
target_include_directories(hand-motion-bridge PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/../../common)
target_compile_definitions(hand-motion-bridge PRIVATE
  HMB_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/excerpt.csv")
target_link_libraries(hand-motion-bridge PRIVATE SDL3::SDL3)
if(NOT MSVC)
  target_compile_options(hand-motion-bridge PRIVATE -Wall -Wextra)
endif()
