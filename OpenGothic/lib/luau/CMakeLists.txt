# Pinned upstream Luau 0.748 (3d91c9d5c280c95bfc4be63aac46012cf861d394).
# Only Common, Ast, Bytecode, Compiler and VM are vendored.
cmake_minimum_required(VERSION 3.15)

add_library(Luau.Common STATIC
  Common/src/BytecodeWire.cpp
  Common/src/StringUtils.cpp
  Common/src/TimeTrace.cpp
)
target_compile_features(Luau.Common PUBLIC cxx_std_17)
target_include_directories(Luau.Common SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Common/include)

add_library(Luau.Ast STATIC
  Ast/src/Allocator.cpp
  Ast/src/Ast.cpp
  Ast/src/Confusables.cpp
  Ast/src/Cst.cpp
  Ast/src/Lexer.cpp
  Ast/src/Location.cpp
  Ast/src/Parser.cpp
  Ast/src/PrettyPrinter.cpp
)
target_compile_features(Luau.Ast PUBLIC cxx_std_17)
target_include_directories(Luau.Ast SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Ast/include)
target_link_libraries(Luau.Ast PUBLIC Luau.Common)

add_library(Luau.Bytecode STATIC
  Bytecode/src/BytecodeBuilder.cpp
  Bytecode/src/BytecodeDump.cpp
  Bytecode/src/BytecodeGraph.cpp
  Bytecode/src/Sccp.cpp
)
target_compile_features(Luau.Bytecode PUBLIC cxx_std_17)
target_include_directories(Luau.Bytecode SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Bytecode/include)
target_link_libraries(Luau.Bytecode PUBLIC Luau.Common)

add_library(Luau.Compiler STATIC
  Compiler/src/Builtins.cpp
  Compiler/src/BuiltinFolding.cpp
  Compiler/src/Compiler.cpp
  Compiler/src/ConstantFolding.cpp
  Compiler/src/CostModel.cpp
  Compiler/src/TableShape.cpp
  Compiler/src/Types.cpp
  Compiler/src/ValueTracking.cpp
  Compiler/src/lcode.cpp
)
target_compile_features(Luau.Compiler PUBLIC cxx_std_17)
target_include_directories(Luau.Compiler SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Compiler/include)
target_link_libraries(Luau.Compiler PUBLIC Luau.Ast Luau.Bytecode)

add_library(Luau.VM STATIC
  VM/src/lapi.cpp
  VM/src/laux.cpp
  VM/src/lbaselib.cpp
  VM/src/lbitlib.cpp
  VM/src/lbuffer.cpp
  VM/src/lbuflib.cpp
  VM/src/lbuiltins.cpp
  VM/src/lclass.cpp
  VM/src/lclasslib.cpp
  VM/src/lcorolib.cpp
  VM/src/ldblib.cpp
  VM/src/ldebug.cpp
  VM/src/ldo.cpp
  VM/src/lfunc.cpp
  VM/src/lgc.cpp
  VM/src/lgcdebug.cpp
  VM/src/linit.cpp
  VM/src/lmathlib.cpp
  VM/src/lmem.cpp
  VM/src/lnumprint.cpp
  VM/src/lobject.cpp
  VM/src/loslib.cpp
  VM/src/lperf.cpp
  VM/src/lstate.cpp
  VM/src/lstring.cpp
  VM/src/lstrlib.cpp
  VM/src/ltable.cpp
  VM/src/ltablib.cpp
  VM/src/ltm.cpp
  VM/src/ludata.cpp
  VM/src/lutf8lib.cpp
  VM/src/lveclib.cpp
  VM/src/lvector.cpp
  VM/src/lvmexecute.cpp
  VM/src/lvmload.cpp
  VM/src/lvmutils.cpp
)
target_compile_features(Luau.VM PRIVATE cxx_std_17)
target_include_directories(Luau.VM SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/VM/include)
target_link_libraries(Luau.VM PUBLIC Luau.Common)

if(MSVC)
  foreach(_luau_t Luau.Common Luau.Ast Luau.Bytecode Luau.Compiler Luau.VM)
    target_compile_options(${_luau_t} PRIVATE /D_CRT_SECURE_NO_WARNINGS)
  endforeach()
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND MSVC_VERSION GREATER_EQUAL 1924)
    set_source_files_properties(VM/src/lvmexecute.cpp PROPERTIES COMPILE_FLAGS /d2ssa-pre-)
  endif()
else()
  foreach(_luau_t Luau.Common Luau.Ast Luau.Bytecode Luau.Compiler Luau.VM)
    target_compile_options(${_luau_t} PRIVATE -Wno-unused -Wno-sign-compare -Wno-conversion)
  endforeach()
  target_compile_options(Luau.VM PRIVATE -fno-math-errno)
endif()
