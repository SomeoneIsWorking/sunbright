# sms-recomp — the standalone recomp runtime (two-runtime doctrine, CLAUDE.md).
# Builds the generated PPC C++ against rt_core.cpp. No Dolphin, no decomp interop.
cmake_minimum_required(VERSION 3.20)
project(sms_recomp CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# lucent is THE C++20 logger for this project (global directive) — never hand-roll another.
include(FetchContent)
FetchContent_Declare(lucent
  GIT_REPOSITORY https://github.com/SomeoneIsWorking/lucent.git
  GIT_TAG main)
FetchContent_MakeAvailable(lucent)

set(GEN "${CMAKE_CURRENT_SOURCE_DIR}/generated")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${GEN}/functions_*.cpp" "${GEN}/jump_table.cpp")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "No generated sources. Run: sunbright-recomp scratch/bin/sms.dol --output sms-recomp/generated")
endif()

add_library(sms-recomp-guest STATIC ${GEN_SOURCES})
target_include_directories(sms-recomp-guest PUBLIC "${GEN}" "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
# Generated code is machine-emitted: its warnings are noise, not signal.
target_compile_options(sms-recomp-guest PRIVATE -w)

add_library(sms-recomp-rt STATIC runtime/rt_core.cpp)
target_include_directories(sms-recomp-rt PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/runtime" "${GEN}")
target_link_libraries(sms-recomp-rt PUBLIC lucent::lucent)

# Host harness: loads the DOL into guest RAM and runs the entry point.
add_executable(sms-recomp host/main.cpp)
target_link_libraries(sms-recomp PRIVATE sms-recomp-rt sms-recomp-guest lucent::lucent)
