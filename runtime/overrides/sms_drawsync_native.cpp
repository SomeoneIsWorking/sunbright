// Native TDrawSyncManager sync thread — PC-native port of threadFunc (own the pollution/draw-sync
// breakpoint pipeline). Decomp: reference/sms/src/System/DrawSyncManager.cpp.
//
// WHY (root cause, measured 2026-06-10): the original threadFunc advances its frame FIFO and the
// CP breakpoint by COUNTING draw-sync token messages. Dolphin's PixelEngine COALESCES token
// interrupts (one event in flight, only the LATEST token value survives), so under load a
// boundary's token message is lost; the GPU parks at the breakpoint, produces no further tokens,
// and the pipeline wedges forever (per-minute /drawsync counters: tokens/callbacks/sleeps/wakes
// all flat, GPU rp==bp). On real hardware every token interrupt is delivered; the counting
// protocol is sound there and unsound here — so we own the accounting and make it state-based:
// the CP READ POINTER is ground truth. "GPU parked at the breakpoint" MEANS that boundary's
// frame (and its token) is done — advance, breakpoint to the next boundary, never wedge.
//
// The guest objects stay authoritative (TFifo in guest RAM, layout from the decomp + verified
// live); range token CALLBACKS (pollution counters) still flow through the unchanged
// GXSetDrawSyncCallback path — only the FIFO/breakpoint accounting moves here.
#include "../overrides.h"
#include "../intrinsics.h"
#include "../dolphin_hook.h"
#include <cstdio>
#include <chrono>
#include <thread>

#ifdef HAVE_DOLPHIN_CORE
#include "Core/System.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"

namespace {

constexpr u32 OS_RECEIVE_MSG     = 0x80346258u;  // OSReceiveMessage(queue, &msg, flags)
constexpr u32 GX_ENABLE_BREAKPT  = 0x8035bbf8u;
constexpr u32 GX_DISABLE_BREAKPT = 0x8035bc90u;
// TDrawSyncManager layout (decomp ctor + live verification):
constexpr u32 OFF_MSGQUEUE = 0x328;
constexpr u32 OFF_FIFO_PTR = 0x348;
// TFifo layout: {void** mData, int mCapacity, int mReadIdx, u32 mWriteIdx}

struct TFifoView {
    u32 obj;
    u32 data()  const { return MEM_R32(obj + 0x0); }
    u32 cap()   const { return MEM_R32(obj + 0x4); }
    u32 ridx()  const { return MEM_R32(obj + 0x8); }
    u32 widx()  const { return MEM_R32(obj + 0xC); }
    u32 loop(u32 i) const { return i >= cap() + 1 ? 0 : i; }
    int size() const {
        u32 r = ridx(), w = widx();
        return r <= w ? (int)(w - r) : (int)(w + cap() + 1 - r);
    }
    u32 read()  const { return MEM_R32(data() + loop(ridx() + 1) * 4); }
    void push(u32 v)  { MEM_W32(data() + widx() * 4, v); MEM_W32(obj + 0xC, loop(widx() + 1)); }
    void advance()    { MEM_W32(obj + 0x8, loop(ridx() + 1)); }
};

inline u32 cp_read_ptr() {
    auto& fifo = Core::System::GetInstance().GetCommandProcessor().GetFifo();
    return 0x80000000u | fifo.CPReadPointer.load(std::memory_order_relaxed);
}
inline bool gpu_at_breakpoint() {
    auto& fifo = Core::System::GetInstance().GetCommandProcessor().GetFifo();
    return fifo.bFF_BPEnable.load(std::memory_order_relaxed) &&
           fifo.CPReadPointer.load(std::memory_order_relaxed) ==
               fifo.CPBreakpoint.load(std::memory_order_relaxed);
}

void apply_bp_policy(CPUState& cpu, TFifoView& f) {
    // Faithful threadFunc policy: 1 outstanding → no breakpoint (GPU free);
    // ≥2 → breakpoint at the FIRST outstanding boundary (the GPU may run one frame ahead).
    const int n = f.size();
    if (n <= 0) return;
    if (n == 1) {
        call_ppc(cpu, GX_DISABLE_BREAKPT);
    } else {
        cpu.gpr[3] = f.read();
        call_ppc(cpu, GX_ENABLE_BREAKPT);
    }
}

// Idle-driver loss recovery. The guest threadFunc stays authoritative (its blocking
// OSReceiveMessage parks naturally — a polling replacement broke the frame barrier: a
// perpetually-Ready thread means block_drain never completes, zero frames; measured run 105).
// When every guest thread is blocked and the GPU is parked exactly on the fifo's next boundary
// with the message queue empty, the boundary's token was lost (PE coalescing): post a synthetic
// token-0 message through the NORMAL queue. The real threadFunc wakes, advances one frame, and
// repoints the breakpoint — the original code path does all the work.
constexpr u32 SM_INSTANCE_GLOBAL = 0x8040E1D0u;  // TDrawSyncManager::smInstance (r13-0x5FF0)
constexpr u32 OS_SEND_MSG        = 0x80346190u;  // OSSendMessage(queue,msg,flags) — verify at init

bool sunbright_drawsync_recover_impl(CPUState& cpu) {
    const u32 inst = MEM_R32(SM_INSTANCE_GLOBAL);
    if (!inst) return false;
    const u32 queue = inst + OFF_MSGQUEUE;
    if (MEM_R32(queue + 0x1C) != 0) return false;       // usedCount: messages pending → not lost
    TFifoView f{MEM_R32(inst + OFF_FIFO_PTR)};
    if (!f.obj || f.size() < 2) return false;
    if (!gpu_at_breakpoint() || cp_read_ptr() != f.read()) return false;
    static long rec = 0;
    if (rec++ < 64)
        fprintf(stderr, "[drawsync-native] recovering lost token at boundary %08x\n", f.read());
    cpu.gpr[3] = queue;
    cpu.gpr[4] = 0;                                      // token 0: "frame done" per Sub semantics
    cpu.gpr[5] = 0;                                      // NOBLOCK (queue verified non-full: empty)
    call_ppc(cpu, OS_SEND_MSG);
    return true;
}

}  // namespace

bool sunbright_drawsync_recover(CPUState& cpu) {
#ifdef HAVE_DOLPHIN_CORE
    return sunbright_drawsync_recover_impl(cpu);
#else
    (void)cpu; return false;
#endif
}

namespace {
}  // namespace
#endif  // HAVE_DOLPHIN_CORE
