#include "bse/frame_rate_logic.h"

#include <array>
#include <cassert>
#include <stdexcept>

int main() {
    std::array<float, 9> within{0.0f, 1.0f, 0.0f, 6.0f, 0.0f,
                                4.0f, 2.0f, 8.0f, 10.0f};
    assert(sb::bse::advance_hx_motion(within, 2.0f) == 12.0f);
    assert(within[6] == 4.0f);
    assert(within[7] == 8.5f);

    std::array<float, 9> above{9.0f, 0.0f, 0.0f, 6.0f, 0.0f,
                               4.0f, 2.0f, 8.0f, 10.0f};
    assert(sb::bse::advance_hx_motion(above, 4.0f) == 10.875f);
    assert(above[6] == 3.5f);
    assert(above[7] == 8.25f);

    bool rejected = false;
    try {
        sb::bse::advance_hx_motion(within, 0.0f);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    return 0;
}
