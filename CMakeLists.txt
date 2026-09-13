cmake_minimum_required(VERSION 3.20)
project(LPTA LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Point -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm if auto-detection picks the
# wrong installed version (this machine has 14/15/17/18/20 side by side).
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} in ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH ${LLVM_CMAKE_DIR})
include(AddLLVM)

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})

add_llvm_pass_plugin(LPTAInstrumentation
  src/LPTAInstrumentation.cpp
)
