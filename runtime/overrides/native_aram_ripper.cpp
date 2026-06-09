// Native JKRDvdAramRipper::loadToAram — PC-native port of the DVD→ARAM load (boot choke).
//
// THE CHOKE. TApplication::setupThreadFuncLogo loads /data/game_6.arc and /data/guide.arc to
// ARAM via JKRDvdAramRipper::loadToAram. The SDK implementation (decomp:
// reference/sms_decomp/src/JSystem/JKernel/JKRDvdAramRipper.cpp) streams the file DVD→RAM→ARAM
// through JKRAramPiece::orderSync message round-trips with the JKRAram command thread — a
// pipeline of cross-thread waits that never completes under the native scheduler: the setup
// thread parks WAITING inside callCommand_Async forever, so gameLoop's
// OSIsThreadTerminated(&gSetupThread) gate holds boot in APP_STATE_NLOGO.
//
// THE PORT. The whole transaction is deterministic data movement: read the file from our disc
// volume (host-side, synchronous — same bytes the DVD DMA would deliver), Yaz0-decompress
// host-side when asked, allocate the destination from the GUEST ARAM heap (heap bookkeeping
// stays guest-owned: JKRAramHeap::alloc via the recomp), and memcpy the result into Dolphin's
// ARAM buffer (raw bytes — see native_aram.cpp). No DVD interrupt, no ARAM stream thread, no
// message queues, no timing.
//
// GMSE01 specifics (disassembly-verified):
//   loadToAram(JKRDvdFile*,u32,JKRExpandSwitch,u32,u32) = 0x802EBCC8 (the funnel: the char*
//     and entrynum variants build a JKRDvdFile and bl here, so one override covers all three)
//   JKRDvdFile: DVDFileInfo @+0x5C → disc startAddr @+0x8C, length @+0x90; mAramBlock @+0x4C
//   JKRAram::sAramObject @ 0x8040E230; its JKRAramHeap* @+0x78
//   JKRAramHeap::alloc(this,size,mode) = 0x802BDB04 (mode 0 = HEAD)
//   JKRAramBlock: mAddress @+0x14

#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef HAVE_DOLPHIN_CORE
#include "../native_os.h"
#include "Core/System.h"
#include "Core/HW/DSP.h"

bool sunbright_disc_read(u64 offset, u64 length, u8* out);   // native_dvd.cpp

namespace {

constexpr u32 LOAD_TO_ARAM   = 0x802EBCC8u;
constexpr u32 ARAM_HEAP_ALLOC = 0x802BDB04u;
constexpr u32 S_ARAM_OBJECT  = 0x8040E230u;

inline u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

// Yaz0 ("SZS") decompression — byte-exact SDK algorithm (see decompSZS_subroutine in the decomp).
void yaz0_decompress(const u8* src, std::vector<u8>& dst, u32 out_size) {
    dst.resize(out_size);
    src += 0x10;
    u32 d = 0, code = 0;
    int bits = 0;
    while (d < out_size) {
        if (bits == 0) { code = *src++; bits = 8; }
        if (code & 0x80) {
            dst[d++] = *src++;
        } else {
            const u32 b1 = src[0], b2 = src[1];
            src += 2;
            const u32 dist = ((b1 & 0x0F) << 8 | b2) + 1;
            u32 n = b1 >> 4;
            n = n ? n + 2 : (u32)(*src++) + 0x12;
            for (u32 i = 0; i < n && d < out_size; i++, d++) dst[d] = dst[d - dist];
        }
        code <<= 1;
        bits--;
    }
}

// JKRAramBlock* JKRDvdAramRipper::loadToAram(JKRDvdFile* file, u32 address,
//                                            JKRExpandSwitch expand, u32 offset, u32 size_limit)
void native_load_to_aram(CPUState& cpu) {
    const u32 file = cpu.gpr[3], address = cpu.gpr[4], expand = cpu.gpr[5];
    const u32 skip = cpu.gpr[6], limit = cpu.gpr[7];
    static const bool dbg = getenv("SUNBRIGHT_DBG_ARAM") != nullptr;

    const u32 disc_off = mem_r32(file + 0x8C);
    u32 file_size      = mem_r32(file + 0x90);
    if (limit && file_size > limit) file_size = limit;
    const u32 read_size = (file_size + 31) & ~31u;

    std::vector<u8> raw(read_size);
    if (!sunbright_disc_read(disc_off, read_size, raw.data())) {
        fprintf(stderr, "[aram_ripper] disc read FAILED file=%08x off=0x%x len=%u\n",
                file, disc_off, read_size);
        cpu.gpr[3] = 0;
        return;
    }

    // Decompress if requested and actually compressed (Yaz0). An uncompressed payload under
    // EXPAND falls back to a raw copy, exactly like the SDK (compression==0 → DEFAULT).
    std::vector<u8> out;
    const u8* data;
    u32 data_size;
    if (expand == 1 && read_size >= 0x10 && memcmp(raw.data(), "Yaz0", 4) == 0) {
        u32 usize = be32(raw.data() + 4);
        if (limit && usize > limit) usize = limit;
        yaz0_decompress(raw.data(), out, usize);
        data = out.data(); data_size = usize;
    } else {
        data = raw.data(); data_size = read_size;
    }
    if (skip) { data += skip; data_size -= skip; }

    // Destination: caller-supplied ARAM address, or allocate from the guest ARAM heap.
    u32 aram_addr = address, block = 0;
    if (aram_addr == 0) {
        const u32 heap = mem_r32(mem_r32(S_ARAM_OBJECT) + 0x78);
        const u32 saved_lr = cpu.lr;
        cpu.gpr[3] = heap;
        cpu.gpr[4] = (data_size + 31) & ~31u;
        cpu.gpr[5] = 0;                          // JKRAramHeap::HEAD
        cpu.lr = LOAD_TO_ARAM;
        call_ppc(cpu, ARAM_HEAP_ALLOC);
        cpu.lr = saved_lr;
        block = cpu.gpr[3];
        if (!block) {
            fprintf(stderr, "[aram_ripper] guest ARAM alloc FAILED size=%u\n", data_size);
            cpu.gpr[3] = 0;
            return;
        }
        aram_addr = mem_r32(block + 0x14);       // JKRAramBlock::mAddress
        mem_w32(file + 0x4C, block);             // dvdFile->mAramBlock
    }

    u8* aram = Core::System::GetInstance().GetDSP().GetARAMPtr();
    if (aram_addr + data_size > 0x01000000u) {
        fprintf(stderr, "[aram_ripper] BAD aram dest %08x len=%u\n", aram_addr, data_size);
        cpu.gpr[3] = 0;
        return;
    }
    memcpy(aram + aram_addr, data, data_size);

    if (dbg) fprintf(stderr, "[aram_ripper] file=%08x disc=0x%x %u bytes -> aram %08x (block=%08x, %s)\n",
                     file, disc_off, data_size, aram_addr, block,
                     data == raw.data() ? "raw" : "yaz0");

    // Same return contract as the original sync loadToAram.
    cpu.gpr[3] = address ? 0xFFFFFFFFu : block;
}

}  // namespace

void native_aram_ripper_register() {
    native_os_register(LOAD_TO_ARAM, native_load_to_aram);
    fprintf(stderr, "[aram_ripper] registered native DVD->ARAM ripper (loadToAram 802ebcc8)\n");
}
#else
void native_aram_ripper_register() {}
#endif  // HAVE_DOLPHIN_CORE
