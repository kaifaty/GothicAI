# World cloth (common/physics/worldcloth.cpp) and its cooked bundle (worldclothbundle.cpp) go with the cooked
# chunks into gothic-base-runtime: the cook (worldchunkcook.cpp) runs the island search, the game takes the bundle.
# Static collision's input from the cooked chunks (worldcollisionmesh.cpp) leaves the bundle's triangles out.
# The rules (data/cloth-world.csv) are compiled in as clothworlddata.h. Included by every build that has
# gothic-base-runtime through BaseRuntime.cmake, after BaseImport.cmake; outside it, since that file is a base
# format input.
set(WORLD_CLOTH_CSV "${BASE_ROOT}/data/cloth-world.csv")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WORLD_CLOTH_CSV}")
file(READ "${WORLD_CLOTH_CSV}" CLOTH_WORLD_DATA)
configure_file("${BASE_ROOT}/common/physics/clothworlddata.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/worldcloth-generated/clothworlddata.h" @ONLY)
target_include_directories(gothic-base-runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/worldcloth-generated")
target_sources(gothic-base-runtime PRIVATE "${BASE_ROOT}/common/physics/worldcloth.cpp"
  "${BASE_ROOT}/common/physics/worldclothbundle.cpp" "${BASE_ROOT}/common/physics/worldcollisionmesh.cpp")
# The island search keeps the game's floating point (it was compiled with Jolt's PUBLIC options): no contraction
# into fused multiply-adds, so the pins are the same on every machine that cooks or reads a bundle.
set_source_files_properties("${BASE_ROOT}/common/physics/worldcloth.cpp"
  PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/fp:precise,-ffp-contract=off>")
