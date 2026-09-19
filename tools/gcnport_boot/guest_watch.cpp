// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_watch.h"

#include <cstdio>
#include <utility>

#include "Core/HW/Memmap.h"

namespace sunbright::gcnport_boot {
namespace {

constexpr std::uint32_t GUEST_RAM_START = 0x80000000;
constexpr std::uint32_t GUEST_RAM_END = 0x81800000;

// Where an indirect watch's window currently is, or zero if the pointer does not lead to one.
//
// A small-data singleton slot is zero until its object is constructed, and a run that samples it
// before then must say it found nothing rather than dump the words at the bottom of the guest's
// address space and let them read as state.
std::uint32_t resolve(const Memory::MemoryManager& memory, const GuestWatch& watch) {
    if (!watch.indirect) {
        return watch.address;
    }
    const std::uint32_t pointer = memory.Read_U32(watch.address);
    if (pointer < GUEST_RAM_START || pointer % 4 != 0) {
        return 0;
    }
    const std::uint32_t base = pointer + watch.offset;
    if (base < GUEST_RAM_START || base + watch.words * 4 > GUEST_RAM_END) {
        return 0;
    }
    return base;
}

void sample_one(const Memory::MemoryManager& memory, GuestWatch& watch, std::uint64_t blocks_run,
                std::uint32_t retrace_count) {
    watch.samples += 1;
    const std::uint32_t base = resolve(memory, watch);
    if (base == 0) {
        watch.unresolvedSamples += 1;
        return;
    }
    if (base != watch.resolved) {
        std::printf("gmse01_boot: watch 0x%08x now follows 0x%08x at block %llu (retrace %u)\n",
                    watch.address, base, static_cast<unsigned long long>(blocks_run),
                    retrace_count);
        watch.resolved = base;
        watch.previous.clear();
    }
    std::vector<std::uint32_t> current(watch.words);
    for (std::uint32_t word = 0; word < watch.words; ++word) {
        current[word] = memory.Read_U32(base + word * 4);
    }
    if (current == watch.previous) {
        return;
    }
    const bool first = watch.previous.empty();
    if (!first) {
        watch.changes += 1;
    }
    std::printf("gmse01_boot: watch 0x%08x %s at block %llu (retrace %u):", base,
                first ? "first sample" : "changed", static_cast<unsigned long long>(blocks_run),
                retrace_count);
    for (const std::uint32_t word : current) {
        std::printf(" %08x", word);
    }
    std::printf("\n");
    watch.previous = std::move(current);
}

} // namespace

void GuestWatchSet::announce() const {
    for (const GuestWatch& watch : watches_) {
        if (watch.indirect) {
            std::printf("gmse01_boot: watching %u word(s) at offset 0x%x of whatever 0x%08x "
                        "points at\n",
                        watch.words, watch.offset, watch.address);
        } else {
            std::printf("gmse01_boot: watching 0x%08x (%u word(s)) for changes\n", watch.address,
                        watch.words);
        }
    }
}

void GuestWatchSet::sample(const Memory::MemoryManager& memory, std::uint64_t blocks_run,
                           std::uint32_t retrace_count) {
    for (GuestWatch& watch : watches_) {
        sample_one(memory, watch, blocks_run, retrace_count);
    }
}

void GuestWatchSet::report() const {
    for (const GuestWatch& watch : watches_) {
        std::printf("gmse01_boot: watch 0x%08x sampled %llu time(s), changed %llu time(s)",
                    watch.address, static_cast<unsigned long long>(watch.samples),
                    static_cast<unsigned long long>(watch.changes));
        if (watch.indirect) {
            std::printf("; %llu sample(s) found no window behind the pointer",
                        static_cast<unsigned long long>(watch.unresolvedSamples));
            if (watch.resolved != 0) {
                std::printf(", last followed 0x%08x", watch.resolved);
            }
        }
        std::printf("\n");
    }
}

} // namespace sunbright::gcnport_boot
