# sms-boot — Super Mario Sunshine decomp (reference/sms) compiled native, rendered
# through Aurora. ONE binary, ONE runtime, ONE thread: the game runs on the
# process main thread; Aurora (render + audio + input + DVD) is IO called from
# inside the game's frame via the sb_frame_present seam (runtime/frame_seam.cpp).
# All I/O is synchronous and unthrottled. Engine-level PC-native mods live inside
# reference/sms behind SMS_NATIVE_PLATFORM.
#
# Layout:
#   main.cpp     — entry point (aurora init, DVD mount, game on main thread)
#   runtime/     — PC runtime seams: frame present/pace, SDK stubs, watchdog,
#                  audio pump, BARC-backed Vload
#   shims/       — RE'd game-logic spec headers included by reference/sms
#   assets/      — BE→host asset byteswappers
#   boot_stubs/  — scaffold stubs for unported game classes

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED PROJECT_NAME)
    project(sms-native CXX)
endif()

set(SMS_NATIVE_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Game-logic sources = reference/sms/src/**.cpp MINUS the GC SDK/CRT dirs (Aurora
# supplies dolphin/*; PowerPC_EABI_Support/TRK_MINNOW_DOLPHIN are PPC CRT).
file(GLOB_RECURSE SMS_NATIVE_SOURCES CONFIGURE_DEPENDS
     "${SMS_NATIVE_REPO_ROOT}/reference/sms/src/*.cpp"
     "${SMS_NATIVE_REPO_ROOT}/reference/sms/src/*.c")
list(FILTER SMS_NATIVE_SOURCES EXCLUDE REGEX
     "/reference/sms/src/(dolphin|PowerPC_EABI_Support|TRK_MINNOW_DOLPHIN)/")
# OdemuExi2 debugger driver, dspproc/dsptask/osdsp — PPC-hardware/DSP register
# access, not portable native. Retired debugger/DSP paths.
list(FILTER SMS_NATIVE_SOURCES EXCLUDE REGEX
     "/reference/sms/src/(OdemuExi2|JSystem/dspproc|JSystem/dsptask|JSystem/osdsp)")
# hx_wiper.c / THPPlayer/*.c: HAM movie-wipe controller + Nintendo THP video
# playback. Both drive GX directly and depend on cache/DVD hardware. Movies are
# skipped under SMS_NATIVE_PLATFORM=1; sdk_gap_stubs.cpp provides linkable
# no-ops for the exported entry points.
list(FILTER SMS_NATIVE_SOURCES EXCLUDE REGEX
     "/reference/sms/src/GC2D/hx_wiper\\.c$")
list(FILTER SMS_NATIVE_SOURCES EXCLUDE REGEX
     "/reference/sms/src/THPPlayer/")

# JASVload wants JaiArcS.hed which isn't on the disc FST; replaced by
# sms_boot_audio.cpp's BARC-backed loader.
list(FILTER SMS_NATIVE_SOURCES EXCLUDE REGEX
     "/reference/sms/src/JSystem/JAudio/JASystem/JASVload\\.cpp$")

# Pure PPC-hardware TUs — no meaningful native implementation; Aurora owns the
# equivalent surfaces (GX register I/O, OS panics, glibc math).
list(FILTER SMS_NATIVE_SOURCES EXCLUDE REGEX
     "/reference/sms/src/JSystem/JUtility/JUTException\\.cpp$")
list(FILTER SMS_NATIVE_SOURCES EXCLUDE REGEX
     "/reference/sms/src/JSystem/JUtility/JUTDirectPrint\\.cpp$")
list(FILTER SMS_NATIVE_SOURCES EXCLUDE REGEX
     "/reference/sms/src/JSystem/JRenderer\\.cpp$")
# JPAField.cpp was previously excluded as "PPC-only impl". Not true — the source
# in reference/sms is portable C++ and its ctor `unk0(this)` is required to
# initialize JSUPtrLink members. Without it the stub in boot_stubs was leaving
# mData/mPtrList/mPrev/mNext uninitialized, and JPAEmitterManager's fields loop
# then re-entered a "stale" field (previously used on a reset SolidHeap) with
# mPtrList=non-null and crashed inside JSUPtrList::prepend's unlink path.

list(APPEND SMS_NATIVE_SOURCES
     "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/sms_boot_audio.cpp")

# .c decomp TUs (THPPlayer/*.c) — compile as C++ so they see the C++ Aurora
# headers and pick up the same -include shim.
foreach(_src ${SMS_NATIVE_SOURCES})
    if(_src MATCHES "\\.c$")
        set_source_files_properties("${_src}" PROPERTIES LANGUAGE CXX)
    endif()
endforeach()

add_library(sms-native STATIC ${SMS_NATIVE_SOURCES})

set_target_properties(sms-native PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

# Stage reference/sms/include without its dolphin/ subdir; Aurora supplies dolphin/.
set(SMS_INCLUDE_STAGED "${CMAKE_CURRENT_BINARY_DIR}/staged_sms_include")
file(REMOVE_RECURSE "${SMS_INCLUDE_STAGED}")
file(COPY "${SMS_NATIVE_REPO_ROOT}/reference/sms/include/"
     DESTINATION "${SMS_INCLUDE_STAGED}"
     PATTERN "dolphin" EXCLUDE
     PATTERN "dolphin.h" EXCLUDE)

# Three orthogonal compile-time gates:
#   TARGET_PC          — Aurora's fixed-width typedefs (BOOL/s32/u32/etc.).
#                        Owned by Aurora, not by reference/sms game code.
#   SMS_NATIVE_PLATFORM — PC ENGINE gate: single-threaded, sync/instant loading,
#                        no OSCreateThread carriers, host-malloc paths, fail-fast
#                        panics. Purely game-side. Has nothing to do with rendering.
#   SMS_AURORA         — AURORA RENDERER gate: skip raw GC display-list callsites
#                        (Aurora extends the fifo format with GX_AURORA subcommands
#                        + size/le payloads), route J3DLoadArrayBasePtr through
#                        the AURORA_LOAD_ARRAYBASE opcode. Enabled iff we're
#                        rendering through aurora::gx.
# Two-project TDD: an alternate renderer (e.g. a future SDL3-GPU path) would
# leave SMS_NATIVE_PLATFORM=1 but drop SMS_AURORA.
target_compile_definitions(sms-native PUBLIC
    TARGET_PC=1
    SMS_NATIVE_PLATFORM=1
    SMS_AURORA=1)

target_include_directories(sms-native PUBLIC
    "${SMS_NATIVE_REPO_ROOT}/extern/aurora/include"
    "${SMS_INCLUDE_STAGED}"
    "${SMS_NATIVE_REPO_ROOT}/sms-boot/assets"
    # RE'd game-logic spec headers (sms_boot_*.h) that SMS_NATIVE_PLATFORM
    # branches in reference/sms include.
    "${SMS_NATIVE_REPO_ROOT}/sms-boot/shims")

target_compile_options(sms-native PRIVATE
    "SHELL:-include ${SMS_NATIVE_REPO_ROOT}/extern/aurora/include/dolphin/types.h"
    -w
    -Wno-narrowing
    -fcommon
    $<$<CXX_COMPILER_ID:GNU>:-fno-reorder-blocks-and-partition>
    $<$<CXX_COMPILER_ID:GNU>:-finput-charset=UTF-8>
    $<$<CXX_COMPILER_ID:GNU>:-fexec-charset=SHIFT_JIS>)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# sms-gd — decomp GC GD (display-list builder), compiled from reference/sms.
# ---------------------------------------------------------------------------
file(GLOB SMS_GD_SOURCES CONFIGURE_DEPENDS
     "${SMS_NATIVE_REPO_ROOT}/reference/sms/src/dolphin/gd/*.c")
# GDLight.c duplicates aurora::gd's GDLight.cpp; Aurora's is canonical and paired
# with its GX pipeline, so use it.
list(FILTER SMS_GD_SOURCES EXCLUDE REGEX "/GDLight\\.c$")
add_library(sms-gd STATIC ${SMS_GD_SOURCES})
set_target_properties(sms-gd PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
    LINKER_LANGUAGE CXX)
set_source_files_properties(${SMS_GD_SOURCES} PROPERTIES LANGUAGE CXX)
target_include_directories(sms-gd PUBLIC
    "${SMS_NATIVE_REPO_ROOT}/extern/aurora/include"
    "${SMS_INCLUDE_STAGED}")
target_compile_definitions(sms-gd PUBLIC TARGET_PC=1 SMS_NATIVE_PLATFORM=1 SMS_AURORA=1)
target_compile_options(sms-gd PRIVATE
    -include "${SMS_NATIVE_REPO_ROOT}/extern/aurora/include/dolphin/types.h"
    -w -fcommon)

# ---------------------------------------------------------------------------
# sms-assets — BE→host asset byteswap (BMD/BMT/anim/TIMG/RARC) for LE hosts.
# ---------------------------------------------------------------------------
add_library(sms-assets STATIC
    "${SMS_NATIVE_REPO_ROOT}/sms-boot/assets/bmd_swap.cpp"
    "${SMS_NATIVE_REPO_ROOT}/sms-boot/assets/bmt_swap.cpp"
    "${SMS_NATIVE_REPO_ROOT}/sms-boot/assets/anm_swap.cpp"
    "${SMS_NATIVE_REPO_ROOT}/sms-boot/assets/timg_swap.cpp"
    "${SMS_NATIVE_REPO_ROOT}/sms-boot/assets/rarc.cpp")
set_target_properties(sms-assets PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sms-assets PUBLIC
    "${SMS_NATIVE_REPO_ROOT}/sms-boot/assets")
target_compile_options(sms-assets PRIVATE -Wall -Wextra)

enable_testing()

# ---------------------------------------------------------------------------
# sms-boot — the ONE binary. reference/sms decomp compiled with
# SMS_NATIVE_PLATFORM=1, rendered through Aurora (SDL3 + Dawn/WebGPU).
# ---------------------------------------------------------------------------
if(TARGET aurora_core)
    add_executable(sms-boot
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/main.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/frame_seam.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/audio_out.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/watchdog.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/sdk_stubs.cpp")
    set_target_properties(sms-boot PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(sms-boot PRIVATE
        "${SMS_NATIVE_REPO_ROOT}/extern/aurora/include"
        "${SMS_INCLUDE_STAGED}")
    # sdk_stubs.cpp speaks the GC SDK surface (dolphin/* headers, u32/BOOL
    # types) — compile it like the decomp TUs.
    set_source_files_properties(
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/sdk_stubs.cpp" PROPERTIES
        COMPILE_OPTIONS "-include;${SMS_NATIVE_REPO_ROOT}/extern/aurora/include/dolphin/types.h;-w;-fcommon"
        COMPILE_DEFINITIONS "TARGET_PC=1;SMS_NATIVE_PLATFORM=1;SMS_AURORA=1")

    # boot_stubs/ — scaffold stubs for unported game classes; replaced with
    # real ports as the boot-order path exercises each one.
    file(GLOB SMS_BOOT_STUBS CONFIGURE_DEPENDS
         "${SMS_NATIVE_REPO_ROOT}/sms-boot/boot_stubs/*.cpp"
         "${SMS_NATIVE_REPO_ROOT}/sms-boot/boot_stubs/*.S")
    if(SMS_BOOT_STUBS)
        enable_language(ASM)
        add_library(sms-bootstubs STATIC ${SMS_BOOT_STUBS})
        set_target_properties(sms-bootstubs PROPERTIES
            CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        target_include_directories(sms-bootstubs PRIVATE
            "${SMS_NATIVE_REPO_ROOT}/extern/aurora/include"
            "${SMS_INCLUDE_STAGED}")
        target_compile_options(sms-bootstubs PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:-include;${SMS_NATIVE_REPO_ROOT}/extern/aurora/include/dolphin/types.h>"
            "$<$<COMPILE_LANGUAGE:CXX>:-DTARGET_PC=1>"
            "$<$<COMPILE_LANGUAGE:CXX>:-DSMS_NATIVE_PLATFORM=1>"
            "$<$<COMPILE_LANGUAGE:CXX>:-DSMS_AURORA=1>"
            -w -fcommon)
    endif()

    # -rdynamic so backtrace_symbols in the watchdog handler resolves names.
    target_link_options(sms-boot PRIVATE -rdynamic)
    target_link_libraries(sms-boot PRIVATE
        "$<LINK_GROUP:RESCAN,sms-native,sms-gd,sms-assets,$<TARGET_NAME_IF_EXISTS:sms-bootstubs>>"
        aurora::core
        aurora::gx
        aurora::audio
        aurora::main
        aurora::vi
        aurora::os
        aurora::mtx
        aurora::pad
        aurora::si
        aurora::card
        aurora::dvd
        aurora::gd
        aurora::ms
        Threads::Threads)
endif()
