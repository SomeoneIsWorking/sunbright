// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_lighting_probe.h"

#include <sunbright/native_render/j3d_stage_lighting.h>

#include <cstdio>
#include <span>

namespace sunbright::gcnport_boot {
namespace {

// TLightCommon::setLight(const JDrama::TGraphics*, int): `this`, the graphics object and the index.
constexpr std::size_t THIS_REGISTER = 3;
constexpr std::size_t GRAPHICS_REGISTER = 4;
constexpr std::size_t INDEX_REGISTER = 5;

bool read_through_guest_context(sb::title_adapter::GuestAddress address,
                                std::span<std::uint8_t> destination, void* context) {
    auto* const guest = static_cast<gcnport::GuestContext*>(context);
    return guest->read_memory(address, std::as_writable_bytes(destination));
}

// A colour rounded to a byte per channel and packed, so two publications of the same authored
// colour land in the same histogram bucket while a genuinely different one does not.
[[nodiscard]] std::uint32_t quantize(const sb::native_render::Color& color) noexcept {
    const auto channel = [](float value) {
        const float clamped = value < 0.0F ? 0.0F : (value > 1.0F ? 1.0F : value);
        return static_cast<std::uint32_t>(clamped * 255.0F + 0.5F);
    };
    return channel(color.r) << 24U | channel(color.g) << 16U | channel(color.b) << 8U |
           channel(color.a);
}

void print_histogram(const char* label, const std::map<std::uint32_t, std::uint64_t>& histogram,
                     std::uint64_t untracked) {
    if (histogram.empty()) {
        std::printf("gmse01_boot:   %s: none recorded\n", label);
        return;
    }
    std::printf("gmse01_boot:   %s:", label);
    for (const auto& [value, count] : histogram) {
        std::printf(" %u=%llu", value, static_cast<unsigned long long>(count));
    }
    if (untracked != 0) {
        std::printf(" (+%llu past the tracked distinct values)",
                    static_cast<unsigned long long>(untracked));
    }
    std::printf("\n");
}

} // namespace

void GuestLightingProbe::record(std::map<std::uint32_t, std::uint64_t>& histogram,
                                std::uint64_t& untracked, std::uint32_t value) {
    if (histogram.size() < MAX_DISTINCT_VALUES || histogram.contains(value)) {
        histogram[value] += 1;
        return;
    }
    untracked += 1;
}

gcnport::HookResult GuestLightingProbe::operator()(gcnport::GuestContext& guest) {
    entries_ += 1;

    const auto light =
        static_cast<sb::title_adapter::GuestAddress>(guest.general_register(THIS_REGISTER));
    const auto graphics =
        static_cast<sb::title_adapter::GuestAddress>(guest.general_register(GRAPHICS_REGISTER));
    const auto index = static_cast<std::int32_t>(guest.general_register(INDEX_REGISTER));
    const sb::title_adapter::GuestMemory memory{read_through_guest_context, &guest};

    record(indices_, indicesUntracked_, static_cast<std::uint32_t>(index));

    sb::native_render::J3dStageLightingInput input{};
    sb::title_adapter::GuestStageLighting read{};
    const sb::title_adapter::GuestStageLightingError error =
        read_guest_stage_lighting(memory, light, graphics, index, {}, input, read);
    errors_[error] += 1;
    if (error != sb::title_adapter::GuestStageLightingError::None) {
        return gcnport::HookResult::call_original_once();
    }

    published_ += 1;
    usedLocalPosition_ += read.usedLocalPosition ? 1 : 0;
    usedLocalColor_ += read.usedLocalColor ? 1 : 0;
    effectEnabled_ += read.effectEnabled ? 1 : 0;
    record(lightSlots_, lightSlotsUntracked_, read.lightSlot);
    record(ambientSlots_, ambientSlotsUntracked_, read.ambientSlot);
    record(lightCounts_, lightCountsUntracked_, static_cast<std::uint32_t>(read.lightCount));
    record(ambientCounts_, ambientCountsUntracked_, static_cast<std::uint32_t>(read.ambientCount));
    const std::uint64_t rig = static_cast<std::uint64_t>(quantize(input.primaryColor)) << 32U |
                              quantize(input.ambientColor);
    if (rigs_.size() < MAX_DISTINCT_VALUES || rigs_.contains(rig)) {
        rigs_[rig] += 1;
    } else {
        rigsUntracked_ += 1;
    }

    // Published through the shipping path rather than kept here, so the material classifier reads
    // it exactly as the decomp runtime's own bridge leaves it.
    sb::native_render::publish_j3d_stage_lighting(input);

    if (reports_ < maxReports_) {
        reports_ += 1;
        std::printf("gmse01_boot:   stage light %llu: this=0x%08x graphics=0x%08x idx=%d "
                    "shininess=%.3f light=(%.1f,%.1f,%.1f) colour=%08x ambient=%08x effect=%d\n",
                    static_cast<unsigned long long>(reports_), light, graphics, index,
                    static_cast<double>(input.shininess),
                    static_cast<double>(input.primaryWorldPosition.x),
                    static_cast<double>(input.primaryWorldPosition.y),
                    static_cast<double>(input.primaryWorldPosition.z), quantize(input.primaryColor),
                    quantize(input.ambientColor), input.effectEnabled ? 1 : 0);
    }
    return gcnport::HookResult::call_original_once();
}

void GuestLightingProbe::report() const {
    std::printf("gmse01_boot: guest lighting probe: %llu relight(s), %llu published\n",
                static_cast<unsigned long long>(entries_),
                static_cast<unsigned long long>(published_));
    std::printf("gmse01_boot:   stage lighting errors:");
    if (errors_.empty()) {
        std::printf(" none recorded -- the hook never ran");
    }
    for (const auto& [error, count] : errors_) {
        std::printf(" %s=%llu", sb::title_adapter::guest_stage_lighting_error_name(error),
                    static_cast<unsigned long long>(count));
    }
    std::printf("\n");
    std::printf("gmse01_boot:   %llu used a local position, %llu a local colour, %llu enabled the "
                "effect light\n",
                static_cast<unsigned long long>(usedLocalPosition_),
                static_cast<unsigned long long>(usedLocalColor_),
                static_cast<unsigned long long>(effectEnabled_));
    print_histogram("relight indices", indices_, indicesUntracked_);
    print_histogram("light group slots", lightSlots_, lightSlotsUntracked_);
    print_histogram("light group sizes", lightCounts_, lightCountsUntracked_);
    print_histogram("ambient group slots", ambientSlots_, ambientSlotsUntracked_);
    print_histogram("ambient group sizes", ambientCounts_, ambientCountsUntracked_);
    std::printf("gmse01_boot:   distinct published rigs (light colour, ambient colour):");
    if (rigs_.empty()) {
        std::printf(" none recorded -- nothing was published");
    }
    for (const auto& [rig, count] : rigs_) {
        std::printf(" %08x/%08x=%llu", static_cast<std::uint32_t>(rig >> 32U),
                    static_cast<std::uint32_t>(rig), static_cast<unsigned long long>(count));
    }
    if (rigsUntracked_ != 0) {
        std::printf(" (+%llu past the tracked distinct values)",
                    static_cast<unsigned long long>(rigsUntracked_));
    }
    std::printf("\n");
}

} // namespace sunbright::gcnport_boot
