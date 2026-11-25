cmake_minimum_required(VERSION 3.22)
project(society_sim LANGUAGES CXX)

find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)
find_package(nanobind CONFIG REQUIRED)
find_package(Threads REQUIRED)
add_subdirectory(../OpenGothic/common/society society-core)

nanobind_add_module(_native bindings.cpp)
target_link_libraries(_native PRIVATE society-core Threads::Threads)
if(NOT MSVC)
  target_compile_options(_native PRIVATE -Wall -Wconversion -Werror)
endif()
install(TARGETS _native LIBRARY DESTINATION society_sim)
