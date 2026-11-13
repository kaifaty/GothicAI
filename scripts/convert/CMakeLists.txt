cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
project(gothic_content_convert LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(ZK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZK_ENABLE_ASAN OFF CACHE BOOL "" FORCE)
set(ZK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ZK_ENABLE_DEPRECATION OFF CACHE BOOL "" FORCE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(BUILD_SQUISH_WITH_SSE2 OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory(../../OpenGothic/lib/ZenKit zenkit EXCLUDE_FROM_ALL)
add_library(miniz STATIC ../../OpenGothic/lib/miniz/miniz.c)
set(DM_BUILD_STATIC ON CACHE INTERNAL "")
set(DM_ENABLE_ASAN OFF CACHE INTERNAL "")
add_subdirectory(../../OpenGothic/lib/dmusic dmusic EXCLUDE_FROM_ALL)
include(../../OpenGothic/cmake/BaseImport.cmake)
# runtime manifest reading and cooked chunks, outside the base format fingerprint (see OpenGothic/CMakeLists.txt)
target_sources(gothic-base-runtime PRIVATE "${BASE_ROOT}/common/worldchunkmanifest.cpp"
  "${BASE_ROOT}/common/worldchunkbinary.cpp" "${BASE_ROOT}/common/worldchunkindex.cpp" "${BASE_ROOT}/common/worldchunkcook.cpp"
  "${BASE_ROOT}/common/worldchunkassemble.cpp" "${BASE_ROOT}/common/worldchunkcookedsource.cpp")
include(../../OpenGothic/cmake/WorldCloth.cmake)
if(MSVC)
  # the base C sources read dmusic's _Atomic structs (see OpenGothic/CMakeLists.txt)
  target_compile_options(gothic-base-runtime PRIVATE $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
  target_compile_options(gothic-base-import PRIVATE $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
endif()
include(../../OpenGothic/cmake/VideoImport.cmake)
add_subdirectory(../../OpenGothic/common/daedalus daedalus)
if(MSVC)
  # M_PI in the Bink decoder; OpenGothic/CMakeLists.txt defines this for every target
  target_compile_definitions(gothic-video-import PRIVATE _USE_MATH_DEFINES)
endif()
endif()
add_executable(content-convert convert.cpp stressbase.cpp stressmeshes.cpp worldcook.cpp ../../OpenGothic/common/basestore.cpp
  ../../OpenGothic/common/world/voblayer.cpp ../../OpenGothic/common/world/movablevobs.cpp
  ../../OpenGothic/common/game/definitions/particlecontent.cpp
  ../../OpenGothic/common/game/definitions/definitiontypes.cpp)
get_filename_component(VOB_LAYER_MOVABLE_CSV "${CMAKE_CURRENT_SOURCE_DIR}/../../OpenGothic/data/movable-vobs.csv" ABSOLUTE)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VOB_LAYER_MOVABLE_CSV}")
endif()
file(READ "${VOB_LAYER_MOVABLE_CSV}" MOVABLE_VOBS_CSV)
configure_file(../../OpenGothic/common/world/movablevobsdata.h.in vob-layer-generated/movablevobsdata.h @ONLY)
target_include_directories(content-convert PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/vob-layer-generated"
  "${CMAKE_BINARY_DIR}/baseimport-generated")
target_include_directories(content-convert PRIVATE ../../OpenGothic/common)
# time-world packs static collision's two inputs with the game's PackedMesh PK_Physic. It needs Tempest's graphics
# library, so only the build with the game (OpenGothic/CMakeLists.txt) has it; the converter alone skips those rows.
if(TARGET Tempest)
  target_sources(content-convert PRIVATE ../../OpenGothic/common/graphics/mesh/submesh/packedmesh.cpp
    ../../OpenGothic/common/graphics/mesh/texeldensity.cpp ../../OpenGothic/common/game/compatibility/phoenix.cpp)
  target_compile_definitions(content-convert PRIVATE CONTENT_CONVERT_PACKED_MESH=1)
  # A frame-profile build compiles Tempest with the recorder's hooks (OPENGOTHIC_ENABLE_FRAME_PROFILE); the game
  # links the recorder from OpenGothic, the converter needs its own copy of the standalone file.
  if(OPENGOTHIC_ENABLE_FRAME_PROFILE)
    target_sources(content-convert PRIVATE ../../OpenGothic/common/frameprofile.cpp)
    target_compile_definitions(content-convert PRIVATE OPENGOTHIC_ENABLE_FRAME_PROFILE=1)
  endif()
endif()
target_include_directories(content-convert SYSTEM PRIVATE ../../OpenGothic/lib/rapidjson/include ../../OpenGothic/lib/miniz)
if(NOT MSVC)
  target_compile_options(content-convert PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wconversion>)
endif()
# Offline particle checks (check-particles) use the game's particlecontent loader and VM without writing base data.
# Base import never executes this VM, so gothic-daedalus stays outside the base format digest.
target_link_libraries(content-convert PRIVATE gothic-base-import gothic-video-import gothic-daedalus)
