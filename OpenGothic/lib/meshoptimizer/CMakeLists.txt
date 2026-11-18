# Pinned upstream v1.3, with the library sources only.
file(GLOB MESHOPTIMIZER_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(meshoptimizer STATIC ${MESHOPTIMIZER_SOURCES})
target_include_directories(meshoptimizer SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
