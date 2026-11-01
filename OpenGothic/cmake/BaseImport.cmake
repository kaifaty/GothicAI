get_filename_component(BASE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/BaseFormat.cmake")
# The four Dx8 files keep the game's old compile context: TSF_NO_STDIO (the root add_definitions) and
# no floating-point contraction (inherited there from Jolt's PUBLIC options), without linking Jolt.
# hydra.cpp includes the engine's TinySoundFont by explicit path; GothicKit's <tsf.h> stays its own.
set_source_files_properties(
  "${BASE_ROOT}/common/dmusic/dlscollection.cpp" "${BASE_ROOT}/common/dmusic/wave.cpp"
  "${BASE_ROOT}/common/dmusic/soundfont.cpp" "${BASE_ROOT}/common/dmusic/hydra.cpp"
  PROPERTIES COMPILE_DEFINITIONS TSF_NO_STDIO
             COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/fp:precise,-ffp-contract=off>")
# The musicnative sources see dmusic's private structs (_Internal.h, thread/Thread.h): it must use the C11-thread
# backend the linked dmusic target was built with. Only that one private choice is mirrored.
get_target_property(BASE_DMUSIC_DEFINITIONS dmusic COMPILE_DEFINITIONS)
if("_DM_USE_NATIVE_THREAD=1" IN_LIST BASE_DMUSIC_DEFINITIONS)
  set_property(SOURCE "${BASE_ROOT}/common/musicnative.c" "${BASE_ROOT}/common/musicnativeimport.c"
    APPEND PROPERTY COMPILE_DEFINITIONS _DM_USE_NATIVE_THREAD=1)
endif()
# Tempest's native file, directory and text-codec code that FileUtil and the Dx8 search use, plus the
# memory streams and log the Dx8 wave and DLS parsers use.
# The game links the whole Tempest target; the converter builds only these sources.
set(BASE_RUNTIME_SOURCES
  common/baseidentity.cpp common/sha256.cpp common/worldchunks.cpp common/worlddata.cpp common/modeldata.cpp
  common/fontdata.cpp common/programdata.cpp common/outputunits.cpp common/audiodata.cpp common/basemesh.cpp
  common/gothicglb.cpp common/gltfmesh.cpp common/utils/fileutil.cpp common/musicdata.cpp common/musicnative.c
  common/dmusic/dlsruntime.cpp common/dmusic/styleruntime.cpp common/dmusic/waveruntime.cpp
  common/dmusic/hydra.cpp common/dmusic/soundfont.cpp)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/baseimport-generated/baseformat.h"
  CONTENT "#pragma once\ninline constexpr char BaseFormatSha256[] = \"@BASE_FORMAT_SHA256@\";\n" @ONLY)
set(BASE_IMPORT_SOURCES ${BASE_SOURCES})
list(REMOVE_ITEM BASE_IMPORT_SOURCES ${BASE_RUNTIME_SOURCES})
list(TRANSFORM BASE_IMPORT_SOURCES PREPEND "${BASE_ROOT}/")
list(TRANSFORM BASE_RUNTIME_SOURCES PREPEND "${BASE_ROOT}/")
add_library(gothic-base-runtime STATIC ${BASE_RUNTIME_SOURCES})
target_sources(gothic-base-runtime PRIVATE "${BASE_ROOT}/common/worldchunksruntime.cpp")
add_library(gothic-base-import STATIC ${BASE_IMPORT_SOURCES})
target_include_directories(gothic-base-runtime PUBLIC "${BASE_ROOT}/common" "${BASE_ROOT}/lib/Tempest/Engine/include")
target_include_directories(gothic-base-runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/baseimport-generated")
target_include_directories(gothic-base-runtime SYSTEM PRIVATE "${BASE_ROOT}/lib/rapidjson/include" "${BASE_ROOT}/lib/miniz"
  "${BASE_ROOT}/lib/dmusic/src" "${BASE_ROOT}/lib/dmusic/vendor/TinySoundFont")
set_target_properties(gothic-base-runtime PROPERTIES C_STANDARD 11)
target_link_libraries(gothic-base-runtime PUBLIC zenkit miniz dmusic)
target_link_libraries(gothic-base-import PUBLIC gothic-base-runtime)
target_include_directories(gothic-base-import PUBLIC "${BASE_ROOT}/common" "${BASE_ROOT}/lib/Tempest/Engine/include")
target_include_directories(gothic-base-import PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/baseimport-generated")
target_include_directories(gothic-base-import SYSTEM PRIVATE "${BASE_ROOT}/lib/rapidjson/include" "${BASE_ROOT}/lib/miniz"
  "${BASE_ROOT}/lib/dmusic/src" "${BASE_ROOT}/lib/dmusic/vendor/TinySoundFont")
# The source manifest runs the game's dmusic loader (target `dmusic`, static, added by the
# including project) and reads its parsed structures, which need C11.
set_target_properties(gothic-base-import PROPERTIES C_STANDARD 11)
target_link_libraries(gothic-base-import PUBLIC zenkit miniz dmusic)
if(TARGET Tempest)
  target_link_libraries(gothic-base-runtime PUBLIC Tempest)
else()
  list(TRANSFORM BASE_NATIVE_IO PREPEND "${BASE_ROOT}/")
  add_library(gothic-native-io STATIC ${BASE_NATIVE_IO})
  target_include_directories(gothic-native-io PUBLIC "${BASE_ROOT}/lib/Tempest/Engine/include"
    PRIVATE "${BASE_ROOT}/lib/Tempest/Engine")
  if(WIN32)
    target_compile_definitions(gothic-native-io PRIVATE NOMINMAX)
  endif()
  target_link_libraries(gothic-base-runtime PUBLIC gothic-native-io)
endif()
if(WIN32)
  target_link_libraries(gothic-base-runtime PUBLIC shlwapi)
endif()
# Import tools can link ZenKit's Squish alongside Tempest's
# different version. Keep both implementations and give the import copy its own symbols.
target_compile_definitions(squish PRIVATE squish=gothic_import_squish)
target_compile_definitions(zenkit PRIVATE squish=gothic_import_squish)
if(NOT MSVC)
  target_compile_options(gothic-base-import PRIVATE -Wall -Wconversion)
  target_compile_options(gothic-base-runtime PRIVATE -Wall -Wconversion)
endif()
