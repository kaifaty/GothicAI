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
# The existing dependencies are third-party libraries and the two mixed runtimes, except for
# the VM. Copy their usage requirements as well as the target's own compile settings above.
get_target_property(platform_dependencies OpenGothic LINK_LIBRARIES)
list(REMOVE_ITEM platform_dependencies gothic-daedalus)
target_link_libraries(engine-platform PUBLIC ${platform_dependencies})
# OpenGothic uses the plain signature, whose interface is public by default.
target_link_libraries(OpenGothic engine-platform)

set(link_main "${CMAKE_CURRENT_BINARY_DIR}/engine-platform-link.cpp")
file(CONFIGURE OUTPUT "${link_main}" CONTENT "int main() {}\n")
add_executable(engine-platform-link "${link_main}")
set(link_map "${CMAKE_CURRENT_BINARY_DIR}/engine-platform-link-$<CONFIG>.map")
if(MSVC)
  target_link_libraries(engine-platform-link PRIVATE engine-platform)
  target_link_options(engine-platform-link PRIVATE
    "/WHOLEARCHIVE:$<TARGET_FILE:engine-platform>" "/MAP:${link_map}" /OPT:NOREF /INCREMENTAL:NO)
elseif(APPLE)
  target_link_libraries(engine-platform-link PRIVATE engine-platform)
  target_link_options(engine-platform-link PRIVATE
    "LINKER:-force_load,$<TARGET_FILE:engine-platform>" "LINKER:-map,${link_map}"
    "LINKER:-undefined,error")
else()
  target_link_libraries(engine-platform-link PRIVATE
    "-Wl,--whole-archive" engine-platform "-Wl,--no-whole-archive")
  target_link_options(engine-platform-link PRIVATE "LINKER:-Map,${link_map}" "LINKER:--no-undefined")
endif()

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
set(map_inputs "${CMAKE_CURRENT_BINARY_DIR}/engine-platform-map-inputs.cmake")
file(GENERATE OUTPUT "${map_inputs}" CONTENT
  "set(PLATFORM_RULES [==[${guard_rules}]==])\nset(PLATFORM_MEMBERS [==[${members}]==])\n")
if(MSVC)
  # /MAP lists public symbols, rather than an exhaustive archive-member load table. Native
  # validation is needed before relying on it for the mixed-runtime boundary.
  message(WARNING "engine-platform-link: mixed-runtime map check disabled on MSVC (GothicAI#1324); whole-archive link remains enabled")
else()
  set(map_check "${CMAKE_CURRENT_LIST_DIR}/EnginePlatformMap.cmake")
  set_property(TARGET engine-platform-link APPEND PROPERTY LINK_DEPENDS
    "${map_check}" "${map_inputs}" "${guard_rules}" "${CMAKE_CURRENT_LIST_DIR}/EngineLayerGuard.cmake")
  add_custom_command(TARGET engine-platform-link POST_BUILD
    COMMAND "${CMAKE_COMMAND}" "-DPLATFORM_MAP=${link_map}" "-DPLATFORM_INPUTS=${map_inputs}"
      -P "${map_check}"
    COMMENT "Checking engine-platform-link: mixed runtime objects must belong to platform"
    VERBATIM)
endif()
foreach(target Gothic2Notr gothic-body-runner)
  if(TARGET ${target})
    add_dependencies(${target} engine-platform-link)
  endif()
endforeach()
