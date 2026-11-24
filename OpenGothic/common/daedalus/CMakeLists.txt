# Copyright © 2021-2024 GothicKit Contributors.
# SPDX-License-Identifier: MIT
# Copy of ZenKit 1.3.0 with project modifications.
# MIT license: OpenGothic/lib/ZenKit/license.md (../../lib/ZenKit/license.md).

add_library(gothic-script-data STATIC script.cpp classes.cpp)
target_include_directories(gothic-script-data PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(TARGET Tempest)
  target_link_libraries(gothic-script-data PRIVATE Tempest)
else()
  target_link_libraries(gothic-script-data PRIVATE gothic-native-io)
endif()

add_library(gothic-script-state STATIC vm.cpp)
target_link_libraries(gothic-script-state PUBLIC gothic-script-data)

add_library(gothic-daedalus STATIC vm_bytecode.cpp)
target_link_libraries(gothic-daedalus PUBLIC gothic-script-state)
