cmake_minimum_required(VERSION 3.21)
project(LPTA LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Default to the stable LLVM 23 series. An explicit LLVM_DIR also supports
# the LLVM 18 baseline; never silently auto-select an older installation.
if(DEFINED LLVM_DIR)
  find_package(LLVM REQUIRED CONFIG)
else()
  find_package(LLVM 23 REQUIRED CONFIG)
endif()
if(LLVM_VERSION_MAJOR LESS 18)
  message(FATAL_ERROR "LPTA requires LLVM 18 or newer")
endif()
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} in ${LLVM_DIR}")

foreach(tool clang opt)
  # NO_CACHE prevents a reconfigure from retaining another LLVM's tools.
  find_program(LPTA_${tool} NAMES ${tool} HINTS "${LLVM_TOOLS_BINARY_DIR}"
    NO_DEFAULT_PATH NO_CACHE REQUIRED)
  execute_process(COMMAND "${LPTA_${tool}}" --version
    OUTPUT_VARIABLE tool_version ERROR_VARIABLE tool_error RESULT_VARIABLE tool_status)
  string(REGEX MATCH "version ([0-9]+\\.[0-9]+\\.[0-9]+)" tool_match "${tool_version}")
  if(NOT tool_status EQUAL 0 OR NOT CMAKE_MATCH_1 VERSION_EQUAL LLVM_PACKAGE_VERSION)
    message(FATAL_ERROR "${LPTA_${tool}} does not match LLVM ${LLVM_PACKAGE_VERSION}: ${tool_version}${tool_error}")
  endif()
endforeach()

list(APPEND CMAKE_MODULE_PATH ${LLVM_CMAKE_DIR})
include(AddLLVM)

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})

add_llvm_pass_plugin(LPTAInstrumentation
  src/LPTAInstrumentation.cpp
)

# Data for the demo and smoke test, in version/clang/opt/plugin order.
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/lpta-tools.txt" CONTENT
  "${LLVM_PACKAGE_VERSION}\n${LPTA_clang}\n${LPTA_opt}\n$<TARGET_FILE:LPTAInstrumentation>\n")

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  add_test(NAME lpta-smoke COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test/check_smoke.py"
    --build-dir "${CMAKE_BINARY_DIR}" --source "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
