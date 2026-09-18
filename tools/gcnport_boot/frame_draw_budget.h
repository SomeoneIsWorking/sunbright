// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

// Which of each frame's draws reach the sink.
//
// A defect that appears somewhere in a stack of blended draws cannot be attributed from the
// finished frame: every draw contributed to the pixel. Rendering a bounded prefix and moving the
// bound is what attributes it -- the frame either shows the defect or does not, and the draw where
// that changes is the one that introduces it.
//
// Bounding the prefix names the draw; it does not show it. Dropping a prefix as well leaves a
// single draw on an empty frame, which is the only view in which that draw's own geometry and
// colour can be read rather than inferred from what it did to what was already there.
//
// The renderer owns when a frame begins and the publisher owns what a draw is. Neither owns the
// other, so the count they share is its own object rather than a field one of them reaches into.

namespace sunbright::gcnport_boot {

class FrameDrawBudget {
  public:
    // A skip of zero and a limit of zero is unbounded, which is what an ordinary run passes: the
    // budget is then inert and every draw is taken.
    FrameDrawBudget(std::uint64_t skip, std::uint64_t limit) noexcept
        : skip_(skip), limit_(limit) {}

    void begin_frame() noexcept {
        frames_ += 1;
        taken_ = 0;
        offered_ = 0;
    }

    // Answers whether this draw is within the frame's budget, and counts it either way. The skip
    // is counted against the same ordinal the limit is, so `--draw-skip 29 --draw-limit 1` is
    // exactly draw 30 of the listing and of every bisection that named it.
    [[nodiscard]] bool take() noexcept {
        offered_ += 1;
        if (offered_ <= skip_) {
            withheld_ += 1;
            return false;
        }
        if (limit_ == 0 || taken_ < limit_) {
            taken_ += 1;
            return true;
        }
        withheld_ += 1;
        return false;
    }

    // This frame's draw ordinal, 1-based, counting withheld draws: the number a bound of N
    // admits up to. A listing numbered by anything else could not be lined up with a bisection.
    [[nodiscard]] std::uint64_t offered() const noexcept { return offered_; }
    [[nodiscard]] bool bounded() const noexcept { return limit_ != 0 || skip_ != 0; }
    [[nodiscard]] std::uint64_t limit() const noexcept { return limit_; }
    [[nodiscard]] std::uint64_t skip() const noexcept { return skip_; }
    [[nodiscard]] std::uint64_t withheld() const noexcept { return withheld_; }
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }

  private:
    std::uint64_t skip_ = 0;
    std::uint64_t limit_ = 0;
    std::uint64_t taken_ = 0;
    std::uint64_t offered_ = 0;
    std::uint64_t withheld_ = 0;
    std::uint64_t frames_ = 0;
};

} // namespace sunbright::gcnport_boot
