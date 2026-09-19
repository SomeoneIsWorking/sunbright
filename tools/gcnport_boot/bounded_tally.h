// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>

namespace sunbright::gcnport_boot {

// Counting how often each distinct value a probe sees occurs, with a bound on how many distinct
// ones are kept.
//
// A title's authored values -- copy regions, viewport rectangles, projection matrices -- are a
// handful repeated thousands of times, and that is the answer these probes exist to report. What
// makes the bound necessary is the case where the reading is wrong: a misread field produces a new
// value almost every time, and an unbounded map of them grows without limit while saying nothing.
//
// Past the bound the count of values not kept is reported separately rather than folded into the
// total, so the set is always either complete or visibly incomplete. A probe that silently dropped
// them would print a set that looks authored and is not.
template <typename Key> class BoundedTally {
  public:
    explicit BoundedTally(std::size_t capacity) noexcept : capacity_(capacity) {}

    void add(const Key& key) {
        if (counts_.size() < capacity_ || counts_.contains(key)) {
            counts_[key] += 1;
            return;
        }
        pastCapacity_ += 1;
    }

    [[nodiscard]] const std::map<Key, std::uint64_t>& counts() const noexcept { return counts_; }
    [[nodiscard]] std::uint64_t past_capacity() const noexcept { return pastCapacity_; }
    [[nodiscard]] bool complete() const noexcept { return pastCapacity_ == 0; }

  private:
    std::size_t capacity_ = 0;
    std::uint64_t pastCapacity_ = 0;
    std::map<Key, std::uint64_t> counts_;
};

// Prints a tally as one line: the label, then each kept value and its count, then -- when the set
// is incomplete -- how many occurrences were not kept. The incomplete case is printed here rather
// than by each caller, because a caller that forgets it prints a bounded set as if it were the
// whole of what the title did, which is the one reading this type exists to prevent.
//
// `describe` writes one key, without a leading space; the separator is this function's.
template <typename Key, typename Describe>
void print_tally(const char* label, const BoundedTally<Key>& tally, Describe describe) {
    std::printf("gmse01_boot:   %s:", label);
    for (const auto& [key, count] : tally.counts()) {
        std::printf(" ");
        describe(key);
        std::printf("=%llu", static_cast<unsigned long long>(count));
    }
    if (!tally.complete()) {
        std::printf(" (+%llu past the set)",
                    static_cast<unsigned long long>(tally.past_capacity()));
    }
    std::printf("\n");
}

} // namespace sunbright::gcnport_boot
