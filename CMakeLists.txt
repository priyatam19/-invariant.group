cmake_minimum_required(VERSION 3.21)
project(LPTA LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Some LLVM-dev packagings (observed: Ubuntu's llvm-18-dev) export a cmake
# dependency on an imported `zstd::libzstd_shared` target without shipping a
# zstd cmake config to actually define it -- find_package(LLVM) below then
# fails with "target was not found" even though the system has a perfectly
# usable libzstd.so. Official LLVM release tarballs (e.g. the 23.1.1 one
# tools/install_llvm23.sh fetches) don't hit this; only define the target
# ourselves, and only if it's genuinely missing, so this is a no-op there.
if(NOT TARGET zstd::libzstd_shared)
  find_library(LPTA_SYSTEM_ZSTD NAMES zstd)
  if(NOT LPTA_SYSTEM_ZSTD)
    # No -dev package, so no unversioned libzstd.so symlink either -- fall
    # back to the versioned runtime lib (present via the plain "zstd"/
    # "libzstd1" runtime package).
    file(GLOB LPTA_ZSTD_VERSIONED
      /usr/lib/*/libzstd.so.1 /usr/lib/libzstd.so.1 /usr/lib64/libzstd.so.1)
    if(LPTA_ZSTD_VERSIONED)
      list(GET LPTA_ZSTD_VERSIONED 0 LPTA_SYSTEM_ZSTD)
    endif()
  endif()
  if(LPTA_SYSTEM_ZSTD)
    add_library(zstd::libzstd_shared SHARED IMPORTED)
    set_target_properties(zstd::libzstd_shared PROPERTIES
      IMPORTED_LOCATION "${LPTA_SYSTEM_ZSTD}")
  endif()
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

# Tier 1 experiment (RESEARCH.md §8): standalone tool, not an opt plugin --
# needs to construct/mutate IR and DominatorTree directly, which a pass
# plugin's extension points don't allow. llvm_map_components_to_libnames is
# LLVM's own portable API for this: it resolves to the combined dylib when
# an installation provides one (e.g. Ubuntu's llvm-18-dev) and to the
# modular per-component libs otherwise (e.g. the official release tarball
# fetched by tools/install_llvm23.sh, which ships no combined libLLVM.so at
# all) -- linking a hardcoded `-lLLVM` here previously broke the latter.
llvm_map_components_to_libnames(LPTA_BENCH_LIBS core support)
add_executable(lpta_dt_microbench bench/dt_microbench.cpp)
target_link_libraries(lpta_dt_microbench PRIVATE ${LPTA_BENCH_LIBS})

# Data for the demo and smoke test, in version/clang/opt/plugin order.
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/lpta-tools.txt" CONTENT
  "${LLVM_PACKAGE_VERSION}\n${LPTA_clang}\n${LPTA_opt}\n$<TARGET_FILE:LPTAInstrumentation>\n")

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  add_test(NAME lpta-smoke COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test/check_smoke.py"
    --build-dir "${CMAKE_BINARY_DIR}" --source "${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME lpta-liveness COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test/check_liveness.py"
    --build-dir "${CMAKE_BINARY_DIR}" --source "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
