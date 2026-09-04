cmake_minimum_required(VERSION 3.20)
project(sunbright_native_components CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

include(cmake/SunbrightBuildPolicy.cmake)
sunbright_enable_fast_debug_builds()

find_package(SDL3 3 CONFIG REQUIRED)

# Sunbright currently has no gameplay executable. The retired executor was removed before gcnport
# exists. Keep the independently reusable,
# renderer-neutral library buildable while the shared Dolphin-JIT executor is built.
add_subdirectory(native-render)

message(STATUS
  "Sunbright gameplay unavailable: shared gcnport with an embedded Dolphin JIT is not implemented")
