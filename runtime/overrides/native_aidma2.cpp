// ─────────────────────────────────────────────────────────────────────────────
// native_aidma2.cpp — PC-native ownership of the AI audio-DMA block countdown +
//                     AID interrupt raise, WITHOUT reading DSP_CONTROL back through
//                     the guest MMU.
//
// OPT-IN: SUNBRIGHT_NATIVE_AIDMA=1. Default OFF = current aid_native.cpp behavior
// (Dolphin runs the DMA body; the AID raise is detected by reading DSP_CONTROL
// 0xCC00500A back through the MMU — fragile in real mode). This file is INERT
// unless the flag is set: both seam functions short-circuit to "not handled".
//
// Clean NEW-FILE rewrite of scratch/native_drafts/native_ai_dma.cpp. The RE is
// unchanged; the integration approach differs: NO existing-file edits. aid_native.cpp
// already owns the only __wrap of UpdateAudioDMA (one wrap per symbol — we cannot
// wrap it again), so this module exposes a function the existing wrap calls, plus an
// MMIO observe-write hook. Both seams are call sites documented below.
//
// ── WHY (the black box being removed) ────────────────────────────────────────
// The AI audio-DMA feeds 8 stereo samples (32 bytes) per tick from guest RAM to the
// sink; when the programmed block count wraps to 0 it re-arms and raises INT_AID (the
// per-frame audio heartbeat → __AIDHandler → JAS syncAudio). The HW engine:
//   SystemTimers AudioDMACallback (SystemTimers.cpp:85)
//     → DSP::DSPManager::UpdateAudioDMA() (DSP.cpp:424-454):
//        if (AudioDMAControl.Enable) {
//          SendAIBuffer(<32B @ current_source_address>, 8);
//          if (remaining_blocks_count) { remaining_blocks_count--; current_source_address+=32; }
//          if (remaining_blocks_count == 0) {        // wrap → re-arm + raise
//            current_source_address = SourceAddress;
//            remaining_blocks_count = AudioDMAControl.NumBlocks;
//            GenerateDSPInterrupt(INT_AID, 0);
//          }
//        } else SendAIBuffer(zero, 8);
// aid_native.cpp's wrap runs that body, then DETECTS the AID raise by reading
// DSP_CONTROL through the MMU (mem_r16) — which fails ("Unable to resolve read
// cc00500a") when the guest is momentarily in real mode. That MMU readback is the
// black box this replaces.
//
// ── DESIGN (own the DMA state from guest MMIO WRITES; never read HW back) ─────
// AI-DMA registers (DSP.cpp:63-67, GC ai.c via __DSPRegs[24..27]):
//   0xCC005030 AUDIO_DMA_START_HI    : SourceAddress hi 16b (mask 0x03ff)
//   0xCC005032 AUDIO_DMA_START_LO    : SourceAddress lo 16b (mask 0xffe0, 32B aligned)
//   0xCC005036 AUDIO_DMA_CONTROL_LEN : NumBlocks:15 | Enable:bit15 (0x8000)
//   SourceAddress = ((HI&0x03ff)<<16) | (LO&0xffe0)  (DSP.cpp:318-320,203)
//   On a 0→1 Enable edge HW latches current_source_address=SourceAddress and
//   remaining_blocks_count=NumBlocks (DSP.cpp:328-337) — replicated exactly.
//
// sunbright_ai_dma_observe_write(ea,v) mirrors START_HI/LO/CONTROL_LEN into our own
// struct and does the Enable-edge latch — fed by the memory_bridge observe hook.
// sunbright_ai_dma_tick() runs the faithful UpdateAudioDMA body on Dolphin's own
// AudioDMACallback cadence (called from aid_native's existing wrap), feeds the block
// via AudioCommon::SendAIBuffer (the SAME sink → na_push_dsp), decrements OUR count,
// and on wrap re-arms + raises AID by calling sunbright_aid_raise() (sets aid_native's
// g_aid_due directly — no DSP_CONTROL write, no MMU read). It returns true when it
// serviced the tick so the caller skips Dolphin's __real_UpdateAudioDMA AND its
// DSP_CONTROL-readback AID detection.
//
// ════════════════════════════════════════════════════════════════════════════
// ==== REQUIRED SEAM (apply manually — two call sites; NO new __wrap) ====
//
//   (A) File: runtime/overrides/aid_native.cpp — inside the EXISTING
//       __wrap__ZN3DSP10DSPManager14UpdateAudioDMAEv(void* self) body, replace the
//       opening so the native tick gets first refusal (it self-disables when the flag
//       is off, returning false → unchanged behavior):
//
//           extern "C" bool sunbright_ai_dma_tick();
//           if (sunbright_ai_dma_tick()) return;      // native owner serviced this tick
//           __real__ZN3DSP10DSPManager14UpdateAudioDMAEv(self);
//           if (!native_chain_enabled()) return;
//           ... (existing DSP_CONTROL-readback AID detection unchanged) ...
//
//       i.e. add ONLY the two lines (extern decl + the `if (...tick()) return;`)
//       BEFORE the existing __real_ call. Default-off: tick() returns false, so the
//       existing path runs verbatim.
//
//   (B) File: runtime/memory_bridge.cpp — inside mem_w16_slow(u32 ea, u16 v), AFTER
//       the existing `MMIO_W(16, ea, v);` store, add:
//
//           extern "C" void sunbright_ai_dma_observe_write(uint32_t ea, uint16_t v);
//           if ((ea & 0x0FFFFFF0u) == 0x0C005030u) sunbright_ai_dma_observe_write(ea, v);
//
//   Both seam functions self-disable when SUNBRIGHT_NATIVE_AIDMA is unset, so the
//   seam is a no-op for the default build. This file LINKS without the seam applied
//   (it only references sunbright_aid_raise(), defined in aid_native.cpp, which is
//   always linked).
// ════════════════════════════════════════════════════════════════════════════
//
// ── HOW TO VERIFY ────────────────────────────────────────────────────────────
//  Build (NEW file → reconfigure): cmake -B build ... && cmake --build build -j$(nproc).
//  Apply (A)+(B). A/B headless with audio dump:
//    SUNBRIGHT_HEADLESS=1 SUNBRIGHT_DUMP_AUDIO=1 ./run.sh                       # baseline
//    SUNBRIGHT_HEADLESS=1 SUNBRIGHT_NATIVE_AIDMA=1 SUNBRIGHT_DUMP_AUDIO=1 ./run.sh  # native
//  Compare scratch/wav/native_dsp.wav RMS/sec (tools/audio/wav_rms.py): native must
//  match baseline (sustained music/SE, no silence, no garbage). With the flag ON there
//  must be NO "Unable to resolve read cc00500a" in the log (we never read HW back).
//  SUNBRIGHT_DBG_AIDMA=1 → blocks-fed climbing + aid-raises ticking (~per audio frame).
// ─────────────────────────────────────────────────────────────────────────────

#include "../overrides.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#ifdef HAVE_DOLPHIN_CORE
#include "Core/System.h"
#include "Core/HW/Memmap.h"
#include "AudioCommon/AudioCommon.h"

// Reuse aid_native.cpp's level-triggered AID flag (set DIRECTLY — no DSP_CONTROL/MMU).
extern "C" void sunbright_aid_raise();

namespace {

bool native_aidma_enabled() {
    // Honor the oracle guard like aid_native: DISABLE_RECOMP keeps pure Dolphin.
    static const bool on = (getenv("SUNBRIGHT_NATIVE_AIDMA") != nullptr) &&
                           (getenv("SUNBRIGHT_DISABLE_RECOMP") == nullptr);
    return on;
}

// PC-native shadow of the AI-DMA engine, tracked from guest MMIO writes only.
struct AiDmaState {
    std::atomic<uint16_t> ctrl_hi{0};   // masked AUDIO_DMA_START_HI
    std::atomic<uint16_t> ctrl_lo{0};   // masked AUDIO_DMA_START_LO
    std::atomic<uint16_t> control{0};   // AUDIO_DMA_CONTROL_LEN (NumBlocks:15 | Enable:bit15)
    // Live transfer (touched only on the AudioDMACallback cadence in tick()).
    uint32_t current_source_address = 0;
    uint16_t remaining_blocks_count = 0;
    uint32_t source_address = 0;        // latched base for re-arm
    bool was_enabled = false;
};
AiDmaState g;

constexpr uint16_t kEnableBit = 0x8000u;
inline uint16_t num_blocks(uint16_t control) { return (uint16_t)(control & 0x7fffu); }
inline uint32_t src_addr(uint16_t hi, uint16_t lo) {
    return ((uint32_t)(hi & 0x03ffu) << 16) | (uint32_t)(lo & 0xffe0u);
}

std::atomic<unsigned long> g_blocks_fed{0}, g_aid_raises{0};

}  // namespace

// Observe guest writes to the AI-DMA control registers (fed by the mem_w16_slow seam).
extern "C" void sunbright_ai_dma_observe_write(uint32_t ea, uint16_t v) {
    if (!native_aidma_enabled()) return;
    switch (ea & 0x0000000Fu) {
    case 0x0:  // 0xCC005030 START_HI
        g.ctrl_hi.store((uint16_t)(v & 0x03ffu), std::memory_order_relaxed);
        break;
    case 0x2:  // 0xCC005032 START_LO
        g.ctrl_lo.store((uint16_t)(v & 0xffe0u), std::memory_order_relaxed);
        break;
    case 0x6: {  // 0xCC005036 CONTROL_LEN
        const uint16_t old = g.control.load(std::memory_order_relaxed);
        g.control.store(v, std::memory_order_relaxed);
        if (!(old & kEnableBit) && (v & kEnableBit)) {        // 0→1 Enable edge: latch
            const uint32_t sa = src_addr(g.ctrl_hi.load(std::memory_order_relaxed),
                                         g.ctrl_lo.load(std::memory_order_relaxed));
            g.source_address          = sa;
            g.current_source_address  = sa;
            g.remaining_blocks_count  = num_blocks(v);
            g.was_enabled             = true;
        } else if (!(v & kEnableBit)) {
            g.was_enabled = false;
        }
        break;
    }
    default:  // 0x4 BLOCKS_LENGTH, 0xA BLOCKS_LEFT (read-only) — ignore
        break;
    }
}

// Faithful UpdateAudioDMA body. Called from aid_native's existing wrap on Dolphin's
// AudioDMACallback cadence. Returns true when serviced (caller then skips both
// __real_UpdateAudioDMA and the DSP_CONTROL-readback AID detection).
extern "C" bool sunbright_ai_dma_tick() {
    if (!native_aidma_enabled()) return false;

    auto& system = Core::System::GetInstance();
    auto& memory = system.GetMemory();
    static short zero_samples[8 * 2] = {0};

    const uint16_t control = g.control.load(std::memory_order_relaxed);
    if (control & kEnableBit) {
        // Raw host pointer into guest RAM (DSP.cpp:432-434) — NOT a guest-MMU read,
        // so real-mode is irrelevant. Samples stay big-endian R-L; na_push_dsp byteswaps.
        short* address =
            reinterpret_cast<short*>(memory.GetPointerForRange(g.current_source_address, 32));
        if (address) AudioCommon::SendAIBuffer(system, address, 8);
        else         AudioCommon::SendAIBuffer(system, &zero_samples[0], 8);
        g_blocks_fed.fetch_add(1, std::memory_order_relaxed);

        if (g.remaining_blocks_count != 0) {
            g.remaining_blocks_count--;
            g.current_source_address += 32;
        }
        if (g.remaining_blocks_count == 0) {                  // wrap → re-arm + raise AID
            g.current_source_address = g.source_address;
            g.remaining_blocks_count = num_blocks(control);
            sunbright_aid_raise();                            // sets g_aid_due — no MMU
            g_aid_raises.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        AudioCommon::SendAIBuffer(system, &zero_samples[0], 8);
    }

    static const bool dbg = getenv("SUNBRIGHT_DBG_AIDMA") != nullptr;
    if (dbg) {
        static time_t last = 0;
        const time_t now = time(nullptr);
        if (now != last) {
            last = now;
            fprintf(stderr,
                    "[aidma] blocks=%lu aid_raises=%lu en=%d src=%08x rem=%u numblk=%u\n",
                    g_blocks_fed.load(std::memory_order_relaxed),
                    g_aid_raises.load(std::memory_order_relaxed),
                    (control & kEnableBit) ? 1 : 0, g.current_source_address,
                    g.remaining_blocks_count, num_blocks(control));
        }
    }
    return true;
}

#else  // !HAVE_DOLPHIN_CORE — keep the seams resolvable
extern "C" void sunbright_ai_dma_observe_write(uint32_t, uint16_t) {}
extern "C" bool sunbright_ai_dma_tick() { return false; }
#endif
