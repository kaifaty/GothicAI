cmake_minimum_required(VERSION 3.20)

project(OpenGothic LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(BUILD_SHARED_LIBS OFF)

option(OPENGOTHIC_ENABLE_FRAME_PROFILE "Compile opt-in frame profiling diagnostics" OFF)
option(OPENGOTHIC_ENABLE_APP_CADENCE "Compile the normal-game death cadence benchmark" OFF)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/opengothic)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/opengothic)
set(CMAKE_DEBUG_POSTFIX "")
set(CMAKE_RELWITHDEBINFO_POSTFIX "")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_SKIP_RPATH ON)

add_library(${PROJECT_NAME} STATIC)
if(OPENGOTHIC_ENABLE_FRAME_PROFILE)
  target_compile_definitions(${PROJECT_NAME} PUBLIC OPENGOTHIC_ENABLE_FRAME_PROFILE=1)
endif()
if(OPENGOTHIC_ENABLE_APP_CADENCE)
  target_compile_definitions(${PROJECT_NAME} PUBLIC OPENGOTHIC_ENABLE_APP_CADENCE=1)
endif()
if(ANDROID)
  add_library(Gothic2Notr SHARED)
  add_library(Spacer SHARED)
else()
  add_executable(Gothic2Notr)
  add_executable(Spacer)
endif()

if(MSVC)
  add_definitions(-D_USE_MATH_DEFINES)
  add_definitions(-D_CRT_SECURE_NO_WARNINGS)
  add_definitions(-D_CRT_NONSTDC_NO_DEPRECATE)
  add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4800")
endif()

if(NOT MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wconversion -Wno-strict-aliasing -Werror)
  if(CMAKE_COMPILER_IS_GNUCC AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "8.0")
    target_compile_options(${PROJECT_NAME} PRIVATE -Wno-class-memaccess)
  endif()
  if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "7.1" AND NOT APPLE AND NOT ${CMAKE_CXX_COMPILER_ID} MATCHES "Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE -Wno-format-truncation)
  endif()
  if(UNIX)
    # -rdynamic is needed to allow obtaining meaningful backtraces
    target_link_options(${PROJECT_NAME} PRIVATE -rdynamic)
  endif()
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU") # gcc
    # required by pthread_setname_np()
    add_definitions(-D_GNU_SOURCE)
  endif()
endif()

if(WIN32)
  add_definitions(-DNOMINMAX)
  if(MSVC)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup" )
  elseif(${CMAKE_CXX_COMPILER_ID} MATCHES "Clang")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -mwindows -lpthread")
  else()
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -mwindows -lstdc++fs")
  endif()
elseif(IOS)
  enable_language(OBJCXX)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0)
elseif(APPLE)
  enable_language(OBJCXX)
endif()

if(IOS)
  file(GLOB_RECURSE ObjCSOURCES "common/*.mm")
elseif(APPLE)
  target_link_libraries(${PROJECT_NAME} "-framework AppKit")
  file(GLOB_RECURSE ObjCSOURCES "common/*.mm")
endif()

## OpenGothic

# sources
file(GLOB_RECURSE OPENGOTHIC_SOURCES "common/*.h" "common/*.cpp")
foreach(source baseidentity basestoreimport sha256 baseimport basesources worldchunks worldchunksruntime worlddata modeldata fontdata programdata outputunits audiodata basemesh gothicglb gltfmesh asset/fromzenkit asset/worldfromzenkit asset/modelfromzenkit
               utils/fileutil dmusic/sourcefile dmusic/riff dmusic/segment dmusic/track dmusic/style dmusic/band
               dmusic/reference dmusic/info dmusic/pattern musicdata dmusic/dlscollection dmusic/wave dmusic/soundfont
               dmusic/hydra dmusic/directmusicimport dmusic/dlsruntime dmusic/styleruntime dmusic/waveruntime
               videodata videoimport bink/video bink/frame)
  list(REMOVE_ITEM OPENGOTHIC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/common/${source}.cpp")
endforeach()
if(NOT OPENGOTHIC_ENABLE_FRAME_PROFILE)
  list(REMOVE_ITEM OPENGOTHIC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/common/frameprofile.cpp")
endif()
if(NOT OPENGOTHIC_ENABLE_APP_CADENCE)
  list(REMOVE_ITEM OPENGOTHIC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/common/appcadence.cpp")
endif()
file(GLOB_RECURSE SPACER_SOURCES "spacer/*.h" "spacer/*.cpp")

target_sources(${PROJECT_NAME} PRIVATE ${OPENGOTHIC_SOURCES} ${ObjCSOURCES})

# The movable-vob allowlist is data, embedded so installed builds do not depend
# on their working directory. Editing the CSV also triggers reconfiguration.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/data/movable-vobs.csv")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/data/movable-vobs.csv" MOVABLE_VOBS_CSV)
configure_file(common/world/movablevobsdata.h.in generated/movablevobsdata.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/data/destructible-vobs.csv")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/data/destructible-vobs.csv" DESTRUCTIBLE_VOBS_CSV)
configure_file(common/physics/fracturedata.h.in generated/fracturedata.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/data/cloth-vobs.csv")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/data/cloth-vobs.csv" CLOTH_VOBS_DATA)
configure_file(common/graphics/clothdata.h.in generated/clothdata.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/data/cloth-world.csv")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/data/cloth-world.csv" CLOTH_WORLD_DATA)
configure_file(common/physics/clothworlddata.h.in generated/clothworlddata.h @ONLY)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# shaders
add_subdirectory(shader)
target_link_libraries(${PROJECT_NAME} GothicShaders)

# editor icons and such
add_subdirectory(spacer/assets)

include_directories("common")

## submodule dependencies

# edd-dbg
if(WIN32)
  add_subdirectory(lib/edd-dbg)
  include_directories(lib/edd-dbg/include)
  target_link_libraries(${PROJECT_NAME} edd_dbg)
endif()

if(WIN32)
  target_link_libraries(${PROJECT_NAME} shlwapi DbgHelp)
elseif(ANDROID)
  target_link_libraries(${PROJECT_NAME} dl)
elseif(UNIX)
  target_link_libraries(${PROJECT_NAME} -lpthread -ldl)
endif()

# ZenKit
set(BUILD_SQUISH_WITH_SSE2     OFF CACHE INTERNAL "")
set(ZK_ENABLE_ASAN             OFF CACHE INTERNAL "")
set(ZK_ENABLE_DEPRECATION      OFF CACHE INTERNAL "")

add_subdirectory(lib/ZenKit)
target_link_libraries(${PROJECT_NAME} zenkit)

# dmusic
set(DM_BUILD_STATIC            ON  CACHE INTERNAL "")
set(DM_ENABLE_ASAN             OFF CACHE INTERNAL "")

add_subdirectory(lib/dmusic)
target_link_libraries(${PROJECT_NAME} dmusic)

# TinySoundFont
add_definitions(-DTSF_NO_STDIO)
include_directories(lib/TinySoundFont)

# Rapidjson
include_directories(lib/rapidjson/include)
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE lib/rapidjson/include)

# Tempest
if(WIN32)
  set(TEMPEST_BUILD_SHARED ON  CACHE INTERNAL "")
else()
  set(TEMPEST_BUILD_SHARED OFF CACHE INTERNAL "")
endif()
add_subdirectory(lib/Tempest/Engine)
if(OPENGOTHIC_ENABLE_FRAME_PROFILE)
  target_compile_definitions(Tempest PRIVATE OPENGOTHIC_ENABLE_FRAME_PROFILE=1)
  target_include_directories(Tempest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
endif()
include_directories(lib/Tempest/Engine/include)
target_link_libraries(${PROJECT_NAME} Tempest)

# zip
include_directories(lib/miniz)
add_definitions(-DMINIZ_NO_STDIO)
add_library(miniz STATIC EXCLUDE_FROM_ALL "lib/miniz/miniz.h" "lib/miniz/miniz.c")
target_link_libraries(${PROJECT_NAME} miniz)
include(cmake/BaseImport.cmake)
include(cmake/VideoImport.cmake)
target_link_libraries(${PROJECT_NAME} gothic-base-runtime gothic-video-runtime)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/convert/CMakeLists.txt")
  add_subdirectory(../scripts/convert content-convert)
  add_dependencies(Gothic2Notr content-convert)
else()
  message(STATUS "content-convert is not built: scripts/convert is not available")
endif()
target_link_libraries(${PROJECT_NAME} zlibstatic)
target_include_directories(${PROJECT_NAME} PRIVATE
  lib/Tempest/Engine/thirdparty/zlib ${CMAKE_BINARY_DIR}/lib/Tempest/thirdparty/zlib)

# Recast/Detour are used only by the opt-in body-runner navmesh commands.
add_subdirectory(lib/recastnavigation)
target_link_libraries(${PROJECT_NAME} Recast Detour)

# Jolt Physics: single precision, static library, portable instruction sets by default.
# x86-64-v3 adds AVX2, F16C, LZCNT and TZCNT (never FMA) for hosts that have them, such as the
# headless body runner on the Ryzen: there Jolt's SSE2 fallbacks (half floats, popcount, shuffles)
# cost ~2 ms of a 25 ms tick, mostly in foot IK rays. CROSS_PLATFORM_DETERMINISTIC keeps the
# results identical across both sets.
set(OPENGOTHIC_JOLT_X86 "portable" CACHE STRING "Jolt x86 instruction sets: portable or x86-64-v3")
set_property(CACHE OPENGOTHIC_JOLT_X86 PROPERTY STRINGS portable x86-64-v3)
if(OPENGOTHIC_JOLT_X86 STREQUAL "x86-64-v3")
  set(JOLT_X86_ENABLED SSE4_1 SSE4_2 AVX AVX2 LZCNT TZCNT F16C)
elseif(OPENGOTHIC_JOLT_X86 STREQUAL "portable")
  set(JOLT_X86_ENABLED "")
else()
  message(FATAL_ERROR "OPENGOTHIC_JOLT_X86 must be portable or x86-64-v3")
endif()
set(JPH_BUILD_SHARED_LIBS                OFF CACHE INTERNAL "")
set(DOUBLE_PRECISION                     OFF CACHE INTERNAL "")
set(CROSS_PLATFORM_DETERMINISTIC          ON CACHE INTERNAL "")
set(OVERRIDE_CXX_FLAGS                    OFF CACHE INTERNAL "")
set(INTERPROCEDURAL_OPTIMIZATION          OFF CACHE INTERNAL "")
set(USE_STATIC_MSVC_RUNTIME_LIBRARY       OFF CACHE INTERNAL "")
set(ENABLE_ALL_WARNINGS                   OFF CACHE INTERNAL "")
set(ENABLE_INSTALL                       OFF CACHE INTERNAL "")
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE   OFF CACHE INTERNAL "")
set(DEBUG_RENDERER_IN_DISTRIBUTION        OFF CACHE INTERNAL "")
set(PROFILER_IN_DEBUG_AND_RELEASE         OFF CACHE INTERNAL "")
set(PROFILER_IN_DISTRIBUTION              OFF CACHE INTERNAL "")
set(ENABLE_OBJECT_STREAM                 OFF CACHE INTERNAL "")
set(JPH_USE_DX12                         OFF CACHE INTERNAL "")
set(JPH_USE_VK                           OFF CACHE INTERNAL "")
set(JPH_USE_MTL                          OFF CACHE INTERNAL "")
set(JPH_USE_CPU_COMPUTE                  OFF CACHE INTERNAL "")
foreach(JOLT_X86_FEATURE SSE4_1 SSE4_2 AVX AVX2 AVX512 LZCNT TZCNT F16C FMADD)
  if(JOLT_X86_FEATURE IN_LIST JOLT_X86_ENABLED)
    set(USE_${JOLT_X86_FEATURE} ON CACHE INTERNAL "")
  else()
    set(USE_${JOLT_X86_FEATURE} OFF CACHE INTERNAL "")
  endif()
endforeach()
add_subdirectory(lib/JoltPhysics/Build)

# Jolt's inline headers inherit the same floating point behavior as its library.
# Treat the upstream headers as system headers under the engine's -Werror policy.
get_target_property(JOLT_INCLUDE_DIRECTORIES Jolt INTERFACE_INCLUDE_DIRECTORIES)
set_property(TARGET Jolt PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
  "${JOLT_INCLUDE_DIRECTORIES}")
if(XCODE AND OPENGOTHIC_JOLT_X86 STREQUAL "portable")
  # Upstream adds SSE4.2 for Xcode unconditionally; retain the portable x86 baseline.
  set_property(TARGET Jolt PROPERTY XCODE_ATTRIBUTE_OTHER_CPLUSPLUSFLAGS[arch=x86_64]
    "$(inherited) -msse2")
endif()
if(MSVC)
  target_compile_options(Jolt PUBLIC /fp:precise)
else()
  target_compile_options(Jolt PUBLIC -ffp-contract=off)
endif()
target_link_libraries(${PROJECT_NAME} Jolt)

# script for launching in binary directory
if(WIN32)
    add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/scripts/Gothic2Notr.bat
        ${CMAKE_CURRENT_BINARY_DIR}/opengothic/Gothic2Notr.bat)
endif()

# in debug mode, enable sanitizers
if(${CMAKE_BUILD_TYPE} MATCHES "Debug" AND NOT ANDROID)
  add_compile_options(-fsanitize=address)
  add_compile_options(-fsanitize=leak)
  add_link_options   (-fsanitize=address)
  add_link_options   (-fsanitize=leak)
endif()

# the game
if(IOS)
  set_directory_properties("${CMAKE_SOURCE_DIR}" PROPERTIES
    PROPERTY XCODE_STARTUP_PROJECT "Gothic2Notr")
  set_target_properties(Gothic2Notr PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "opengothic.gothic2"
    MACOSX_BUNDLE_BUNDLE_VERSION 1
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "iPhone Developer")
endif()

target_sources(Gothic2Notr PRIVATE game/main.cpp)
if(WIN32)
  target_sources(Gothic2Notr PRIVATE icon.rc)
endif()
target_link_libraries(Gothic2Notr PRIVATE OpenGothic)

# external AI control bridge (fork-specific, see ai_bridge/)
add_subdirectory(ai_bridge)
target_link_libraries(Gothic2Notr PRIVATE AiBridge)

# Uses the shared engine/CPU simulation without constructing a window or graphics/audio device.
if(UNIX AND NOT IOS AND NOT ANDROID)
  add_executable(gothic-body-runner game/bodyrunner.cpp)
  target_link_libraries(gothic-body-runner PRIVATE OpenGothic AiBridge)
  # Pure production resolver/command gates: no assets, simulation or rendering device.
  add_executable(gothic-body-resolver-test tests/body_resolver_test.cpp)
  target_link_libraries(gothic-body-resolver-test PRIVATE OpenGothic AiBridge)
  # Real NPC/world fixture for growing and shrinking the live observation subscription.
  add_executable(gothic-body-watch-test tests/body_watch_test.cpp)
  target_link_libraries(gothic-body-watch-test PRIVATE OpenGothic AiBridge)
  target_include_directories(gothic-body-watch-test SYSTEM BEFORE PRIVATE ${PROJECT_SOURCE_DIR}/lib/rapidjson/include)
  target_compile_options(gothic-body-watch-test PRIVATE -Wall -Wconversion -Werror)
  # Real Resources/material loading with temporary DDS files; no game assets or window.
  add_executable(gothic-material-texture-test EXCLUDE_FROM_ALL tests/material_texture_test.cpp)
  target_link_libraries(gothic-material-texture-test PRIVATE OpenGothic AiBridge)
  target_include_directories(gothic-material-texture-test SYSTEM BEFORE PRIVATE ${PROJECT_SOURCE_DIR}/lib/rapidjson/include)
  target_compile_options(gothic-material-texture-test PRIVATE -Wall -Wconversion -Werror)
  # Indexed GLB replacement and fallback, generated assets only; no game or window.
  add_executable(gothic-mesh-gltf-test EXCLUDE_FROM_ALL tests/mesh_gltf_test.cpp)
  target_link_libraries(gothic-mesh-gltf-test PRIVATE OpenGothic AiBridge gothic-base-import)
  target_include_directories(gothic-mesh-gltf-test SYSTEM BEFORE PRIVATE ${PROJECT_SOURCE_DIR}/lib/rapidjson/include)
  target_compile_definitions(gothic-mesh-gltf-test PRIVATE
    GOTHIC_TEST_BARREL_SCRIPT="${PROJECT_SOURCE_DIR}/../scripts/meshes/make_test_barrel.py")
  target_compile_options(gothic-mesh-gltf-test PRIVATE -Wall -Wconversion -Werror)
endif()

# Asset-free contracts; run explicitly with their check-* targets.
if(NOT ANDROID AND NOT IOS)
  add_executable(gothic-navpolicy-contract EXCLUDE_FROM_ALL
    tests/navpolicy_contract.cpp common/navpolicy.cpp common/game/navigationmemory.cpp)
  target_include_directories(gothic-navpolicy-contract PRIVATE
    common lib/Tempest/Engine/include lib/Tempest/Engine
    lib/Tempest/Engine/thirdparty/zlib ${CMAKE_BINARY_DIR}/lib/Tempest/thirdparty/zlib)
  target_link_libraries(gothic-navpolicy-contract PRIVATE zlibstatic)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  add_executable(gothic-bodypolicy-contract EXCLUDE_FROM_ALL
    tests/bodypolicy_contract.cpp common/game/bodypolicy.cpp common/nets/smallnet.cpp)
  target_include_directories(gothic-bodypolicy-contract PRIVATE
    common lib/Tempest/Engine/thirdparty/zlib ${CMAKE_BINARY_DIR}/lib/Tempest/thirdparty/zlib)
  target_include_directories(gothic-bodypolicy-contract SYSTEM BEFORE PRIVATE lib/rapidjson/include)
  target_link_libraries(gothic-bodypolicy-contract PRIVATE zlibstatic)
  find_program(BODY_POLICY_UV_EXECUTABLE NAMES uv HINTS "$ENV{HOME}/.local/bin")
  if(BODY_POLICY_UV_EXECUTABLE)
    set(BODY_POLICY_PYTHON ${BODY_POLICY_UV_EXECUTABLE} run --no-project --with numpy python)
  elseif(Python3_Interpreter_FOUND)
    set(BODY_POLICY_PYTHON ${Python3_EXECUTABLE})
  endif()
  if(BODY_POLICY_PYTHON)
    add_custom_command(OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/body-engine-fixture/fixture.bodynet
        ${CMAKE_CURRENT_BINARY_DIR}/body-engine-fixture/golden.jsonl.gz
      COMMAND ${CMAKE_COMMAND} -E env
        OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 VECLIB_MAXIMUM_THREADS=1
        ${BODY_POLICY_PYTHON} -I ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/body_net.py
        fixture ${CMAKE_CURRENT_BINARY_DIR}/body-engine-fixture
      DEPENDS ../scripts/body_net.py ../scripts/body_net_numpy.py
        ../scripts/body_policy_net_check.py ../scripts/body_trajectory_dataset.py
      VERBATIM)
    add_custom_target(gothic-body-engine-fixture DEPENDS
      ${CMAKE_CURRENT_BINARY_DIR}/body-engine-fixture/fixture.bodynet
      ${CMAKE_CURRENT_BINARY_DIR}/body-engine-fixture/golden.jsonl.gz)
    if(UNIX)
    add_executable(gothic-body-engine-test EXCLUDE_FROM_ALL tests/body_engine_test.cpp)
    target_link_libraries(gothic-body-engine-test PRIVATE OpenGothic AiBridge zlibstatic)
    target_include_directories(gothic-body-engine-test SYSTEM BEFORE PRIVATE lib/rapidjson/include)
    target_include_directories(gothic-body-engine-test PRIVATE lib/Tempest/Engine/thirdparty/zlib
      ${CMAKE_BINARY_DIR}/lib/Tempest/thirdparty/zlib)
    target_compile_options(gothic-body-engine-test PRIVATE -Wall -Wconversion -Werror)
    target_compile_definitions(gothic-body-engine-test PRIVATE
      GOTHIC_BODY_ENGINE_NET="${CMAKE_CURRENT_BINARY_DIR}/body-engine-fixture/fixture.bodynet"
      GOTHIC_BODY_ENGINE_GOLDEN="${CMAKE_CURRENT_BINARY_DIR}/body-engine-fixture/golden.jsonl.gz")
    add_dependencies(gothic-body-engine-test gothic-body-engine-fixture)
    endif()
    if(TARGET gothic-body-watch-test)
      target_compile_definitions(gothic-body-watch-test PRIVATE
        GOTHIC_BODY_ENGINE_NET="${CMAKE_CURRENT_BINARY_DIR}/body-engine-fixture/fixture.bodynet")
      add_dependencies(gothic-body-watch-test gothic-body-engine-fixture)
    endif()
    add_custom_target(check-bodypolicy
      COMMAND ${CMAKE_COMMAND} -E env
        OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 VECLIB_MAXIMUM_THREADS=1
        ${BODY_POLICY_PYTHON} -I ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/body_net.py
        fixture ${CMAKE_CURRENT_BINARY_DIR}/bodypolicy-fixture
      COMMAND $<TARGET_FILE:gothic-bodypolicy-contract>
        ${CMAKE_CURRENT_BINARY_DIR}/bodypolicy-fixture/fixture.bodynet
        ${CMAKE_CURRENT_BINARY_DIR}/bodypolicy-fixture/golden.jsonl.gz
      DEPENDS gothic-bodypolicy-contract USES_TERMINAL)
  endif()
  if(Python3_Interpreter_FOUND)
    add_custom_target(check-navpolicy
      COMMAND ${CMAKE_COMMAND} -E env
        OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 VECLIB_MAXIMUM_THREADS=1
        ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/check_nav_policy.py
        --output ${CMAKE_CURRENT_BINARY_DIR}/navpolicy-fixture --steps 1000
      COMMAND $<TARGET_FILE:gothic-navpolicy-contract>
        ${CMAKE_CURRENT_BINARY_DIR}/navpolicy-fixture/fixture.navbc
        ${CMAKE_CURRENT_BINARY_DIR}/navpolicy-fixture/parity.bin.gz
      DEPENDS gothic-navpolicy-contract USES_TERMINAL)
  endif()
  add_executable(gothic-navkeyteacher-contract EXCLUDE_FROM_ALL
    tests/navkeyteacher_contract.cpp common/navkeyteacher.cpp)
  target_include_directories(gothic-navkeyteacher-contract PRIVATE
    common lib/Tempest/Engine/include lib/Tempest/Engine)
  add_custom_target(check-navkeyteacher
    COMMAND $<TARGET_FILE:gothic-navkeyteacher-contract>
    DEPENDS gothic-navkeyteacher-contract USES_TERMINAL)
  add_executable(gothic-navmemoryteacher-contract EXCLUDE_FROM_ALL
    tests/navmemoryteacher_contract.cpp common/navmemoryteacher.cpp)
  target_include_directories(gothic-navmemoryteacher-contract PRIVATE
    common lib/Tempest/Engine/include lib/Tempest/Engine
    lib/Tempest/Engine/thirdparty/zlib ${CMAKE_BINARY_DIR}/lib/Tempest/thirdparty/zlib)
  target_link_libraries(gothic-navmemoryteacher-contract PRIVATE zlibstatic)
  if(NOT MSVC)
    target_compile_options(gothic-navmemoryteacher-contract PRIVATE -Wall -Wconversion -Werror)
  endif()
  add_custom_target(check-navmemoryteacher
    COMMAND $<TARGET_FILE:gothic-navmemoryteacher-contract> --self-test
    DEPENDS gothic-navmemoryteacher-contract USES_TERMINAL)
  add_executable(gothic-navmemoryteacher-diagnostics EXCLUDE_FROM_ALL
    tests/navmemoryteacher_diagnostics.cpp common/navmemoryteacher.cpp)
  target_include_directories(gothic-navmemoryteacher-diagnostics PRIVATE
    common lib/Tempest/Engine/include lib/Tempest/Engine
    lib/Tempest/Engine/thirdparty/zlib ${CMAKE_BINARY_DIR}/lib/Tempest/thirdparty/zlib)
  target_link_libraries(gothic-navmemoryteacher-diagnostics PRIVATE zlibstatic)
  if(NOT MSVC)
    target_compile_options(gothic-navmemoryteacher-diagnostics PRIVATE -Wall -Wconversion -Werror)
  endif()
  add_custom_target(check-navmemoryteacher-diagnostics
    COMMAND $<TARGET_FILE:gothic-navmemoryteacher-diagnostics> --self-test
    DEPENDS gothic-navmemoryteacher-diagnostics USES_TERMINAL)
  add_executable(gothic-navmesh-contract EXCLUDE_FROM_ALL
    tests/navmesh_contract.cpp common/physics/navmesh.cpp)
  target_link_libraries(gothic-navmesh-contract PRIVATE Recast Detour)
  if(NOT MSVC)
    target_compile_options(gothic-navmesh-contract PRIVATE -Wall -Wconversion -Werror)
  endif()
  add_custom_target(check-navmesh
    COMMAND $<TARGET_FILE:gothic-navmesh-contract>
    DEPENDS gothic-navmesh-contract USES_TERMINAL)
  add_executable(gothic-navoracle-contract EXCLUDE_FROM_ALL
    tests/navoracle_contract.cpp common/physics/navmesh.cpp)
  target_link_libraries(gothic-navoracle-contract PRIVATE Recast Detour)
  if(NOT MSVC)
    target_compile_options(gothic-navoracle-contract PRIVATE -Wall -Wconversion -Werror)
  endif()
  add_custom_target(check-navoracle
    COMMAND $<TARGET_FILE:gothic-navoracle-contract>
    DEPENDS gothic-navoracle-contract USES_TERMINAL)
  add_executable(gothic-aim-contract EXCLUDE_FROM_ALL tests/aim_contract.cpp)
  target_link_libraries(gothic-aim-contract PRIVATE OpenGothic AiBridge gothic-base-import)
  add_custom_target(check-aim
    COMMAND $<TARGET_FILE:gothic-aim-contract>
    DEPENDS gothic-aim-contract USES_TERMINAL)
  add_executable(gothic-aimwalk-contract EXCLUDE_FROM_ALL tests/aimwalk_contract.cpp)
  target_link_libraries(gothic-aimwalk-contract PRIVATE OpenGothic AiBridge gothic-base-import)
  add_custom_target(check-aimwalk
    COMMAND $<TARGET_FILE:gothic-aimwalk-contract>
    DEPENDS gothic-aimwalk-contract USES_TERMINAL)
  add_executable(gothic-physics-contract EXCLUDE_FROM_ALL tests/physics_contract.cpp)
  target_link_libraries(gothic-physics-contract PRIVATE OpenGothic AiBridge gothic-base-import)
  add_custom_target(check-physics
    COMMAND $<TARGET_FILE:gothic-physics-contract>
    DEPENDS gothic-physics-contract
    USES_TERMINAL)
  add_executable(gothic-cloth-contract EXCLUDE_FROM_ALL tests/cloth_contract.cpp)
  target_link_libraries(gothic-cloth-contract PRIVATE OpenGothic AiBridge gothic-base-import)
  add_executable(gothic-clothgeometry-contract EXCLUDE_FROM_ALL
    tests/clothgeometry_contract.cpp common/physics/clothgeometry.cpp)
  target_include_directories(gothic-clothgeometry-contract PRIVATE common)
  add_executable(gothic-worldcloth-contract EXCLUDE_FROM_ALL tests/worldcloth_contract.cpp)
  target_link_libraries(gothic-worldcloth-contract PRIVATE OpenGothic AiBridge)
  add_executable(gothic-worldbase-contract EXCLUDE_FROM_ALL tests/worldbase_contract.cpp)
  target_link_libraries(gothic-worldbase-contract PRIVATE OpenGothic AiBridge gothic-base-import gothic-video-import)
  add_executable(gothic-asset-digest EXCLUDE_FROM_ALL tests/asset_digest.cpp)
  target_link_libraries(gothic-asset-digest PRIVATE OpenGothic AiBridge gothic-base-import gothic-video-import)
  target_include_directories(gothic-asset-digest SYSTEM BEFORE PRIVATE ${PROJECT_SOURCE_DIR}/lib/rapidjson/include)
  add_executable(gothic-worlddata-contract EXCLUDE_FROM_ALL tests/worlddata_contract.cpp)
  target_link_libraries(gothic-worlddata-contract PRIVATE OpenGothic AiBridge gothic-base-import gothic-video-import)
  target_include_directories(gothic-worlddata-contract SYSTEM BEFORE PRIVATE ${PROJECT_SOURCE_DIR}/lib/rapidjson/include)
  add_executable(gothic-wayheights-contract EXCLUDE_FROM_ALL tests/wayheights_contract.cpp)
  target_link_libraries(gothic-wayheights-contract PRIVATE OpenGothic AiBridge gothic-base-import gothic-video-import)
  add_executable(gothic-vob-layer-test EXCLUDE_FROM_ALL tests/vob_layer_test.cpp)
  target_link_libraries(gothic-vob-layer-test PRIVATE OpenGothic AiBridge gothic-base-import gothic-video-import)
  target_include_directories(gothic-vob-layer-test SYSTEM BEFORE PRIVATE ${PROJECT_SOURCE_DIR}/lib/rapidjson/include)
  target_compile_definitions(gothic-vob-layer-test PRIVATE
    GOTHIC_VOB_FIXTURE_SCRIPT="${PROJECT_SOURCE_DIR}/../scripts/vob_layer_fixture.py"
    GOTHIC_CONTENT_CONVERT="$<TARGET_FILE:content-convert>"
    GOTHIC_BASE_FORMAT_SHA256="${BASE_FORMAT_SHA256}")
  add_dependencies(gothic-vob-layer-test content-convert)
  if(NOT MSVC)
    target_compile_options(gothic-vob-layer-test PRIVATE -Wall -Wconversion -Werror)
  endif()
  add_executable(gothic-basecontent-contract EXCLUDE_FROM_ALL tests/basecontent_contract.cpp)
  target_link_libraries(gothic-basecontent-contract PRIVATE OpenGothic AiBridge gothic-base-import gothic-video-import)
  target_include_directories(gothic-basecontent-contract SYSTEM BEFORE PRIVATE ${PROJECT_SOURCE_DIR}/lib/rapidjson/include)
  if(UNIX)
    add_executable(gothic-video-contract EXCLUDE_FROM_ALL tests/video_contract.cpp)
    target_link_libraries(gothic-video-contract PRIVATE OpenGothic AiBridge gothic-base-import gothic-video-import)
    target_compile_options(gothic-video-contract PRIVATE -Wall -Wconversion -Werror)
    add_executable(gothic-base-store-contract EXCLUDE_FROM_ALL tests/basestore_contract.cpp)
    target_link_libraries(gothic-base-store-contract PRIVATE OpenGothic AiBridge gothic-base-import gothic-video-import)
    target_include_directories(gothic-base-store-contract SYSTEM BEFORE PRIVATE ${PROJECT_SOURCE_DIR}/lib/rapidjson/include)
    target_compile_options(gothic-base-store-contract PRIVATE -Wall -Wconversion -Werror)
  endif()
  if(NOT MSVC)
    target_compile_options(gothic-worlddata-contract PRIVATE -Wall -Wconversion -Werror)
    target_compile_options(gothic-wayheights-contract PRIVATE -Wall -Wconversion -Werror)
    target_compile_options(gothic-basecontent-contract PRIVATE -Wall -Wconversion -Werror)
    target_compile_options(gothic-worldbase-contract PRIVATE -Wall -Wconversion -Werror)
  endif()
  add_custom_target(check-cloth
    COMMAND $<TARGET_FILE:gothic-cloth-contract>
    COMMAND $<TARGET_FILE:gothic-clothgeometry-contract>
    COMMAND $<TARGET_FILE:gothic-worldcloth-contract>
    DEPENDS gothic-cloth-contract gothic-clothgeometry-contract gothic-worldcloth-contract USES_TERMINAL)
  add_executable(gothic-ragdoll-contract EXCLUDE_FROM_ALL tests/physics_ragdoll_contract.cpp)
  target_link_libraries(gothic-ragdoll-contract PRIVATE OpenGothic AiBridge gothic-base-import)
  add_executable(gothic-ragdoll-save-contract EXCLUDE_FROM_ALL tests/ragdoll_save_contract.cpp)
  target_link_libraries(gothic-ragdoll-save-contract PRIVATE OpenGothic AiBridge)
  add_custom_target(check-ragdoll-save
    COMMAND $<TARGET_FILE:gothic-ragdoll-save-contract>
    DEPENDS gothic-ragdoll-save-contract USES_TERMINAL)
  add_custom_target(check-ragdoll
    COMMAND $<TARGET_FILE:gothic-ragdoll-contract>
    COMMAND $<TARGET_FILE:gothic-ragdoll-save-contract>
    DEPENDS gothic-ragdoll-contract gothic-ragdoll-save-contract USES_TERMINAL)
  add_executable(gothic-instance-storage-contract EXCLUDE_FROM_ALL tests/instance_storage_contract.cpp)
  target_link_libraries(gothic-instance-storage-contract PRIVATE OpenGothic AiBridge)
  add_custom_target(check-instance-storage
    COMMAND $<TARGET_FILE:gothic-instance-storage-contract>
    DEPENDS gothic-instance-storage-contract USES_TERMINAL)
endif()

# editor

if(NOT ANDROID AND NOT IOS)
  add_executable(gothic-npc-deeds-contract EXCLUDE_FROM_ALL tests/npc_deeds_contract.cpp)
  target_include_directories(gothic-npc-deeds-contract PRIVATE common)
  add_custom_target(check-npc-deeds
    COMMAND $<TARGET_FILE:gothic-npc-deeds-contract>
    DEPENDS gothic-npc-deeds-contract USES_TERMINAL)
endif()

# Navigation eye has an independent contract block to keep parallel navigation work isolated.
if(NOT ANDROID AND NOT IOS)
  add_executable(gothic-navigation-eye-contract EXCLUDE_FROM_ALL tests/navigation_eye_contract.cpp)
  target_link_libraries(gothic-navigation-eye-contract PRIVATE OpenGothic AiBridge gothic-base-import)
  add_custom_target(check-navigation-eye
    COMMAND $<TARGET_FILE:gothic-navigation-eye-contract>
    DEPENDS gothic-navigation-eye-contract USES_TERMINAL)
  add_executable(gothic-navigation-memory-contract EXCLUDE_FROM_ALL tests/navigation_memory_contract.cpp)
  target_link_libraries(gothic-navigation-memory-contract PRIVATE OpenGothic AiBridge)
  add_custom_target(check-navigation-memory
    COMMAND $<TARGET_FILE:gothic-navigation-memory-contract>
    DEPENDS gothic-navigation-memory-contract USES_TERMINAL)
  add_executable(gothic-navigation-roads-contract EXCLUDE_FROM_ALL
    tests/navigation_roads_contract.cpp common/game/knownroads.cpp)
  target_include_directories(gothic-navigation-roads-contract PRIVATE
    common lib/Tempest/Engine/include lib/Tempest/Engine)
  add_custom_target(check-navigation-roads
    COMMAND $<TARGET_FILE:gothic-navigation-roads-contract>
    DEPENDS gothic-navigation-roads-contract USES_TERMINAL)
endif()

target_sources(Spacer PRIVATE ${SPACER_SOURCES} spacer/main.cpp)
target_include_directories(Spacer PRIVATE "spacer")
target_link_libraries(Spacer PRIVATE OpenGothic SpacerAssets gothic-base-import gothic-video-import)

if(ANDROID)
  add_android_apk(OpenGothic-apk
    PACKAGE_NAME org.opengothic.app
    CODE Gothic2Notr
    MANIFEST android/AndroidManifest.xml)
endif()

# installation
install(TARGETS Gothic2Notr Spacer DESTINATION bin)
