# SPDX-License-Identifier: GPL-2.0-or-later
include_guard(GLOBAL)

# Wires Sunbright's pinned `extern/gcnport` submodule (and its own pinned Dolphin fork submodule
# at extern/gcnport/extern/dolphin) as a real CMake dependency, so tools/gcnport_boot links
# against Dolphin's `core`/`common`/`uicommon` targets through ordinary target_link_libraries()
# transitive propagation instead of a hand-assembled linker command line.
#
# This builds only Dolphin's Core library and its dependency graph (no Qt/NoGUI/CLI frontend, no
# unrelated audio/video backends) -- the same slice gcnport's own
# extern/gcnport/tools/gcnport_tools/dolphin_runtime.py configures to build and run its Dolphin
# regression test. That Python module remains the single source of truth for gcnport's own
# verification build; this list mirrors its DOLPHIN_OPTIONS/LINUX_OPTIONS because CMake configure
# time cannot import it directly. Keep both option lists in sync when either changes.
#
# ENABLE_TESTS stays OFF here (unlike gcnport's own verifier): this target only needs the `core`/
# `common`/`uicommon` libraries GcnPortRuntime.cpp already compiles into `core` unconditionally; it
# does not need Dolphin's own gtest binary.
function(sunbright_add_gcnport_dolphin_runtime)
  set(gcnport_dir "${CMAKE_SOURCE_DIR}/extern/gcnport")
  set(dolphin_dir "${gcnport_dir}/extern/dolphin")

  if(NOT EXISTS "${gcnport_dir}/CMakeLists.txt")
    message(FATAL_ERROR
      "Sunbright requires the pinned extern/gcnport submodule for the gcnport boot tool; "
      "run 'git submodule update --init extern/gcnport' first")
  endif()
  if(NOT EXISTS "${dolphin_dir}/CMakeLists.txt")
    message(FATAL_ERROR
      "Sunbright requires gcnport's pinned Dolphin fork submodule; run "
      "'git submodule update --init --recursive extern/gcnport' first")
  endif()

  set(dolphin_options
    ENABLE_TESTS
    ENABLE_QT
    ENABLE_NOGUI
    ENABLE_CLI_TOOL
    ENABLE_VULKAN
    ENABLE_SDL
    ENABLE_CUBEB
    ENABLE_ALSA
    ENABLE_PULSEAUDIO
    ENABLE_LLVM
    ENCODE_FRAMEDUMPS
    USE_UPNP
    USE_DISCORD_PRESENCE
    USE_MGBA
    USE_RETRO_ACHIEVEMENTS
    ENABLE_ANALYTICS
    ENABLE_AUTOUPDATE
    USE_SYSTEM_LIBS
  )
  foreach(option_name IN LISTS dolphin_options)
    set(${option_name} OFF CACHE BOOL "" FORCE)
  endforeach()

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    foreach(option_name IN ITEMS ENABLE_X11 ENABLE_EGL ENABLE_HWDB ENABLE_EVDEV)
      set(${option_name} OFF CACHE BOOL "" FORCE)
    endforeach()
  endif()

  # EXCLUDE_FROM_ALL: an ordinary `cmake --build build` for Sunbright's own product must not
  # silently start compiling the whole Dolphin fork. Only an explicit
  # `cmake --build build --target sunbright_gcnport_boot` (or a target that depends on it) pulls
  # `core`/`common`/`uicommon` in.
  # Dolphin resolves its own data files -- the GameCube IPL font substitutes and the DSP ROM and
  # coefficient tables -- through File::GetSysDirectory(). On Linux that is a bare relative "sys/"
  # unless LINUX_LOCAL_DEV is set, in which case it is a Sys directory beside the executable, which
  # is how Dolphin's own from-source development builds work. Left unresolved, a title's
  # OSGetFontTexture path reads through null font pointers and Dolphin reports "Trying to access
  # Windows-1252 fonts but they are not loaded". Common is where that path is compiled, so the
  # option has to be set before Dolphin's own CMakeLists is processed.
  if(UNIX AND NOT APPLE)
    set(LINUX_LOCAL_DEV ON CACHE BOOL "" FORCE)
  endif()

  add_subdirectory("${dolphin_dir}" "${CMAKE_BINARY_DIR}/gcnport_dolphin" EXCLUDE_FROM_ALL)

  # The checkout actually being built against, so a consumer never guesses the path and it is never
  # a literal in a tracked file.
  set(SUNBRIGHT_GCNPORT_DOLPHIN_SYS_DIR "${dolphin_dir}/Data/Sys" PARENT_SCOPE)
endfunction()
