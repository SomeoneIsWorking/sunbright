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
target_link_libraries(sms-recomp-overrides PUBLIC nod::nod aurora::gx aurora::pad)
target_link_libraries(sms-recomp-overrides PUBLIC lucent::lucent)

# nod supplies random-access reads of the disc image (ISO/RVZ/...). The DI device serves
# the guest's own DVD library at the REGISTER level, so nothing marshals guest structs —
# the game's DVD code, FST parsing and file reads all run as recompiled PPC. That is why
# this links nod directly rather than aurora's SDK-level DVD implementation.
# Aurora provides the GX implementation. aurora_fifo_replay() consumes a raw GX command
# stream, which is exactly what the guest writes to the gather pipe, so no translation layer
# is needed for the command stream itself — only for pointers (see dev_gxfifo.cpp).
# DVD stays OFF: this runtime serves the disc itself through nod (runtime/disc.cpp).
set(AURORA_ENABLE_GX ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../extern/aurora" aurora-build EXCLUDE_FROM_ALL)

set(AURORA_NOD_PROVIDER "auto" CACHE STRING "")
set(AURORA_NOD_LINKAGE "static" CACHE STRING "")
set(AURORA_NOD_VERSION "v2.0.0-alpha.10" CACHE STRING "")
include("${CMAKE_CURRENT_SOURCE_DIR}/../extern/aurora/cmake/AuroraNodProvider.cmake")

add_library(sms-recomp-rt STATIC runtime/rt_core.cpp runtime/mmio.cpp
                                 runtime/dev_aram.cpp
                                 runtime/dev_dsp.cpp
                                 runtime/dev_ai.cpp
                                 runtime/dev_exi.cpp
                                 runtime/dev_vi.cpp
                                 runtime/dev_si.cpp
                                 runtime/dev_sram.cpp
                                 runtime/dev_di.cpp
                                 runtime/dev_pi.cpp
                                 runtime/disc.cpp
                                 runtime/dev_mi.cpp
                                 runtime/dev_gxregs.cpp
                                 runtime/dev_gxfifo.cpp
                                 runtime/boot_env.cpp
                                 runtime/guest_sched.cpp)
target_include_directories(sms-recomp-rt PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/runtime" "${CMAKE_CURRENT_SOURCE_DIR}/overrides" "${GEN}")
target_link_libraries(sms-recomp-rt PUBLIC lucent::lucent nod::nod aurora::gx)

# Host harness: loads the DOL into guest RAM and runs the entry point.
# runtime/sb_log.cpp is compiled into the EXECUTABLE, not into sms-recomp-rt. It exists solely to
# satisfy aurora's WEAK sb_log_enabled/sb_logf, and a weak UNDEFINED reference does not pull a
# member out of a static archive — nothing strongly references sb_log.o, so from a library it is
# never extracted and the weak symbol silently stays null. That is precisely the failure it was
# added to fix, so it must be linked unconditionally.
add_executable(sms-recomp host/main.cpp runtime/sb_log.cpp)
# -rdynamic puts func_<addr> into the dynamic symbol table so rt_dump_guest_stack can
# name the guest frames. Recompiled bodies nest on the host stack, so the host backtrace
# is the guest call stack — but only if the names resolve.
target_link_options(sms-recomp PRIVATE -rdynamic)
target_link_libraries(sms-recomp PRIVATE sms-recomp-rt sms-recomp-guest
                      sms-recomp-overrides lucent::lucent
                      # aurora_core and aurora_gx reference each other, so both must be on
                      # the link line together for the static archives to resolve.
                      aurora::core aurora::gx aurora::core
                      aurora::main aurora::vi aurora::os aurora::mtx aurora::pad)
