// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace Memory {
class MemoryManager;
}

namespace sunbright::gcnport_boot {

// One --watch-guest request: a window of guest memory whose every change is printed.
//
// The window is named either by its own address or, for an `indirect` watch, by the address of a
// pointer to it plus an `offset`. Almost every piece of state worth watching in GMSE01 is a field
// of a singleton reached through a small-data slot, and that slot is null until the object is
// constructed, so naming the field directly would mean running once to learn the address and again
// to read it.
struct GuestWatch {
    std::uint32_t address = 0;
    std::uint32_t words = 0;
    bool indirect = false;
    std::uint32_t offset = 0;

    std::vector<std::uint32_t> previous;
    std::uint64_t samples = 0;
    std::uint64_t changes = 0;
    // For an indirect watch: the window the pointer last resolved to, and how many samples found
    // no window at all. A pointer that is still null is why a watch printed nothing, and saying so
    // is the difference between "this state never changed" and "this state was never looked at".
    std::uint32_t resolved = 0;
    std::uint64_t unresolvedSamples = 0;
};

// The set of windows a run follows, from the line that says what it will watch to the line that
// says what it saw.
//
// This owns the whole of a watch's life so that a run which followed nothing still says so: a
// sample that cannot resolve its pointer is counted rather than dropped, and the closing report
// names that count. A watch that silently printed nothing would read exactly like state that never
// changed.
class GuestWatchSet {
  public:
    explicit GuestWatchSet(std::vector<GuestWatch> watches) : watches_(std::move(watches)) {}

    [[nodiscard]] bool empty() const noexcept { return watches_.empty(); }

    // Prints what this run will follow, before it follows anything.
    void announce() const;
    void sample(const Memory::MemoryManager& memory, std::uint64_t blocks_run,
                std::uint32_t retrace_count);
    void report() const;

  private:
    std::vector<GuestWatch> watches_;
};

} // namespace sunbright::gcnport_boot
