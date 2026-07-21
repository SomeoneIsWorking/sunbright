#pragma once
#include <cstdint>
#include <cstring>

// Gekko (PowerPC 750CL) CPU state shared between recompiled code and runtime.
// All PPC registers the recompiler can read/write.
// 32-bit mode only (GC never uses 64-bit mode).

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8  = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
using f32 = float;
using f64 = double;

struct CRField {
    u8 lt : 1;
    u8 gt : 1;
    u8 eq : 1;
    u8 so : 1;
};

struct XERReg {
    u8 so : 1;  // summary overflow
    u8 ov : 1;  // overflow
    u8 ca : 1;  // carry
    u8 pad : 5;
    u8 byte_count;
};

// Paired-single register: each FPR holds two f32 ps0/ps1
struct PSReg {
    f64 ps0;
    f64 ps1;
};

struct CPUState {
    u32    gpr[32];     // General-purpose registers
    PSReg  fpr[32];     // FP regs (ps0 = high half used for scalar FP)
    u32    lr;          // Link register
    u32    ctr;         // Count register
    CRField cr[8];      // Condition register fields CR0–CR7
    XERReg xer;         // XER
    u32    srr0;        // Save/restore register 0 (exception PC)
    u32    srr1;        // Save/restore register 1 (exception MSR)
    u32    fpscr;       // FP status/control
    u32    pc;          // Program counter (for exceptions/HLE)

    // GQR (graphics quantization registers) — for psq_l/psq_st
    u32    gqr[8];

    void reset() { std::memset(this, 0, sizeof(*this)); }
};

// CR0 update after integer ops (when Rc=1)
inline void update_cr0(CPUState& cpu, s32 result) {
    cpu.cr[0].lt = (result < 0);
    cpu.cr[0].gt = (result > 0);
    cpu.cr[0].eq = (result == 0);
    cpu.cr[0].so = cpu.xer.so;
}

// CR1 update after FP ops (when Rc=1) — copies FPSCR exception bits
inline void update_cr1(CPUState& cpu) {
    cpu.cr[1].lt = (cpu.fpscr >> 31) & 1;  // FX
    cpu.cr[1].gt = (cpu.fpscr >> 30) & 1;  // FEX
    cpu.cr[1].eq = (cpu.fpscr >> 29) & 1;  // VX
    cpu.cr[1].so = (cpu.fpscr >> 28) & 1;  // OX
}

inline u32 cr_to_u32(const CPUState& cpu) {
    u32 val = 0;
    for (int i = 0; i < 8; i++) {
        val |= (cpu.cr[i].lt << 3 | cpu.cr[i].gt << 2 |
                cpu.cr[i].eq << 1 | cpu.cr[i].so) << (28 - i * 4);
    }
    return val;
}

inline void u32_to_cr(CPUState& cpu, u32 val, u32 mask = 0xFF) {
    for (int i = 0; i < 8; i++) {
        if ((mask >> (7 - i)) & 1) {
            int shift = 28 - i * 4;
            cpu.cr[i].lt = (val >> (shift + 3)) & 1;
            cpu.cr[i].gt = (val >> (shift + 2)) & 1;
            cpu.cr[i].eq = (val >> (shift + 1)) & 1;
            cpu.cr[i].so = (val >> shift) & 1;
        }
    }
}
