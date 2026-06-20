// ar_dsp_impl.cpp — native PC seam for the GC ARAM (AR/ARQ) + DSP mailbox (E7-adjacent).
//
// Implements the 8 unresolved AR*/DSP* symbols. In the native engine AUDIO IS
// native_jas (it synthesizes voices from ROM-decoded waves directly), so the GC
// audio-HW path — uploading wave banks to ARAM via ARQ DMA and the DSP mailbox — is
// INERT: nothing on the host renders from ARAM and there is no DSP. We keep a faithful
// ARAM ADDRESS allocator (so the game's ARAM bookkeeping gets consistent offsets) and
// model ARQ DMA as instant completion; DSP reports no mail.
//
// LANDMINE: ARQCallback / ARStartDMA take 32-bit ARAM + main-mem addresses (GC
// pointers were 32-bit). On a 64-bit host a real main-mem pointer can't round-trip
// through the u32 arg — the same 32-bit-userdata class flagged in the journal. This
// is harmless while the path is inert (native_jas); if a boot path genuinely DMAs
// through ARAM, the SDK signatures need widening (a cross-cutting effort), not a hack.

#include <dolphin/ar.h>   // ARAM + ARQ (ARQRequest/ARQCallback live here too)
#include <dolphin/dsp.h>
#include <cstdint>
#include <mutex>

namespace {
std::mutex g_ar_mu;
constexpr u32 kAramSize = 16u << 20;   // 16 MB ARAM, as on GC
u32 g_ar_top = 0;                       // bump pointer (ARAM offset)
u32 g_ar_base = 0;                      // first allocatable ARAM address
bool g_ar_inited = false;
}

extern "C" {

// ---- AR (ARAM) ------------------------------------------------------------
u32 ARInit(u32* /*stack_index_addr*/, u32 /*num_entries*/) {
    std::lock_guard<std::mutex> lk(g_ar_mu);
    if (!g_ar_inited) { g_ar_base = 0; g_ar_top = 0; g_ar_inited = true; }
    return g_ar_base;
}
u32 ARGetBaseAddress(void) { return g_ar_base; }
u32 ARGetSize(void)        { return kAramSize; }

// Bump-allocate an ARAM block (32-byte aligned, GC convention). Returns the ARAM
// address (offset); 0 only if exhausted (the game checks against ARGetSize).
u32 ARAlloc(u32 length) {
    std::lock_guard<std::mutex> lk(g_ar_mu);
    u32 addr = g_ar_top;
    u32 len = (length + 31u) & ~31u;
    if ((uint64_t)g_ar_top + len > kAramSize) return addr;  // exhausted (clamp)
    g_ar_top += len;
    return addr;
}

// ---- ARQ (ARAM DMA queue) -------------------------------------------------
void ARQInit(void) {}

void ARQPostRequest(struct ARQRequest* request, u32 /*owner*/, u32 /*type*/,
                    u32 /*priority*/, u32 /*source*/, u32 /*dest*/, u32 /*length*/,
                    ARQCallback callback) {
    // No ARAM hardware: model the DMA as instantly complete. Fire the completion
    // callback synchronously (most callbacks just post an OSMessage / set a flag).
    // ARQRequestRef is pointer-width natively, so the request pointer round-trips
    // losslessly (JKRAramPiece::doneDMA casts it back to JKRAMCommand*).
    if (callback) callback((ARQRequestRef)request);
}

// ---- DSP mailbox ----------------------------------------------------------
u32 DSPCheckMailFromDSP(void) { return 0; }   // no DSP -> never any mail pending
u32 DSPReadMailFromDSP(void)  { return 0; }

} // extern "C"
