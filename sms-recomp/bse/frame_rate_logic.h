// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>

namespace sb::bse {

float advance_hx_motion(std::array<float, 9>& state, float rateMultiplier);

} // namespace sb::bse
