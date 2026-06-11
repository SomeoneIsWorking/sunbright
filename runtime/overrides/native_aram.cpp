// Native ARAM transfer service — PC-native port of the GameCube ARQ (ARAM DMA queue).
//
// WHY. Boot's archive/audio pipeline stages all funnel ARAM traffic through ARQPostRequest
// (0x80353BDC): JKRAramPiece::orderSync/orderAsync (archive loads to/from ARAM), the JKRAram
// command thread, and JASystem's wave loading (TApplication::setupThreadFuncLogo busy-yields on
// gpMSound->checkWaveOnAram until the waves land in ARAM). The SDK implementation queues the
// request, kicks the AR DMA hardware, and completes via the ARQ interrupt — making every one of
// those boot stages dependent on emulated DMA timing + interrupt delivery. On a PC "ARAM" is
// just a 16 MB host buffer (Dolphin keeps it raw — its DMA does swap64(Read_U64), i.e. a
// byte-identical copy), so the faithful native port is: do the copy NOW, fill the task like the
// hardware completion would, and invoke the request callback synchronously. No DMA interrupt,
// no queue, no timing.
//
// ARQRequest layout (from the ARQPostRequest prologue stores at 0x80353BDC):
//   +0x00 next (queue link)   +0x04 owner   +0x08 type (0 = MRAM→ARAM, 1 = ARAM→MRAM)
//   +0x10 source              +0x14 dest    +0x18 length   +0x1C callback
// MRAM addresses arrive PHYSICAL (0x0xxxxxxx); ARAM addresses are ARAM offsets. The default
// callback (0x80353AA4) is a bare blr — skipped. Registered on the native_os seam so both the
// recomp call path and interpreter-run threads (the JKRAram command thread) hit it.

#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef HAVE_DOLPHIN_CORE
#include "../native_os.h"
#include "../dolphin_hook.h"
#include "Core/System.h"
#include "Core/HW/DSP.h"

namespace {

constexpr u32 ARQ_POST_REQUEST = 0x80353BDCu;
constexpr u32 ARQ_DEFAULT_CB   = 0x80353AA4u;  // bare blr

// ISR-faithful callback serialization. The real ARQ completes via a LATER interrupt: the post
// returns first, then the callback runs. Calling the callback synchronously INSIDE the post
// inverted that ordering — a callback that re-posts (the JAS wave-streamer chunk pump) ran its
// whole chain before the original caller's post-return bookkeeping, and the loader's queued
// NEXT FILE was lost (wScene_10.aw never streamed → silent select-scene instruments, the
// choppy-music bug, 2026-06-11). Defer: queue completions; the OUTERMOST post drains the queue
// after it returns-equivalent (its own fill is done), so each callback still runs "immediately
// after its DMA" but never inside another post's frame.
struct PendingCb { u32 cb, task; };
std::vector<PendingCb> g_pending;
bool g_draining = false;

void native_arq_post_request(CPUState& cpu) {
    const u32 task = cpu.gpr[3];
    const u32 owner = cpu.gpr[4], type = cpu.gpr[5];
    const u32 source = cpu.gpr[7], dest = cpu.gpr[8], length = cpu.gpr[9];
    const u32 cb = cpu.gpr[10];

    auto& sys = Core::System::GetInstance();
    u8* aram = sys.GetDSP().GetARAMPtr();

    const u32 mram_phys = (type == 0) ? source : dest;     // 0 = MRAM→ARAM
    const u32 aram_off  = (type == 0) ? dest : source;
    u8* mram = sb_ram_fast(0x80000000u | (mram_phys & 0x01FFFFFFu));
    if (!mram || aram_off + length > 0x01000000u) {
        fprintf(stderr, "[native_aram] BAD request task=%08x type=%u mram=%08x aram=%08x len=%u\n",
                task, type, mram_phys, aram_off, length);
        return;
    }
    if (type == 0) memcpy(aram + aram_off, mram, length);  // raw bytes — see header comment
    else           memcpy(mram, aram + aram_off, length);

    // Fill the task exactly as ARQPostRequest does, minus the queue link (never queued).
    mem_w32(task + 0x00, 0);
    mem_w32(task + 0x04, owner);
    mem_w32(task + 0x08, type);
    mem_w32(task + 0x10, source);
    mem_w32(task + 0x14, dest);
    mem_w32(task + 0x18, length);
    mem_w32(task + 0x1C, cb ? cb : ARQ_DEFAULT_CB);

    static const bool dbg = getenv("SUNBRIGHT_DBG_ARAM") != nullptr;
    if (dbg) fprintf(stderr, "[native_aram] %s mram=%08x aram=%08x len=%u cb=%08x\n",
                     type == 0 ? "MRAM->ARAM" : "ARAM->MRAM", mram_phys, aram_off, length, cb);

    // Hardware completion: the ARQ ISR pops the request and calls its callback(task) — AFTER
    // the post returns. Queue it; the outermost post drains (ISR-like serialization, see above).
    if (cb != 0 && cb != ARQ_DEFAULT_CB)
        g_pending.push_back({cb, task});
    if (!g_draining) {
        g_draining = true;
        while (!g_pending.empty()) {
            PendingCb p = g_pending.front();
            g_pending.erase(g_pending.begin());
            const u32 saved_lr = cpu.lr;
            cpu.gpr[3] = p.task;
            cpu.lr = ARQ_POST_REQUEST;
            call_ppc(cpu, p.cb);
            cpu.lr = saved_lr;
        }
        g_draining = false;
    }
    cpu.gpr[3] = task;   // void return; keep r3 harmless
}

}  // namespace

void native_aram_register() {
    native_os_register(ARQ_POST_REQUEST, native_arq_post_request);
    fprintf(stderr, "[native_aram] registered native ARQ service (ARQPostRequest 80353bdc)\n");
}
#else
void native_aram_register() {}
#endif  // HAVE_DOLPHIN_CORE
