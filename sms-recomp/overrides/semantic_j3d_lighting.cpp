#include "semantic_j3d_lighting.h"
#include "guest_byte_reader.h"
#include "overrides.h"

#include <sunbright/native_render/j3d_stage_lighting.h>
#include <sunbright/native_render/semantic_sink.h>

#include <algorithm>
#include <array>
#include <cstdint>

extern "C" void func_80229610(CPUState&); // TLightMario::setLight
extern "C" void func_80229a30(CPUState&); // TLightCommon::setLight
extern "C" void func_80229ca0(CPUState&); // TLightCommon::getLightPosition
extern "C" void func_80229cec(CPUState&); // TLightCommon::getAmbColor
extern "C" void func_80229d78(CPUState&); // TLightCommon::getLightColor

namespace {

constexpr u32 kViewMatrix = 0xB4;
constexpr u32 kShininess = 0x10;
constexpr u32 kLightManagerSdaOffset = 24844;
constexpr u32 kEffectColor = 0x18;
constexpr u32 kEffectPosition = 0x1C;
constexpr u32 kEffectAlphaScale = 0x28;
constexpr u32 kEffectEnabled = 0x54;
constexpr u32 kEffectValid = 0x55;

sb::recomp::SemanticJ3dLightingStats g_stats{};

enum class PublishResult : std::uint8_t {
    Success,
    ViewFailure,
    ShininessFailure,
    PrimaryPositionFailure,
    ManagerFailure,
    EffectFailure,
};

bool read_vec3(const sb::recomp::BigEndianGuestReader& reader, u32 address,
               sb::native_render::Vec3& value) noexcept {
    return reader.f32(address, value.x) && reader.f32(address + 4, value.y) &&
           reader.f32(address + 8, value.z);
}

bool read_view(const sb::recomp::BigEndianGuestReader& reader, u32 graphics,
               sb::native_render::Matrix3x4& view) noexcept {
    for (std::size_t index = 0; index < view.value.size(); ++index) {
        if (!reader.f32(graphics + kViewMatrix + static_cast<u32>(index * sizeof(float)),
                        view.value[index])) {
            return false;
        }
    }
    return true;
}

bool call_light_position(const CPUState& source, u32 self, u32 index,
                         const sb::recomp::BigEndianGuestReader& reader,
                         sb::native_render::Vec3& position) noexcept {
    CPUState probe = source;
    probe.gpr[3] = self;
    probe.gpr[4] = index;
    func_80229ca0(probe);
    return probe.gpr[3] != 0 && read_vec3(reader, probe.gpr[3], position);
}

sb::native_render::Color call_light_color(const CPUState& source, u32 self, u32 index) noexcept {
    CPUState probe = source;
    probe.gpr[3] = self;
    probe.gpr[4] = index;
    func_80229d78(probe);
    return sb::native_render::color_from_rgba8(probe.gpr[3]);
}

sb::native_render::Color call_ambient_color(const CPUState& source, u32 self, u32 index) noexcept {
    CPUState probe = source;
    probe.gpr[3] = self;
    probe.gpr[4] = index;
    func_80229cec(probe);
    return sb::native_render::color_from_rgba8(probe.gpr[3]);
}

PublishResult publish_lighting(const CPUState& cpu, u32 self, u32 graphics, u32 index) noexcept {
    const sb::recomp::BigEndianGuestReader reader(sb::recomp::live_guest_byte_reader());
    sb::native_render::J3dStageLightingInput input{};
    const u32 lightIndex = index * 2U;
    if (!read_view(reader, graphics, input.view))
        return PublishResult::ViewFailure;
    if (!reader.f32(self + kShininess, input.shininess))
        return PublishResult::ShininessFailure;
    if (!call_light_position(cpu, self, lightIndex, reader, input.primaryWorldPosition))
        return PublishResult::PrimaryPositionFailure;
    input.primaryColor = call_light_color(cpu, self, lightIndex);
    input.ambientColor = call_ambient_color(cpu, self, index);

    u32 manager = 0;
    std::uint8_t enabled = 0;
    std::uint8_t valid = 0;
    if (!reader.u32(cpu.gpr[13] - kLightManagerSdaOffset, manager)) {
        return PublishResult::ManagerFailure;
    }
    if (manager != 0 && (!reader.u8(manager + kEffectEnabled, enabled) ||
                         !reader.u8(manager + kEffectValid, valid))) {
        return PublishResult::ManagerFailure;
    }
    input.effectEnabled = manager != 0 && enabled != 0 && valid != 0;
    if (input.effectEnabled) {
        std::uint32_t packedColor = 0;
        float alphaScale = 0.0F;
        if (!read_vec3(reader, manager + kEffectPosition, input.effectWorldPosition) ||
            !reader.u32(manager + kEffectColor, packedColor) ||
            !reader.f32(manager + kEffectAlphaScale, alphaScale)) {
            return PublishResult::EffectFailure;
        }
        input.effectColor = sb::native_render::color_from_rgba8(packedColor);
        input.effectColor.a = std::clamp(input.effectColor.a * alphaScale, 0.0F, 1.0F);
    }
    sb::native_render::publish_j3d_stage_lighting(input);
    return PublishResult::Success;
}

void run_set_light(CPUState& cpu, void (*body)(CPUState&)) {
    const u32 self = cpu.gpr[3];
    const u32 graphics = cpu.gpr[4];
    const u32 index = cpu.gpr[5];
    body(cpu);
    if (!sb::native_render::has_semantic_sink()) {
        sb::native_render::clear_j3d_stage_lighting();
        return;
    }
    ++g_stats.attempts;
    const PublishResult result = publish_lighting(cpu, self, graphics, index);
    if (result == PublishResult::Success) {
        ++g_stats.published;
        return;
    }
    if (result == PublishResult::ViewFailure)
        ++g_stats.viewFailures;
    else if (result == PublishResult::ShininessFailure)
        ++g_stats.shininessFailures;
    else if (result == PublishResult::PrimaryPositionFailure)
        ++g_stats.primaryPositionFailures;
    else if (result == PublishResult::ManagerFailure)
        ++g_stats.managerFailures;
    else if (result == PublishResult::EffectFailure)
        ++g_stats.effectFailures;
    sb::native_render::clear_j3d_stage_lighting();
}

void common_set_light(CPUState& cpu) {
    run_set_light(cpu, func_80229a30);
}

void mario_set_light(CPUState& cpu) {
    run_set_light(cpu, func_80229610);
}

} // namespace

namespace sb::recomp {

SemanticJ3dLightingStats semantic_j3d_lighting_stats() noexcept {
    return g_stats;
}

} // namespace sb::recomp

SB_OVERRIDE(0x80229a30u, common_set_light, "TLightCommon::setLight",
            "semantic renderer: publish high-level stage point lights; real body runs")
SB_OVERRIDE(0x80229610u, mario_set_light, "TLightMario::setLight",
            "semantic renderer: publish high-level player point lights; real body runs")
