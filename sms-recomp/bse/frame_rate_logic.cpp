// SPDX-License-Identifier: GPL-3.0-only
// Adapted from BetterSunshineEngine's FPS patch at commit
// 69baa4f15bfb2980670cbde638fc22f97a394385.

#include "frame_rate_logic.h"

#include <stdexcept>

namespace sb::bse {

float advance_hx_motion(std::array<float, 9>& state, float rateMultiplier) {
    if (!(rateMultiplier > 0.0f))
        throw std::invalid_argument("BSE frame-rate multiplier must be positive");

    if (state[0] <= state[7]) {
        if (state[1] <= state[7])
            state[6] += state[5] / rateMultiplier;
    } else {
        state[6] += state[3] / rateMultiplier;
    }
    state[7] += 1.0f / rateMultiplier;
    state[8] += state[6] / rateMultiplier;
    return state[8];
}

} // namespace sb::bse
