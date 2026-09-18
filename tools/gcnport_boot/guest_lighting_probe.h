// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>

#include <sunbright/title_adapter/guest_stage_lighting.h>

#include "gcnport/guest_context.h"
#include "gcnport/native_hooks.h"

namespace sunbright::gcnport_boot {

// A native hook on GMSE01's stage-light owner that reads the light rig it is about to broadcast and
// publishes it through `native_render::publish_j3d_stage_lighting`.
//
// This is the input the material probe was missing. Measured without it, 37,482 of the 44,314
// material packets this scene draws were refused by every family for one reason -- `lighting` --
// because a lit family cannot be classified against a `ModelLightingContext` that does not exist.
//
// The seam is `TLightCommon::setLight` (0x80229a30) and its byte-identical override
// `TLightMario::setLight` (0x80229610), taken at entry: `this` in r3, the `JDrama::TGraphics*` in
// r4, and the light index in r5. Entry is the right moment because every value the shared input
// needs is already reachable from those three -- the view matrix is inline in the graphics object,
// and the light and ambient colours come from the scene's group arrays through the same offsets the
// game's own getters use. Nothing is replaced: every entry ends in `call_original_once`, so the
// title configures GX exactly as it did.
class GuestLightingProbe {
  public:
    explicit GuestLightingProbe(std::uint64_t max_reports) noexcept : maxReports_(max_reports) {}

    gcnport::HookResult operator()(gcnport::GuestContext& guest);

    void report() const;

  private:
    // Light indices are small by construction -- a scene's light groups hold a handful of entries.
    // The cap bounds a misread rather than letting it grow a histogram without limit.
    static constexpr std::size_t MAX_DISTINCT_VALUES = 32;

    void record(std::map<std::uint32_t, std::uint64_t>& histogram, std::uint64_t& untracked,
                std::uint32_t value);

    std::uint64_t maxReports_ = 0;
    std::uint64_t entries_ = 0;
    std::uint64_t reports_ = 0;
    std::uint64_t published_ = 0;
    std::uint64_t usedLocalPosition_ = 0;
    std::uint64_t usedLocalColor_ = 0;
    std::uint64_t effectEnabled_ = 0;
    std::map<sb::title_adapter::GuestStageLightingError, std::uint64_t> errors_;
    std::map<std::uint32_t, std::uint64_t> indices_;
    std::uint64_t indicesUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> lightSlots_;
    std::uint64_t lightSlotsUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> ambientSlots_;
    std::uint64_t ambientSlotsUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> lightCounts_;
    std::uint64_t lightCountsUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> ambientCounts_;
    std::uint64_t ambientCountsUntracked_ = 0;
    // The published rig itself, quantized so the histogram counts distinct rigs rather than float
    // noise. A scene that relights every frame with the same values should show one entry.
    std::map<std::uint64_t, std::uint64_t> rigs_;
    std::uint64_t rigsUntracked_ = 0;
};

} // namespace sunbright::gcnport_boot
