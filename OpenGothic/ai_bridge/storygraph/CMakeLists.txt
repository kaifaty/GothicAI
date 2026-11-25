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
  # runtime manifest reading, cooked chunks, world cloth and the MSVC flag (one list with the game)
  include(../../cmake/BaseRuntime.cmake)
  # basestore.cpp, linked into the tool below, also resolves the video part
  include(../../cmake/VideoImport.cmake)
  add_subdirectory(../../common/daedalus daedalus)
  if(MSVC)
    # M_PI in the Bink decoder; the game's root CMakeLists.txt defines this for every target
    target_compile_definitions(gothic-video-import PRIVATE _USE_MATH_DEFINES)
  endif()
endif()

add_library(StoryGraphRuntime STATIC
  expr.cpp expr.h
  exprjson.cpp exprjson.h exprjsonwrite.h
  predicates.cpp predicates.h
  nativeexec.cpp nativeexec.h execcore.h
  program.h vocab.h
  ../textcodec.cpp ../textcodec.h
  ../../common/game/definitions/stockstoryprograms.cpp ../../common/game/definitions/stockstoryprograms.h)
target_include_directories(StoryGraphRuntime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(StoryGraphRuntime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../common)
target_include_directories(StoryGraphRuntime SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/rapidjson/include)

add_library(StoryGraphLift STATIC
  bytecode.cpp bytecode.h
  lifter.cpp lifter.h)
target_link_libraries(StoryGraphLift PUBLIC StoryGraphRuntime gothic-script-data)

add_library(StoryGraph STATIC
  eval.cpp eval.h
  exec.cpp exec.h
  graphjson.cpp graphjson.h
  model.cpp model.h
  program.cpp)
target_link_libraries(StoryGraph PUBLIC StoryGraphLift gothic-script-state)
target_link_libraries(StoryGraph PRIVATE gothic-base-runtime)

if(NOT MSVC)
  target_compile_options(StoryGraphRuntime PRIVATE -Wall -Wconversion -Werror)
  target_compile_options(StoryGraphLift PRIVATE -Wall -Wconversion -Werror)
  target_compile_options(StoryGraph PRIVATE -Wall -Wconversion -Werror)
endif()

# The game build exposes the tool as an explicit target, without building it by default.
option(STORYGRAPH_TOOL "build the storygraph command line tool" ${PROJECT_IS_TOP_LEVEL})
add_executable(storygraph main.cpp oumessages.cpp scan.cpp check.cpp check.h fuzz.cpp saveworld.cpp saveworld.h
                            effects.cpp effects.h execcheck.cpp execcheck.h snapshot.h
                            ../../common/basestore.cpp)
target_link_libraries(storygraph PRIVATE StoryGraph gothic-base-import gothic-video-import gothic-daedalus)
if(NOT STORYGRAPH_TOOL)
  set_target_properties(storygraph PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
if(NOT MSVC)
  target_compile_options(storygraph PRIVATE -Wall -Wconversion -Werror)
endif()

# The server's reader only needs the prepared output-unit catalogue.
add_executable(storygraph-ou EXCLUDE_FROM_ALL oumain.cpp oumessages.cpp ../../common/basestore.cpp)
target_include_directories(storygraph-ou SYSTEM PRIVATE ../../lib/rapidjson/include)
target_link_libraries(storygraph-ou PRIVATE gothic-base-runtime gothic-video-runtime)
if(NOT MSVC)
  target_compile_options(storygraph-ou PRIVATE -Wall -Wconversion -Werror)
endif()
