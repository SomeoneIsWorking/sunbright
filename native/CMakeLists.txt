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

# The decomp's JASystem::Vload sequence loader is the DEAD path for SMS (it needs
# JaiArcS.hed, which is not on the disc FST). Replace it with a native BARC-backed
# loader (native/src/sms_boot_audio.cpp) that sources sequences from mSound.aaf +
# sequence.arc, so JAIBasic::loadArcSeqData succeeds at stage entry.
list(FILTER SMS_NATIVE_SOURCES EXCLUDE REGEX
     "/reference/sms/src/JSystem/JAudio/JASystem/JASVload\\.cpp$")
list(APPEND SMS_NATIVE_SOURCES
     "${SMS_NATIVE_REPO_ROOT}/native/src/sms_boot_audio.cpp"
     # Owns the active-camera transform for the native scene render (gpCamera -> view+proj).
     "${SMS_NATIVE_REPO_ROOT}/native/src/camera_latch.cpp"
     # Drives the real scene-draw flow natively (TSmJ3DScn::perform(8)) so entry() runs.
     "${SMS_NATIVE_REPO_ROOT}/native/src/scene_drive.cpp"
     # sb_get_app_state() — TApplication::mAppState reader for sb_parity_dump.h (which lives
     # in native/render/ and can't include <System/Application.hpp>). Session 16 STATE-MISMATCH gate.
     "${SMS_NATIVE_REPO_ROOT}/native/src/sb_parity_appstate.cpp"
     # sb_pin_state_populate — game-state fingerprint reader for the scene-sync harness.
     # Populates SbPinGameState (runtime/pin_state_schema.h) from the live C++ game objects.
     # Symmetric to the oracle-side populator in runtime/gx_capture.cpp (guest RAM reader).
     "${SMS_NATIVE_REPO_ROOT}/native/src/pin_state_native.cpp"
     # sb::engine::mode() — runtime render-sink toggle (NATIVE_PC vs GX_ORACLE).
     # SB_RENDER env selects; today only NATIVE_PC is wired. See runtime/engine.h.
     "${SMS_NATIVE_REPO_ROOT}/native/src/engine.cpp")

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
    "${SMS_NATIVE_REPO_ROOT}/reference/sms/include"
    # BE->host asset byteswap layer (bmd_swap_to_host), wired into the BMD/BDL
    # model load path for swap-at-load. Symbols come from sms-assets at link.
    "${SMS_NATIVE_REPO_ROOT}/native/assets"
    # Pure renderer-port headers used by scene_drive (sms_boot_setlight.h — the
    # TLightCommon::setLight stage-light selection).
    "${SMS_NATIVE_REPO_ROOT}/native/render")

target_compile_options(sms-native PRIVATE
    # Force-include the portable Gekko intrinsics + fixed-width types shim.
    -include "${SMS_NATIVE_REPO_ROOT}/native/shim/gekko_intrinsics.h"
    # The decomp predates C++11 narrowing strictness (Metrowerks allowed it).
    # This silences brace-init narrowing ONLY; pointer truncation still errors
    # as 'loses precision'. -w hides the rest of the noise (warnings, not errors).
    # SCOPE NOTE (2026-07-02): this is retained on sms-native (decomp) because
    # ~35+ narrowing sites remain across ~500 reference/sms TUs. The obvious
    # MWERKS-narrowing sentinels (u16=-1, s16=0xC000, GXColorS10 negative
    # channel constants) were fixed inline; harder cases (e.g. 0x1000000 into
    # a u8 field where the struct layout may be wrong, not the value) remain.
    # -Wno-narrowing here is a TARGET-level exclusion for a DECOMP library —
    # NOT a per-file first-party suppression the build-hygiene directive bans.
    -w
    -Wno-narrowing
    # SDK headers declare OS/HW globals as tentative defs without extern; -fcommon
    # merges them (Metrowerks behavior) so the eventual full link doesn't hit
    # -fno-common multiple-definition errors across the 577 TUs.
    -fcommon
    # The decomp is full of MWERKS-ism: value-returning functions that fall off
    # the end without a `return` (e.g. JUTGamePad::read returns u32 but has no
    # return; the value is unused). MWERKS compiled these benignly (the function
    # returns normally with a garbage result). GCC's -O2 hot/cold block
    # PARTITIONING moves such a function's cold path into a separate `.cold`
    # section, laid out adjacent to the NEXT function's `.cold` — so a missing-
    # return fall-through runs straight into a foreign cleanup landing pad
    # (_Unwind_Resume) and crashes. Disabling the partition keeps every block
    # inside its own function, so a fall-through hits that function's own
    # epilogue/ret (the original benign behavior) instead of foreign code.
    # GCC-only: hot/cold block partitioning. clang doesn't do the partition, so
    # the fallthrough-into-foreign-cleanup case can't arise under clang.
    $<$<CXX_COMPILER_ID:GNU>:-fno-reorder-blocks-and-partition>
    # The decomp source is UTF-8, but the GameCube assets store Japanese names
    # in Shift-JIS (the original game was compiled from Shift-JIS source). The
    # JDrama NameRef tree is searched by node NAME (e.g. TApplication::unk30 =
    # search("ステージ毎シナリオアーカイブ名群")): calcKeyCode + strcmp compare a
    # source string literal against the SJIS asset bytes, so a UTF-8 literal
    # never matches (search returns null -> null-deref). Emit narrow string
    # literals as Shift-JIS so they match the assets, exactly as the original
    # build did. ASCII is identical in SJIS, so type strings / printf formats
    # are unaffected. (-finput-charset names the source encoding for the
    # conversion.)
    # clang doesn't support SHIFT_JIS in -fexec-charset (iconv-backed on GCC).
    # Under clang the JDrama NameRef search will miss any Japanese-name node —
    # first-party clang builds are for portability CI only, not runtime; the
    # SJIS-name game paths are exercised under the GCC build.
    $<$<CXX_COMPILER_ID:GNU>:-finput-charset=UTF-8>
    $<$<CXX_COMPILER_ID:GNU>:-fexec-charset=SHIFT_JIS>)

# SMS_NATIVE_PLATFORM: the "no emulation chasing" gate (CLAUDE.md top rule, 2026-07-03).
# Under this define, effects RE'd from the game are ported to native PC (SDL3 GPU / native
# shaders) instead of being routed through the recompiled J3D→GX→TEV-emulator chain. Set
# PUBLIC so downstream consumers (sms-render, sms-boot) also see it.
target_compile_definitions(sms-native PUBLIC SMS_NATIVE_PLATFORM=1)

# ---------------------------------------------------------------------------
# sms-platform — the native PC implementations of the GameCube SDK seams the
# game links (#include <dolphin/...>). Phase-2 work (native/platform/README.md).
# Grows seam by seam; each <sub>_impl.cpp defines the unresolved SDK symbols for
# one subsystem (MTX/OS/DVD/VI/GX/...). Built clean (-Wall -Wextra), real C++ —
# these are NEW native code, not decomp, so they get full warnings.
# ---------------------------------------------------------------------------
file(GLOB SMS_PLATFORM_SOURCES CONFIGURE_DEPENDS
     "${SMS_NATIVE_REPO_ROOT}/native/platform/*_impl.cpp")
# The GX-seam FIFO recorder (feeds Dolphin's OpcodeDecoder in the GX_ORACLE sink).
list(APPEND SMS_PLATFORM_SOURCES
     "${SMS_NATIVE_REPO_ROOT}/native/platform/gx_fifo.cpp")

find_package(Threads REQUIRED)

add_library(sms-platform STATIC ${SMS_PLATFORM_SOURCES})
set_target_properties(sms-platform PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sms-platform PUBLIC
    "${SMS_NATIVE_REPO_ROOT}/native/platform"
    "${SMS_NATIVE_REPO_ROOT}/native/shim"
    "${SMS_NATIVE_REPO_ROOT}/native/render"   # gx_imm_xform.h (immediate-mode capture)
    "${SMS_NATIVE_REPO_ROOT}/runtime/render"  # tex_decode.h (SB_IMM_PRIM_DBG sprite dump)
    "${SMS_NATIVE_REPO_ROOT}/runtime/ngx"     # ngx_render_data.h (NgxTevState per imm batch)
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
    -w -fcommon)
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
    "${SMS_NATIVE_REPO_ROOT}/native/assets/bmt_swap.cpp"
    "${SMS_NATIVE_REPO_ROOT}/native/assets/anm_swap.cpp"
    "${SMS_NATIVE_REPO_ROOT}/native/assets/timg_swap.cpp"
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

    # glslang for the RUNTIME GLSL->SPIR-V compiler (glsl_compile.cpp).
    #
    # When native/ is included from the root CMakeLists.txt, Dolphin's bundled
    # glslang has already been added via add_subdirectory and exposes the plain
    # target names (glslang / SPIRV / glslang-default-resource-limits) into our
    # scope — reuse them directly. Calling find_package(glslang CONFIG) here
    # would pull in the SYSTEM Fedora glslang config file, which tries to
    # (re)define the namespaced glslang::* targets, and clashes with what Dolphin
    # has already partially defined (glslang::SPIRV etc.). Standalone native/
    # builds don't have Dolphin's targets, so fall back to find_package or the
    # system libs.
    if(TARGET SPIRV AND TARGET glslang)
        set(SB_NATIVE_GLSLANG_LIBS SPIRV glslang glslang-default-resource-limits)
    else()
        find_package(glslang CONFIG QUIET)
        if(TARGET glslang::SPIRV)
            set(SB_NATIVE_GLSLANG_LIBS glslang::SPIRV glslang::glslang
                glslang::glslang-default-resource-limits)
        else()
            set(SB_NATIVE_GLSLANG_LIBS glslang glslang-default-resource-limits SPIRV
                SPIRV-Tools-opt SPIRV-Tools)
        endif()
    endif()

    # ── SDL3 GPU (GX→SDL3-GPU switch, docs/gx_sdlgpu_switch.md) ─────────────────────────────
    # System SDL3 (sdl3-devel / pkg-config sdl3). gx_sdlgpu.cpp is the SDL3 GPU backend — the GX
    # seam's renderer, and now the sole renderer (the bespoke nvk Vulkan rasterizer was retired).
    find_package(SDL3 QUIET)
    if(NOT TARGET SDL3::SDL3)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
        add_library(SDL3::SDL3 ALIAS PkgConfig::SDL3)
    endif()

    add_library(sms-render STATIC
        "${SMS_NATIVE_REPO_ROOT}/native/render/ngx_native_glue.cpp"
        "${SMS_NATIVE_REPO_ROOT}/native/render/gx_sdlgpu.cpp"
        # The sms-boot VI present hook (SDL3 GPU render + frame dump).
        "${SMS_NATIVE_REPO_ROOT}/native/render/sms_boot_present.cpp"
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
    # See sms-native above. The present layer needs the gate to override the frame clear.
    target_compile_definitions(sms-render PUBLIC SMS_NATIVE_PLATFORM=1)
    target_include_directories(sms-render PUBLIC
        # shim/ FIRST so ngx_decode's #include "gx_parse.h" gets the Dolphin-free shadow.
        "${SMS_NATIVE_REPO_ROOT}/native/render/shim"
        "${SMS_NATIVE_REPO_ROOT}/native/render"
        "${SMS_NATIVE_REPO_ROOT}/runtime/ngx"
        "${SMS_NATIVE_REPO_ROOT}/runtime/render"
        "${SB_SHADER_GEN}" "${SB_VULKAN_INC}")
    target_link_libraries(sms-render PUBLIC "${SB_VULKAN_LIB}" ${SB_NATIVE_GLSLANG_LIBS} SDL3::SDL3)
    # tev_shader.cpp / glsl_compile.cpp are SHIPPING ngx code (predates strict warnings);
    # -Wall -Wextra applies to the new nvk code, but keep the reused TUs from erroring.
    target_compile_options(sms-render PRIVATE -Wall -Wextra)
    set_source_files_properties(
        "${SMS_NATIVE_REPO_ROOT}/runtime/render/tev_shader.cpp"
        "${SMS_NATIVE_REPO_ROOT}/runtime/render/glsl_compile.cpp"
        PROPERTIES COMPILE_OPTIONS "-w")

    # ── Parity-focused renderer TDD (native/render/tests/parity/*_test.cpp) ────────────────
    # Faithfulness tests: render a KNOWN input through the SDL3 GPU renderer (sb::gxsdl) and
    # assert the readback pixels against SPEC-computed ground truth (GX semantics), built from
    # small scope (clear, single triangle, single textured quad, one TEV stage, blend, depth)
    # up to larger scenes. The old render tests (nvk-based, self-consistency only) were removed:
    # they didn't prove correctness. See docs/gx_sdlgpu_switch.md ("Parity-focused TDD").
    file(GLOB SB_PARITY_TESTS CONFIGURE_DEPENDS
         "${SMS_NATIVE_REPO_ROOT}/native/render/tests/parity/*_test.cpp")
    foreach(pt ${SB_PARITY_TESTS})
        get_filename_component(pt_name "${pt}" NAME_WE)
        add_executable(sms-${pt_name} "${pt}")
        set_target_properties(sms-${pt_name} PROPERTIES
            CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
        target_link_libraries(sms-${pt_name} PRIVATE sms-render sms-platform)
        add_test(NAME parity_${pt_name} COMMAND sms-${pt_name})
        set_tests_properties(parity_${pt_name} PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=offscreen")
    endforeach()
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
    "${SMS_NATIVE_REPO_ROOT}/native/render/tests/j3dload_test.cpp"
    # sb_tex_decode/sb_tex_pad_w/sb_tex_pad_h have no weak fallback (real, always-buildable,
    # GPU-free code — gx_imm_impl.cpp calls them unconditionally), so link the actual decoder
    # rather than stub it out. Everything else this test's link closure needs from the render-
    # capture pipeline (sb_boot_capture_phase et al.) IS weak-declared — see JDRDrawBufObj.cpp /
    # J3DDrawBuffer.cpp / Map.cpp / gx_impl.cpp — and resolves to a safe no-op here.
    "${SMS_NATIVE_REPO_ROOT}/runtime/render/tex_decode.cpp")
set_target_properties(sms-j3dload_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sms-j3dload_test PRIVATE
    "${SMS_NATIVE_REPO_ROOT}/native/shim"
    "${SMS_NATIVE_REPO_ROOT}/reference/sms/include"
    "${SMS_NATIVE_REPO_ROOT}/native/assets"
    "${SMS_NATIVE_REPO_ROOT}/runtime/render")
target_compile_options(sms-j3dload_test PRIVATE
    -include "${SMS_NATIVE_REPO_ROOT}/native/shim/gekko_intrinsics.h"
    -w -fcommon)
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
# parity_sweep_logic — Python unit tests for the CROSS-ENGINE comparison brain
# (tools/render/parity_sweep.py): the geometry + LIGHTING window-summary diff used
# to compare sms-boot vs the Dolphin-GX/ngx oracle for title/file-select fidelity.
# Pure logic over synthetic JSONL; no ROM/GPU/Dolphin. TDD ground truth for the
# light-count/ambient/material/projType divergence detection.
# ---------------------------------------------------------------------------
find_program(PYTHON3_EXE NAMES python3 python)
if(PYTHON3_EXE)
    add_test(NAME parity_sweep_logic
             COMMAND "${PYTHON3_EXE}" "${SMS_NATIVE_REPO_ROOT}/tools/render/parity_sweep_test.py")
endif()

# ---------------------------------------------------------------------------
# sms-boot — the native bring-up executable: PlatformInit (--wrap=main) -> the game's
# own main() (TApplication). Won't LINK until the remaining seams + game C++ stubs
# resolve; the link failure reveals the true critical-path stub set. Opt-in (it is
# EXPECTED to fail-link today) so it never breaks the default build/ctest.
# ---------------------------------------------------------------------------
option(SMS_BUILD_BOOT "Build the native bring-up exe (expected to fail-link until stubs land)" OFF)
if(SMS_BUILD_BOOT)
    add_executable(sms-boot
        "${SMS_NATIVE_REPO_ROOT}/native/src/boot.cpp"
        # Headless scripted controller input (SB_PAD_SCRIPT / SB_PAD_DRIVE).
        "${SMS_NATIVE_REPO_ROOT}/native/src/pad_driver.cpp"
        # SB_HARNESS=<test-name>: Tier-1 vs Tier-2 in-process parity harness.
        "${SMS_NATIVE_REPO_ROOT}/native/src/render_parity.cpp"
        # Early heap for glslang static-init allocs (runs before main / PlatformInit).
        "${SMS_NATIVE_REPO_ROOT}/native/src/boot_heap_bringup.cpp"
        # SLICE 3a renderer-attach: live J3D scene capture (J3DShape::draw -> NDC tris,
        # drained by the present hook). Needs BOTH the J3D headers (reference/sms/include +
        # gekko force-include, like the loader tests) AND the render headers (nvk/ngx, via
        # the gx_parse shim) — only sms-boot has both include sets, so it is compiled here
        # rather than in sms-render. Provides the weak sb_boot_capture_j3d{,_take} symbols.
        "${SMS_NATIVE_REPO_ROOT}/native/render/sms_boot_j3d_capture.cpp"
        # Material/TEV/texture extraction from the captured shape's J3DMaterial (same J3D +
        # render include sets as the capture).
        "${SMS_NATIVE_REPO_ROOT}/native/render/sms_boot_material.cpp")
    set_source_files_properties(
        "${SMS_NATIVE_REPO_ROOT}/native/render/sms_boot_j3d_capture.cpp"
        "${SMS_NATIVE_REPO_ROOT}/native/render/sms_boot_material.cpp" PROPERTIES
        # ORDER MATTERS: native/render/shim FIRST so ngx_decode's #include "gx_parse.h"
        # gets the Dolphin-free shadow; native/shim BEFORE reference/sms/include so
        # <dolphin/types.h> resolves to the host shadow (u32=uint32_t) and NOT the GC SDK
        # header (u32=unsigned long) — otherwise the gx_parse shim's u32=uint32_t conflicts.
        INCLUDE_DIRECTORIES
            "${SMS_NATIVE_REPO_ROOT}/native/render/shim;${SMS_NATIVE_REPO_ROOT}/native/render;${SMS_NATIVE_REPO_ROOT}/runtime/ngx;${SMS_NATIVE_REPO_ROOT}/runtime/render;${SMS_NATIVE_REPO_ROOT}/native/shim;${SMS_NATIVE_REPO_ROOT}/reference/sms/include"
        COMPILE_OPTIONS "-include;${SMS_NATIVE_REPO_ROOT}/native/shim/gekko_intrinsics.h;-fcommon;-w")
    set_target_properties(sms-boot PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(sms-boot PRIVATE
        "${SMS_NATIVE_REPO_ROOT}/native/platform" "${SMS_NATIVE_REPO_ROOT}/native/shim"
        "${SMS_NATIVE_REPO_ROOT}/reference/sms/include")
    # boot_heap_bringup.cpp pulls JSystem headers — needs the gekko intrinsics force-include.
    set_source_files_properties(
        "${SMS_NATIVE_REPO_ROOT}/native/src/boot_heap_bringup.cpp" PROPERTIES
        COMPILE_OPTIONS "-include;${SMS_NATIVE_REPO_ROOT}/native/shim/gekko_intrinsics.h;-fcommon")
    # -rdynamic: export symbols into the dynamic table so the watchdog's backtrace
    # (backtrace_symbols_fd) prints function names, not bare addresses.
    target_link_options(sms-boot PRIVATE -Wl,--wrap=main -rdynamic)
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
            "$<$<COMPILE_LANGUAGE:CXX>:-include;${SMS_NATIVE_REPO_ROOT}/native/shim/gekko_intrinsics.h>"
            -w)
    endif()
    # Same cyclic closure as the j3dload test — boot pulls the GD display-list builder
    # (sms-gd) and the BE->host asset swappers (sms-assets) through the game's vtables,
    # so link the full RESCAN group (not just sms-native+sms-platform).
    target_link_libraries(sms-boot PRIVATE
        # sms-render is inside the RESCAN group because sb_parity_emit (in sms-render) calls
        # sb_get_app_state (in sms-native). Without RESCAN, sms-render is linked after
        # sms-native and the undef ref never resolves.
        "$<LINK_GROUP:RESCAN,sms-native,sms-gd,sms-assets,sms-platform,sms-render,$<TARGET_NAME_IF_EXISTS:sms-bootstubs>>"
        Threads::Threads)
    # The native renderer (VI present hook -> nvk -> frame dump) is MANDATORY: sms-boot IS the
    # native renderer, and its single boot path always drives the SDL present loop. The build
    # requires Vulkan/glslang (sms-render); there is no non-render configuration.
    if(NOT TARGET sms-render)
        message(FATAL_ERROR "sms-boot requires the native renderer (sms-render), but it was not built "
                            "(Vulkan/glslang not found). The native renderer is mandatory.")
    endif()
    target_link_libraries(sms-boot PRIVATE sms-render)

    # ── GX_ORACLE render sink (Path C): link Dolphin's video backend ──────────────
    # sb::engine::mode() == GX_ORACLE routes the game's GX calls through Dolphin's
    # videovulkan backend (same pipeline build/sunbright uses as the oracle). Both
    # sinks are compiled in; the user selects at runtime via SB_RENDER. Compile-time
    # SMS_NATIVE_PLATFORM still governs game-code execution (audio/threading/memory) —
    # only the render sink is toggleable.
    #
    # `videovulkan` is defined by the root CMakeLists.txt when `native/` is included
    # via add_subdirectory(native). Standalone `native/` builds (without root) don't
    # get the oracle sink and skip the link — the NATIVE_PC sink is fully functional
    # on its own.
    # Tier-2 (SB_RENDER=oracle) direct-write bridge: bypasses the FIFO byte
    # reconstruction path and calls Dolphin's LoadBPReg/LoadXFReg directly.
    # See native/render/oracle_direct.{h,cpp}.
    if(TARGET videovulkan)
        # Dolphin's core/videocommon require Host_* + Discord::* symbols the frontend
        # normally provides. sms-boot is not a real emulator front-end — provide
        # no-op stubs (see native/src/dolphin_host_shims.cpp).
        target_sources(sms-boot PRIVATE
            "${SMS_NATIVE_REPO_ROOT}/native/src/dolphin_host_shims.cpp"
            "${SMS_NATIVE_REPO_ROOT}/native/render/oracle_present.cpp"
            # Direct-write bridge: sb::oracle::bp_write / xf_write route into
            # Dolphin's LoadBPReg / LoadXFReg without going through FIFO byte
            # reconstruction. Also touches Dolphin headers → C++23.
            "${SMS_NATIVE_REPO_ROOT}/native/render/oracle_direct.cpp")
        set_source_files_properties(
            "${SMS_NATIVE_REPO_ROOT}/native/render/oracle_present.cpp"
            "${SMS_NATIVE_REPO_ROOT}/native/render/oracle_direct.cpp"
            "${SMS_NATIVE_REPO_ROOT}/native/src/dolphin_host_shims.cpp"
            PROPERTIES COMPILE_OPTIONS "-std=c++23")
        target_include_directories(sms-boot PRIVATE
            "${SMS_NATIVE_REPO_ROOT}/externals/dolphin/Source/Core")
        target_link_libraries(sms-boot PRIVATE videovulkan videocommon core uicommon common)
        target_compile_definitions(sms-boot PRIVATE SB_HAVE_ORACLE_SINK=1)
    else()
        message(STATUS "sms-boot: standalone native build — GX_ORACLE sink unavailable "
                       "(build from the root CMakeLists.txt to link Dolphin).")
    endif()
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
        "${SMS_NATIVE_REPO_ROOT}/runtime/ngx"
        # native/render — pure renderer-port headers (e.g. sms_boot_setlight.h,
        # the TLightCommon::setLight stage-light selection that setlight_test covers).
        "${SMS_NATIVE_REPO_ROOT}/native/render")
    target_compile_options(sms-${test_name} PRIVATE -fcommon)
    # sms-gd (GD display-list builder, decomp source) is referenced by gd_test;
    # sms-assets (BE->host asset swappers) by anm_swap_test/bmd swaps; a static lib
    # contributes nothing to tests that don't reference it (its PUBLIC include dir
    # also makes "anm_swap.h" resolvable).
    target_link_libraries(sms-${test_name} PRIVATE sms-platform sms-gd sms-assets)
    add_test(NAME platform_${test_name} COMMAND sms-${test_name})
endforeach()
