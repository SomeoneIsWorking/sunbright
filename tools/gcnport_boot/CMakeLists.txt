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
  guest_report.cpp
  boot_options.cpp
    guest_lighting_probe.cpp
    guest_projection_probe.cpp
    guest_draw_publisher.cpp
    draw_listing.cpp
    guest_frame_renderer.cpp
    frame_image_file.cpp
    family_color_map.cpp
    guest_model_probe.cpp
    guest_j2d_context_probe.cpp
    guest_picture_probe.cpp
    guest_screen_space.cpp
    guest_matrix_state.cpp
    guest_matrix_probe.cpp
    guest_glyph_probe.cpp
    guest_pad_probe.cpp
    guest_solid_rectangle_probe.cpp
    guest_material_probe.cpp
  guest_shape_probe.cpp
  guest_efb_copy_probe.cpp
  guest_viewport_probe.cpp
  "${GCPORT_DOLPHIN_STUB_HOST}"
)
# `sunbright-native-render-sdl3` is here for --render-frames: the diagnostic draws the title's own
# shapes through the shipping passes rather than a second renderer written for it. It brings SDL3
# with it, which the run only initializes when that flag is given -- a run without it opens no
# device and stays exactly as headless as it was.
target_link_libraries(sunbright_gcnport_boot PRIVATE gcnport::dolphin sunbright-title-adapter
                                                     sunbright-native-render-sdl3)
gcnport_stage_dolphin_sys(sunbright_gcnport_boot "${GCPORT_DOLPHIN_SYS_DIR}")
