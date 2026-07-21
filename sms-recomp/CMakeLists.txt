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

# Overrides are part of the runtime, not the guest: they REPLACE recompiled bodies, so
# they must not be compiled with the generated code's relaxed warning settings.
#
# OBJECT, not STATIC: each override self-registers from a static initializer and exports
# nothing else, so in a static archive the linker discards the whole object as unused and
# the override silently never installs. An object library always contributes its objects.
file(GLOB SB_OVERRIDES "${CMAKE_CURRENT_SOURCE_DIR}/overrides/*.cpp")
add_library(sms-recomp-overrides OBJECT ${SB_OVERRIDES})
target_include_directories(sms-recomp-overrides PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime" "${CMAKE_CURRENT_SOURCE_DIR}/overrides" "${GEN}")
target_link_libraries(sms-recomp-overrides PUBLIC lucent::lucent)

add_library(sms-recomp-rt STATIC runtime/rt_core.cpp runtime/mmio.cpp
                                 runtime/dev_aram.cpp)
target_include_directories(sms-recomp-rt PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/runtime" "${CMAKE_CURRENT_SOURCE_DIR}/overrides" "${GEN}")
target_link_libraries(sms-recomp-rt PUBLIC lucent::lucent)

# Host harness: loads the DOL into guest RAM and runs the entry point.
add_executable(sms-recomp host/main.cpp)
# -rdynamic puts func_<addr> into the dynamic symbol table so rt_dump_guest_stack can
# name the guest frames. Recompiled bodies nest on the host stack, so the host backtrace
# is the guest call stack — but only if the names resolve.
target_link_options(sms-recomp PRIVATE -rdynamic)
target_link_libraries(sms-recomp PRIVATE sms-recomp-rt sms-recomp-guest
                      sms-recomp-overrides lucent::lucent)
