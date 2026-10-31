# P17 is a separate part: none of these files enter BASE_FORMAT_INPUTS.
set(VIDEO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(VIDEO_ROOT "${VIDEO_ROOT}" ABSOLUTE)
set(VIDEO_FORMAT_INPUTS
  common/videodata.h common/videodata.cpp common/videoimport.cpp common/videoio.h common/bink/video.h common/bink/video.cpp
  common/bink/frame.h common/bink/frame.cpp common/sha256.h common/sha256.cpp
  lib/miniz/miniz.h lib/miniz/miniz.c cmake/VideoImport.cmake)
list(SORT VIDEO_FORMAT_INPUTS)
set(VIDEO_FORMAT_SUMS "")
foreach(input IN LISTS VIDEO_FORMAT_INPUTS)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VIDEO_ROOT}/${input}")
  file(SHA256 "${VIDEO_ROOT}/${input}" sum)
  string(APPEND VIDEO_FORMAT_SUMS "${input} ${sum}\n")
endforeach()
string(SHA256 VIDEO_FORMAT_SHA256 "${VIDEO_FORMAT_SUMS}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/videoimport-generated/videoformat.h"
  CONTENT "#pragma once\ninline constexpr char VideoFormatSha256[] = \"@VIDEO_FORMAT_SHA256@\";\n" @ONLY)
add_library(gothic-video-runtime STATIC "${VIDEO_ROOT}/common/videodata.cpp")
target_include_directories(gothic-video-runtime PUBLIC "${VIDEO_ROOT}/common"
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/videoimport-generated")
target_include_directories(gothic-video-runtime SYSTEM PRIVATE "${VIDEO_ROOT}/lib/miniz" "${VIDEO_ROOT}/lib/rapidjson/include")
target_link_libraries(gothic-video-runtime PUBLIC gothic-base-runtime)
add_library(gothic-video-import STATIC
  "${VIDEO_ROOT}/common/videoimport.cpp" "${VIDEO_ROOT}/common/basestoreimport.cpp" "${VIDEO_ROOT}/common/bink/video.cpp" "${VIDEO_ROOT}/common/bink/frame.cpp")
target_include_directories(gothic-video-import PUBLIC "${VIDEO_ROOT}/common"
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/videoimport-generated")
target_include_directories(gothic-video-import SYSTEM PRIVATE "${VIDEO_ROOT}/lib/miniz" "${VIDEO_ROOT}/lib/rapidjson/include")
target_link_libraries(gothic-video-import PUBLIC gothic-base-import gothic-video-runtime)
if(NOT MSVC)
  target_compile_options(gothic-video-import PRIVATE -Wall -Wconversion -ffp-contract=off)
endif()
