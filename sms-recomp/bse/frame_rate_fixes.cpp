// SPDX-License-Identifier: GPL-3.0-only
// BetterSunshineEngine frame-rate behavior adapted to Sunbright's runtime
// override seam. Source specification:
// https://github.com/JoshuaMKW/BetterSunshineEngine/blob/69baa4f15bfb2980670cbde638fc22f97a394385/src/patches/fps.cpp
//
// BSE installs PPC branches at individual call sites. Sunbright keeps the
// original recompiled bodies alive, so call-site-specific behavior is expressed
// as a scoped runtime override: run the original callee everywhere except the
// exact return addresses BSE patches.

#include "app/frame_rate.h"
#include "frame_rate_logic.h"
#include "overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <array>
#include <algorithm>
#include <cstdlib>

extern "C" void func_800067e8(CPUState&); // local sqrt helper
extern "C" void func_80151c88(CPUState&); // TTalk2D2::perform
extern "C" void func_80181d74(CPUState&); // HX_MotionUpdate
extern "C" void func_801f761c(CPUState&); // TJointCoin::loadAfter
extern "C" void func_80238f08(CPUState&); // MActor::getFrameCtrl
extern "C" void func_802a7bd8(CPUState&); // SMSGetAnmFrameRate

namespace {

constexpr u32 kBoidSqrtReturn = 0x800066e8u;

// Return PCs immediately after BSE's AnimalBird and BossEel FPS patch sites.
constexpr std::array<u32, 22> kFixedDeltaReturns{
    0x8000ceb4u, 0x8000d1dcu, 0x8000d1fcu,
    0x800d05a0u, 0x800d07a4u, 0x800d089cu, 0x800d0b64u,
    0x800d0e10u, 0x800d112cu, 0x800d12f4u, 0x800d1480u,
    0x800d15c4u, 0x800d1c9cu, 0x800d1d6cu, 0x800d2080u,
    0x800d2368u, 0x800d243cu, 0x800d24fcu, 0x800d2714u,
    0x800d2adcu, 0x800d2f90u, 0x800d3354u,
};

bool uses_fixed_delta(u32 returnPc) {
    return std::find(kFixedDeltaReturns.begin(), kFixedDeltaReturns.end(),
                     returnPc) != kFixedDeltaReturns.end();
}

void bse_sqrt(CPUState& cpu) {
    func_800067e8(cpu);
    if (cpu.lr == kBoidSqrtReturn)
        cpu.fpr[1].ps0 *= sb::app::frame_rate::boid_speed_scale();
}

void bse_animation_rate(CPUState& cpu) {
    if (uses_fixed_delta(cpu.lr)) {
        cpu.fpr[1].ps0 = sb::app::frame_rate::fixed_delta_animation_rate();
        return;
    }
    func_802a7bd8(cpu);
}

void set_actor_frame_rate(CPUState& cpu, u32 actor) {
    CPUState accessor = cpu;
    accessor.gpr[3] = actor;
    accessor.gpr[4] = 0;
    func_80238f08(accessor);
    const u32 frameCtrl = accessor.gpr[3];
    if (sb_ram_fast(frameCtrl) == nullptr) {
        lucent::error("bse", "TJointCoin returned invalid frame controller 0x{:08x}",
                      frameCtrl);
        std::abort();
    }
    sb_wf32(frameCtrl + 0x0c,
             sb::app::frame_rate::joint_coin_animation_rate());
}

void bse_joint_coin_load_after(CPUState& cpu) {
    const u32 self = cpu.gpr[3];
    func_801f761c(cpu);
    set_actor_frame_rate(cpu, sb_r32(self + 0x74));
    set_actor_frame_rate(cpu, sb_r32(self + 0x138));
}

void bse_talk_perform(CPUState& cpu) {
    constexpr u32 kTalkMode = 0x248;
    constexpr u32 kEntryTimer = 0x251;
    constexpr u32 kOpeningMode = 3;
    const u32 self = cpu.gpr[3];
    const u32 before = sb_r32(self + kTalkMode);
    func_80151c88(cpu);
    const u32 after = sb_r32(self + kTalkMode);
    if (before != kOpeningMode && after == kOpeningMode)
        sb_w8(self + kEntryTimer, static_cast<u8>(
            sb::app::frame_rate::textbox_entry_frames()));
}

void bse_hx_motion_update(CPUState& cpu) {
    const u32 state = cpu.gpr[3];
    std::array<float, 9> values;
    for (unsigned index = 0; index < values.size(); ++index)
        values[index] = sb_rf32(state + index * sizeof(float));
    const float position = sb::bse::advance_hx_motion(
        values, static_cast<float>(sb::app::frame_rate::game_rate_multiplier()));
    for (unsigned index = 6; index < values.size(); ++index)
        sb_wf32(state + index * sizeof(float), values[index]);
    cpu.fpr[1].ps0 = position;
}

} // namespace

SB_OVERRIDE(0x800067e8u, bse_sqrt, "sqrt (boid-local call site)",
            "BetterSunshineEngine: keep boid travel speed stable across native game rates")
SB_OVERRIDE(0x802a7bd8u, bse_animation_rate, "SMSGetAnmFrameRate",
            "BetterSunshineEngine: fixed-delta AnimalBird and BossEel animations")
SB_OVERRIDE(0x801f761cu, bse_joint_coin_load_after, "TJointCoin::loadAfter",
            "BetterSunshineEngine: stable joint-coin and Sand Bird animation rate")
SB_OVERRIDE(0x80151c88u, bse_talk_perform, "TTalk2D2::perform",
            "BetterSunshineEngine: scale textbox opening timer with native game rate")
SB_OVERRIDE(0x80181d74u, bse_hx_motion_update, "HX_MotionUpdate",
            "BetterSunshineEngine: integrate wipe motion at the selected native game rate")
