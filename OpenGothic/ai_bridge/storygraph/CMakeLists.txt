# storygraph: lifts the story layer of a compiled GOTHIC.DAT (dialogs, quest variables) into a graph.
# See docs/story-graph.md in the monorepo.
#
# Standalone build (shared base importer, no game):
#   cmake -S OpenGothic/ai_bridge/storygraph -B OpenGothic/ai_bridge/storygraph/build -G Ninja
#   cmake --build OpenGothic/ai_bridge/storygraph/build

cmake_minimum_required(VERSION 3.21)
project(StoryGraph LANGUAGES C CXX)

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
  endif()
  set(BUILD_SQUISH_WITH_SSE2 OFF CACHE INTERNAL "")
  set(ZK_ENABLE_ASAN         OFF CACHE INTERNAL "")
  set(ZK_ENABLE_DEPRECATION  OFF CACHE INTERNAL "")
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/ZenKit ${CMAKE_CURRENT_BINARY_DIR}/zenkit)
  add_library(miniz STATIC ../../lib/miniz/miniz.c)
  set(DM_BUILD_STATIC ON CACHE INTERNAL "")
  set(DM_ENABLE_ASAN OFF CACHE INTERNAL "")
  add_subdirectory(../../lib/dmusic dmusic EXCLUDE_FROM_ALL)
  include(../../cmake/BaseImport.cmake)
  if(MSVC)
    # the base C sources read dmusic's _Atomic structs (see the game's CMakeLists.txt)
    target_compile_options(gothic-base-runtime PRIVATE $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
    target_compile_options(gothic-base-import PRIVATE $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
  endif()
  # basestore.cpp, linked into the tool below, also resolves the video part
  include(../../cmake/VideoImport.cmake)
  if(MSVC)
    # M_PI in the Bink decoder; the game's root CMakeLists.txt defines this for every target
    target_compile_definitions(gothic-video-import PRIVATE _USE_MATH_DEFINES)
  endif()
endif()

set(STORYGRAPH_SOURCES
  bytecode.cpp bytecode.h
  eval.cpp     eval.h
  exec.cpp     exec.h
  expr.cpp     expr.h
  exprjson.cpp exprjson.h
  lifter.cpp   lifter.h
  model.cpp    model.h
  predicates.cpp predicates.h
  program.cpp  program.h
  vocab.h
  ../textcodec.cpp ../textcodec.h)
if(PROJECT_IS_TOP_LEVEL)
  add_library(StoryGraph STATIC ${STORYGRAPH_SOURCES})
  target_link_libraries(StoryGraph PUBLIC zenkit)
else()
  # inside the game the objects go into ai_bridge, which gets ZenKit through the engine: linking ZenKit
  # here again reorders the static libraries and pulls in a second copy of squish
  add_library(StoryGraph OBJECT ${STORYGRAPH_SOURCES})
  target_include_directories(StoryGraph PRIVATE $<TARGET_PROPERTY:zenkit,INTERFACE_INCLUDE_DIRECTORIES>)
  target_compile_definitions(StoryGraph PRIVATE $<TARGET_PROPERTY:zenkit,INTERFACE_COMPILE_DEFINITIONS>)
endif()
target_include_directories(StoryGraph PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
# rapidjson 1.1 trips modern clang warnings; treat it as a system header
target_include_directories(StoryGraph SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/rapidjson/include)

if(NOT MSVC)
  target_compile_options(StoryGraph PRIVATE -Wall -Wconversion -Werror)
endif()

# the command line tool; inside the game build (ai_bridge) only the library is needed
option(STORYGRAPH_TOOL "build the storygraph command line tool" ${PROJECT_IS_TOP_LEVEL})
if(STORYGRAPH_TOOL)
  add_executable(storygraph main.cpp scan.cpp check.cpp check.h fuzz.cpp saveworld.cpp saveworld.h
                            effects.cpp effects.h execcheck.cpp execcheck.h snapshot.h
                            ../../common/basestore.cpp)
  target_link_libraries(storygraph PRIVATE StoryGraph gothic-base-import gothic-video-import)
  if(NOT MSVC)
    target_compile_options(storygraph PRIVATE -Wall -Wconversion -Werror)
  endif()
endif()
