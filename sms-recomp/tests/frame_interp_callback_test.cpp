#include "frame_interp/frame_interp.h"

#include <cstdio>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression);  \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

namespace {

bool g_lerpEnabled = true;

struct Observation {
    unsigned calls = 0;
    bool sawSimFrame = false;
    float lastAlpha = 0.0f;
};

void observe(bool isSimFrame, void* user) {
    auto& observation = *static_cast<Observation*>(user);
    ++observation.calls;
    observation.sawSimFrame |= isSimFrame;
    observation.lastAlpha = sb::frame_interp::interpolation_step();
}

} // namespace

bool sbr_lerp_enabled() {
    return g_lerpEnabled;
}

namespace sb::app::frame_rate {
bool interpolation_matches_refresh() noexcept {
    return false;
}
} // namespace sb::app::frame_rate

namespace aurora::gfx {
void snap_next_interpolation() {}
namespace interp {
void name_population(uint8_t, const char*) {}
void report_audit() {}
void report_ortho_motion() {}
void report_vertex_interp() {}
} // namespace interp
} // namespace aurora::gfx

int main() {
    Observation first;
    Observation next;

    // Registration happens during game work. The frame boundary seals it for every retained
    // presentation of that completed tick; neither sample is a simulation-frame callback.
    sb::frame_interp::add_interpolation_callback(&observe, &first);
    sb::frame_interp::begin_sim_tick();
    sb::frame_interp::present_interpolated_frame(0.25f);
    CHECK(first.calls == 1);
    CHECK(!first.sawSimFrame);
    CHECK(first.lastAlpha == 0.25f);
    sb::frame_interp::present_interpolated_frame(0.75f);
    CHECK(first.calls == 2);
    CHECK(first.lastAlpha == 0.75f);

    // A registration after the boundary belongs to the next simulation tick. It must not mutate
    // the sealed list while this tick is being dispatched.
    sb::frame_interp::add_interpolation_callback(&observe, &next);
    sb::frame_interp::present_interpolated_frame(0.9f);
    CHECK(first.calls == 3);
    CHECK(next.calls == 0);

    // The next boundary replaces, rather than appends to, the sealed list.
    sb::frame_interp::begin_sim_tick();
    sb::frame_interp::present_interpolated_frame(0.5f);
    CHECK(first.calls == 3);
    CHECK(next.calls == 1);
    CHECK(!next.sawSimFrame);
    CHECK(next.lastAlpha == 0.5f);

    // Disabled interpolation refuses registrations instead of retaining them until a later enable.
    g_lerpEnabled = false;
    sb::frame_interp::add_interpolation_callback(&observe, &first);
    g_lerpEnabled = true;
    sb::frame_interp::begin_sim_tick();
    sb::frame_interp::present_interpolated_frame(0.5f);
    CHECK(first.calls == 3);
    CHECK(next.calls == 1);
}
