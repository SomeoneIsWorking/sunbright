cmake_minimum_required(VERSION 3.20)
project(sunbright CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ───────────────────────────────────────────────────────────────────
option(SUNBRIGHT_USE_DOLPHIN_DISCIO  "Link recompiler against Dolphin DiscIO" ON)
option(SUNBRIGHT_USE_DOLPHIN_CORE    "Link runtime against Dolphin Core"      ON)

# ── Dolphin submodule ─────────────────────────────────────────────────────────
set(DOLPHIN_DIR "${CMAKE_SOURCE_DIR}/externals/dolphin")

if(NOT EXISTS "${DOLPHIN_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Dolphin submodule not found.\n"
        "Run: git submodule update --init --recursive externals/dolphin"
    )
endif()

# Suppress Dolphin's own executables/tests — we only need its libraries
set(DOLPHIN_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(ENABLE_QT                OFF CACHE BOOL "" FORCE)
set(ENABLE_WXWIDGETS         OFF CACHE BOOL "" FORCE)
set(ENABLE_SDL               OFF CACHE BOOL "" FORCE)
set(ENABLE_EVDEV             OFF CACHE BOOL "" FORCE)
set(ENABLE_BLUEZ             OFF CACHE BOOL "" FORCE)
set(ENABLE_PULSEAUDIO        OFF CACHE BOOL "" FORCE)
set(ENABLE_ALSA              OFF CACHE BOOL "" FORCE)
set(USE_UPNP                 OFF CACHE BOOL "" FORCE)
set(USE_MGBA                 OFF CACHE BOOL "" FORCE)
set(USE_DISCORD_PRESENCE     OFF CACHE BOOL "" FORCE)
set(ENABLE_AUTOUPDATE        OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS             OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS        OFF CACHE BOOL "" FORCE)

# When Dolphin is a subdirectory, ${CMAKE_SOURCE_DIR} in its Vulkan backend
# resolves to our root, not Dolphin's. Add the correct VMA path globally.
include_directories(BEFORE
    "${DOLPHIN_DIR}/Externals/VulkanMemoryAllocator/include"
    "${DOLPHIN_DIR}/Externals/Vulkan-Headers/include"
)

add_subdirectory("${DOLPHIN_DIR}" dolphin EXCLUDE_FROM_ALL)

# ── Recompiler tool ───────────────────────────────────────────────────────────
file(GLOB_RECURSE RECOMP_SOURCES CONFIGURE_DEPENDS tools/recompiler/*.cpp)
list(FILTER RECOMP_SOURCES EXCLUDE REGEX "tools/recompiler/tests/")   # tests have their own main()
add_executable(sunbright-recomp ${RECOMP_SOURCES})

target_include_directories(sunbright-recomp PRIVATE
    tools/recompiler
    runtime
    "${DOLPHIN_DIR}/Source/Core"
    "${CMAKE_BINARY_DIR}/dolphin/Source/Core"  # for generated headers
)

target_compile_definitions(sunbright-recomp PRIVATE
    $<$<BOOL:${SUNBRIGHT_USE_DOLPHIN_DISCIO}>:HAVE_DOLPHIN_DISCIO=1>
)

if(SUNBRIGHT_USE_DOLPHIN_DISCIO)
    target_link_libraries(sunbright-recomp PRIVATE discio core)
endif()

target_compile_options(sunbright-recomp PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

# ── Jingle extractor (PC-native ROM audio proof tool) ────────────────────────
add_executable(sunbright-jingle tools/jingle/jingle_extract.cpp tools/recompiler/host_stubs.cpp)
target_include_directories(sunbright-jingle PRIVATE
    "${DOLPHIN_DIR}/Source/Core"
    "${CMAKE_BINARY_DIR}/dolphin/Source/Core"
)
target_link_libraries(sunbright-jingle PRIVATE discio)

# ── Recompiler unit tests (sunbright-recomp-test) ────────────────────────────
# Self-contained: only the decode/emit/collect units, no disc/Dolphin I/O.
add_executable(sunbright-recomp-test
    tools/recompiler/tests/recomp_test.cpp
    tools/recompiler/func_collect.cpp
    tools/recompiler/ppc_decoder.cpp
    tools/recompiler/ppc_mnemonic.cpp
    tools/recompiler/c_emitter.cpp
)
target_include_directories(sunbright-recomp-test PRIVATE tools/recompiler runtime)
target_compile_options(sunbright-recomp-test PRIVATE -Wall -Wextra -Wno-unused-parameter)
enable_testing()
add_test(NAME recomp_test COMMAND sunbright-recomp-test)

# Seeded type-recovery unit tests (the tailored-recomp boundary's field-access half).
add_executable(sunbright-type-recovery-test
    tools/recompiler/tests/type_recovery_test.cpp
    tools/recompiler/type_recovery.cpp
    tools/recompiler/func_collect.cpp
    tools/recompiler/ppc_decoder.cpp
    tools/recompiler/ppc_mnemonic.cpp
)
target_include_directories(sunbright-type-recovery-test PRIVATE tools/recompiler runtime)
target_compile_options(sunbright-type-recovery-test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME type_recovery_test COMMAND sunbright-type-recovery-test)

# GameCube/host ABI offset computation (the type-DB construction core).
add_executable(sunbright-abi-layout-test
    tools/recompiler/tests/abi_layout_test.cpp
    tools/recompiler/abi_layout.cpp
)
target_include_directories(sunbright-abi-layout-test PRIVATE tools/recompiler runtime)
target_compile_options(sunbright-abi-layout-test PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-invalid-offsetof)
add_test(NAME abi_layout_test COMMAND sunbright-abi-layout-test)

# Decomp header field-list parser (front end of the tailored-recomp type DB): parses real
# reference/sms headers and validates the ABI engine against their annotated offsets at scale.
add_executable(sunbright-decomp-parse-test
    tools/recompiler/tests/decomp_parse_test.cpp
    tools/recompiler/decomp_parse.cpp
    tools/recompiler/abi_layout.cpp
)
target_include_directories(sunbright-decomp-parse-test PRIVATE tools/recompiler runtime)
target_compile_options(sunbright-decomp-parse-test PRIVATE -Wall -Wextra -Wno-unused-parameter)
# Runs from the repo root so the corpus sweep can read reference/sms/include.
add_test(NAME decomp_parse_test COMMAND sunbright-decomp-parse-test
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# GNU-v2 demangler -> this/param engine-type seeds (the signature half of the type DB).
add_executable(sunbright-func-sig-test
    tools/recompiler/tests/func_sig_test.cpp
    tools/recompiler/func_sig.cpp
)
target_include_directories(sunbright-func-sig-test PRIVATE tools/recompiler runtime)
target_compile_options(sunbright-func-sig-test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME func_sig_test COMMAND sunbright-func-sig-test
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# End-to-end COMPLETE-coverage proof on REAL game code (the de-risk #2 gate): full auto pipeline
# (header parse -> layout, demangle -> signature, DOL decode, CFG recovery) vs the decomp source.
# SKIPs cleanly when the extracted DOL (gitignored) is absent, so a ROM-less checkout still passes.
add_executable(sunbright-coverage-real-test
    tools/recompiler/tests/coverage_real_test.cpp
    tools/recompiler/dol_parser.cpp
    tools/recompiler/ppc_decoder.cpp
    tools/recompiler/ppc_mnemonic.cpp
    tools/recompiler/func_collect.cpp
    tools/recompiler/decomp_parse.cpp
    tools/recompiler/abi_layout.cpp
    tools/recompiler/func_sig.cpp
    tools/recompiler/type_recovery.cpp
    tools/recompiler/c_emitter.cpp
)
target_include_directories(sunbright-coverage-real-test PRIVATE tools/recompiler runtime)
target_compile_options(sunbright-coverage-real-test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME coverage_real_test COMMAND sunbright-coverage-real-test
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ── Launcher executable (SDL2 window + Dolphin Core) ─────────────────────────
# Everything — launcher, runtime bridge, hand-written overrides, and the
# statically-recompiled game code — is compiled into one self-contained binary.
# No shared libraries, no dlopen, no dynamic-symbol resolution to get wrong.
find_package(SDL2 REQUIRED)

# Hand-written native overrides for functions the recompiler can't reproduce.
# CONFIGURE_DEPENDS: re-glob at build time — a NEW override file must never be
# silently left out of the binary (cost: the committed TAfterEffect/EfbCtrlTex
# widescreen fixes 26556fa never ran; the "fixed" smear shipped broken).
file(GLOB SUNBRIGHT_OVERRIDE_SOURCES CONFIGURE_DEPENDS runtime/overrides/*.cpp)

# Generated game code (present after /recompile). The recompiler splits function
# bodies across many address-bucketed generated/functions_*.cpp files (plus a
# shared functions.h and jump_table.cpp) so they compile in parallel. The weak
# fallback table lets the binary link even before generation; the generated
# strong symbols win.
file(GLOB SUNBRIGHT_GENERATED_SOURCES CONFIGURE_DEPENDS
    generated/functions_*.cpp
    generated/jump_table.cpp
)
if(SUNBRIGHT_GENERATED_SOURCES)
    # Generated code benefits from aggressive optimization, but NOT -ffast-math:
    # that uses reciprocal approximations for divides and reassociates, diverging
    # from the GameCube's IEEE FP. -ffp-contract=off stops the compiler fusing a
    # separate fmul+fadd into an FMA (Gekko rounds those twice; only fmadd is fused,
    # which the emitter handles explicitly via std::fma).
    set_source_files_properties(${SUNBRIGHT_GENERATED_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-O3;-ffp-contract=off")
endif()

add_executable(sunbright
    runtime/main_sdl.cpp
    runtime/jit_hook.cpp
    runtime/sunbright_hooks.cpp
    runtime/sunbright_bridge.cpp
    runtime/dolphin_hook.cpp
    runtime/memory_bridge.cpp
    runtime/gx_stream.cpp
    runtime/gx_parse.cpp
    runtime/ngx/ngx_decode.cpp
    runtime/ngx/ngx_vertex.cpp
    runtime/ngx/ngx_mesh.cpp
    runtime/render/tex_decode.cpp
    runtime/render/tex_decode_selftest.cpp
    runtime/render/vk_quad.cpp
    runtime/render/vk_mesh.cpp
    runtime/render/j2d_walk.cpp
    runtime/render/j2d_render.cpp
    runtime/gpfifo_wrap.cpp
    runtime/native_threads.cpp
    runtime/native_os.cpp
    runtime/os_hle.cpp
    runtime/overrides.cpp
    runtime/probe_server.cpp
    runtime/watchdog.cpp
    runtime/sb_spin.cpp
    runtime/pe_token_wrap.cpp
    runtime/coretiming_trace.cpp
    runtime/mixer_trace.cpp
    runtime/native_audio.cpp
    runtime/native_jas.cpp
    runtime/vpb_trace.cpp
    runtime/recomp_table_weak.cpp
    ${SUNBRIGHT_OVERRIDE_SOURCES}
    ${SUNBRIGHT_GENERATED_SOURCES}
)

target_include_directories(sunbright PRIVATE
    runtime
    "${DOLPHIN_DIR}/Source/Core"
    "${CMAKE_BINARY_DIR}/dolphin/Source/Core"
    "${DOLPHIN_DIR}/Externals/fmt/fmt/include"
)

# Mirror Dolphin's target-arch define (it sets these via add_definitions) for our target. Must match
# the host arch: _M_ARM_64 on Apple Silicon / aarch64, _M_X86_64 on x86-64. Hardcoding x86_64 breaks
# the arm64 (macOS) build.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    set(_SUNBRIGHT_ARCH_DEFINE _M_ARM_64=1)
else()
    set(_SUNBRIGHT_ARCH_DEFINE _M_X86_64=1)
endif()

target_compile_definitions(sunbright PRIVATE
    HAVE_DOLPHIN_CORE=1
    HAVE_DOLPHIN_DISCIO=1
    HAVE_DOLPHIN_MEMMAP=1  # memory_bridge routes through Dolphin's real MemMap
    ${_SUNBRIGHT_ARCH_DEFINE}
)

target_link_libraries(sunbright PRIVATE
    core discio uicommon
    videovulkan          # N2: native renderer reuses Dolphin's VkDevice during bring-up
    SDL2::SDL2
    pthread
)

# JitTrampoline interception is now a direct fork hook (Common/SunbrightHooks.h
# sb_slot_jit_trampoline, set in JitCommon/JitBase.cpp, installed by sb_install_hooks() /
# runtime/sunbright_hooks.cpp). No linker --wrap remains — the build links under ld64 (macOS/arm64)
# as well as GNU ld. See docs/re_notes/wrap_removal.md.
# (macOS frameworks (Cocoa/Metal/QuartzCore/IOKit) come transitively from the Dolphin core/uicommon
# libs and SDL2, so no explicit framework links are needed here.)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS sunbright DESTINATION bin)
install(TARGETS sunbright-recomp DESTINATION bin)

message(STATUS "Sunbright build configured")
message(STATUS "  Dolphin DiscIO: ${SUNBRIGHT_USE_DOLPHIN_DISCIO}")
message(STATUS "  Dolphin Core:   ${SUNBRIGHT_USE_DOLPHIN_CORE}")
