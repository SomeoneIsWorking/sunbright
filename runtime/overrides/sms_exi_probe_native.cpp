// Native __EXIProbe — drops the emulated-time insertion debounce (boot determinism).
//
// The SDK __EXIProbe (0x8036A2D8) reports whether a device is present on an EXI channel, but
// holds the answer FALSE until ~3 time units have elapsed since the device was first seen
// (OSGetTime-based, per-channel start time at 0x800030C0+chan*4) — physical contact-bounce
// protection. The TCardManager boot thread busy-loops CARDProbeEx→__EXIProbe until that window
// elapses, which makes boot progress depend on emulated time crawling forward. An emulated EXI
// device does not bounce: this native port keeps the full attach/detach CSR bookkeeping and
// answers from the live EXT bit immediately, with no time gate. (Disassembly-derived layout:
// ecb = 0x804040A0 + chan*0x40, state at +0xC (bit 0x8 = attached), idTime at +0x20;
// CSR = 0xCC006800 + chan*0x14, bit 0x1000 = EXT (device present), bit 0x800 = EXT IRQ latch.)

#include "../overrides.h"
#include "../intrinsics.h"

#ifdef HAVE_DOLPHIN_CORE

namespace {

constexpr u32 ECB_BASE     = 0x804040A0u;  // EXI channel control blocks, stride 0x40
constexpr u32 PROBE_START  = 0x800030C0u;  // __EXIProbeStartTime[chan]
constexpr u32 EXI_CSR_BASE = 0xCC006800u;  // channel status register, stride 0x14

// 0x8036A2D8 __EXIProbe(int chan) -> BOOL
SUNBRIGHT_OVERRIDE(ov___EXIProbe, 0x8036A2D8u) {
    const u32 chan = cpu.gpr[3];
    if (chan == 2) { cpu.gpr[3] = 1; return; }   // chan 2 (SP1) always "present"

    const u32 ecb   = ECB_BASE + chan * 0x40;
    const u32 start = PROBE_START + chan * 4;
    const u32 csr_a = EXI_CSR_BASE + chan * 0x14;
    const u32 csr   = sb_r32(csr_a);
    const u32 state = sb_r32(ecb + 0x0C);
    u32 ret = 1;

    if (!(state & 0x8u)) {                       // not attached yet
        if (csr & 0x800u) {                      // EXT IRQ latched (insert/remove edge): ack it
            sb_w32(csr_a, (csr & 0x7F5u) | 0x800u);
            sb_w32(ecb + 0x20, 0);
            sb_w32(start, 0);
        }
        if (csr & 0x1000u) {                     // device present — no debounce, answer now
            if (sb_r32(start) == 0) sb_w32(start, 1);
        } else {
            sb_w32(ecb + 0x20, 0);
            sb_w32(start, 0);
            ret = 0;
        }
    } else {                                     // attached: present and no pending detach edge
        if (!(csr & 0x1000u) || (csr & 0x800u)) {
            sb_w32(ecb + 0x20, 0);
            sb_w32(start, 0);
            ret = 0;
        }
    }
    cpu.gpr[3] = ret;
}

}  // namespace

#endif  // HAVE_DOLPHIN_CORE
