// PC-native port of func_803433b4 — __OSInitAudioSystem (GC OS DSP boot + ARAM init).
//
// THE BUG: the recompiled __OSInitAudioSystem runs as native C on our call stack and never
// returns to the CPU loop, so it cannot advance Dolphin's CoreTiming / fake time base. The
// function is full of hardware-settle busy-waits that only clear once CoreTiming/TB advances:
//   • DSP-reset poll          (DSP_CONTROL bit0)  — HLE clears it synchronously anyway
//   • two AR-DMA-complete polls(DSP_CONTROL 0x20, INT_ARAM) — set by a CoreTiming event
//                                                   (DSP::CompleteARAM) scheduled ticksToTransfer
//                                                   cycles out; the DATA is already moved
//                                                   synchronously inside Do_ARAM_DMA().
//   • DSPInitCode poll        (DSP_CONTROL 0x400) — DSPHLE sets it on the DSPInit 1->0 edge and
//                                                   only clears it after FakeTimeBase advances
//                                                   130 ticks (modeled boot latency).
//   • mail-from-DSP poll      (MAIL_FROM_HI 0x8000)— INITUCode::Initialize() PushMail(0x80544348)
//                                                   runs synchronously on SetUCode, so the mail is
//                                                   already present.
//   • OSGetTick settle delay  (2194 ticks)         — pure timed delay.
// Under DSP-HLE every one of these awaited signals is *deferred latency*; the functional work
// (ucode load via SetUCode, ARAM DMA data movement, mail push) is done synchronously by Dolphin.
// A native-stack recomp spin can never satisfy them -> boot hangs in __OSInitAudioSystem.
//
// THE FIX (own it natively, drop the waits): a PC build has no reason to busy-wait on hardware
// settle. This override performs every MMIO configuration access in the original order — so
// Dolphin's DSP-HLE / ARAM state ends up identical — but drops the spin/back-edge of each wait
// loop and the OSGetTick delays. The DSP is "booted" and ARAM configured instantly.
//
// Faithful to the original's observable effects:
//   • saves 128B of [0x81000000] to arena scratch, loads the DSP init data (0x803e6e98) there,
//     flushes it, runs the boot/ARAM-DMA sequence, restores the 128B — net-zero on that block.
//   • the two AR DMAs (MRAM 0x01000000 -> ARAM 0, 32B) still run via the CNT-write side effect,
//     so ARAM[0..32] gets the init data exactly as on hardware.
//   • pops the INIT-ucode mail (reads MAIL_FROM_LO 0xcc005006) so the HLE mailbox stays clean.
//   • uses C locals for all scratch, so the non-volatile r25..r31 the original saves/restores are
//     never touched (callee-saved contract preserved without a stack frame).
#include "../overrides.h"
#include "../intrinsics.h"
#include "../dolphin_hook.h"

namespace {

// DSP / ARAM interface registers (Dolphin DSP.cpp).
constexpr u32 DSP_BASE        = 0xcc005000u;
constexpr u32 DSP_MAIL_FROM_H = 0xcc005004u;  // bit15 = mail-present
constexpr u32 DSP_MAIL_FROM_L = 0xcc005006u;  // reading pops the mail
constexpr u32 DSP_CONTROL     = 0xcc00500au;  // bit0 reset, 0x20 INT_ARAM, 0x400 DSPInitCode, 0x800 DSPInit
constexpr u32 DSP_REG_12      = 0xcc005012u;  // written 0x43 by the original
constexpr u32 AR_DMA_MMADDR   = 0xcc005020u;
constexpr u32 AR_DMA_ARADDR   = 0xcc005024u;
constexpr u32 AR_DMA_CNT      = 0xcc005028u;  // low write triggers Do_ARAM_DMA()

constexpr u32 OS_GET_ARENA_HI = 0x80343394u;  // OSGetArenaHi()  -> r3
constexpr u32 MEMCPY          = 0x800031f4u;  // memcpy(r3=dst, r4=src, r5=n)
constexpr u32 DSP_INIT_FLUSH  = 0x8034368cu;  // flush helper (addr=r3, len=r4)
constexpr u32 INIT_DATA_SRC   = 0x803e6e98u;  // 128B of DSP init data
constexpr u32 SCRATCH_BLOCK   = 0x81000000u;  // boot uCode staging block

inline void call(CPUState& cpu, u32 addr, u32 ret_lr) {
    cpu.lr = ret_lr;
    call_ppc(cpu, addr);
}

void ov_os_init_audio(CPUState& cpu) {
    const u32 entry_lr = cpu.lr;

    // --- save [0x81000000..+128] to arena scratch, then load DSP init data there & flush it ---
    call(cpu, OS_GET_ARENA_HI, 0x803433c8u);
    const u32 arena_hi = cpu.gpr[3];
    cpu.gpr[3] = arena_hi - 128; cpu.gpr[4] = SCRATCH_BLOCK;  cpu.gpr[5] = 128; call(cpu, MEMCPY, 0x803433d8u);
    cpu.gpr[3] = SCRATCH_BLOCK;  cpu.gpr[4] = INIT_DATA_SRC;  cpu.gpr[5] = 128; call(cpu, MEMCPY, 0x803433ecu);
    cpu.gpr[3] = SCRATCH_BLOCK;  cpu.gpr[4] = 128;                               call(cpu, DSP_INIT_FLUSH, 0x803433f8u);

    // --- DSP boot + ARAM init: same MMIO sequence, every busy-wait dropped ---
    MEM_W16(DSP_REG_12, 0x43);
    MEM_W16(DSP_CONTROL, 0x8ac);
    MEM_W16(DSP_CONTROL, MEM_R16(DSP_CONTROL) | 0x1);   // assert DSPReset (HLE: SetUCode(ROM), clears sync)
    // [drop] poll DSP_CONTROL bit0 until clear

    MEM_W16(DSP_BASE, 0);
    // [drop] drain-mailbox wait; do one read pair (faithful single satisfied iteration / mail pop)
    (void)MEM_R16(DSP_MAIL_FROM_H);
    (void)MEM_R16(DSP_MAIL_FROM_L);

    // AR DMA #1: MRAM 0x01000000 -> ARAM 0, 32 bytes (Do_ARAM_DMA moves the data synchronously)
    MEM_W32(AR_DMA_MMADDR, 0x1000000);
    MEM_W32(AR_DMA_ARADDR, 0);
    MEM_W32(AR_DMA_CNT,    32);
    // [drop] poll DSP_CONTROL 0x20 (INT_ARAM) until set
    MEM_W16(DSP_CONTROL, MEM_R16(DSP_CONTROL));         // W1C the (deferred) INT_ARAM
    // [drop] OSGetTick settle delay (2194 ticks)

    // AR DMA #2: same transfer
    MEM_W32(AR_DMA_MMADDR, 0x1000000);
    MEM_W32(AR_DMA_ARADDR, 0);
    MEM_W32(AR_DMA_CNT,    32);
    // [drop] poll DSP_CONTROL 0x20 until set
    MEM_W16(DSP_CONTROL, MEM_R16(DSP_CONTROL));         // W1C the (deferred) INT_ARAM

    // clear DSPInit (0x800) -> HLE: SetUCode(INIT_AUDIO_SYSTEM) + INITUCode::Initialize() pushes mail
    MEM_W16(DSP_CONTROL, MEM_R16(DSP_CONTROL) & 0xfffff7ffu);
    // [drop] poll DSP_CONTROL 0x400 (DSPInitCode) until clear  <-- the 130-tick latency that hangs us

    MEM_W16(DSP_CONTROL, MEM_R16(DSP_CONTROL) & 0xfffffffbu);  // clear bit 0x4
    // [drop] poll MAIL_FROM_HI 0x8000 until set (mail already pushed synchronously)
    (void)MEM_R16(DSP_MAIL_FROM_H);

    MEM_W16(DSP_CONTROL, MEM_R16(DSP_CONTROL) | 0x4);   // set bit 0x4
    (void)MEM_R16(DSP_MAIL_FROM_L);                     // pop the INIT-ucode mail (0x80544348)
    MEM_W16(DSP_CONTROL, 0x8ac);
    MEM_W16(DSP_CONTROL, MEM_R16(DSP_CONTROL) | 0x1);   // assert DSPReset again
    // [drop] poll DSP_CONTROL bit0 until clear

    // --- restore [0x81000000..+128] from arena scratch (fresh OSGetArenaHi, as the original) ---
    call(cpu, OS_GET_ARENA_HI, 0x8034354cu);
    const u32 arena_hi2 = cpu.gpr[3];
    cpu.gpr[3] = SCRATCH_BLOCK; cpu.gpr[4] = arena_hi2 - 128; cpu.gpr[5] = 128;
    call(cpu, MEMCPY, 0x8034355cu);

    cpu.gpr[3] = SCRATCH_BLOCK;   // original returns dst of the last memcpy
    cpu.lr = entry_lr;
}

const bool os_init_audio_native_reg = [] {
    register_override(0x803433b4u, &ov_os_init_audio);
    return true;
}();

}  // namespace
