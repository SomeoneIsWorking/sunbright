// ar_dsp_impl.cpp — native PC seam for the GC ARAM (AR/ARQ) + DSP mailbox (E7-adjacent).
//
// Implements the unresolved AR*/DSP* symbols. AUDIO is native_jas (it synthesizes
// voices from ROM-decoded waves directly), so the GC audio-HW path through ARAM is
// inert for audio. But the 2D/UI ARCHIVE path is NOT inert: SMSLoadArchiveARAM streams
// guide.szs/game_6.szs into ARAM and SMSSwitch2DArchive copies them back out (ARAM ->
// main RAM, Yaz0-decompressing) to mount the "guide" 2D volume. That genuinely DMAs
// real bytes through ARAM, so ARAM must be REAL host memory and the ARQ DMA must be a
// real copy — not the old instant-complete-no-copy stub (which left ARAM empty and
// crashed TConsoleStr::load with an unmounted "guide" volume).
//
// LP64: the main-memory address is a host pointer (64-bit). The whole ARAM DMA chain
// (JKRAramPiece mSrc/mDst, orderSync/Pcs, ARQPostRequest source/dest, ARStartDMA
// mainmem_addr) was widened to pointer width (ARMemAddr in dolphin/ar.h) so the host
// pointer round-trips losslessly. The ARAM-side address stays a small offset into the
// 16 MB host buffer below.

#include <dolphin/ar.h>   // ARAM + ARQ (ARQRequest/ARQCallback live here too)
#include <dolphin/dsp.h>
#include <dolphin/os.h>   // OSPanic (fail-fast on out-of-range DMA)
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <mutex>

namespace {
std::mutex g_ar_mu;
constexpr u32 kAramSize = 16u << 20;   // 16 MB ARAM, as on GC
u32 g_ar_top = 0;                       // bump pointer (ARAM offset)
u32 g_ar_base = 0;                      // first allocatable ARAM address
bool g_ar_inited = false;

// Host-backed ARAM. ARAM addresses are offsets into this buffer (g_ar_base == 0).
// Lazily allocated on first ARInit so a non-ARAM run pays nothing.
u8* g_aram = nullptr;

void aram_copy(int dir, ARMemAddr source, ARMemAddr dest, u32 length) {
    // dir: ARAM_DIR_MRAM_TO_ARAM(0) -> source=main host ptr, dest=ARAM offset
    //      ARAM_DIR_ARAM_TO_MRAM(1) -> source=ARAM offset, dest=main host ptr
    if (length == 0) return;
    if (!g_aram)
        OSPanic(__FILE__, __LINE__, "aram_copy before ARInit (g_aram null)");
    if (dir == ARAM_DIR_ARAM_TO_MRAM) {
        u32 off = (u32)source;
        if ((uint64_t)off + length > kAramSize)
            OSPanic(__FILE__, __LINE__,
                    "ARAM->MRAM DMA out of range: off=%x len=%x aramSize=%x",
                    off, length, kAramSize);
        std::memcpy((void*)dest, g_aram + off, length);
    } else {
        u32 off = (u32)dest;
        if ((uint64_t)off + length > kAramSize)
            OSPanic(__FILE__, __LINE__,
                    "MRAM->ARAM DMA out of range: off=%x len=%x aramSize=%x",
                    off, length, kAramSize);
        std::memcpy(g_aram + off, (const void*)source, length);
    }
}
} // namespace

extern "C" {

// ---- AR (ARAM) ------------------------------------------------------------
u32 ARInit(u32* /*stack_index_addr*/, u32 /*num_entries*/) {
    std::lock_guard<std::mutex> lk(g_ar_mu);
    if (!g_ar_inited) {
        g_ar_base = 0; g_ar_top = 0;
        g_aram = (u8*)std::calloc(kAramSize, 1);
        if (!g_aram)
            OSPanic(__FILE__, __LINE__, "ARInit: failed to allocate %u-byte ARAM",
                    kAramSize);
        g_ar_inited = true;
    }
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

// Synchronous ARAM<->main-RAM DMA (the SDK's low-level entry). aram.c/arq.c are
// excluded from the native build, so this is the only ARStartDMA.
void ARStartDMA(u32 type, ARMemAddr mainmem_addr, u32 aram_addr, u32 length) {
    if (type == ARAM_DIR_ARAM_TO_MRAM)
        aram_copy(ARAM_DIR_ARAM_TO_MRAM, aram_addr, mainmem_addr, length);
    else
        aram_copy(ARAM_DIR_MRAM_TO_ARAM, mainmem_addr, aram_addr, length);
}

// ---- ARQ (ARAM DMA queue) -------------------------------------------------
void ARQInit(void) {}

void ARQPostRequest(struct ARQRequest* request, u32 /*owner*/, u32 type,
                    u32 /*priority*/, ARMemAddr source, ARMemAddr dest, u32 length,
                    ARQCallback callback) {
    // No ARAM hardware: perform the copy immediately, then fire the completion
    // callback synchronously (the callbacks just post an OSMessage / set a flag).
    // `type` is the transfer direction (JKRAramPiece passes mTransferDirection).
    // ARQRequestRef is pointer-width natively, so the request pointer round-trips
    // losslessly (JKRAramPiece::doneDMA casts it back to JKRAMCommand*).
    aram_copy((int)type, source, dest, length);
    if (callback) callback((ARQRequestRef)request);
}

// ---- DSP mailbox ----------------------------------------------------------
u32 DSPCheckMailFromDSP(void) { return 0; }   // no DSP -> never any mail pending
u32 DSPReadMailFromDSP(void)  { return 0; }

} // extern "C"
