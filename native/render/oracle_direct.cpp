// oracle_direct.cpp — implementation. See oracle_direct.h for design rationale.
//
// This file is ONLY compiled+linked when Dolphin's videovulkan target is
// visible in the CMake scope (native/CMakeLists.txt: `if(TARGET videovulkan)`).
// The standalone `native/`-only build omits it; the header is still safe to
// include there because the direct-write functions are weak-linked.

#include "oracle_direct.h"
#include "../../runtime/engine.h"

// Dolphin's own register-write entry points. These are what
// OpcodeDecoder::RunFifo calls after parsing FIFO bytes; calling them directly
// skips the byte encode/decode round-trip.
#include "VideoCommon/BPMemory.h"       // LoadBPReg
#include "VideoCommon/XFMemory.h"       // LoadXFReg

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

// Weak so the Tier-1-only build still links.
extern "C" void sb_oracle_present_frame(void* framebuffer, void* user) __attribute__((weak));
// Dedicated init entry point in oracle_present.cpp. Brings Dolphin's video
// backend up without producing a frame — critical for the harness so the
// oracle_XXXX.ppm slot isn't consumed by an init-only side effect.
extern "C" int sb_oracle_init_only(void) __attribute__((weak));

namespace sb::oracle {

namespace {
std::atomic<bool> s_ready{false};
std::atomic<bool> s_init_tried{false};
} // namespace

bool ready() { return s_ready.load(std::memory_order_acquire); }

bool ensure_up() {
    // Only meaningful under GX_ORACLE mode.
    if (sb::engine::mode() != sb::engine::RenderMode::GX_ORACLE) return false;
    if (s_ready.load(std::memory_order_acquire)) return true;

    // The video-backend init is inside oracle_present.cpp (already knows how
    // to set the WSI, Vulkan backend, and Dolphin's UICommon paths). Invoke
    // it lazily by triggering one present-frame call — that path sets s_ready
    // as a side effect via g_gfx becoming non-null. The dummy present skips
    // its actual render work when we detect the "first init" state.
    if (s_init_tried.exchange(true, std::memory_order_acq_rel)) {
        // Second attempt: init previously failed. Return whatever ready is.
        return s_ready.load(std::memory_order_acquire);
    }

    if (!&sb_oracle_init_only) {
        std::fprintf(stderr, "[oracle-direct] sb_oracle_init_only not linked\n");
        return false;
    }
    if (sb_oracle_init_only()) {
        // sb_oracle_direct_mark_ready(1) is called inside try_init_video_backend
        // on success, so s_ready is already true here.
        return true;
    }
    std::fprintf(stderr, "[oracle-direct] init failed\n");
    return false;
}

// Called from oracle_present.cpp AFTER Dolphin is UP but BEFORE the first
// real drain, so subsequent direct-writes route to the live Dolphin.
extern "C" void sb_oracle_direct_mark_ready(int up) {
    s_ready.store(up != 0, std::memory_order_release);
}

void bp_write(uint8_t reg, uint32_t value_24bit) {
    if (!ready()) return;
    // LoadBPReg is Dolphin's own function that OpcodeDecoder calls after
    // decoding a 0x61 opcode. Cycles-into-future = 0 (execute now).
    ::LoadBPReg(reg, value_24bit & 0x00FFFFFFu, 0);
}

void xf_write(uint16_t base_addr, uint32_t nwords, const uint32_t* be_words) {
    if (!ready() || nwords == 0 || !be_words) return;
    // LoadXFReg takes a byte array of `transfer_size` bytes (== nwords * 4).
    // Words must be big-endian on the wire (LoadXFReg unswaps back to host).
    // Pass as a byte buffer.
    ::LoadXFReg(base_addr, (uint8_t)(nwords * 4),
                reinterpret_cast<const uint8_t*>(be_words));
}

void xf_write_1(uint16_t addr, uint32_t value_be) {
    xf_write(addr, 1, &value_be);
}

void xf_write_f32(uint16_t addr, uint32_t nwords, const float* host_words) {
    if (!ready() || nwords == 0 || !host_words) return;
    // Bit-cast float → u32 host-endian, then byteswap to BE for LoadXFReg.
    std::vector<uint32_t> be(nwords);
    for (uint32_t i = 0; i < nwords; ++i) {
        uint32_t u;
        std::memcpy(&u, &host_words[i], 4);
        be[i] = __builtin_bswap32(u);
    }
    xf_write(addr, nwords, be.data());
}

} // namespace sb::oracle
