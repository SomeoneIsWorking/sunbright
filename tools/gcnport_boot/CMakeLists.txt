# SPDX-License-Identifier: GPL-2.0-or-later
#
# Standalone maintainer diagnostic: boots the real, gitignored, user-supplied GMSE01 main.dol
# through gcnport. This is NOT part of the gameplay product target (see AGENTS.md /
# docs/issues/0037...): it exists to observe and report exact boot progress against the real retail
# image. It never links into native-render or a gameplay executable.
include(GcnPortDependency)
sunbright_add_gcnport()

# `gcnport::dolphin` brings Dolphin's `core`/`uicommon`, its Source/Core include directories, its
# C++23 selection and its architecture macros with it as usage requirements, so none of that is
# restated here. `core` also calls back into a set of Host_* free functions a real frontend
# (DolphinQt/DolphinNoGUI) normally implements for UI events; none of that applies to a headless
# diagnostic, and Dolphin's own test binary answers the identical requirement with its reusable,
# frontend-neutral StubHost.cpp, which gcnport points consumers at.
add_executable(sunbright_gcnport_boot
  gmse01_boot.cpp
  "${GCPORT_DOLPHIN_STUB_HOST}"
)
target_link_libraries(sunbright_gcnport_boot PRIVATE gcnport::dolphin)
gcnport_stage_dolphin_sys(sunbright_gcnport_boot "${GCPORT_DOLPHIN_SYS_DIR}")
