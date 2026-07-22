// native_card.cpp — the memory card as a NATIVE, SYNCHRONOUS service.
//
// This runtime is not bound by GameCube interrupts. The CARD SDK drives the card through EXI
// immediate/DMA transfers whose completions arrive as EXI TC interrupts; reproducing that here
// means reproducing an interrupt controller to deliver events a host filesystem answers
// instantly. So replace the HARDWARE layer only, and let every filesystem structure and policy
// stay in the recompiled SDK — directory/FAT verification, CARDCheck, CARDOpen/Read/Write and
// TCardManager all keep running as retail code:
//
//   CARDProbeEx        -> the card is present in slot A; report size/sector from the host image.
//   CARDMountAsync     -> fill CARDControl, read the 5 system blocks into the caller's workArea,
//                         run the recompiled __CARDVerify, then __CARDPutControlBlock and the
//                         api callback. No EXI attach, no unlock handshake, no interrupts.
//   __CARDReadSegment  -> a host read/write at card->addr, then run the completion callback
//   __CARDWritePage       SYNCHRONOUSLY. The SDK's own chain callbacks do the looping — they
//   __CARDEraseSector     call back into us for the next segment.
//
// Because completions happen before the API returns, __CARDSync's result check never sleeps and
// the entire lost-wakeup / lost-interrupt class is gone by construction. This is the same shape
// as DVD (synchronous inline completion) and it is a restoration: the pre-retirement runtime
// carried this service and it was lost with the rest of runtime/overrides/.
//
// BACKING STORE: Dolphin's own GC memory card image, so saves stay interchangeable with
// Dolphin. Override the path with SBR_CARD_A.

#include "overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <fcntl.h>
#include <glob.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

extern u8* g_ram_base;

namespace {
// Guest pointers are cached/uncached virtual addresses; g_ram_base is indexed by the physical
// offset. Every other device masks the same way — omitting it here indexed ~2 GB past the
// arena and dumped core inside the mount.
inline u8* host_ptr(u32 guest_addr) { return g_ram_base + (guest_addr & 0x01FFFFFFu); }
} // namespace

namespace {

// Recompiled SDK routines we still defer to: the card's FILESYSTEM policy stays the game's.
constexpr u32 CARD_VERIFY       = 0x8035796cu;   // __CARDVerify(card) -> r3 = result
constexpr u32 CARD_PUTCTRLBLOCK = 0x8035532cu;   // __CARDPutControlBlock(card, result)
constexpr u32 OS_LOCK_SRAM_EX   = 0x80347798u;   // __OSLockSramEx() -> r3 = OSSramEx*
constexpr u32 OS_UNLOCK_SRAM_EX = 0x80347b20u;   // __OSUnlockSramEx(flush)

constexpr u32 CARD_BLOCK0 = 0x80403460u;         // __CARDBlock[0]

// CARDControl field offsets.
constexpr u32 F_ATTACHED   = 0x00;
constexpr u32 F_RESULT     = 0x04;
constexpr u32 F_SIZE       = 0x08;   // u16, Mbit
constexpr u32 F_SECTORSIZE = 0x0C;
constexpr u32 F_CBLOCK     = 0x10;   // u16
constexpr u32 F_LATENCY    = 0x14;
constexpr u32 F_ID         = 0x18;   // u8[12], the card's flash ID
constexpr u32 F_MOUNTSTEP  = 0x24;
constexpr u32 F_WORKAREA   = 0x80;
constexpr u32 F_CURRENTDIR = 0x84;
constexpr u32 F_CURRENTFAT = 0x88;
constexpr u32 F_ADDR       = 0xB0;
constexpr u32 F_BUFFER     = 0xB4;
constexpr u32 F_EXTCB      = 0xC4;
constexpr u32 F_APICB      = 0xD0;

constexpr u32 SECTOR        = 0x2000;   // 8 KiB
constexpr u32 SYSTEM_BLOCKS = 5;        // header, dir, dirBak, fat, fatBak
constexpr s32 R_READY = 0, R_BUSY = -1, R_NOCARD = -3;

int   g_fd = -1;
off_t g_size = 0;
bool  g_open_failed = false;

int card_fd() {
    if (g_fd >= 0 || g_open_failed) return g_fd;

    std::string path;
    if (const char* p = std::getenv("SBR_CARD_A")) path = p;
    else if (const char* home = std::getenv("HOME")) {
        // Dolphin's slot-A image; the region letter varies, so glob for it.
        glob_t g{};
        const std::string pat = std::string(home) + "/.local/share/dolphin-emu/GC/MemoryCardA.*.raw";
        if (glob(pat.c_str(), 0, nullptr, &g) == 0 && g.gl_pathc > 0) path = g.gl_pathv[0];
        globfree(&g);
    }

    if (path.empty()) {
        // An absent card is a legitimate console state and the game handles it; say so once.
        lucent::info("card", "no memory card image found — slot A is empty (set SBR_CARD_A)");
        g_open_failed = true;
        return -1;
    }

    g_fd = open(path.c_str(), O_RDWR);
    if (g_fd < 0) {
        lucent::info("card", "cannot open {} — slot A is empty", path);
        g_open_failed = true;
        return -1;
    }
    struct stat st{};
    fstat(g_fd, &st);
    g_size = st.st_size;
    if (g_size < (off_t)(SECTOR * SYSTEM_BLOCKS)) {
        lucent::error("card", "{} is {} bytes — too small to hold the card's system area",
                      path, (long long)g_size);
        std::abort();
    }
    lucent::info("card", "slot A: {} ({} Mbit, {} blocks)", path,
                 (long long)(g_size * 8 / (1024 * 1024)), (long long)(g_size / SECTOR));
    return g_fd;
}

// Run one of the SDK's own chain callbacks. They advance the transfer and often call straight
// back into us for the next segment, which is exactly how the retail interrupt-driven flow
// progresses — only without the interrupt.
void run_callback(CPUState& cpu, u32 cb, u32 chan, s32 result) {
    if (cb < 0x80000000u) return;
    CPUState c = cpu;                  // scratch copy; the callee preserves non-volatiles
    c.gpr[3] = chan;
    c.gpr[4] = (u32)result;
    call_ppc(c, cb);
}

// s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize)
void card_probe_ex(CPUState& cpu) {
    const u32 chan = cpu.gpr[3], mem_p = cpu.gpr[4], sec_p = cpu.gpr[5];
    if (chan != 0 || card_fd() < 0) { cpu.gpr[3] = (u32)R_NOCARD; return; }
    if (mem_p >= 0x80000000u) sb_w32(mem_p, (u32)(g_size * 8 / (1024 * 1024)));   // Mbit
    if (sec_p >= 0x80000000u) sb_w32(sec_p, SECTOR);
    cpu.gpr[3] = (u32)R_READY;
}

// s32 CARDMountAsync(s32 chan, void* workArea, CARDCallback detachCb, CARDCallback attachCb)
void card_mount_async(CPUState& cpu) {
    const u32 chan = cpu.gpr[3], work = cpu.gpr[4];
    const u32 detach_cb = cpu.gpr[5], attach_cb = cpu.gpr[6];
    if (chan != 0 || card_fd() < 0 || work < 0x80000000u) { cpu.gpr[3] = (u32)R_NOCARD; return; }

    const u32 card = CARD_BLOCK0;
    if ((s32)sb_r32(card + F_RESULT) == R_BUSY) { cpu.gpr[3] = (u32)R_BUSY; return; }

    sb_w32(card + F_RESULT, (u32)R_BUSY);
    sb_w32(card + F_WORKAREA, work);
    sb_w32(card + F_EXTCB, detach_cb);
    sb_w32(card + F_APICB, attach_cb);
    sb_w32(card + F_ATTACHED, 1);
    sb_w32(card + F_CURRENTDIR, 0);
    sb_w32(card + F_CURRENTFAT, 0);
    sb_w16(card + F_SIZE, (u16)(g_size * 8 / (1024 * 1024)));
    sb_w32(card + F_SECTORSIZE, SECTOR);
    sb_w16(card + F_CBLOCK, (u16)(g_size / SECTOR));
    sb_w32(card + F_LATENCY, 4);
    sb_w32(card + F_MOUNTSTEP, 2 + SYSTEM_BLOCKS);

    // Card identity. The SDK verifies a formatted card's header serial against the SRAM flash-ID
    // record, and a real mount refreshes SRAM from the card's physical flash ID every time. This
    // card's ID is constant, so write it (and its checksum) through the recompiled
    // __OSLockSramEx/__OSUnlockSramEx — otherwise the game's own CARDFormat serial fails to
    // verify on the NEXT boot and the card reads as corrupt every start.
    {
        static const u8 kFlashID[12] = {'S','U','N','B','R','I','G','H','T','C','R','D'};
        CPUState c = cpu;
        call_ppc(c, OS_LOCK_SRAM_EX);
        const u32 sram = c.gpr[3];
        if (sram >= 0x80000000u) {
            u8 sum = 0;
            for (u32 i = 0; i < 12; ++i) { sb_w8(sram + i, kFlashID[i]); sum += kFlashID[i]; }
            sb_w8(sram + 38u, (u8)~sum);          // flashIDCheckSum[0], OSSramEx +0x26
            for (u32 i = 0; i < 12; ++i) sb_w8(card + F_ID + i, kFlashID[i]);
        }
        CPUState u = cpu;
        u.gpr[3] = 1;                              // flush: mark SRAM dirty
        call_ppc(u, OS_UNLOCK_SRAM_EX);
    }

    // System area -> workArea, then the game's own verifier decides whether it is usable.
    s32 result = R_READY;
    {
        static u8 buf[SECTOR * SYSTEM_BLOCKS];
        if (pread(g_fd, buf, sizeof buf, 0) != (ssize_t)sizeof buf) result = R_NOCARD;
        else std::memcpy(host_ptr(work), buf, sizeof buf);
    }
    if (result == R_READY) {
        CPUState c = cpu;
        c.gpr[3] = card;
        call_ppc(c, CARD_VERIFY);                  // dir/FAT checksums, retail code
        result = (s32)c.gpr[3];
    }

    {   // Publishes card->result and releases the control block.
        CPUState c = cpu;
        c.gpr[3] = card;
        c.gpr[4] = (u32)result;
        call_ppc(c, CARD_PUTCTRLBLOCK);
    }

    // __CARDMountCallback's final phase: consume and run the api callback. A synchronous mount
    // passes __CARDSyncCallback, whose wake hits an empty queue because nobody ever slept.
    const u32 api = sb_r32(card + F_APICB);
    sb_w32(card + F_APICB, 0);
    run_callback(cpu, api, chan, result);

    static bool logged = false;
    if (!logged) {
        logged = true;
        lucent::info("card", "mount: verify={} ({} Mbit, {} blocks)", result,
                     (long long)(g_size * 8 / (1024 * 1024)), (long long)(g_size / SECTOR));
    }
    cpu.gpr[3] = (u32)(result < 0 ? result : R_READY);
}

// s32 __CARDReadSegment(s32 chan, CARDCallback callback) — 512 bytes @card->addr -> card->buffer
void card_read_segment(CPUState& cpu) {
    const u32 chan = cpu.gpr[3], cb = cpu.gpr[4];
    const u32 card = CARD_BLOCK0;
    const u32 addr = sb_r32(card + F_ADDR), dst = sb_r32(card + F_BUFFER);
    s32 result = R_READY;
    u8 buf[512];
    if (card_fd() < 0 || (off_t)(addr + sizeof buf) > g_size ||
        pread(g_fd, buf, sizeof buf, addr) != (ssize_t)sizeof buf)
        result = R_NOCARD;
    else
        std::memcpy(host_ptr(dst), buf, sizeof buf);
    run_callback(cpu, cb, chan, result);   // the SDK chain callback advances and recurses
    cpu.gpr[3] = (u32)result;
}

// s32 __CARDWritePage(s32 chan, CARDCallback callback) — 128 bytes card->buffer -> @card->addr
void card_write_page(CPUState& cpu) {
    const u32 chan = cpu.gpr[3], cb = cpu.gpr[4];
    const u32 card = CARD_BLOCK0;
    const u32 addr = sb_r32(card + F_ADDR), src = sb_r32(card + F_BUFFER);
    s32 result = R_READY;
    u8 buf[128];
    if (card_fd() < 0 || (off_t)(addr + sizeof buf) > g_size) result = R_NOCARD;
    else {
        std::memcpy(buf, host_ptr(src), sizeof buf);
        if (pwrite(g_fd, buf, sizeof buf, addr) != (ssize_t)sizeof buf) result = R_NOCARD;
    }
    run_callback(cpu, cb, chan, result);
    cpu.gpr[3] = (u32)result;
}

// s32 __CARDEraseSector(s32 chan, u32 addr, CARDCallback callback) — a sector back to 0xFF
void card_erase_sector(CPUState& cpu) {
    const u32 chan = cpu.gpr[3], addr = cpu.gpr[4], cb = cpu.gpr[5];
    s32 result = R_READY;
    static u8 ff[SECTOR];
    std::memset(ff, 0xFF, sizeof ff);
    if (card_fd() < 0 || (off_t)(addr + SECTOR) > g_size ||
        pwrite(g_fd, ff, SECTOR, addr) != (ssize_t)SECTOR)
        result = R_NOCARD;
    run_callback(cpu, cb, chan, result);
    cpu.gpr[3] = (u32)result;
}

} // namespace

// The same slot-A image, for readers that want the save data without going through the guest's
// CARD state machine (fastboot parses a save block host-side before the game ever mounts).
int sbr_card_image_fd() { return card_fd(); }

SB_OVERRIDE(0x803580a8u, card_probe_ex,     "CARDProbeEx",
            "native card service: the host image is always present, no EXI probe")
SB_OVERRIDE(0x8035873cu, card_mount_async,  "CARDMountAsync",
            "native card service: system area read and verified inline, no interrupts")
SB_OVERRIDE(0x80354e70u, card_read_segment, "__CARDReadSegment",
            "host read completing before the call returns")
SB_OVERRIDE(0x80354fa4u, card_write_page,   "__CARDWritePage",
            "host write completing before the call returns")
SB_OVERRIDE(0x803550c0u, card_erase_sector, "__CARDEraseSector",
            "host erase completing before the call returns")
