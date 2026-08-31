#include <MarioUtil/DamageFog.hpp>

#include <cassert>
#include <cmath>

namespace {

bool near(f32 actual, f32 expected) {
    return std::fabs(actual - expected) < 0.0001f;
}

} // namespace

int main() {
    const SMSDamageFog::Range reset = SMSDamageFog::resetRange(300000.0f);
    assert(near(reset.start, 299999.0f));
    assert(near(reset.end, 300000.0f));

    const SMSDamageFog::Range centered = SMSDamageFog::activeRange(-1000.0f, 0.0f);
    assert(near(centered.start, 300.0f));
    assert(near(centered.end, 1500.0f));

    const SMSDamageFog::Range positivePulse = SMSDamageFog::activeRange(-1000.0f, 1.0f);
    assert(near(positivePulse.start, 600.0f));
    assert(near(positivePulse.end, 1800.0f));

    const SMSDamageFog::Range negativePulse = SMSDamageFog::activeRange(-1000.0f, -1.0f);
    assert(near(negativePulse.start, 0.0f));
    assert(near(negativePulse.end, 1200.0f));

    assert(SMSDamageFog::waveAngle(0) == 0);
    assert(SMSDamageFog::waveAngle(1) == static_cast<s16>(0x0888));
    assert(SMSDamageFog::waveAngle(30) == -16);
    assert(SMSDamageFog::waveAngle(31) == static_cast<s16>(0x0878));
    assert(SMSDamageFog::waveAngle(32) == static_cast<s16>(0x1100));
}
