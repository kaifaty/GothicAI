# Copyright © 2021-2024 GothicKit Contributors.
# SPDX-License-Identifier: MIT
# Copy of ZenKit 1.3.0 with project modifications.
# MIT license: OpenGothic/lib/ZenKit/license.md (../../lib/ZenKit/license.md).

add_library(gothic-daedalus STATIC vm.cpp script.cpp classes.cpp)
target_include_directories(gothic-daedalus PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
# Only from_program needs ZenKit's program record during P21a.
target_link_libraries(gothic-daedalus PRIVATE zenkit)
if(TARGET Tempest)
  target_link_libraries(gothic-daedalus PRIVATE Tempest)
else()
  target_link_libraries(gothic-daedalus PRIVATE gothic-native-io)
endif()
