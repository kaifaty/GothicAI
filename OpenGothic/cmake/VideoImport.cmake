get_filename_component(VIDEO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/VideoFormat.cmake")
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
