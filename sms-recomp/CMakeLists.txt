# sms-recomp — the standalone recomp runtime (two-runtime doctrine, CLAUDE.md).
# Builds the generated PPC C++ against rt_core.cpp. No Dolphin, no decomp interop.
cmake_minimum_required(VERSION 3.20)
project(sms_recomp CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "sms-recomp requires clang++; configure with -DCMAKE_CXX_COMPILER=clang++")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
# CONFIGURE_DEPENDS: without it a NEW source file is not picked up until someone
# reconfigures by hand, and the failure is a link error naming a function you just wrote
# — which reads as a typo, not as a stale glob.
file(GLOB SB_OVERRIDES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/overrides/*.cpp")
add_library(sms-recomp-overrides OBJECT ${SB_OVERRIDES})
target_include_directories(sms-recomp-overrides PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime" "${CMAKE_CURRENT_SOURCE_DIR}/runtime/devices" "${CMAKE_CURRENT_SOURCE_DIR}/runtime/render" "${CMAKE_CURRENT_SOURCE_DIR}/overrides"
    "${CMAKE_CURRENT_SOURCE_DIR}/frame_interp" "${GEN}")
target_link_libraries(sms-recomp-overrides PUBLIC nod::nod aurora::gx aurora::pad)
target_link_libraries(sms-recomp-overrides PUBLIC lucent::lucent)

# frame_interp/ — ALL interpolated-60fps code, in one place. See docs/60fps/README.md for the map.
#
# Its own library rather than more files under overrides/ because it is a SUBSYSTEM, not a
# collection of unrelated hooks: it was previously spread across overrides/ (four files),
# runtime/ (two) and aurora, under three different names, and nothing in the tree said the six
# belonged together. A directory says it.
#
# OBJECT for the same reason overrides/ is: these files self-register guest hooks from static
# initializers and export nothing else, so a static archive lets the linker discard the whole
# object as unused and the hook silently never installs — which for an interpolation seam looks
# exactly like "interpolation is not implemented for that thing".
file(GLOB SB_FRAME_INTERP CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/frame_interp/*.cpp")
add_library(sms-recomp-frame-interp OBJECT ${SB_FRAME_INTERP})
target_include_directories(sms-recomp-frame-interp PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime" "${CMAKE_CURRENT_SOURCE_DIR}/runtime/devices" "${CMAKE_CURRENT_SOURCE_DIR}/runtime/render" "${CMAKE_CURRENT_SOURCE_DIR}/overrides"
    "${CMAKE_CURRENT_SOURCE_DIR}/frame_interp" "${GEN}")
target_link_libraries(sms-recomp-frame-interp PUBLIC nod::nod aurora::gx aurora::pad lucent::lucent)

# nod supplies random-access reads of the disc image (ISO/RVZ/...). The DI device serves
# the guest's own DVD library at the REGISTER level, so nothing marshals guest structs —
# the game's DVD code, FST parsing and file reads all run as recompiled PPC. That is why
# this links nod directly rather than aurora's SDK-level DVD implementation.
# Aurora provides the GX implementation. aurora_fifo_replay() consumes a raw GX command
# stream, which is exactly what the guest writes to the gather pipe, so no translation layer
# is needed for the command stream itself — only for pointers (see dev_gxfifo.cpp).
# DVD stays OFF: this runtime serves the disc itself through nod (runtime/devices/disc.cpp).
set(AURORA_ENABLE_GX ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_RMLUI ON CACHE BOOL "Enable the Dusklight-style Sunbright settings UI" FORCE)
set(AURORA_BUILD_TESTS OFF CACHE BOOL "Do not register Aurora's standalone tests when embedded" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../extern/aurora" aurora-build EXCLUDE_FROM_ALL)

set(AURORA_NOD_PROVIDER "auto" CACHE STRING "")
set(AURORA_NOD_LINKAGE "static" CACHE STRING "")
set(AURORA_NOD_VERSION "v2.0.0-alpha.10" CACHE STRING "")
include("${CMAKE_CURRENT_SOURCE_DIR}/../extern/aurora/cmake/AuroraNodProvider.cmake")

# Application policy and UI follow Dusklight's ownership split: typed settings are independent of
# RmlUi, while ui/ owns documents and presentation. Runtime seams consume sms-recomp-app directly;
# they never ask the menu what a setting means.
add_library(sms-recomp-app STATIC app/settings.cpp app/frame_rate.cpp)
target_include_directories(sms-recomp-app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sms-recomp-app PUBLIC lucent::lucent)

add_library(sms-recomp-ui STATIC ui/event.cpp ui/document.cpp ui/window.cpp
                                 ui/settings_menu.cpp ui/runtime.cpp ui/ui.cpp)
target_include_directories(sms-recomp-ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sms-recomp-ui PUBLIC sms-recomp-app aurora::core lucent::lucent)

target_link_libraries(sms-recomp-overrides PUBLIC sms-recomp-app sms-recomp-ui)
target_link_libraries(sms-recomp-frame-interp PUBLIC sms-recomp-app)

# BetterSunshineEngine-derived gameplay timing fixes are an enhancement
# subsystem, separate from HW/OS overrides and from render interpolation. The
# recompiled retail bodies remain linked; these runtime overrides can super-call
# them and are independent of recompiler regeneration.
add_library(sms-recomp-bse OBJECT bse/frame_rate_fixes.cpp bse/frame_rate_logic.cpp)
target_include_directories(sms-recomp-bse PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
    "${CMAKE_CURRENT_SOURCE_DIR}/overrides" "${GEN}")
target_link_libraries(sms-recomp-bse PUBLIC sms-recomp-app lucent::lucent)

add_library(sms-recomp-rt STATIC runtime/rt_core.cpp runtime/devices/mmio.cpp
                                 runtime/devices/dev_aram.cpp
                                 runtime/devices/dev_aid.cpp
                                 runtime/devices/dsp_mixer.cpp
                                 runtime/devices/dev_dsp.cpp
                                 runtime/devices/dev_ai.cpp
                                 runtime/devices/dev_exi.cpp
                                 runtime/devices/dev_vi.cpp
                                 runtime/devices/dev_si.cpp
                                 runtime/devices/dev_sram.cpp
                                 runtime/devices/dev_di.cpp
                                 runtime/devices/dev_pi.cpp
                                 runtime/devices/disc.cpp
                                 runtime/devices/dev_mi.cpp
                                 runtime/devices/dev_gxregs.cpp
                                 runtime/devices/dev_gxfifo.cpp
                                 runtime/devices/gx_fifo_2d.cpp
                                 runtime/devices/gx_fifo_input.cpp
                                 runtime/devices/gx_fifo_vertex_layout.cpp
                                 runtime/boot_env.cpp
                                 runtime/guest_sched.cpp)
target_include_directories(sms-recomp-rt PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/runtime" "${CMAKE_CURRENT_SOURCE_DIR}/runtime/devices" "${CMAKE_CURRENT_SOURCE_DIR}/runtime/render" "${CMAKE_CURRENT_SOURCE_DIR}/overrides" "${CMAKE_CURRENT_SOURCE_DIR}/frame_interp" "${GEN}")
target_link_libraries(sms-recomp-rt PUBLIC lucent::lucent nod::nod aurora::gx aurora::audio)

# Host harness: loads the DOL into guest RAM and runs the entry point.
# runtime/sb_log.cpp is compiled into the EXECUTABLE, not into sms-recomp-rt. It exists solely to
# satisfy aurora's WEAK sb_log_enabled/sb_logf, and a weak UNDEFINED reference does not pull a
# member out of a static archive — nothing strongly references sb_log.o, so from a library it is
# never extracted and the weak symbol silently stays null. That is precisely the failure it was
# added to fix, so it must be linked unconditionally.
add_executable(sms-recomp host/main.cpp host/dol_loader.cpp runtime/sb_log.cpp runtime/render/tev_eval.cpp runtime/probe_server.cpp
                           runtime/render/native_render.cpp runtime/render/scene.cpp runtime/render/j3d_decode.cpp runtime/render/gx_texture.cpp runtime/render/state_oracle.cpp
                           runtime/render/render_compare.cpp runtime/render/frame_smoothness.cpp)
# -rdynamic puts func_<addr> into the dynamic symbol table so rt_dump_guest_stack can
# name the guest frames. Recompiled bodies nest on the host stack, so the host backtrace
# is the guest call stack — but only if the names resolve.
target_link_options(sms-recomp PRIVATE -rdynamic)
target_link_libraries(sms-recomp PRIVATE sms-recomp-rt sms-recomp-guest
                      sms-recomp-overrides sms-recomp-frame-interp sms-recomp-bse
                      sms-recomp-app sms-recomp-ui
                      lucent::lucent
                      # aurora_core and aurora_gx reference each other, so both must be on
                      # the link line together for the static archives to resolve.
                      aurora::core aurora::gx aurora::core
                      aurora::main aurora::vi aurora::os aurora::mtx aurora::pad
                      SDL3::SDL3)

# ---------------------------------------------------------------------------------------------
# Tests. The TEV pipeline is checked against values hand-derived from the SDK, with no GPU, no
# frame and no game — which is the whole point: before this the pixel pipeline existed only as a
# shader and could not be asked a question except by rendering a frame and inferring backwards.
enable_testing()
add_executable(tev_eval_test tests/tev_eval_test.cpp runtime/render/tev_eval.cpp)
target_link_libraries(tev_eval_test PRIVATE lucent::lucent)
target_include_directories(tev_eval_test PRIVATE runtime)
add_test(NAME tev_eval COMMAND tev_eval_test)

add_executable(gx_light_test tests/gx_light_test.cpp runtime/render/gx_light.cpp)
target_link_libraries(gx_light_test PRIVATE lucent::lucent)
target_include_directories(gx_light_test PRIVATE runtime runtime/render runtime/devices)
add_test(NAME gx_light COMMAND gx_light_test)

add_executable(gx_fifo_input_test tests/gx_fifo_input_test.cpp
                                  runtime/devices/gx_fifo_input.cpp
                                  runtime/devices/gx_fifo_vertex_layout.cpp)
target_include_directories(gx_fifo_input_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME gx_fifo_input COMMAND gx_fifo_input_test)

add_executable(guest_address_table_test tests/guest_address_table_test.cpp)
target_include_directories(guest_address_table_test PRIVATE runtime)
add_test(NAME guest_address_table COMMAND guest_address_table_test)

add_executable(settings_test tests/settings_test.cpp)
target_link_libraries(settings_test PRIVATE sms-recomp-app)
add_test(NAME settings COMMAND settings_test)

add_executable(bse_frame_rate_test tests/bse_frame_rate_test.cpp
                                   bse/frame_rate_logic.cpp)
target_include_directories(bse_frame_rate_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME bse_frame_rate COMMAND bse_frame_rate_test)

add_executable(hud_window_layout_test tests/hud_window_layout_test.cpp
                                      overrides/hud_window_layout.cpp)
target_include_directories(hud_window_layout_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME hud_window_layout COMMAND hud_window_layout_test)

# The interpolation pairing self-test, reached through the main binary with SBR_INTERP_SELFTEST=1.
# It is pure CPU work — local buffers and aurora's own pairing tables — but until now it only ran
# from inside interpolate_recorded_frame, so proving the pairing maths required a GPU frame. It is
# the one part of the 60fps stack that can be checked without a run, and it was the part that could
# not be. No window, no ROM, no device is created on this path.
add_test(NAME interp_pairing COMMAND sms-recomp)
set_tests_properties(interp_pairing PROPERTIES ENVIRONMENT "SBR_INTERP_SELFTEST=1")
