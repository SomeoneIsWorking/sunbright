// dev_ai.cpp — AI (audio interface) registers.
//
// AI is the DMA engine that streams samples out to the DAC. This port sends audio to the
// host instead, so nothing here drives a real output — but the block is also a CLOCK, and
// OS code reads its sample counter during init. That counter must actually advance: code
// that samples it twice to measure an interval would divide by zero, or spin forever
// waiting for it to move, if it were frozen.

#include "mmio.h"

#include <lucent/log.h>

#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

constexpr u32 AI_CONTROL      = 0x6C00;
constexpr u32 AI_VOLUME       = 0x6C04;
constexpr u32 AI_SAMPLE_COUNT = 0x6C08;
constexpr u32 AI_INT_TIMING   = 0x6C0C;

// The DAC runs at 48 kHz; the sample counter ticks at that rate whenever the interface is
// enabled. Deriving it from a real monotonic clock keeps intervals measured against it
// honest, rather than inventing a cadence.
constexpr u64 kSampleRate = 48000;

u32 g_reg[4];
struct timespec g_t0;

u64 now_ns() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)(t.tv_sec - g_t0.tv_sec) * 1000000000ull + (u64)t.tv_nsec - (u64)g_t0.tv_nsec;
}

u32& reg(u32 off) { return g_reg[(off - 0x6C00) >> 2]; }

u32 ai_read(u32 ea, unsigned width) {
    const u32 o = ea & 0xFFFFu;
    if (width != 4) {
        lucent::error("ai", "unsupported {}-byte read @ 0x{:08x}", width, ea);
        std::abort();
    }
    if (o == AI_SAMPLE_COUNT) return (u32)(now_ns() * kSampleRate / 1000000000ull);
    return reg(o);
}

void ai_write(u32 ea, unsigned width, u32 value) {
    const u32 o = ea & 0xFFFFu;
    if (width != 4) {
        lucent::error("ai", "unsupported {}-byte write @ 0x{:08x}", width, ea);
        std::abort();
    }
    // Writing the sample counter resets it — that is how the SDK zeroes the clock before
    // timing something, so honour it by rebasing rather than storing a value we ignore.
    if (o == AI_SAMPLE_COUNT) { clock_gettime(CLOCK_MONOTONIC, &g_t0); return; }
    reg(o) = value;
}

} // namespace

void ai_device_init() {
    std::memset(g_reg, 0, sizeof(g_reg));
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    reg(AI_CONTROL)    = 0;
    reg(AI_VOLUME)     = 0;
    reg(AI_INT_TIMING) = 0;
    mmio_register(MmioDevice{0xCC006C00u, 0xCC006C10u, "ai", &ai_read, &ai_write});
}
