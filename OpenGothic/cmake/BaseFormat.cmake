# Native data is shared; source parsers belong only to the converter. Paths in the digest are relative
# to OpenGothic so identical sources have the same identity on every machine.
if(NOT DEFINED BASE_ROOT)
  get_filename_component(BASE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
set(BASE_SOURCES
  common/baseidentity.cpp common/sha256.cpp common/baseimport.cpp common/basesources.cpp common/basemusic.c common/worldchunks.cpp common/worlddata.cpp common/modeldata.cpp common/fontdata.cpp
  common/programdata.cpp common/outputunits.cpp common/audiodata.cpp common/basemesh.cpp common/gothicglb.cpp common/gltfmesh.cpp common/asset/fromzenkit.cpp common/asset/worldfromzenkit.cpp common/asset/modelfromzenkit.cpp
  # The game's own path helpers, Dx8 file search and Dx8 parsers, owned here once.
  common/utils/fileutil.cpp common/dmusic/sourcefile.cpp common/dmusic/riff.cpp common/dmusic/segment.cpp common/dmusic/track.cpp
  common/dmusic/style.cpp common/dmusic/band.cpp common/dmusic/reference.cpp common/dmusic/info.cpp common/dmusic/pattern.cpp
  # P16: the Dx8 DLS parser with its SoundFont/Hydra finalization, and both music record codecs.
  common/dmusic/dlscollection.cpp common/dmusic/wave.cpp common/dmusic/soundfont.cpp common/dmusic/hydra.cpp
  common/musicdata.cpp common/musicnative.c common/musicnativeimport.c
  common/dmusic/directmusicimport.cpp common/dmusic/dlsruntime.cpp common/dmusic/styleruntime.cpp common/dmusic/waveruntime.cpp)
set(BASE_NATIVE_IO
  lib/Tempest/Engine/io/rfile.cpp lib/Tempest/Engine/io/idevice.cpp lib/Tempest/Engine/utility/dir.cpp
  lib/Tempest/Engine/utility/textcodec.cpp lib/Tempest/Engine/exceptions/exception.cpp
  lib/Tempest/Engine/io/memreader.cpp lib/Tempest/Engine/io/memwriter.cpp lib/Tempest/Engine/io/odevice.cpp
  lib/Tempest/Engine/io/wfile.cpp lib/Tempest/Engine/utility/log.cpp)
if(CMAKE_SCRIPT_MODE_FILE)
  set(BASE_FORMAT_CONFIGURE_DEPENDS)
else()
  set(BASE_FORMAT_CONFIGURE_DEPENDS CONFIGURE_DEPENDS)
endif()
file(GLOB_RECURSE BASE_FORMAT_INPUTS ${BASE_FORMAT_CONFIGURE_DEPENDS} RELATIVE "${BASE_ROOT}"
  "${BASE_ROOT}/common/asset/*.h" "${BASE_ROOT}/common/asset/fromzenkit.cpp"
  "${BASE_ROOT}/lib/ZenKit/src/*" "${BASE_ROOT}/lib/ZenKit/include/*"
  "${BASE_ROOT}/lib/ZenKit/vendor/libsquish/*.h" "${BASE_ROOT}/lib/ZenKit/vendor/libsquish/*.cpp"
  "${BASE_ROOT}/lib/rapidjson/include/*.h"
  "${BASE_ROOT}/lib/dmusic/src/*" "${BASE_ROOT}/lib/dmusic/include/*"
  "${BASE_ROOT}/lib/Tempest/Engine/io/*" "${BASE_ROOT}/lib/Tempest/Engine/utility/*" "${BASE_ROOT}/lib/Tempest/Engine/exceptions/*")
# One list for every build topology: the format digest never depends on which targets exist.
# Runtime chunk reading (worldchunksruntime.*) is deliberately outside this
# fingerprint: changing residency metadata or reading must not reimport the base.
list(APPEND BASE_FORMAT_INPUTS ${BASE_SOURCES} ${BASE_NATIVE_IO}
  common/sha256.h common/baseimport.h common/basemusic.h common/worldchunks.h common/worlddata.h common/modeldata.h common/fontdata.h common/programdata.h common/outputunits.h common/audiodata.h common/basemesh.h
  common/gothicglb.h common/gltfmesh.h common/utils/fileutil.h common/dmusic/sourcefile.h common/dmusic/riff.h common/dmusic/segment.h
  common/dmusic/track.h common/dmusic/style.h common/dmusic/band.h common/dmusic/reference.h common/dmusic/info.h common/dmusic/pattern.h
  common/dmusic/structs.h common/dmusic/dlscollection.h common/dmusic/wave.h common/dmusic/soundfont.h common/dmusic/hydra.h
  common/musicdata.h common/musicnative.h lib/TinySoundFont/tsf.h lib/Tempest/Engine/sound/sound.h
  lib/Tempest/Engine/include/Tempest/Sound lib/Tempest/Engine/include/Tempest/MemReader lib/Tempest/Engine/include/Tempest/MemWriter
  lib/Tempest/Engine/include/Tempest/ODevice lib/Tempest/Engine/include/Tempest/Log
  lib/Tempest/Engine/system/platform.h lib/Tempest/Engine/include/Tempest/File lib/Tempest/Engine/include/Tempest/Dir
  lib/Tempest/Engine/include/Tempest/TextCodec lib/Tempest/Engine/include/Tempest/Except lib/Tempest/Engine/include/Tempest/IDevice
  lib/Tempest/Engine/include/Tempest/Platform lib/cgltf/cgltf.h lib/miniz/miniz.h lib/miniz/miniz.c
  cmake/BaseImport.cmake cmake/BaseFormat.cmake)
list(REMOVE_DUPLICATES BASE_FORMAT_INPUTS)
list(SORT BASE_FORMAT_INPUTS)
set(BASE_FORMAT_SUMS "")
foreach(input IN LISTS BASE_FORMAT_INPUTS)
  if(NOT CMAKE_SCRIPT_MODE_FILE AND NOT input STREQUAL "cmake/BaseImport.cmake")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BASE_ROOT}/${input}")
  endif()
  file(SHA256 "${BASE_ROOT}/${input}" sum)
  string(APPEND BASE_FORMAT_SUMS "${input} ${sum}\n")
endforeach()
string(SHA256 BASE_FORMAT_SHA256 "${BASE_FORMAT_SUMS}")
if(CMAKE_SCRIPT_MODE_FILE)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${BASE_FORMAT_SHA256}")
endif()
