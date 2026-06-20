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

# ---------------------------------------------------------------------------
# sms-gd — the GameCube GD (GX Display-list builder) library, compiled from the
# REAL decomp source (reference/sms/src/dolphin/gd/*.c), NOT reimplemented. GD is
# pure big-endian byte-building into a memory buffer (no hardware), so it ports
# verbatim. The J3D loader uses it at load time to bake each shape's VCD/VAT
# command buffer (J3DShape::makeVcdVatCmd) and material display lists. Compiled
# like sms-native (shim force-included, -w, -fcommon) since it is decomp source.
# Its only external dep is DCFlushRange (cache no-op in os_impl.cpp).
# ---------------------------------------------------------------------------
file(GLOB SMS_GD_SOURCES CONFIGURE_DEPENDS
     "${SMS_NATIVE_REPO_ROOT}/reference/sms/src/dolphin/gd/*.c")
add_library(sms-gd STATIC ${SMS_GD_SOURCES})
set_target_properties(sms-gd PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
    # .c decomp sources are GC C; compile them as C++ (the shim + headers assume it).
    LINKER_LANGUAGE CXX)
set_source_files_properties(${SMS_GD_SOURCES} PROPERTIES LANGUAGE CXX)
target_include_directories(sms-gd PUBLIC
    "${SMS_NATIVE_REPO_ROOT}/native/shim"
    "${SMS_NATIVE_REPO_ROOT}/reference/sms/include")
target_compile_options(sms-gd PRIVATE
    -include "${SMS_NATIVE_REPO_ROOT}/native/shim/gekko_intrinsics.h"
    -w -Wno-narrowing -fcommon)
target_link_libraries(sms-gd PUBLIC sms-platform)

# ---------------------------------------------------------------------------
# sms-assets — the native BE->host asset byteswap layer. GameCube on-disk assets
# are big-endian; the decomp loaders read multibyte fields by casting raw file
# bytes to C structs, which misreads on a little-endian host. This layer produces
# a HOST-ENDIAN copy of a BMD/BDL model ("swap at load"), so the pristine decomp
# J3DModelLoader reads it correctly. Recovered from the retired port/ tree (memory
# `field-flip-removed` flagged it reusable); plain portable C++, no shim, no decomp
# headers. Includes the Yaz0/RARC reader (rarc.cpp) for archive extraction.
# ---------------------------------------------------------------------------
add_library(sms-assets STATIC
    "${SMS_NATIVE_REPO_ROOT}/native/assets/bmd_swap.cpp"
    "${SMS_NATIVE_REPO_ROOT}/native/assets/rarc.cpp")
set_target_properties(sms-assets PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sms-assets PUBLIC
    "${SMS_NATIVE_REPO_ROOT}/native/assets")
target_compile_options(sms-assets PRIVATE -Wall -Wextra)

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
    add_custom_command(
        OUTPUT "${SB_SHADER_GEN}/tex_vert_spv.h"
        COMMAND "${SB_GLSLANG}" -V --vn tex_vert_spv "${SB_SHADER_DIR}/tex.vert"
                -o "${SB_SHADER_GEN}/tex_vert_spv.h"
        DEPENDS "${SB_SHADER_DIR}/tex.vert" VERBATIM)
    add_custom_command(
        OUTPUT "${SB_SHADER_GEN}/tex_frag_spv.h"
        COMMAND "${SB_GLSLANG}" -V --vn tex_frag_spv "${SB_SHADER_DIR}/tex.frag"
                -o "${SB_SHADER_GEN}/tex_frag_spv.h"
        DEPENDS "${SB_SHADER_DIR}/tex.frag" VERBATIM)
    # TEV path: a fixed vertex shader (embedded) feeds the runtime-generated TEV
    # fragment shader (tev_shader.cpp -> glsl_compile.cpp, compiled per material).
    add_custom_command(
        OUTPUT "${SB_SHADER_GEN}/tev_vert_spv.h"
        COMMAND "${SB_GLSLANG}" -V --vn tev_vert_spv "${SB_SHADER_DIR}/tev.vert"
                -o "${SB_SHADER_GEN}/tev_vert_spv.h"
        DEPENDS "${SB_SHADER_DIR}/tev.vert" VERBATIM)

    # System glslang for the RUNTIME GLSL->SPIR-V compiler (glsl_compile.cpp). This is
    # the same shipping wrapper the Dolphin build uses; here it links the standalone
    # system glslang (Fedora glslang-devel). Prefer the CONFIG package (namespaced
    # targets, transitive deps); fall back to plain -l names.
    find_package(glslang CONFIG QUIET)
    if(TARGET glslang::SPIRV)
        set(SB_NATIVE_GLSLANG_LIBS glslang::SPIRV glslang::glslang
            glslang::glslang-default-resource-limits)
    else()
        set(SB_NATIVE_GLSLANG_LIBS glslang glslang-default-resource-limits SPIRV
            SPIRV-Tools-opt SPIRV-Tools)
    endif()

    add_library(sms-render STATIC
        "${SMS_NATIVE_REPO_ROOT}/native/render/nvk.cpp"
        "${SMS_NATIVE_REPO_ROOT}/native/render/ngx_native_glue.cpp"
        # Reused SHIPPING TEV combiner generator + runtime GLSL->SPIR-V compiler (NOT
        # forked) — the same code the ngx renderer uses, Dolphin-free here.
        "${SMS_NATIVE_REPO_ROOT}/runtime/render/tev_shader.cpp"
        "${SMS_NATIVE_REPO_ROOT}/runtime/render/glsl_compile.cpp"
        # Reused PURE ngx geometry decoders (NOT forked — these ARE the shipping
        # renderer's GC display-list -> native-mesh path), compiled Dolphin-free via
        # the gx_parse.h shadow below.
        "${SMS_NATIVE_REPO_ROOT}/runtime/ngx/ngx_decode.cpp"
        "${SMS_NATIVE_REPO_ROOT}/runtime/ngx/ngx_vertex.cpp"
        "${SMS_NATIVE_REPO_ROOT}/runtime/ngx/ngx_mesh.cpp"
        "${SMS_NATIVE_REPO_ROOT}/runtime/render/tex_decode.cpp"
        "${SB_SHADER_GEN}/tri_vert_spv.h"
        "${SB_SHADER_GEN}/tri_frag_spv.h"
        "${SB_SHADER_GEN}/tex_vert_spv.h"
        "${SB_SHADER_GEN}/tex_frag_spv.h"
        "${SB_SHADER_GEN}/tev_vert_spv.h")
    set_target_properties(sms-render PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(sms-render PUBLIC
        # shim/ FIRST so ngx_decode's #include "gx_parse.h" gets the Dolphin-free shadow.
        "${SMS_NATIVE_REPO_ROOT}/native/render/shim"
        "${SMS_NATIVE_REPO_ROOT}/native/render"
        "${SMS_NATIVE_REPO_ROOT}/runtime/ngx"
        "${SMS_NATIVE_REPO_ROOT}/runtime/render"
        "${SB_SHADER_GEN}" "${SB_VULKAN_INC}")
    target_link_libraries(sms-render PUBLIC "${SB_VULKAN_LIB}" ${SB_NATIVE_GLSLANG_LIBS})
    # tev_shader.cpp / glsl_compile.cpp are SHIPPING ngx code (predates strict warnings);
    # -Wall -Wextra applies to the new nvk code, but keep the reused TUs from erroring.
    target_compile_options(sms-render PRIVATE -Wall -Wextra)
    set_source_files_properties(
        "${SMS_NATIVE_REPO_ROOT}/runtime/render/tev_shader.cpp"
        "${SMS_NATIVE_REPO_ROOT}/runtime/render/glsl_compile.cpp"
        PROPERTIES COMPILE_OPTIONS "-w")

    # Render tests (e.g. tests/render/*_test.cpp) link sms-render. Headless; a frame
    # render+readback is verified by pixel checks (lavapipe works with no GPU).
    # j3dload_test is handled separately below (it links the full game-logic loader,
    # not sms-render, and must build even without Vulkan).
    file(GLOB SB_RENDER_TESTS CONFIGURE_DEPENDS
         "${SMS_NATIVE_REPO_ROOT}/native/render/tests/*_test.cpp")
    list(FILTER SB_RENDER_TESTS EXCLUDE REGEX "/j3dload_test\\.cpp$")
    list(FILTER SB_RENDER_TESTS EXCLUDE REGEX "/j3dmesh_test\\.cpp$")
    foreach(rt ${SB_RENDER_TESTS})
        get_filename_component(rt_name "${rt}" NAME_WE)
        add_executable(sms-${rt_name} "${rt}")
        set_target_properties(sms-${rt_name} PROPERTIES
            CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        target_link_libraries(sms-${rt_name} PRIVATE sms-render sms-platform)
        add_test(NAME render_${rt_name} COMMAND sms-${rt_name})
    endforeach()

    # j3dmesh_test — render a REAL BMD shape (loaded via the game's J3D loader) through
    # nvk. Needs the full loader (sms-native/gd/assets) AND the renderer (sms-render).
    add_executable(sms-j3dmesh_test
        "${SMS_NATIVE_REPO_ROOT}/native/render/tests/j3dmesh_test.cpp")
    set_target_properties(sms-j3dmesh_test PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(sms-j3dmesh_test PRIVATE
        "${SMS_NATIVE_REPO_ROOT}/native/shim"
        "${SMS_NATIVE_REPO_ROOT}/reference/sms/include"
        "${SMS_NATIVE_REPO_ROOT}/native/assets"
        "${SMS_NATIVE_REPO_ROOT}/native/render"
        "${SMS_NATIVE_REPO_ROOT}/runtime/ngx"
        "${SMS_NATIVE_REPO_ROOT}/runtime/render")
    target_compile_options(sms-j3dmesh_test PRIVATE
        -include "${SMS_NATIVE_REPO_ROOT}/native/shim/gekko_intrinsics.h"
        -w -Wno-narrowing -fcommon)
    target_link_libraries(sms-j3dmesh_test PRIVATE
        sms-render
        "$<LINK_GROUP:RESCAN,sms-native,sms-gd,sms-assets,sms-platform>"
        Threads::Threads)
    add_test(NAME native_j3dmesh_test COMMAND sms-j3dmesh_test
             WORKING_DIRECTORY "${SMS_NATIVE_REPO_ROOT}")
else()
    message(STATUS "sms-render: Vulkan/glslang not found; skipping native renderer")
endif()

# ---------------------------------------------------------------------------
# j3dload_test — the PRODUCER milestone: load a REAL BMD through the game's OWN
# J3D loader against native structs (no Dolphin/PPC/GC), verify shape/material/
# joint counts. Links the full game-logic loader (sms-native) + GD display-list
# builder (sms-gd) + BE->host asset swapper (sms-assets) + platform seams. Builds
# even without Vulkan (no GPU needed — this verifies the producer, not pixels).
# SKIP-passes when the copyrighted BMD asset is absent (gitignored scratch/bmd/).
# ---------------------------------------------------------------------------
add_executable(sms-j3dload_test
    "${SMS_NATIVE_REPO_ROOT}/native/render/tests/j3dload_test.cpp")
set_target_properties(sms-j3dload_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sms-j3dload_test PRIVATE
    "${SMS_NATIVE_REPO_ROOT}/native/shim"
    "${SMS_NATIVE_REPO_ROOT}/reference/sms/include"
    "${SMS_NATIVE_REPO_ROOT}/native/assets")
target_compile_options(sms-j3dload_test PRIVATE
    -include "${SMS_NATIVE_REPO_ROOT}/native/shim/gekko_intrinsics.h"
    -w -Wno-narrowing -fcommon)
# sms-native <-> sms-gd/sms-platform have cyclic cross-references (game code calls
# seams; nothing the reverse, but vtables pull a wide closure) — RESCAN groups the
# archives so the linker iterates until all refs resolve, regardless of order.
target_link_libraries(sms-j3dload_test PRIVATE
    "$<LINK_GROUP:RESCAN,sms-native,sms-gd,sms-assets,sms-platform>"
    Threads::Threads)
# Run from the repo root so the default scratch/bmd/ asset path resolves.
add_test(NAME native_j3dload_test COMMAND sms-j3dload_test
         WORKING_DIRECTORY "${SMS_NATIVE_REPO_ROOT}")

# ---------------------------------------------------------------------------
# sms-boot — the native bring-up executable: PlatformInit (--wrap=main) -> the game's
# own main() (TApplication). Won't LINK until the remaining seams + game C++ stubs
# resolve; the link failure reveals the true critical-path stub set. Opt-in (it is
# EXPECTED to fail-link today) so it never breaks the default build/ctest.
# ---------------------------------------------------------------------------
option(SMS_BUILD_BOOT "Build the native bring-up exe (expected to fail-link until stubs land)" OFF)
if(SMS_BUILD_BOOT)
    add_executable(sms-boot "${SMS_NATIVE_REPO_ROOT}/native/src/boot.cpp")
    set_target_properties(sms-boot PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(sms-boot PRIVATE
        "${SMS_NATIVE_REPO_ROOT}/native/platform" "${SMS_NATIVE_REPO_ROOT}/native/shim"
        "${SMS_NATIVE_REPO_ROOT}/reference/sms/include")
    target_link_options(sms-boot PRIVATE -Wl,--wrap=main)
    # boot_stubs/ — SCAFFOLD-to-link layer: faithful-signature stub definitions for the
    # game-class roster the decomp left undefined (actor ctors/vtables/nerves/globals the
    # MarNameRefGen catalogue references but that aren't on the boot->scene hot path).
    # Compiled into a lib in the RESCAN closure; replaced with faithful ports as the
    # runtime actually exercises each class.
    file(GLOB SMS_BOOT_STUBS CONFIGURE_DEPENDS
         "${SMS_NATIVE_REPO_ROOT}/native/boot_stubs/*.cpp"
         "${SMS_NATIVE_REPO_ROOT}/native/boot_stubs/*.S")
    if(SMS_BOOT_STUBS)
        enable_language(ASM)
        add_library(sms-bootstubs STATIC ${SMS_BOOT_STUBS})
        set_target_properties(sms-bootstubs PROPERTIES
            CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        target_include_directories(sms-bootstubs PRIVATE
            "${SMS_NATIVE_REPO_ROOT}/native/shim"
            "${SMS_NATIVE_REPO_ROOT}/reference/sms/include")
        # The C++ force-include must NOT reach the .S stub (assembler can't use <cstring>).
        target_compile_options(sms-bootstubs PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:-include;${SMS_NATIVE_REPO_ROOT}/native/shim/gekko_intrinsics.h;-Wno-narrowing>"
            -w)
    endif()
    # Same cyclic closure as the j3dload test — boot pulls the GD display-list builder
    # (sms-gd) and the BE->host asset swappers (sms-assets) through the game's vtables,
    # so link the full RESCAN group (not just sms-native+sms-platform).
    target_link_libraries(sms-boot PRIVATE
        "$<LINK_GROUP:RESCAN,sms-native,sms-gd,sms-assets,sms-platform,$<TARGET_NAME_IF_EXISTS:sms-bootstubs>>"
        Threads::Threads)
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
        "${SMS_NATIVE_REPO_ROOT}/reference/sms/include"
        # The renderer's pure lighting decoder (ngx_light.h) — gx_test cross-checks the
        # GXSetChanCtrl packing against decode_chanctl().
        "${SMS_NATIVE_REPO_ROOT}/runtime/ngx")
    target_compile_options(sms-${test_name} PRIVATE -fcommon)
    # sms-gd (GD display-list builder, decomp source) is referenced by gd_test;
    # a static lib contributes nothing to tests that don't reference it.
    target_link_libraries(sms-${test_name} PRIVATE sms-platform sms-gd)
    add_test(NAME platform_${test_name} COMMAND sms-${test_name})
endforeach()
