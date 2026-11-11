# Add new census/body-building code outside these files here. ProtoMesh's
# appendItemVertices is already covered by the source vertices in the input key.
set(PLACED_CENSUS_BUILDER_INPUTS
  common/physics/dynamicworld_items.cpp
  common/physics/dynamicworld.cpp
  common/physics/dynamicworld.h
  common/physics/dynamicworld_internal.h
  common/physics/collisionworld.cpp
  common/physics/collisionworld.h
  common/physics/collisionworld_internal.h
  common/physics/physics.h
  # Census ray queries refine native hits using the original support triangles.
  common/physics/physicvbo.cpp
  common/physics/physicvbo.h
  # Vendor changes are recorded here; Core.h defines JPH_VERSION_*.
  lib/VENDORED.md
  lib/JoltPhysics/Jolt/Core/Core.h
  cmake/PlacedCensusBuilder.cmake)

if(CMAKE_SCRIPT_MODE_FILE)
  set(sums "")
  foreach(input IN LISTS PLACED_CENSUS_BUILDER_INPUTS)
    file(SHA256 "${SOURCE}/${input}" sum)
    string(APPEND sums "${input} ${sum}\n")
  endforeach()
  string(SHA256 fingerprint "${sums}")
  set(text "#pragma once\n#define OPENGOTHIC_PLACED_CENSUS_BUILDER \"${fingerprint}\"\n")
  set(old "")
  if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" old)
  endif()
  if(NOT old STREQUAL text)
    get_filename_component(directory "${OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(WRITE "${OUTPUT}" "${text}")
  endif()
endif()
