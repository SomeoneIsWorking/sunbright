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

# ── Renderer unit tests (sunbright-render-test) ──────────────────────────────
# Bottom-up TDD for the native renderer: each pure unit asserts spec-computed
# ground truth (hand-derived expected values), Dolphin-free / no ROM / no GPU.
add_executable(sunbright-render-test
    runtime/render/render_test.cpp
    runtime/ngx/ngx_vertex.cpp
    runtime/ngx/ngx_decode.cpp
    runtime/render/tex_decode.cpp
)
target_include_directories(sunbright-render-test PRIVATE runtime runtime/ngx runtime/render)
target_compile_options(sunbright-render-test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME render_test COMMAND sunbright-render-test)

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

# The static recompiler is ERADICATED from the game binary (2026-06-18, Phase C, user
# directive no-recomp-jit-native-direction): gameplay runs on Dolphin's JIT and the engine
# runs as native overrides. The generated/functions_*.cpp + jump_table.cpp are therefore NO
# LONGER LINKED into `sunbright` — recomp_table_weak.cpp's weak-empty g_recomp_table is the
# only table, so recomp_lookup/recomp_raw always miss (they already return null at runtime).
# The `sunbright-recomp` tool target below is KEPT as OFFLINE static-analysis tooling (decode
# the ROM / disassemble PPC); it just no longer feeds the game.

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
    runtime/render/ngx_present.cpp
    runtime/render/tev_shader.cpp
    runtime/render/glsl_compile.cpp
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
    runtime/recomp_dead_stubs.cpp
    ${SUNBRIGHT_OVERRIDE_SOURCES}
)

target_include_directories(sunbright PRIVATE
    runtime
    "${DOLPHIN_DIR}/Source/Core"
    "${CMAKE_BINARY_DIR}/dolphin/Source/Core"
    "${DOLPHIN_DIR}/Externals/fmt/fmt/include"
    "${DOLPHIN_DIR}/Externals/glslang/glslang"   # N5: own GLSL->SPIR-V (runtime/render/glsl_compile)
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
    glslang SPIRV glslang-default-resource-limits  # N5: own GLSL->SPIR-V (glslang targets)
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
