# sms-boot — Super Mario Sunshine decomp (decomp/sms) compiled native, rendered
# through Aurora. ONE binary, ONE runtime, ONE thread: the game runs on the
# process main thread; Aurora (render + audio + input + DVD) is IO called from
# inside the game's frame via the sb_frame_present seam (runtime/frame_seam.cpp).
# All I/O is synchronous and unthrottled. Engine-level PC-native mods live inside
# decomp/sms behind SMS_NATIVE_PLATFORM.
#
# Layout:
#   main.cpp     — entry point (aurora init, DVD mount, game on main thread)
#   runtime/     — PC runtime seams: frame present/pace, SDK stubs, watchdog,
#                  audio pump, BARC-backed Vload
#   shims/       — RE'd game-logic spec headers included by decomp/sms
#   assets/      — BE→host asset byteswappers
#   boot_stubs/  — scaffold stubs for unported game classes

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED PROJECT_NAME)
    project(sms-native CXX)
endif()

set(SMS_NATIVE_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Game-logic sources = decomp/sms/src/**.cpp MINUS the GC SDK/CRT dirs (Aurora
# supplies dolphin/*; PowerPC_EABI_Support/TRK_MINNOW_DOLPHIN are PPC CRT).
file(GLOB_RECURSE SMS_DECOMP_SOURCES CONFIGURE_DEPENDS
     "${SMS_NATIVE_REPO_ROOT}/decomp/sms/src/*.cpp"
     "${SMS_NATIVE_REPO_ROOT}/decomp/sms/src/*.c")
list(FILTER SMS_DECOMP_SOURCES EXCLUDE REGEX
     "/decomp/sms/src/(dolphin|PowerPC_EABI_Support|TRK_MINNOW_DOLPHIN)/")
# OdemuExi2 debugger driver, dspproc/dsptask/osdsp — PPC-hardware/DSP register
# access, not portable native. Retired debugger/DSP paths.
list(FILTER SMS_DECOMP_SOURCES EXCLUDE REGEX
     "/decomp/sms/src/(OdemuExi2|JSystem/dspproc|JSystem/dsptask|JSystem/osdsp)")
# hx_wiper.c / THPPlayer/*.c: HAM movie-wipe controller + Nintendo THP video
# playback. Both drive GX directly and depend on cache/DVD hardware. Movies are
# skipped under SMS_NATIVE_PLATFORM=1; runtime/sdk_stubs.cpp provides linkable
# no-ops (a small state machine faking an instantly-finished movie) for the
# exported entry points.
list(FILTER SMS_DECOMP_SOURCES EXCLUDE REGEX
     "/decomp/sms/src/GC2D/hx_wiper\\.c$")
list(FILTER SMS_DECOMP_SOURCES EXCLUDE REGEX
     "/decomp/sms/src/THPPlayer/")

# JASVload wants JaiArcS.hed which isn't on the disc FST; replaced by
# sms_boot_audio.cpp's BARC-backed loader.
list(FILTER SMS_DECOMP_SOURCES EXCLUDE REGEX
     "/decomp/sms/src/JSystem/JAudio/JASystem/JASVload\\.cpp$")

# Pure PPC-hardware TUs — no meaningful native implementation; Aurora owns the
# equivalent surfaces (GX register I/O, OS panics, glibc math).
list(FILTER SMS_DECOMP_SOURCES EXCLUDE REGEX
     "/decomp/sms/src/JSystem/JUtility/JUTException\\.cpp$")
list(FILTER SMS_DECOMP_SOURCES EXCLUDE REGEX
     "/decomp/sms/src/JSystem/JUtility/JUTDirectPrint\\.cpp$")
# JRenderer.cpp was previously excluded here as "pure PPC-hardware, no meaningful
# native impl" — false: it holds the REAL J3DGD*/JRNI* BP-register writers
# (JRNISetTevOrder's TREF write among them). Excluding it left every one of
# those 21 writers resolved to sdk_stubs.cpp's empty no-op bodies, so J3D
# materials never wrote their TEV order/channel/texcoord-scale/tex-attr BP
# regs at all — every stage kept aurora's gx.hpp default `texMapId =
# GX_TEXMAP_NULL`, which forces TEXC=0 in the combiner regardless of the
# material's real texture. This is the root cause of the black title
# Sky Xlu/MapOpa backdrop (2026-07-10): geometry rasterizes correctly
# (confirmed via SB_FORCE_COLOR, full-screen magenta coverage) but color
# is forced to 0 because the texture/channel setup was never wired.
# 2026-07-08's 4ed2ac15 already made JRenderer.cpp's decomp body compile
# natively (LP64 casts, GXTexImage*Ids externs) but never removed this
# exclusion, so the fix silently never took effect. Building it natively now;
# the matching sdk_stubs.cpp no-ops for these 21 symbols are removed below
# to avoid duplicate-definition link errors.
# JPAField.cpp was previously excluded as "PPC-only impl". Not true — the source
# in decomp/sms is portable C++ and its ctor `unk0(this)` is required to
# initialize JSUPtrLink members. Without it the stub in boot_stubs was leaving
# mData/mPtrList/mPrev/mNext uninitialized, and JPAEmitterManager's fields loop
# then re-entered a "stale" field (previously used on a reset SolidHeap) with
# mPtrList=non-null and crashed inside JSUPtrList::prepend's unlink path.

set(SMS_SJIS_TOOL "${SMS_NATIVE_REPO_ROOT}/tools/build/encode_sjis_literals.py")
set(SMS_SOURCE_STAGED "${CMAKE_CURRENT_BINARY_DIR}/staged_sms_src")
set(SMS_INCLUDE_STAGED "${CMAKE_CURRENT_BINARY_DIR}/staged_sms_include")

# Before this generated tree existed, staged_sms_include used top-level
# symlinks into decomp/sms/include. Remove that retired layout with the scoped
# project cleanup tool before any custom command can follow it into source.
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${SMS_SJIS_TOOL}"
            --build-root "${CMAKE_BINARY_DIR}"
            --prepare-root "${SMS_SOURCE_STAGED}"
            --prepare-root "${SMS_INCLUDE_STAGED}"
    RESULT_VARIABLE SMS_SJIS_PREPARE_RESULT)
if(NOT SMS_SJIS_PREPARE_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to prepare the Shift-JIS generated source tree")
endif()

# Clang does not support -fexec-charset. Generate byte-exact Shift-JIS
# ordinary literals for the native target while keeping the authoritative
# decomp sources UTF-8. Headers must go through the same transform because
# inline bodies and macros can contain serialized game names too.
set(SMS_NATIVE_SOURCES)
foreach(_src ${SMS_DECOMP_SOURCES})
    file(RELATIVE_PATH _rel "${SMS_NATIVE_REPO_ROOT}/decomp/sms/src" "${_src}")
    set(_staged "${SMS_SOURCE_STAGED}/${_rel}")
    get_filename_component(_staged_dir "${_staged}" DIRECTORY)
    add_custom_command(
        OUTPUT "${_staged}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_staged_dir}"
        COMMAND "${Python3_EXECUTABLE}" "${SMS_SJIS_TOOL}" "${_src}" "${_staged}"
                --output-root "${SMS_SOURCE_STAGED}"
        DEPENDS "${_src}" "${SMS_SJIS_TOOL}"
        VERBATIM)
    list(APPEND SMS_NATIVE_SOURCES "${_staged}")
endforeach()

file(GLOB_RECURSE SMS_DECOMP_HEADERS CONFIGURE_DEPENDS LIST_DIRECTORIES false
     "${SMS_NATIVE_REPO_ROOT}/decomp/sms/include/*")
list(FILTER SMS_DECOMP_HEADERS EXCLUDE REGEX "/decomp/sms/include/dolphin/")
list(FILTER SMS_DECOMP_HEADERS EXCLUDE REGEX "/decomp/sms/include/dolphin\\.h$")
set(SMS_NATIVE_HEADERS)
foreach(_header ${SMS_DECOMP_HEADERS})
    file(RELATIVE_PATH _rel "${SMS_NATIVE_REPO_ROOT}/decomp/sms/include" "${_header}")
    set(_staged "${SMS_INCLUDE_STAGED}/${_rel}")
    get_filename_component(_staged_dir "${_staged}" DIRECTORY)
    add_custom_command(
        OUTPUT "${_staged}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_staged_dir}"
        COMMAND "${Python3_EXECUTABLE}" "${SMS_SJIS_TOOL}" "${_header}" "${_staged}"
                --output-root "${SMS_INCLUDE_STAGED}"
        DEPENDS "${_header}" "${SMS_SJIS_TOOL}"
        VERBATIM)
    list(APPEND SMS_NATIVE_HEADERS "${_staged}")
endforeach()

add_custom_target(sms-native-sjis DEPENDS ${SMS_NATIVE_SOURCES} ${SMS_NATIVE_HEADERS})
list(APPEND SMS_NATIVE_SOURCES
     "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/sms_boot_audio.cpp")

# .c decomp TUs compile as C++ so they see Aurora's C++ headers and pick up
# the same forced-include shim.
foreach(_src ${SMS_NATIVE_SOURCES})
    if(_src MATCHES "\\.c$")
        set_source_files_properties("${_src}" PROPERTIES LANGUAGE CXX)
    endif()
endforeach()

add_library(sms-native STATIC ${SMS_NATIVE_SOURCES})
add_dependencies(sms-native sms-native-sjis)

set_target_properties(sms-native PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

# Three orthogonal compile-time gates:
#   TARGET_PC          — Aurora's fixed-width typedefs (BOOL/s32/u32/etc.).
#                        Owned by Aurora, not by decomp/sms game code.
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
    # branches in decomp/sms include.
    "${SMS_NATIVE_REPO_ROOT}/sms-boot/shims")

target_compile_options(sms-native PRIVATE
    "SHELL:-include ${SMS_NATIVE_REPO_ROOT}/extern/aurora/include/dolphin/types.h"
    -w
    -Wno-narrowing
    -fcommon
    $<$<CXX_COMPILER_ID:GNU>:-fno-reorder-blocks-and-partition>)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# sms-gd — decomp GC GD (display-list builder), compiled from decomp/sms.
# ---------------------------------------------------------------------------
file(GLOB SMS_GD_SOURCES CONFIGURE_DEPENDS
     "${SMS_NATIVE_REPO_ROOT}/decomp/sms/src/dolphin/gd/*.c")
# GDLight.c duplicates aurora::gd's GDLight.cpp; Aurora's is canonical and paired
# with its GX pipeline, so use it.
list(FILTER SMS_GD_SOURCES EXCLUDE REGEX "/GDLight\\.c$")
add_library(sms-gd STATIC ${SMS_GD_SOURCES})
add_dependencies(sms-gd sms-native-sjis)
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
    "${SMS_NATIVE_REPO_ROOT}/sms-boot/assets/bas_swap.cpp"
    "${SMS_NATIVE_REPO_ROOT}/sms-boot/assets/timg_swap.cpp"
    "${SMS_NATIVE_REPO_ROOT}/sms-boot/assets/restlut_swap.cpp"
    "${SMS_NATIVE_REPO_ROOT}/sms-boot/assets/spc_swap.cpp"
    "${SMS_NATIVE_REPO_ROOT}/sms-boot/assets/rarc.cpp"
    "${SMS_NATIVE_REPO_ROOT}/sms-boot/assets/bcsv_swap.cpp")
set_target_properties(sms-assets PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sms-assets PUBLIC
    "${SMS_NATIVE_REPO_ROOT}/sms-boot/assets"
    # shims/ too, so this target can use the project logger (sb_log.h). Without it the only
    # option here was a raw fprintf behind its own env var, which is exactly the idiom the
    # single-logger rule exists to remove.
    "${SMS_NATIVE_REPO_ROOT}/sms-boot/shims")
target_compile_options(sms-assets PRIVATE -Wall -Wextra)

enable_testing()

file(GLOB SMS_ASSET_TESTS CONFIGURE_DEPENDS
     "${SMS_NATIVE_REPO_ROOT}/sms-boot/assets/tests/*_test.cpp")
foreach(_test ${SMS_ASSET_TESTS})
    get_filename_component(_name "${_test}" NAME_WE)
    add_executable(asset-${_name} "${_test}")
    target_link_libraries(asset-${_name} PRIVATE sms-assets)
    add_test(NAME asset_${_name} COMMAND asset-${_name})
endforeach()

# ---------------------------------------------------------------------------
# Shim spec tests — unit tests from RE (spec-derived expected values) for the
# pure game-logic headers in shims/. No ROM, no GPU, no game link.
# ---------------------------------------------------------------------------
file(GLOB SMS_SHIM_TESTS CONFIGURE_DEPENDS
     "${SMS_NATIVE_REPO_ROOT}/sms-boot/shims/tests/*_test.cpp")
foreach(_test ${SMS_SHIM_TESTS})
    get_filename_component(_name "${_test}" NAME_WE)
    add_executable(shim-${_name} "${_test}")
    set_target_properties(shim-${_name} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(shim-${_name} PRIVATE
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/shims")
    add_test(NAME shim-${_name} COMMAND shim-${_name})
endforeach()

# ---------------------------------------------------------------------------
# Runtime seam tests — unit tests from RE for pure-logic ports living in
# runtime/ (not shims/). platform_hx_wipe: the Hx_ wipe/transition state
# machine (runtime/hx_wipe.cpp). No ROM, no GPU, no game link.
# ---------------------------------------------------------------------------
file(GLOB SMS_RUNTIME_TESTS CONFIGURE_DEPENDS
     "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/tests/*_test.cpp")
# These tests compile or link shipping game-logic implementations rather than
# hx_wipe.cpp + aurora::mtx. They get dedicated targets further down, gated on
# aurora_core like sms-boot itself.
list(FILTER SMS_RUNTIME_TESTS EXCLUDE REGEX
     "/sms-boot/runtime/tests/(jaudio_release_test|gx_yscale_test|mapobjtree_initeach_test|bathtub_tumble_test|native_picture_adapter_test)\\.cpp$")
foreach(_test ${SMS_RUNTIME_TESTS})
    get_filename_component(_name "${_test}" NAME_WE)
    add_executable(platform-${_name} "${_test}"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/hx_wipe.cpp")
    add_dependencies(platform-${_name} sms-native-sjis)
    set_target_properties(platform-${_name} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(platform-${_name} PRIVATE
        "${SMS_NATIVE_REPO_ROOT}/extern/aurora/include"
        "${SMS_INCLUDE_STAGED}")
    if(TARGET aurora_mtx)
        # Link aurora's actual C_MTX* implementation (not a hand copy) for
        # tests that check the GC sign convention against it (mtx_lookat_test).
        target_link_libraries(platform-${_name} PRIVATE aurora::mtx)
    endif()
    add_test(NAME platform_${_name} COMMAND platform-${_name})
endforeach()

# ---------------------------------------------------------------------------
# sms-boot — the ONE binary. decomp/sms decomp compiled with
# SMS_NATIVE_PLATFORM=1, rendered through Aurora (SDL3 + Dawn/WebGPU).
# ---------------------------------------------------------------------------
if(TARGET aurora_core)
    add_executable(sms-boot
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/main.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/frame_seam.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/semantic_render.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/trace_seq.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/audio_out.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/jas_kernel_native.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/watchdog.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/sdk_stubs.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/phase_track.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/hx_wipe.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/pad_script.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/pin_state.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/sb_log.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/fifo_player.cpp")
    target_sources(sms-boot PRIVATE
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/native_picture_adapter.cpp")
    add_dependencies(sms-boot sms-native-sjis)
    set_target_properties(sms-boot PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(sms-boot PRIVATE
        "${SMS_NATIVE_REPO_ROOT}/extern/aurora/include"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/shims"
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
        add_dependencies(sms-bootstubs sms-native-sjis)
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
        # Audio milestone 1 landed (docs/audio/native_mixer_plan.md): the JAS
        # KERNEL now runs natively and pushes dac[] buffers to aurora::audio via
        # sb_jas_kernel_frame (sms-boot/runtime/jas_kernel_native.cpp).
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
        sunbright-native-render-sdl3
        Threads::Threads)

    # Production-linked semantic J2D resource test. It uses the real native-layout adapter and
    # actual J2DPicture/JUTTexture/JUTPalette lifetimes while keeping the link narrow enough to use
    # a counting host-allocation gate instead of the full game's global-new routing.
    add_executable(platform-native_picture_adapter_test
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/tests/native_picture_adapter_test.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/native_picture_adapter.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/sb_log.cpp"
        "${SMS_SOURCE_STAGED}/JSystem/J2D/J2DGrafContext.cpp"
        "${SMS_SOURCE_STAGED}/JSystem/J2D/J2DOrthoGraph.cpp"
        "${SMS_SOURCE_STAGED}/JSystem/J2D/J2DPane.cpp"
        "${SMS_SOURCE_STAGED}/JSystem/J2D/J2DPicture.cpp"
        "${SMS_SOURCE_STAGED}/JSystem/JSupport/JSUList.cpp"
        "${SMS_SOURCE_STAGED}/JSystem/JUtility/JUTRect.cpp"
        "${SMS_SOURCE_STAGED}/JSystem/JUtility/JUTTexture.cpp"
        "${SMS_SOURCE_STAGED}/JSystem/JUtility/JUTPalette.cpp")
    add_dependencies(platform-native_picture_adapter_test sms-native-sjis)
    set_target_properties(platform-native_picture_adapter_test PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(platform-native_picture_adapter_test PRIVATE
        "${SMS_NATIVE_REPO_ROOT}/extern/aurora/include"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/shims"
        "${SMS_INCLUDE_STAGED}")
    target_compile_definitions(platform-native_picture_adapter_test PRIVATE
        TARGET_PC=1 SMS_NATIVE_PLATFORM=1 SMS_AURORA=1)
    target_compile_options(platform-native_picture_adapter_test PRIVATE
        "SHELL:-include ${SMS_NATIVE_REPO_ROOT}/extern/aurora/include/dolphin/types.h"
        -w -fcommon -ffunction-sections -fdata-sections)
    target_link_options(platform-native_picture_adapter_test PRIVATE -Wl,--gc-sections)
    target_link_libraries(platform-native_picture_adapter_test PRIVATE
        sunbright-native-render-core aurora::gx aurora::mtx aurora::vi)
    add_test(NAME platform_native_picture_adapter_test COMMAND platform-native_picture_adapter_test)

    # -----------------------------------------------------------------------
    # jaudio_release_test — links the REAL JAIBasic/JAISound code (same
    # sms-native/sms-gd/sms-assets/aurora set as sms-boot, SMS_NATIVE_PLATFORM=1)
    # rather than a hand copy, to lock the JAudio handle-pool release /
    # double-release invariants fixed in decomp/sms fdce9d18. Excluded from
    # the generic runtime-tests glob above (that loop links only hx_wipe.cpp +
    # aurora::mtx; this test needs the full game-logic link set).
    # -----------------------------------------------------------------------
    add_executable(platform-jaudio_release_test
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/tests/jaudio_release_test.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/sdk_stubs.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/sb_log.cpp")
    add_dependencies(platform-jaudio_release_test sms-native-sjis)
    set_target_properties(platform-jaudio_release_test PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(platform-jaudio_release_test PRIVATE
        "${SMS_NATIVE_REPO_ROOT}/extern/aurora/include"
        "${SMS_INCLUDE_STAGED}")
    set_source_files_properties(
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/sdk_stubs.cpp" PROPERTIES
        COMPILE_OPTIONS "-include;${SMS_NATIVE_REPO_ROOT}/extern/aurora/include/dolphin/types.h;-w;-fcommon"
        COMPILE_DEFINITIONS "TARGET_PC=1;SMS_NATIVE_PLATFORM=1;SMS_AURORA=1")
    target_link_libraries(platform-jaudio_release_test PRIVATE
        "$<LINK_GROUP:RESCAN,sms-native,sms-gd,sms-assets,$<TARGET_NAME_IF_EXISTS:sms-bootstubs>>"
        aurora::core
        aurora::gx
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
    add_test(NAME platform_jaudio_release_test COMMAND platform-jaudio_release_test)

    # -----------------------------------------------------------------------
    # gx_yscale_test — links aurora's REAL GXGetNumXfbLines/GXGetYScaleFactor
    # (ported 2026-07-14 from decomp/sms/src/dolphin/gx/GXFrameBuf.c after
    # the silent sdk_stubs versions zeroed every render mode's xfbHeight/
    # viHeight) and sweeps them against a verbatim SDK transcription. Needs
    # the aurora::gx link set, hence the same full-link pattern as
    # jaudio_release_test.
    # -----------------------------------------------------------------------
    add_executable(platform-gx_yscale_test
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/tests/gx_yscale_test.cpp"
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/sdk_stubs.cpp")
    add_dependencies(platform-gx_yscale_test sms-native-sjis)
    set_target_properties(platform-gx_yscale_test PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(platform-gx_yscale_test PRIVATE
        "${SMS_NATIVE_REPO_ROOT}/extern/aurora/include"
        "${SMS_INCLUDE_STAGED}")
    target_link_libraries(platform-gx_yscale_test PRIVATE
        "$<LINK_GROUP:RESCAN,sms-native,sms-gd,sms-assets,$<TARGET_NAME_IF_EXISTS:sms-bootstubs>>"
        aurora::core
        aurora::gx
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
    add_test(NAME platform_gx_yscale_test COMMAND platform-gx_yscale_test)

    # -----------------------------------------------------------------------
    # mapobjtree_initeach_test — pins the RE'd species->leaf-count/param table
    # of the cold-ported TMapObjTree::initEach (decomp/sms/src/MoveBG/
    # MapObjTree.cpp, 2026-07-15). initEach is a dependency-free field-writer,
    # so the test compiles MapObjTree.cpp DIRECTLY (not via the game library)
    # and drives the real linked initEach. -ffunction-sections + --gc-sections
    # drops the unreferenced initMapObj/TMapObjLeaf-ctor (and their heavy
    # actor-graph deps) at link, so no sms-native/aurora link is needed. -w
    # matches the decomp's policy for the MoveBG/JSystem header chain.
    # -----------------------------------------------------------------------
    add_executable(platform-mapobjtree_initeach_test
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/tests/mapobjtree_initeach_test.cpp"
        "${SMS_SOURCE_STAGED}/MoveBG/MapObjTree.cpp")
    add_dependencies(platform-mapobjtree_initeach_test sms-native-sjis)
    set_target_properties(platform-mapobjtree_initeach_test PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(platform-mapobjtree_initeach_test PRIVATE
        -w -ffunction-sections -fdata-sections)
    target_compile_definitions(platform-mapobjtree_initeach_test PRIVATE
        TARGET_PC=1 SMS_NATIVE_PLATFORM=1 SMS_AURORA=1)
    target_include_directories(platform-mapobjtree_initeach_test PRIVATE
        "${SMS_NATIVE_REPO_ROOT}/extern/aurora/include"
        "${SMS_INCLUDE_STAGED}")
    target_link_options(platform-mapobjtree_initeach_test PRIVATE
        "-Wl,--gc-sections")
    add_test(NAME platform_mapobjtree_initeach_test COMMAND platform-mapobjtree_initeach_test)

    # -----------------------------------------------------------------------
    # bathtub_tumble_test — pins TBathtub::tumble's gated angle/strength
    # impulse from GMSE01 0x801fb568. As above, direct compilation plus section
    # GC keeps the test on the shipping function without pulling the actor graph.
    # -----------------------------------------------------------------------
    add_executable(platform-bathtub_tumble_test
        "${SMS_NATIVE_REPO_ROOT}/sms-boot/runtime/tests/bathtub_tumble_test.cpp"
        "${SMS_SOURCE_STAGED}/MoveBG/MapObjCorona.cpp")
    add_dependencies(platform-bathtub_tumble_test sms-native-sjis)
    set_target_properties(platform-bathtub_tumble_test PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(platform-bathtub_tumble_test PRIVATE
        -w -ffunction-sections -fdata-sections)
    target_compile_definitions(platform-bathtub_tumble_test PRIVATE
        TARGET_PC=1 SMS_NATIVE_PLATFORM=1 SMS_AURORA=1)
    target_include_directories(platform-bathtub_tumble_test PRIVATE
        "${SMS_NATIVE_REPO_ROOT}/extern/aurora/include"
        "${SMS_INCLUDE_STAGED}")
    target_link_options(platform-bathtub_tumble_test PRIVATE
        "-Wl,--gc-sections")
    add_test(NAME platform_bathtub_tumble_test COMMAND platform-bathtub_tumble_test)
endif()
