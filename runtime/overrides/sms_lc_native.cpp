// Locked-L1-cache DMA — native overrides (OSCache.c LC* family).
//
// THP video decode uses the 16 KB locked cache (0xE0000000) as its DCT/color-convert
// scratch and moves results out with the locked-cache DMA: LCStoreData chops the buffer
// into 128-block (4 KB) LCStoreBlocks transactions, then LCQueueWait polls HID2's DMA
// queue length until the engine drains. All of that is mtspr DMAU/DMAL/HID2 code →
// function_needs_jit() → every per-chunk call ran under the interpreter via run_jit_sync,
// hundreds of times per movie frame. Combined with the (now fixed) slow-path memory
// accesses into 0xE0000000, the intro THP played at ~6 fps with `drain` eating ~70 ms of
// every VIWaitForRetrace (2026-06-12).
//
// Dolphin backs the locked cache with a flat array and performs the DMA synchronously at
// the DMAL write, so the faithful PC-native equivalent is a memcpy and an always-empty
// queue: LCQueueWait returns immediately, LCFlushQueue has nothing to flush.
//
// Addresses verified against reference/sms/src/dolphin/os/OSCache.c (funcs.txt has a gap
// here): __LCEnable 803437b0, LCEnable 8034387c, LCDisable 803438b4, LCStoreBlocks
// 803438dc, LCStoreData 80343900, LCQueueWait 803439ac, LCFlushQueue 803439c4. The
// load-side pair (LCLoadBlocks/LCLoadData) is dead-stripped from SMS. Enable/Disable/
// AllocTags stay on their existing path (cold — once per movie).

#include "../overrides.h"
#include "../intrinsics.h"
#include <cstring>

namespace {

// Resolve a guest address to a host pointer via the published fast-path bases (RAM or
// locked cache). The LC DMA only ever touches those two; anything else is a guest bug —
// fall back to the byte-wise bridge so the wild-write trap still fires on it.
inline u8* lc_ptr(u32 ea) { return sb_ram_fast(ea); }

void lc_copy(u32 dst, u32 src, u32 bytes) {
    u8* d = lc_ptr(dst);
    u8* s = lc_ptr(src);
    if (d && s && lc_ptr(dst + bytes - 1) && lc_ptr(src + bytes - 1)) {
        std::memcpy(d, s, bytes);          // both sides are guest-byte-order arrays: no swap
        return;
    }
    for (u32 i = 0; i < bytes; i++) sb_w8(dst + i, sb_r8(src + i));
}

// 0x803438dc LCStoreBlocks(void* destAddr, void* srcTag, u32 numBlocks)
// One DMA transaction: numBlocks 32-byte blocks, locked cache → RAM. The hardware length
// field encodes 0 as 128 blocks (how LCStoreData expresses a full 4 KB chunk).
SUNBRIGHT_OVERRIDE(ov_LCStoreBlocks, 0x803438dcu) {
    const u32 dst = cpu.gpr[3];
    const u32 src = cpu.gpr[4];
    u32 blocks = cpu.gpr[5] & 0x7Fu;
    if (blocks == 0) blocks = 128;
    lc_copy(dst, src, blocks * 32);
}

// 0x80343900 LCStoreData(void* destAddr, void* srcAddr, u32 nBytes) -> numTransactions
// Faithful to the SDK loop's arithmetic (callers pass the count to LCQueueWait).
SUNBRIGHT_OVERRIDE(ov_LCStoreData, 0x80343900u) {
    const u32 dst    = cpu.gpr[3];
    const u32 src    = cpu.gpr[4];
    const u32 nBytes = cpu.gpr[5];
    const u32 numBlocks = (nBytes + 31) / 32;
    if (numBlocks) lc_copy(dst, src, numBlocks * 32);
    cpu.gpr[3] = (numBlocks + 127) / 128;
}

// 0x803439ac LCQueueWait(u32 len) — DMA is synchronous natively; the queue is always empty.
SUNBRIGHT_OVERRIDE(ov_LCQueueWait, 0x803439acu) {
    (void)cpu;
}

// 0x803439c4 LCFlushQueue() — nothing ever queues.
SUNBRIGHT_OVERRIDE(ov_LCFlushQueue, 0x803439c4u) {
    (void)cpu;
}

}  // namespace
