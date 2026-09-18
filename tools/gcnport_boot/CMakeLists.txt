# SPDX-License-Identifier: GPL-2.0-or-later
#
# Standalone maintainer diagnostic: boots the real, gitignored, user-supplied GMSE01 main.dol
# through gcnport's public PowerPC::GcnPort adapter. This is NOT part of the gameplay product
# target (see AGENTS.md / docs/issues/0037...): it exists to observe and report exact boot
# progress against the real retail image while gcnport's OS-init/apploader adapter is still being
# built. It never links into native-render or a gameplay executable.
include(GcnPortDependency)
sunbright_add_gcnport_dolphin_runtime()

# `core` calls back into a set of Host_* free functions a real frontend (DolphinQt/DolphinNoGUI)
# normally implements for UI events (title-bar updates, breakpoint/disasm notifications, Discord
# presence, ...); none of that applies to this headless diagnostic. Dolphin's own gtest binary
# faces the identical linking requirement and resolves it with its reusable, frontend-neutral
# Source/UnitTests/StubHost.cpp (a plain do-nothing implementation, not gtest-specific) instead of
# a bespoke Sunbright copy.
add_executable(sunbright_gcnport_boot
  gmse01_boot.cpp
  "${CMAKE_SOURCE_DIR}/extern/gcnport/extern/dolphin/Source/UnitTests/StubHost.cpp"
)
target_link_libraries(sunbright_gcnport_boot PRIVATE core uicommon)
if(NOT EXISTS "${SUNBRIGHT_GCNPORT_DOLPHIN_SYS_DIR}/GC/font_western.bin")
  message(FATAL_ERROR
    "sunbright_gcnport_boot: no Dolphin Sys data at '${SUNBRIGHT_GCNPORT_DOLPHIN_SYS_DIR}'. Without "
    "it a booted title's font and DSP paths read through null pointers. Check out the gcnport "
    "submodule and its own extern/dolphin submodule.")
endif()
# File::GetSysDirectory() resolves to a Sys directory beside the executable under LINUX_LOCAL_DEV,
# so link the checkout's data there rather than copying it: the link tracks the submodule, and a
# stale copy of the DSP ROM or font tables would be invisible.
add_custom_command(TARGET sunbright_gcnport_boot POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E create_symlink
          "${SUNBRIGHT_GCNPORT_DOLPHIN_SYS_DIR}"
          "$<TARGET_FILE_DIR:sunbright_gcnport_boot>/Sys"
  VERBATIM)
# Dolphin's own CMakeLists.txt exposes "Common/CommonTypes.h"-style includes through legacy
# directory-scoped include_directories() calls (both the source tree's Source/Core and its own
# generated-header binary dir), not as a usage requirement on the `core`/`common`/`uicommon`
# targets. A directory-scoped include only reaches subdirectories processed after that call inside
# Dolphin's own CMakeLists, so a sibling directory like this one must repeat it explicitly rather
# than getting it for free through target_link_libraries().
target_include_directories(sunbright_gcnport_boot PRIVATE
  "${CMAKE_SOURCE_DIR}/extern/gcnport/extern/dolphin/Source/Core"
  "${CMAKE_BINARY_DIR}/gcnport_dolphin/Source/Core"
)

# Dolphin's own top-level CMakeLists.txt (and its Source/CMakeLists.txt) select C++23,
# _ARCH_64/_M_X86_64/_M_ARM_64 architecture macros, etc. via directory-scoped set()/
# add_definitions() calls -- again a directory property, not a target usage requirement, so it
# does not reach this sibling directory's target through target_link_libraries() either. Dolphin's
# own public headers (e.g. Core/MachineContext.h) require these macros to already be defined by
# the time they are parsed. Mirror the exact selection Dolphin's own CMakeLists.txt makes instead
# of hand-guessing a single architecture.
set_target_properties(sunbright_gcnport_boot PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x64|x86_64|amd64|AMD64")
  target_compile_definitions(sunbright_gcnport_boot PRIVATE _ARCH_64=1 _M_X86_64=1)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
  target_compile_definitions(sunbright_gcnport_boot PRIVATE _ARCH_64=1 _M_ARM_64=1)
else()
  message(FATAL_ERROR
    "sunbright_gcnport_boot: unsupported architecture '${CMAKE_SYSTEM_PROCESSOR}' with "
    "${CMAKE_SIZEOF_VOID_P}-byte pointers (mirrors Dolphin's own supported-architecture check)")
endif()
