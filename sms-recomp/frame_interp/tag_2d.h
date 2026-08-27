// Stable draw identities for J2D panes whose screen-space transforms animate between game ticks.

#pragma once

#include "populations.h"

#include <intrinsics.h>

namespace sb::frame_interp::two_d {

enum class DrawPath : u32 {
    Picture = 1,
    QuadEmitter = 2,
};

class PaneScope {
  public:
    PaneScope(u32 pane, DrawPath path);
    ~PaneScope();

    PaneScope(const PaneScope&) = delete;
    PaneScope& operator=(const PaneScope&) = delete;

  private:
    PopulationState mPreviousPopulation;
    bool mTagged = false;
};

} // namespace sb::frame_interp::two_d
