# P17 is a separate part: none of these files enter BASE_FORMAT_INPUTS.
if(NOT DEFINED VIDEO_ROOT)
  get_filename_component(VIDEO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
set(VIDEO_FORMAT_INPUTS
  common/videodata.h common/videodata.cpp common/videoimport.cpp common/videoio.h common/bink/video.h common/bink/video.cpp
  common/bink/frame.h common/bink/frame.cpp common/sha256.h common/sha256.cpp
  lib/miniz/miniz.h lib/miniz/miniz.c cmake/VideoImport.cmake cmake/VideoFormat.cmake)
list(SORT VIDEO_FORMAT_INPUTS)
set(VIDEO_FORMAT_SUMS "")
foreach(input IN LISTS VIDEO_FORMAT_INPUTS)
  if(NOT CMAKE_SCRIPT_MODE_FILE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VIDEO_ROOT}/${input}")
  endif()
  file(SHA256 "${VIDEO_ROOT}/${input}" sum)
  string(APPEND VIDEO_FORMAT_SUMS "${input} ${sum}\n")
endforeach()
string(SHA256 VIDEO_FORMAT_SHA256 "${VIDEO_FORMAT_SUMS}")
if(CMAKE_SCRIPT_MODE_FILE)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${VIDEO_FORMAT_SHA256}")
endif()
