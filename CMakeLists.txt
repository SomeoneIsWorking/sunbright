cmake_minimum_required(VERSION 3.21)
project(sunbright_native_components CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(cmake/SunbrightBuildPolicy.cmake)
include(cmake/SunbrightRuntimeDependencies.cmake)
sunbright_enable_fast_debug_builds()

find_package(SDL3 3 CONFIG REQUIRED)
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

# Sunbright currently has no gameplay executable. The retired executor was removed before gcnport
# exists. Keep the independently reusable,
# renderer-neutral library buildable while the shared Dolphin-JIT executor is built.
add_subdirectory(native-render)
add_subdirectory(title-adapter)

# tools/gcnport_boot is a standalone maintainer diagnostic against the pinned extern/gcnport
# submodule (see docs/issues/0037...); it is not the gameplay product target and is excluded from
# the default `all` target (see cmake/GcnPortDependency.cmake). Only present once the operator has
# initialized the submodule -- absence must not fail an ordinary configure that never asked for it.
if(EXISTS "${CMAKE_SOURCE_DIR}/extern/gcnport/CMakeLists.txt")
  add_subdirectory(tools/gcnport_boot)
else()
  message(STATUS
    "Sunbright gcnport boot tool skipped: extern/gcnport submodule not initialized "
    "(run 'git submodule update --init --recursive extern/gcnport')")
endif()

message(STATUS
  "Sunbright gameplay unavailable: gcnport's public boot/execute API exists (see "
  "docs/issues/0037-migrate-sunbright-execution-to-gcnport-dolphin-d.md) but no gameplay executor "
  "target consumes it yet")
