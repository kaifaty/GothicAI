# Split only the existing game source list, after all its compile settings are known.
# EngineLayerGuard.cmake has already defined the shared LAYER_RULES reader/classifier.
if(NOT EXISTS "${guard_rules}")
  message(WARNING "Engine platform library and link check skipped: no layer rules at ${guard_rules}")
  return()
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${guard_rules}")
engine_layer_guard_read_rules("${guard_rules}" platform_patterns platform_layers platform_regexes)
set(platform_sources)
foreach(source IN LISTS OPENGOTHIC_SOURCES ObjCSOURCES)
  file(RELATIVE_PATH relative "${CMAKE_CURRENT_SOURCE_DIR}" "${source}")
  engine_layer_guard_layer_of("${relative}" "${platform_regexes}" "${platform_layers}" layer)
  if(layer STREQUAL "platform")
    list(APPEND platform_sources "${source}")
  endif()
endforeach()
get_target_property(game_sources OpenGothic SOURCES)
list(REMOVE_ITEM game_sources ${platform_sources})
set_property(TARGET OpenGothic PROPERTY SOURCES "${game_sources}")
add_library(engine-platform STATIC ${platform_sources})
target_sources(engine-platform PRIVATE "${PLACED_CENSUS_BUILDER_HEADER}")
foreach(property COMPILE_OPTIONS COMPILE_DEFINITIONS INCLUDE_DIRECTORIES)
  get_target_property(value OpenGothic ${property})
  if(value)
    set_property(TARGET engine-platform PROPERTY ${property} "${value}")
  endif()
endforeach()
# CMake exposes no target property for PRIVATE SYSTEM include directories.
target_include_directories(engine-platform SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lib/rapidjson/include")
# Explicit current link inputs: vendored libraries, generated platform shader data, OS
# libraries and the two mixed runtimes. SOURCE_DIR alone would reject miniz, which is
# created in the root directory. New dependencies require an intentional review here.
set(platform_allowed_dependencies
  GothicShaders edd_dbg dmusic Tempest miniz zlibstatic Recast Detour Jolt
  shlwapi DbgHelp -lpthread -ldl "-framework AppKit"
  gothic-base-runtime gothic-video-runtime)
get_target_property(platform_dependencies OpenGothic LINK_LIBRARIES)
# The VM belongs to OpenGothic only; it has never been an input of the platform link.
list(REMOVE_ITEM platform_dependencies gothic-daedalus)
foreach(dependency IN LISTS platform_dependencies)
  if(NOT dependency IN_LIST platform_allowed_dependencies)
    message(FATAL_ERROR "engine-platform: OpenGothic dependency '${dependency}' is not an allowed platform link input. "
      "Keep RPG, Gothic and bridge libraries on OpenGothic only. Review new vendored/system dependencies "
      "before adding them to platform_allowed_dependencies in cmake/EnginePlatform.cmake.")
  endif()
endforeach()
target_link_libraries(engine-platform PUBLIC ${platform_dependencies})
# OpenGothic uses the plain signature, whose interface is public by default.
target_link_libraries(OpenGothic engine-platform)

# Archive member names come from the actual runtime source lists, never a second layer list.
set(members)
foreach(runtime gothic-base-runtime gothic-video-runtime)
  get_target_property(sources ${runtime} SOURCES)
  get_target_property(source_dir ${runtime} SOURCE_DIR)
  foreach(source IN LISTS sources)
    if(NOT source MATCHES "\\.(cpp|c|mm)$")
      continue()
    endif()
    get_filename_component(absolute "${source}" ABSOLUTE BASE_DIR "${source_dir}")
    file(RELATIVE_PATH relative "${CMAKE_CURRENT_SOURCE_DIR}" "${absolute}")
    get_filename_component(name "${source}" NAME)
    list(APPEND members "${runtime}|${name}.o|${relative}")
  endforeach()
endforeach()
set(ENGINE_LINK_MAP_INPUTS "${CMAKE_CURRENT_BINARY_DIR}/engine-platform-map-inputs.cmake")
set(ENGINE_LINK_MAP_CHECK "${CMAKE_CURRENT_LIST_DIR}/EnginePlatformMap.cmake")
file(GENERATE OUTPUT "${ENGINE_LINK_MAP_INPUTS}" CONTENT
  "set(PLATFORM_RULES [==[${guard_rules}]==])\nset(PLATFORM_MEMBERS [==[${members}]==])\n")

# The link map of a program of the platform: an undefined symbol fails the link, and after it every object of the
# mixed runtimes (gothic-base-runtime, gothic-video-runtime) the linker took must be of `layers` (a ;-list), else the
# build fails. engine-platform-link takes the platform alone, engine-island (Island.cmake) the platform and the island.
function(engine_link_map_check target layers)
  set(map "${CMAKE_CURRENT_BINARY_DIR}/${target}-$<CONFIG>.map")
  if(MSVC)
    # /MAP lists public symbols, rather than an exhaustive archive-member load table. Native
    # validation is needed before relying on it for the mixed-runtime boundary.
    target_link_options(${target} PRIVATE "/MAP:${map}")
    message(WARNING "${target}: mixed-runtime map check disabled on MSVC (GothicAI#1324); the link itself remains")
    return()
  endif()
  if(APPLE)
    target_link_options(${target} PRIVATE "LINKER:-map,${map}" "LINKER:-undefined,error")
  else()
    target_link_options(${target} PRIVATE "LINKER:-Map,${map}" "LINKER:--no-undefined")
  endif()
  string(REPLACE ";" "," allowed "${layers}")
  string(REPLACE ";" " or " named "${layers}")
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
    "${ENGINE_LINK_MAP_CHECK}" "${ENGINE_LINK_MAP_INPUTS}" "${guard_rules}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EngineLayerGuard.cmake")
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" "-DPLATFORM_MAP=${map}" "-DPLATFORM_INPUTS=${ENGINE_LINK_MAP_INPUTS}"
      "-DPLATFORM_TARGET=${target}" "-DPLATFORM_LAYERS=${allowed}" -P "${ENGINE_LINK_MAP_CHECK}"
    COMMENT "Checking ${target}: mixed runtime objects must belong to ${named}"
    VERBATIM)
endfunction()

set(link_main "${CMAKE_CURRENT_BINARY_DIR}/engine-platform-link.cpp")
file(CONFIGURE OUTPUT "${link_main}" CONTENT "int main() {}\n")
add_executable(engine-platform-link "${link_main}")
if(MSVC)
  target_link_libraries(engine-platform-link PRIVATE engine-platform)
  target_link_options(engine-platform-link PRIVATE
    "/WHOLEARCHIVE:$<TARGET_FILE:engine-platform>" /OPT:NOREF /INCREMENTAL:NO)
elseif(APPLE)
  target_link_libraries(engine-platform-link PRIVATE engine-platform)
  target_link_options(engine-platform-link PRIVATE "LINKER:-force_load,$<TARGET_FILE:engine-platform>")
else()
  target_link_libraries(engine-platform-link PRIVATE
    "-Wl,--whole-archive" engine-platform "-Wl,--no-whole-archive")
endif()
engine_link_map_check(engine-platform-link platform)
foreach(target Gothic2Notr gothic-body-runner)
  if(TARGET ${target})
    add_dependencies(${target} engine-platform-link)
  endif()
endforeach()
