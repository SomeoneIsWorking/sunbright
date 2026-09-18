// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

// How many of each frame's draws reach the sink.
//
// A defect that appears somewhere in a stack of blended draws cannot be attributed from the
// finished frame: every draw contributed to the pixel. Rendering a bounded prefix and moving the
// bound is what attributes it -- the frame either shows the defect or does not, and the draw where
// that changes is the one that introduces it.
//
// The renderer owns when a frame begins and the publisher owns what a draw is. Neither owns the
// other, so the count they share is its own object rather than a field one of them reaches into.

namespace sunbright::gcnport_boot {

class FrameDrawBudget {
  public:
    // A limit of zero is unbounded, which is what an ordinary run passes: the budget is then inert
    // and every draw is taken.
    explicit FrameDrawBudget(std::uint64_t limit) noexcept : limit_(limit) {}

    void begin_frame() noexcept {
        frames_ += 1;
        taken_ = 0;
    }

    // Answers whether this draw is within the frame's budget, and counts it either way.
    [[nodiscard]] bool take() noexcept {
        if (limit_ == 0 || taken_ < limit_) {
            taken_ += 1;
            return true;
        }
        withheld_ += 1;
        return false;
    }

    [[nodiscard]] bool bounded() const noexcept { return limit_ != 0; }
    [[nodiscard]] std::uint64_t limit() const noexcept { return limit_; }
    [[nodiscard]] std::uint64_t withheld() const noexcept { return withheld_; }
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }

  private:
    std::uint64_t limit_ = 0;
    std::uint64_t taken_ = 0;
    std::uint64_t withheld_ = 0;
    std::uint64_t frames_ = 0;
};

} // namespace sunbright::gcnport_boot
