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
file(GLOB_RECURSE RECOMP_SOURCES tools/recompiler/*.cpp)
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

# ── Launcher executable (SDL2 window + Dolphin Core) ─────────────────────────
# Everything — launcher, runtime bridge, hand-written overrides, and the
# statically-recompiled game code — is compiled into one self-contained binary.
# No shared libraries, no dlopen, no dynamic-symbol resolution to get wrong.
find_package(SDL2 REQUIRED)

# Hand-written native overrides for functions the recompiler can't reproduce.
file(GLOB SUNBRIGHT_OVERRIDE_SOURCES runtime/overrides/*.cpp)

# Generated game code (present after /recompile). The recompiler splits function
# bodies across many address-bucketed generated/functions_*.cpp files (plus a
# shared functions.h and jump_table.cpp) so they compile in parallel. The weak
# fallback table lets the binary link even before generation; the generated
# strong symbols win.
file(GLOB SUNBRIGHT_GENERATED_SOURCES
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
    runtime/sunbright_bridge.cpp
    runtime/dolphin_hook.cpp
    runtime/memory_bridge.cpp
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

target_compile_definitions(sunbright PRIVATE
    HAVE_DOLPHIN_CORE=1
    HAVE_DOLPHIN_DISCIO=1
    HAVE_DOLPHIN_MEMMAP=1  # memory_bridge routes through Dolphin's real MemMap
    _M_X86_64=1            # Dolphin sets this via add_definitions; mirror it for our target
)

target_link_libraries(sunbright PRIVATE
    core discio uicommon
    SDL2::SDL2
    pthread
)

# Intercept JitTrampoline at link time — no Dolphin source changes needed.
# Use the mangled C++ name (verified via nm libcore.a | grep JitTrampoline).
target_link_options(sunbright PRIVATE
    "-Wl,--wrap=_Z13JitTrampolineR7JitBasej"
    "-Wl,--wrap=_ZN11PixelEngine18PixelEngineManager8SetTokenEtbi"
    # Native AID interrupt ownership (aid_native.cpp): strip INT_AID before PI, deliver natively
    "-Wl,--wrap=_ZN3DSP10DSPManager20GenerateDSPInterruptEml"
    # Native SMS DAC ucode (zelda_ucode_native.cpp): own the mail state machine, no self-halt
    "-Wl,--wrap=_ZN3DSP3HLE12UCodeFactoryEjPNS0_6DSPHLEEb"
    "-Wl,--wrap=_ZN3DSP10DSPManager14UpdateAudioDMAEv"
    "-Wl,--wrap=_ZN3DSP10DSPManager30GenerateDSPInterruptFromDSPEmuENS_16DSPInterruptTypeEi"
    # CoreTiming schedule/remove tracer (memcard EXI lost-completion diagnostic, SUNBRIGHT_DBG_CARD)
    "-Wl,--wrap=_ZN10CoreTiming17CoreTimingManager13ScheduleEventElPNS_9EventTypeEmNS_10FromThreadE"
    "-Wl,--wrap=_ZN10CoreTiming17CoreTimingManager11RemoveEventEPNS_9EventTypeE"
    # Host audio flow meter (SUNBRIGHT_DBG_MIXER): mixer push vs backend pull rates
    "-Wl,--wrap=_ZN5Mixer11PushSamplesEPKsm"
    "-Wl,--wrap=_ZN5Mixer3MixEPsm"
    "-Wl,--wrap=_ZN5Mixer20PushStreamingSamplesEPKsm"
    "-Wl,--wrap=_ZN5Mixer28SetDMAInputSampleRateDivisorEj"
    "-Wl,--wrap=_ZN5Mixer31SetStreamInputSampleRateDivisorEj"
    "-Wl,--wrap=_ZN5Mixer18SetStreamingVolumeEjj"
    # DSP HLE voice parameter tracer (SUNBRIGHT_DUMP_VPB): per-frame VPB fields per voice
    "-Wl,--wrap=_ZN3DSP3HLE18ZeldaAudioRenderer8FetchVPBEtPNS1_3VPBE"
)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS sunbright DESTINATION bin)
install(TARGETS sunbright-recomp DESTINATION bin)

message(STATUS "Sunbright build configured")
message(STATUS "  Dolphin DiscIO: ${SUNBRIGHT_USE_DOLPHIN_DISCIO}")
message(STATUS "  Dolphin Core:   ${SUNBRIGHT_USE_DOLPHIN_CORE}")
