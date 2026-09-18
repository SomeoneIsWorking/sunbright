# SPDX-License-Identifier: GPL-2.0-or-later
include_guard(GLOBAL)

# Wires Sunbright's pinned `extern/gcnport` submodule (and its own pinned Dolphin fork submodule at
# extern/gcnport/extern/dolphin) as a real CMake dependency.
#
# Everything about HOW Dolphin is built belongs to gcnport, not here: which slice of it to compile,
# which options to force off, where its shipped Sys data lives, which include directories, C++
# standard and architecture macros its public headers require. Those all arrive by linking
# `gcnport::dolphin`. This file previously carried its own copy of the option list with a comment
# telling the reader to keep the two in sync by hand -- exactly the instruction that is obeyed until
# it is not -- and its own copy of the include/standard/architecture boilerplate besides.
function(sunbright_add_gcnport)
  set(gcnport_dir "${CMAKE_SOURCE_DIR}/extern/gcnport")

  if(NOT EXISTS "${gcnport_dir}/CMakeLists.txt")
    message(FATAL_ERROR
      "Sunbright requires the pinned extern/gcnport submodule for the gcnport boot tool; "
      "run 'git submodule update --init --recursive extern/gcnport' first")
  endif()

  # The adapter target is what Sunbright links, and building it is what pulls the Dolphin fork in.
  # gcnport's own focused tests are its gate, not Sunbright's.
  set(GCPORT_BUILD_DOLPHIN_ADAPTER ON CACHE BOOL "" FORCE)
  set(GCPORT_BUILD_TESTS OFF CACHE BOOL "" FORCE)

  # EXCLUDE_FROM_ALL: an ordinary `cmake --build build` for Sunbright's own product must not
  # silently start compiling the whole Dolphin fork. Only an explicit
  # `cmake --build build --target sunbright_gcnport_boot` pulls it in.
  add_subdirectory("${gcnport_dir}" "${CMAKE_BINARY_DIR}/gcnport" EXCLUDE_FROM_ALL)
endfunction()
