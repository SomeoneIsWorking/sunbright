# sms-native — the Super Mario Sunshine decomp (reference/sms) compiled as a
# NATIVE PC static library. No GameCube, no Dolphin, no PPC, no recompiler.
#
# This is the phase-2 foundation (see native/platform/README.md and
# debug_journal/2026-06-20_native_engine_bringup_feasibility.md). Phase-1 proved
# all 577 game-logic .cpp compile to objects on a 64-bit host with two shims
# force-included; this target archives them into libsms-native.a. The GameCube
# SDK/hardware seams the game calls (#include <dolphin/...>) are implemented in
# native/platform (TODO bodies for now) — so this library has UNRESOLVED SDK
# symbols by design until the seams + an engine main() link against it.
#
# Standalone build (no Dolphin needed — pure syntax/codegen of the game logic):
#   cmake -S native -B build-native -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-native -j$(nproc) --target sms-native
#
# Or, when included from the top-level project via add_subdirectory(native),
# the `sms-native` target is available there too.

cmake_minimum_required(VERSION 3.20)

# Allow standalone configuration (native/ as its own source dir).
if(NOT DEFINED PROJECT_NAME)
    project(sms-native CXX)
endif()

# Repo root: when standalone, CMAKE_CURRENT_SOURCE_DIR is <repo>/native.
set(SMS_NATIVE_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Game-logic sources = reference/sms/src/**.cpp MINUS the GC SDK/CRT dirs, which
# the platform seams replace (dolphin/, PowerPC_EABI_Support/, TRK_MINNOW_DOLPHIN/).
file(GLOB_RECURSE SMS_NATIVE_SOURCES CONFIGURE_DEPENDS
     "${SMS_NATIVE_REPO_ROOT}/reference/sms/src/*.cpp")
list(FILTER SMS_NATIVE_SOURCES EXCLUDE REGEX
     "/reference/sms/src/(dolphin|PowerPC_EABI_Support|TRK_MINNOW_DOLPHIN)/")

add_library(sms-native STATIC ${SMS_NATIVE_SOURCES})

# C++17 specifically (the decomp is C++17; the top-level sunbright build is C++23,
# so pin this per-target rather than inherit).
set_target_properties(sms-native PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    # MUST be strict ISO C++17 (-std=c++17), NOT GNU (-std=gnu++17, CMake's
    # default): in GNU mode `typeof` is a keyword, but the decomp uses it as a
    # method name (Strategic/spcinterp.hpp `u32 typeof() const`) -> parse error.
    CXX_EXTENSIONS OFF)

target_include_directories(sms-native PUBLIC
    "${SMS_NATIVE_REPO_ROOT}/native/shim"
    "${SMS_NATIVE_REPO_ROOT}/reference/sms/include")

target_compile_options(sms-native PRIVATE
    # Force-include the portable Gekko intrinsics + fixed-width types shim.
    -include "${SMS_NATIVE_REPO_ROOT}/native/shim/gekko_intrinsics.h"
    # The decomp predates C++11 narrowing strictness (Metrowerks allowed it).
    # This silences brace-init narrowing ONLY; pointer truncation still errors
    # as 'loses precision'. -w hides the rest of the noise (warnings, not errors).
    -w
    -Wno-narrowing
    # SDK headers declare OS/HW globals as tentative defs without extern; -fcommon
    # merges them (Metrowerks behavior) so the eventual full link doesn't hit
    # -fno-common multiple-definition errors across the 577 TUs.
    -fcommon)

# ---------------------------------------------------------------------------
# sms-platform — the native PC implementations of the GameCube SDK seams the
# game links (#include <dolphin/...>). Phase-2 work (native/platform/README.md).
# Grows seam by seam; each <sub>_impl.cpp defines the unresolved SDK symbols for
# one subsystem (MTX/OS/DVD/VI/GX/...). Built clean (-Wall -Wextra), real C++ —
# these are NEW native code, not decomp, so they get full warnings.
# ---------------------------------------------------------------------------
file(GLOB SMS_PLATFORM_SOURCES CONFIGURE_DEPENDS
     "${SMS_NATIVE_REPO_ROOT}/native/platform/*_impl.cpp")

find_package(Threads REQUIRED)

add_library(sms-platform STATIC ${SMS_PLATFORM_SOURCES})
set_target_properties(sms-platform PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sms-platform PUBLIC
    "${SMS_NATIVE_REPO_ROOT}/native/platform"
    "${SMS_NATIVE_REPO_ROOT}/native/shim"
    "${SMS_NATIVE_REPO_ROOT}/reference/sms/include")
target_compile_options(sms-platform PRIVATE -Wall -Wextra
    # The SDK headers declare HW-register/OS globals (__VIRegs, __OSBusClock, ...)
    # as tentative definitions WITHOUT extern; -fcommon merges them (Metrowerks
    # common-symbol behavior) instead of -fno-common's multiple-definition error.
    -fcommon)
target_link_libraries(sms-platform PUBLIC Threads::Threads)

# Seam unit tests (TDD: assert spec-computed truth; the tested function IS the
# shipping seam function). Each native/platform/tests/*_test.cpp is its own
# assert-main executable, registered with ctest as platform_<name>.
enable_testing()

# ---------------------------------------------------------------------------
# sms-render — the standalone native Vulkan renderer (no Dolphin, headless offscreen).
# Compiles GLSL shaders to embedded SPIR-V headers (glslangValidator --vn) and builds
# the from-scratch Vulkan core (nvk.cpp). This is the foundation the full native SMS
# renderer (J3D meshes + GXState) plugs into. Skipped if Vulkan/glslang is unavailable.
# ---------------------------------------------------------------------------
find_library(SB_VULKAN_LIB NAMES vulkan)
find_path(SB_VULKAN_INC NAMES vulkan/vulkan.h)
find_program(SB_GLSLANG NAMES glslangValidator)
if(SB_VULKAN_LIB AND SB_VULKAN_INC AND SB_GLSLANG)
    set(SB_SHADER_GEN "${CMAKE_BINARY_DIR}/gen_shaders")
    file(MAKE_DIRECTORY "${SB_SHADER_GEN}")
    set(SB_SHADER_DIR "${SMS_NATIVE_REPO_ROOT}/native/render/shaders")
    add_custom_command(
        OUTPUT "${SB_SHADER_GEN}/tri_vert_spv.h"
        COMMAND "${SB_GLSLANG}" -V --vn tri_vert_spv "${SB_SHADER_DIR}/tri.vert"
                -o "${SB_SHADER_GEN}/tri_vert_spv.h"
        DEPENDS "${SB_SHADER_DIR}/tri.vert" VERBATIM)
    add_custom_command(
        OUTPUT "${SB_SHADER_GEN}/tri_frag_spv.h"
        COMMAND "${SB_GLSLANG}" -V --vn tri_frag_spv "${SB_SHADER_DIR}/tri.frag"
                -o "${SB_SHADER_GEN}/tri_frag_spv.h"
        DEPENDS "${SB_SHADER_DIR}/tri.frag" VERBATIM)

    add_library(sms-render STATIC
        "${SMS_NATIVE_REPO_ROOT}/native/render/nvk.cpp"
        # Reused PURE ngx geometry decoders (NOT forked — these ARE the shipping
        # renderer's GC display-list -> native-mesh path), compiled Dolphin-free via
        # the gx_parse.h shadow below.
        "${SMS_NATIVE_REPO_ROOT}/runtime/ngx/ngx_decode.cpp"
        "${SMS_NATIVE_REPO_ROOT}/runtime/ngx/ngx_vertex.cpp"
        "${SMS_NATIVE_REPO_ROOT}/runtime/ngx/ngx_mesh.cpp"
        "${SB_SHADER_GEN}/tri_vert_spv.h"
        "${SB_SHADER_GEN}/tri_frag_spv.h")
    set_target_properties(sms-render PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(sms-render PUBLIC
        # shim/ FIRST so ngx_decode's #include "gx_parse.h" gets the Dolphin-free shadow.
        "${SMS_NATIVE_REPO_ROOT}/native/render/shim"
        "${SMS_NATIVE_REPO_ROOT}/native/render"
        "${SMS_NATIVE_REPO_ROOT}/runtime/ngx"
        "${SB_SHADER_GEN}" "${SB_VULKAN_INC}")
    target_link_libraries(sms-render PUBLIC "${SB_VULKAN_LIB}")
    target_compile_options(sms-render PRIVATE -Wall -Wextra)

    # Render tests (e.g. tests/render/*_test.cpp) link sms-render. Headless; a frame
    # render+readback is verified by pixel checks (lavapipe works with no GPU).
    file(GLOB SB_RENDER_TESTS CONFIGURE_DEPENDS
         "${SMS_NATIVE_REPO_ROOT}/native/render/tests/*_test.cpp")
    foreach(rt ${SB_RENDER_TESTS})
        get_filename_component(rt_name "${rt}" NAME_WE)
        add_executable(sms-${rt_name} "${rt}")
        set_target_properties(sms-${rt_name} PROPERTIES
            CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        target_link_libraries(sms-${rt_name} PRIVATE sms-render sms-platform)
        add_test(NAME render_${rt_name} COMMAND sms-${rt_name})
    endforeach()
else()
    message(STATUS "sms-render: Vulkan/glslang not found; skipping native renderer")
endif()

file(GLOB SMS_PLATFORM_TESTS CONFIGURE_DEPENDS
     "${SMS_NATIVE_REPO_ROOT}/native/platform/tests/*_test.cpp")
foreach(test_src ${SMS_PLATFORM_TESTS})
    get_filename_component(test_name "${test_src}" NAME_WE)
    add_executable(sms-${test_name} "${test_src}")
    set_target_properties(sms-${test_name} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(sms-${test_name} PRIVATE
        "${SMS_NATIVE_REPO_ROOT}/native/platform"
        "${SMS_NATIVE_REPO_ROOT}/native/shim"
        "${SMS_NATIVE_REPO_ROOT}/reference/sms/include")
    target_compile_options(sms-${test_name} PRIVATE -fcommon)
    target_link_libraries(sms-${test_name} PRIVATE sms-platform)
    add_test(NAME platform_${test_name} COMMAND sms-${test_name})
endforeach()
