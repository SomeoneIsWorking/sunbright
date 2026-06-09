// Native DVD file-read service — PC-native port of the GameCube DVD read path.
//
// WHY (root cause). Under the native host-thread scheduler (docs/native_threading.md) the GC
// DVD command FSM stalls after the FIRST transfer: a thread calls DVDRead → DVDReadAbsAsyncPrio
// (8034da6c), which queues a DVDCommandBlock and relies on the DI hardware interrupt + the OS
// command-queue dispatch ("start next command on completion") to advance. Threads whose entry is
// JIT-only (e.g. the audio thread 802a7878) run wholly under Dolphin's interpreter, where the
// general SUNBRIGHT_OVERRIDE table is never consulted, so they hit the raw GC FSM; the queue's
// dispatch-next step never fires under cooperative native scheduling, so the second read's
// DVDLowRead is never kicked. The audio thread livelocks in DVDRead's wait loop (8034be2c,
// polling block->state@0xc), and thread0 reaches TApplication::mountStageArchive with a NULL
// archive buffer → mountFixed(null) wild read. Confirmed: under native scheduling only
// data/nintendo.szs transfers; pure-Dolphin reads sequence.arc / *.aw / mario.szs / … next.
//
// THE PORT (own the path). Instead of driving the GC DVD hardware FSM, we service the read
// ourselves, synchronously, from our own read-only DiscIO::Volume opened on the same disc image:
// read length bytes at the absolute disc offset straight into guest RAM (CopyToEmu — raw bytes,
// exactly like the DVD DMA), fill the DVDCommandBlock (state=END, transferred=length), and invoke
// the completion callback. No DI registers, no interrupt, no command-queue dispatch. Registered on
// the native_os seam so it intercepts on BOTH the recomp call path and the interpreter (the seam
// call_ppc / interp_run_until both consult) — covering JIT-only threads.
//
// FAITHFULNESS. DVDReadAbsAsyncPrio is the funnel every file read converges on (DVDRead,
// DVDReadPrio, DVDReadAsyncPrio all convert file-relative → absolute and call it). The sync
// wrappers check block->state and never sleep once it is END (so there is no lost-wakeup race —
// the wakeup-before-sleep window the hardware avoids by latency is avoided here by completing
// before return). The async callers get their callback invoked, as the FSM would on completion.
#include "../overrides.h"
#include "../intrinsics.h"      // call_ppc, mem_w32
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

#ifdef HAVE_DOLPHIN_CORE
#include "../native_os.h"
#include "Core/System.h"
#include "Core/HW/Memmap.h"
#include "DiscIO/Volume.h"

extern u32 mem_r32(u32 ea);

// DVDCommandBlock field offsets (from the DVDReadAbsAsyncPrio prologue stores, 8034da6c):
//   0x08 command (1 = READ)   0x0c state   0x10 disc offset   0x14 length
//   0x18 dest addr            0x20 transferred bytes          0x28 prio
namespace {
constexpr u32 CB_COMMAND = 0x08, CB_STATE = 0x0c, CB_OFFSET = 0x10, CB_LENGTH = 0x14,
              CB_ADDR = 0x18, CB_TRANSFERRED = 0x20;
constexpr s32 DVD_STATE_END = 0;          // terminal "done" the sync wait loops test for

std::string                       g_disc_path;
std::unique_ptr<DiscIO::Volume>   g_volume;
bool                              g_open_failed = false;

DiscIO::Volume* volume() {
    if (g_volume || g_open_failed) return g_volume.get();
    if (g_disc_path.empty()) { g_open_failed = true; return nullptr; }
    g_volume = DiscIO::CreateVolume(g_disc_path);
    if (!g_volume) {
        g_open_failed = true;
        fprintf(stderr, "[native_dvd] FAILED to open disc volume '%s' — DVD reads cannot be served\n",
                g_disc_path.c_str());
    } else {
        fprintf(stderr, "[native_dvd] disc volume opened: %s\n", g_disc_path.c_str());
    }
    return g_volume.get();
}

// BOOL DVDReadAbsAsyncPrio(DVDCommandBlock* block, void* addr, s32 length, s32 offset,
//                          s32 prio, DVDCBCallback callback)
void native_dvd_read_abs_async(CPUState& cpu) {
    const u32 block  = cpu.gpr[3];
    const u32 addr   = cpu.gpr[4];
    const s32 length = (s32)cpu.gpr[5];
    const u32 offset = cpu.gpr[6];
    const u32 cb     = cpu.gpr[8];
    static const bool dbg = getenv("SUNBRIGHT_DBG_DVD") != nullptr;

    s32 result = -1;   // DVD_RESULT_FATAL_ERROR
    DiscIO::Volume* v = volume();
    if (v && length > 0 && addr >= 0x80000000u) {
        std::vector<u8> buf((size_t)length);
        if (v->Read(offset, (u64)length, buf.data(), v->GetGamePartition())) {
            // Raw byte copy into guest RAM — exactly what the DVD DMA does (no byteswap;
            // file content is consumed as the big-endian bytes it is on disc).
            Core::System::GetInstance().GetMemory().CopyToEmu(addr, buf.data(), (size_t)length);
            result = length;
        } else {
            fprintf(stderr, "[native_dvd] Volume::Read failed off=0x%x len=%d -> addr=%08x\n",
                    offset, length, addr);
        }
    }

    // Fill the command block as a completed READ.
    mem_w32(block + CB_COMMAND, 1);
    mem_w32(block + CB_OFFSET, offset);
    mem_w32(block + CB_LENGTH, (u32)length);
    mem_w32(block + CB_ADDR, addr);
    mem_w32(block + CB_TRANSFERRED, (u32)(result < 0 ? 0 : result));
    mem_w32(block + CB_STATE, (u32)DVD_STATE_END);

    if (dbg) {
        static unsigned long n = 0;
        fprintf(stderr, "[native_dvd] #%lu read block=%08x off=0x%x len=%d -> addr=%08x result=%d cb=%08x cur=%08x\n",
                n++, block, offset, length, addr, result, cb, mem_r32(0x800000E4));
    }

    // Invoke the completion callback the FSM would have called on the DI interrupt:
    //   void cb(s32 result, DVDCommandBlock* block).  For sync wrappers this is a harmless
    //   wakeup-of-a-not-yet-sleeping thread (their wait loop sees state==END and never sleeps);
    //   for async readers it delivers completion. Non-volatile regs are preserved by the callee
    //   per ABI; r3 below is the BOOL return DVDReadAbsAsyncPrio hands back.
    if (cb >= 0x80000000u) {
        cpu.gpr[3] = (u32)result;
        cpu.gpr[4] = block;
        call_ppc(cpu, cb);
    }
    cpu.gpr[3] = 1;   // return TRUE (command accepted)
}
}  // namespace

void sunbright_native_dvd_set_path(const char* path) {
    if (path) g_disc_path = path;
}

// Host-side disc read for other native services (the ARAM ripper). Same volume, same raw bytes.
bool sunbright_disc_read(u64 offset, u64 length, u8* out) {
    DiscIO::Volume* v = volume();
    return v && v->Read(offset, length, out, v->GetGamePartition());
}

void native_dvd_register() {
    native_os_register(0x8034da6cu, native_dvd_read_abs_async);   // DVDReadAbsAsyncPrio
    fprintf(stderr, "[native_dvd] registered native DVD read service (DVDReadAbsAsyncPrio 8034da6c)\n");
}
#else
void sunbright_native_dvd_set_path(const char*) {}
void native_dvd_register() {}
#endif
