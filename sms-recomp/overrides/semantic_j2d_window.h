#pragma once

#include "cpu_state.h"

// Publishes the final J2DWindow geometry at draw_private entry. The caller may first adjust the
// guest rectangles or pane transform; this seam observes exactly what the retained body will draw.
void submit_semantic_j2d_window(CPUState& cpu);
