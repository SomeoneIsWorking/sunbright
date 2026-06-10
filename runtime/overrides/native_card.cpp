// Native memory-card service — PC-native port of the GameCube CARD hardware layer.
//
// WHY (root cause, 2026-06-10). The CARD SDK drives the memory card through EXI immediate/DMA
// transfers whose completions arrive as EXI TC interrupts, plus a DSP-task security unlock. Under
// the native scheduler + hybrid timing, a memcard EXI DMA's CoreTiming completion event landed
// beyond what the idle driver's Advance ever reached (the global timer crawls ~1 tick/iteration
// while guest threads are parked), so __CARDSync slept forever on __CARDBlock.threadQueue:
// CARDMount wedged at mountStep 6, the TCardManager worker held the manager mutex, the main
// thread blocked on that mutex behind it → file-select menu never appeared, input dead, idle
// deadlock park. Chasing Dolphin's slice arithmetic is the wrong layer — own the behavior
// (dolphin-independence direction: Dolphin for GPU; OS/timing/IO native).
//
// THE PORT. Replace the *hardware* layer only — every FS structure and policy stays in the
// recompiled SDK (dir/FAT verify, CARDCheck, CARDOpen/Read/Write, TCardManager):
//   CARDProbeEx        → card always present in slot A: report size/sector from the host image.
//   CARDMountAsync     → set up CARDControl, read the 5 system blocks from the host image into
//                        the caller's workArea, run the recompiled __CARDVerify, put-control-block
//                        + api callback — no EXI attach, no DSP unlock, no interrupts.
//   __CARDReadSegment  → 512-byte host read at card->addr into card->buffer, then run the
//   __CARDWritePage      completion callback synchronously (the SDK's own chain callbacks do the
//   __CARDEraseSector    looping/advancing — they call back into us for the next segment).
// Completions happen before the API returns, so __CARDSync's result check never sleeps — the
// whole lost-wakeup/lost-interrupt class is gone by construction.
//
// BACKING STORE: Dolphin's own GC memory card image (GC/MemoryCardA.<region>.raw — the same file
// pure-Dolphin A/B runs use, so saves stay interchangeable). Override with SUNBRIGHT_MEMCARD.
#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>

#ifdef HAVE_DOLPHIN_CORE
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <glob.h>

#include "../native_os.h"
#include "Core/System.h"
#include "Core/HW/Memmap.h"

extern u32 mem_r32(u32 ea);
extern void mem_w32(u32 ea, u32 v);
extern void mem_w16(u32 ea, u16 v);
extern void mem_w8(u32 ea, u8 v);
extern "C" void func_8035796c(CPUState&);   // __CARDVerify   (recompiled, pure-RAM)
extern "C" void func_8035532c(CPUState&);   // __CARDPutControlBlock (recompiled)
extern "C" void func_80347798(CPUState&);   // __OSLockSramEx (recompiled)
extern "C" void func_80347b20(CPUState&);   // __OSUnlockSramEx (recompiled)

namespace {

// GMSE01 statics/SDK addresses (reference/sms_gmse01_funcs.txt + CARDManager probing).
constexpr u32 CARD_BLOCK0      = 0x80403460u;   // __CARDBlock[0] (threadQueue @+0x8C verified live)
constexpr u32 PROBE_EX         = 0x803580a8u;   // CARDProbeEx
constexpr u32 MOUNT_ASYNC      = 0x8035873cu;   // CARDMountAsync
constexpr u32 READ_SEGMENT     = 0x80354e70u;   // __CARDReadSegment
constexpr u32 WRITE_PAGE       = 0x80354fa4u;   // __CARDWritePage
constexpr u32 ERASE_SECTOR     = 0x803550c0u;   // __CARDEraseSector

// CARDControl field offsets (reference/sms/include/dolphin/card.h; spot-verified live:
// result@+0x04, mountStep@+0x24, threadQueue@+0x8C).
constexpr u32 F_ATTACHED   = 0x00;
constexpr u32 F_RESULT     = 0x04;
constexpr u32 F_SIZE       = 0x08;   // u16 Mbit
constexpr u32 F_SECTORSIZE = 0x0C;
constexpr u32 F_CBLOCK     = 0x10;   // u16
constexpr u32 F_LATENCY    = 0x14;
constexpr u32 F_MOUNTSTEP  = 0x24;
constexpr u32 F_WORKAREA   = 0x80;
constexpr u32 F_CURRENTDIR = 0x84;
constexpr u32 F_CURRENTFAT = 0x88;
constexpr u32 F_ADDR       = 0xB0;
constexpr u32 F_BUFFER     = 0xB4;
constexpr u32 F_EXTCB      = 0xC4;
constexpr u32 F_APICB      = 0xD0;

constexpr s32 R_READY = 0, R_BUSY = -1, R_NOCARD = -3;
constexpr u32 SECTOR = 0x2000;                 // 8 KB — Dolphin images use the standard sector
constexpr u32 SYSTEM_BLOCKS = 5;               // header, dir, dirBak, fat, fatBak

int   g_fd = -1;
off_t g_size = 0;
bool  g_open_failed = false;

int card_fd() {
    if (g_fd >= 0 || g_open_failed) return g_fd;
    std::string path;
    if (const char* p = getenv("SUNBRIGHT_MEMCARD")) path = p;
    else if (const char* home = getenv("HOME")) {
        // Dolphin's default slot-A image; region letter varies, glob for it.
        glob_t g{};
        std::string pat = std::string(home) + "/.local/share/dolphin-emu/GC/MemoryCardA.*.raw";
        if (glob(pat.c_str(), 0, nullptr, &g) == 0 && g.gl_pathc > 0) path = g.gl_pathv[0];
        globfree(&g);
    }
    if (path.empty() && getenv("HOME"))   // no image yet: create our own default
        path = std::string(getenv("HOME")) + "/.local/share/dolphin-emu/GC/MemoryCardA.USA.raw";
    if (path.empty()) { g_open_failed = true; }
    else {
        g_fd = open(path.c_str(), O_RDWR);
        if (g_fd < 0) {
            // First run: create a blank (0xFF) 16 MB image. Mount will report it BROKEN and the
            // game's own (recompiled) CARDFormat formats it through our erase/write overrides —
            // same flow as inserting a brand-new card on hardware.
            g_fd = open(path.c_str(), O_RDWR | O_CREAT, 0644);
            if (g_fd >= 0) {
                static u8 ff[0x2000];
                memset(ff, 0xFF, sizeof ff);
                for (int i = 0; i < 0x1000000 / 0x2000; i++)
                    if (pwrite(g_fd, ff, sizeof ff, (off_t)i * sizeof ff) != (ssize_t)sizeof ff) {
                        close(g_fd); g_fd = -1; break;
                    }
                if (g_fd >= 0)
                    fprintf(stderr, "[native_card] created blank image %s (game will format it)\n",
                            path.c_str());
            }
        }
        if (g_fd < 0) g_open_failed = true;
        else { struct stat st{}; fstat(g_fd, &st); g_size = st.st_size; }
    }
    if (g_open_failed)
        fprintf(stderr, "[native_card] FAILED to open memcard image (%s) — card reports NOCARD\n",
                path.empty() ? "no path; set SUNBRIGHT_MEMCARD" : path.c_str());
    else
        fprintf(stderr, "[native_card] memcard image: %s (%lld bytes, %lld blocks)\n",
                path.c_str(), (long long)g_size, (long long)(g_size / SECTOR));
    return g_fd;
}

void run_callback(CPUState& cpu, u32 cb, u32 chan, s32 result) {
    if (cb < 0x80000000u) return;
    CPUState c = cpu;                  // callee preserves non-volatiles; scratch copy is fine
    c.gpr[3] = chan;
    c.gpr[4] = (u32)result;
    call_ppc(c, cb);
}

// s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize)
void native_card_probe_ex(CPUState& cpu) {
    const u32 chan = cpu.gpr[3], mem_p = cpu.gpr[4], sec_p = cpu.gpr[5];
    if (chan != 0 || card_fd() < 0) { cpu.gpr[3] = (u32)R_NOCARD; return; }
    if (mem_p >= 0x80000000u) mem_w32(mem_p, (u32)(g_size * 8 / (1024 * 1024)));   // Mbit
    if (sec_p >= 0x80000000u) mem_w32(sec_p, SECTOR);
    cpu.gpr[3] = (u32)R_READY;
}

// s32 CARDMountAsync(s32 chan, void* workArea, CARDCallback detachCb, CARDCallback attachCb)
void native_card_mount_async(CPUState& cpu) {
    const u32 chan = cpu.gpr[3], work = cpu.gpr[4], detach_cb = cpu.gpr[5], attach_cb = cpu.gpr[6];
    if (chan != 0 || card_fd() < 0 || work < 0x80000000u) { cpu.gpr[3] = (u32)R_NOCARD; return; }
    const u32 card = CARD_BLOCK0;
    if ((s32)mem_r32(card + F_RESULT) == R_BUSY) { cpu.gpr[3] = (u32)R_BUSY; return; }

    mem_w32(card + F_RESULT, (u32)R_BUSY);
    mem_w32(card + F_WORKAREA, work);
    mem_w32(card + F_EXTCB, detach_cb);
    mem_w32(card + F_APICB, attach_cb);
    mem_w32(card + F_ATTACHED, 1);
    mem_w32(card + F_CURRENTDIR, 0);
    mem_w32(card + F_CURRENTFAT, 0);
    mem_w16(card + F_SIZE, (u16)(g_size * 8 / (1024 * 1024)));
    mem_w32(card + F_SECTORSIZE, SECTOR);
    mem_w16(card + F_CBLOCK, (u16)(g_size / SECTOR));
    mem_w32(card + F_LATENCY, 4);
    mem_w32(card + F_MOUNTSTEP, 2 + SYSTEM_BLOCKS);

    // Virtual-card identity — the DoMount step-0 port. VerifyID checks the header serial against
    // SRAM flashID scrambled with the format-time rand; real mounts refresh SRAM from the card's
    // physical flash ID every time. Our virtual card has a CONSTANT ID: write it (+ checksum)
    // into SRAM via the recompiled __OSLockSramEx/__OSUnlockSramEx so the game's own CARDFormat
    // serial verifies on every later boot ("memory card corrupt on every start" fix).
    {
        static const u8 kFlashID[12] = {'S','U','N','B','R','I','G','H','T','C','R','D'};
        CPUState c = cpu;
        func_80347798(c);                              // __OSLockSramEx() → r3 = OSSramEx*
        const u32 sram = c.gpr[3];
        if (sram >= 0x80000000u) {
            u8 sum = 0;
            for (int i = 0; i < 12; i++) { mem_w8(sram + (u32)i, kFlashID[i]); sum += kFlashID[i]; }
            mem_w8(sram + 38u, (u8)~sum);              // flashIDCheckSum[0] (OSSramEx +0x26)
            for (int i = 0; i < 12; i++) mem_w8(card + 0x18u + (u32)i, kFlashID[i]);  // card->id
        }
        CPUState u = cpu;
        u.gpr[3] = 1;                                  // unlock(TRUE) — mark SRAM dirty/flush
        func_80347b20(u);                              // __OSUnlockSramEx
    }

    // System area (5 blocks) → workArea, then the recompiled verifier.
    static u8 buf[SECTOR * SYSTEM_BLOCKS];
    s32 result = R_READY;
    if (pread(g_fd, buf, sizeof buf, 0) != (ssize_t)sizeof buf) result = R_NOCARD;
    else Core::System::GetInstance().GetMemory().CopyToEmu(work, buf, sizeof buf);

    if (result == R_READY) {
        CPUState c = cpu;
        c.gpr[3] = card;
        c.lr = 0;                       // recomp body returns via C return
        func_8035796c(c);               // __CARDVerify(card) — dir/FAT checksum check
        result = (s32)c.gpr[3];
    }

    {   // __CARDPutControlBlock(card, result): publishes card->result, releases the control block.
        CPUState c = cpu;
        c.gpr[3] = card;
        c.gpr[4] = (u32)result;
        func_8035532c(c);
    }
    // __CARDMountCallback's final phase: consume + run the api callback (sync mounts pass
    // __CARDSyncCallback, whose wake hits an empty queue — nobody sleeps, result is already set).
    const u32 api = mem_r32(card + F_APICB);
    mem_w32(card + F_APICB, 0);
    run_callback(cpu, api, chan, result);

    static bool logged = false;
    if (!logged) {
        logged = true;
        fprintf(stderr, "[native_card] mount: verify=%d size=%lldMbit blocks=%lld\n",
                result, (long long)(g_size * 8 / (1024 * 1024)), (long long)(g_size / SECTOR));
    }
    cpu.gpr[3] = (u32)(result < 0 ? result : R_READY);
}

// s32 __CARDReadSegment(s32 chan, CARDCallback callback) — 512 bytes @card->addr → card->buffer.
void native_card_read_segment(CPUState& cpu) {
    const u32 chan = cpu.gpr[3], cb = cpu.gpr[4];
    const u32 card = CARD_BLOCK0, addr = mem_r32(card + F_ADDR), dst = mem_r32(card + F_BUFFER);
    s32 result = R_READY;
    u8 buf[512];
    if (card_fd() < 0 || addr + sizeof buf > (u64)g_size ||
        pread(g_fd, buf, sizeof buf, addr) != (ssize_t)sizeof buf)
        result = R_NOCARD;
    else
        Core::System::GetInstance().GetMemory().CopyToEmu(dst, buf, sizeof buf);
    run_callback(cpu, cb, chan, result);   // SDK chain callback: advances addr/xferred, recurses
    cpu.gpr[3] = (u32)result;
}

// s32 __CARDWritePage(s32 chan, CARDCallback callback) — 128 bytes card->buffer → @card->addr.
void native_card_write_page(CPUState& cpu) {
    const u32 chan = cpu.gpr[3], cb = cpu.gpr[4];
    const u32 card = CARD_BLOCK0, addr = mem_r32(card + F_ADDR), src = mem_r32(card + F_BUFFER);
    s32 result = R_READY;
    u8 buf[128];
    if (card_fd() < 0 || addr + sizeof buf > (u64)g_size) result = R_NOCARD;
    else {
        Core::System::GetInstance().GetMemory().CopyFromEmu(buf, src, sizeof buf);
        if (pwrite(g_fd, buf, sizeof buf, addr) != (ssize_t)sizeof buf) result = R_NOCARD;
    }
    run_callback(cpu, cb, chan, result);
    cpu.gpr[3] = (u32)result;
}

// s32 __CARDEraseSector(s32 chan, u32 addr, CARDCallback callback) — sector → 0xFF.
void native_card_erase_sector(CPUState& cpu) {
    const u32 chan = cpu.gpr[3], addr = cpu.gpr[4], cb = cpu.gpr[5];
    s32 result = R_READY;
    static u8 ff[SECTOR];
    memset(ff, 0xFF, sizeof ff);
    if (card_fd() < 0 || addr + SECTOR > (u64)g_size ||
        pwrite(g_fd, ff, SECTOR, addr) != (ssize_t)SECTOR)
        result = R_NOCARD;
    run_callback(cpu, cb, chan, result);
    cpu.gpr[3] = (u32)result;
}

}  // namespace

void native_card_register() {
    native_os_register(PROBE_EX,     native_card_probe_ex);
    native_os_register(MOUNT_ASYNC,  native_card_mount_async);
    native_os_register(READ_SEGMENT, native_card_read_segment);
    native_os_register(WRITE_PAGE,   native_card_write_page);
    native_os_register(ERASE_SECTOR, native_card_erase_sector);
    fprintf(stderr, "[native_card] registered native memory-card service "
                    "(probe/mount/read/write/erase — no EXI, no DSP unlock)\n");
}
#else
void native_card_register() {}
#endif
