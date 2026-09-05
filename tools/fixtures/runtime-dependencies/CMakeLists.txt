cmake_minimum_required(VERSION 3.21)
# This is a configure-only Windows link-graph fixture, not a cross-compile or runtime claim.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_BUILD_TYPE Debug)
project(runtime_dependencies_fixture NONE)
set(CMAKE_CXX_LINK_EXECUTABLE "unused")
enable_testing()
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/SunbrightRuntimeDependencies.cmake")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dependency" "${CMAKE_CURRENT_BINARY_DIR}/bin")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/dependency/SDL3.dll" "synthetic SDL runtime")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/SDL3.lib" "synthetic import library")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/fixture.cpp" "int main() { return 0; }\n")
add_library(SDL3::SDL3 SHARED IMPORTED)
set_target_properties(SDL3::SDL3 PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/dependency/SDL3.dll"
    IMPORTED_IMPLIB "${CMAKE_CURRENT_BINARY_DIR}/SDL3.lib")
add_executable(fixture "${CMAKE_CURRENT_BINARY_DIR}/fixture.cpp")
set_target_properties(fixture PROPERTIES LINKER_LANGUAGE CXX
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
target_link_libraries(fixture PRIVATE SDL3::SDL3)
sunbright_deploy_runtime_dependencies(fixture SDL3::SDL3)
