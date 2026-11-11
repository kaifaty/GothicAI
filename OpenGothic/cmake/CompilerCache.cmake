# Shares compiled objects between the Mac's build directories through ccache, when it is installed.
# Every worktree has its own build directory, and without a shared cache each one compiles the whole engine.
# Ryzen has its own setup in scripts/ryzen-build.sh, which passes the launcher explicitly (empty when it is off).
#
# base_dir (the checkout's root) makes paths relative, and hash_dir=false leaves the working directory out of the
# key: the same source then hits in any worktree. The price: debug info names the directory of whichever worktree
# compiled the object first. compiler_check asks the compiler itself, because /usr/bin/c++ on macOS is a shim whose
# file does not change when Xcode updates. PCH is off: ccache does not cache compilations that use it.
# -DOPENGOTHIC_CCACHE=OFF builds without the cache. Why and the numbers: reports/compute/2026-11-05-ccache/README.md.
option(OPENGOTHIC_CCACHE "Compile through ccache on macOS when it is installed" ON)
if(APPLE AND OPENGOTHIC_CCACHE AND NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
  find_program(OPENGOTHIC_CCACHE_PROGRAM ccache)
  if(OPENGOTHIC_CCACHE_PROGRAM)
    cmake_path(GET CMAKE_SOURCE_DIR PARENT_PATH OPENGOTHIC_CHECKOUT)
    set(launcher ${OPENGOTHIC_CCACHE_PROGRAM}
        base_dir=${OPENGOTHIC_CHECKOUT} hash_dir=false max_size=20G
        "compiler_check=%compiler% --version")
    foreach(language C CXX OBJC OBJCXX)
      set(CMAKE_${language}_COMPILER_LAUNCHER ${launcher})
    endforeach()
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
    message(STATUS "Compiler cache: ${OPENGOTHIC_CCACHE_PROGRAM}")
  endif()
endif()
