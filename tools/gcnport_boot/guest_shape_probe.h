// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>

#include <sunbright/title_adapter/guest_j3d_shape.h>

#include "gcnport/guest_context.h"
#include "gcnport/native_hooks.h"

namespace sunbright::gcnport_boot {

// A native hook that reads the J3DShape the guest is about to draw, through the same
// `title-adapter` code the eventual override will use, and reports what it found.
//
// It does not replace anything: every entry ends in `call_original_once`, so the title draws
// exactly as it did and this measures the reader rather than the renderer. That is the point --
// `title-adapter`'s own test proves the offsets against a synthetic image it wrote itself, which
// cannot tell whether those offsets describe the retail objects GMSE01 actually builds. Only the
// real title can answer that, and only by being read while it runs.
//
// Bounded in the two directions that matter. `max_reports` caps the per-shape detail, because a
// function entered a quarter of a million times cannot print per entry; the histograms below are
// unbounded in coverage and bounded in size, so they describe every entry without growing with the
// run. Both are printed even when they are empty: a probe that reports nothing has not told you
// the title drew nothing, only that it never looked.
class GuestShapeProbe {
  public:
    GuestShapeProbe(sb::title_adapter::GuestAddress system, std::uint64_t max_reports) noexcept
        : system_(system), maxReports_(max_reports) {}

    gcnport::HookResult operator()(gcnport::GuestContext& guest);

    void report() const;

  private:
    // Distinct values are few by construction -- element counts and vertex strides come from a
    // model loader, not from arbitrary data -- but the cap means a corrupt read cannot turn this
    // into unbounded growth, and anything past it is counted rather than dropped.
    static constexpr std::size_t MAX_DISTINCT_VALUES = 32;

    void record(std::map<std::uint32_t, std::uint64_t>& histogram, std::uint64_t& untracked,
                std::uint32_t value);

    sb::title_adapter::GuestAddress system_ = 0;
    std::uint64_t maxReports_ = 0;

    std::uint64_t entries_ = 0;
    std::uint64_t reports_ = 0;
    std::uint64_t shapesRead_ = 0;
    std::uint64_t elementsRead_ = 0;
    std::uint64_t displayListBytes_ = 0;

    std::map<sb::title_adapter::GuestShapeError, std::uint64_t> shapeErrors_;
    std::map<sb::title_adapter::GuestShapeError, std::uint64_t> elementErrors_;
    std::map<std::uint32_t, std::uint64_t> elementCounts_;
    std::uint64_t elementCountsUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> vertexSizes_;
    std::uint64_t vertexSizesUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> descriptorCounts_;
    std::uint64_t descriptorCountsUntracked_ = 0;
};

} // namespace sunbright::gcnport_boot
