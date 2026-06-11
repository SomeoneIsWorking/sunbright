// Native TDrawSyncManager — full PC-native port of the CPU↔GPU frame-pacing protocol
// (reference/sms/src/System/DrawSyncManager.cpp). Replaces the guest message-queue +
// counting-thread implementation entirely; the guest threadFunc thread never receives a
// message and sleeps forever (harmless).
//
// ROOT CAUSE this port removes (pinned live, 2026-06-12): threadFunc's accounting is pure
// message COUNTING — boundary pushes (≥0x80000000) grow its fifo, token messages advance it.
// That is only sound if a frame's token message always arrives AFTER its boundary push; on
// hardware the GX drawsync interrupt physically fires later. In the hybrid, tokens are
// delivered in batches at dispatch points while pushBreakPoint posts immediately from the
// game thread, so a token can be processed BEFORE its frame's boundary push. The advance
// then underflows the fifo (size 0 → 0x28), leaving a permanent phantom entry; the terminal
// state — fifo size 1 (phantom), breakpoint ENABLED at an already-completed boundary, GPU
// parked on it, message queue empty, everyone asleep — was captured live via /gx + /r probes
// (tokens==callbacks==1842: no loss at the PE seam; this was pure ordering). The state-based
// recovery (old sms_drawsync_native.cpp) refused exactly this state (its `size >= 2` guard),
// so the vi.gpu_backpressure loop span forever → watchdog kill = the Delfino-entry freeze.
//
// The native port makes the accounting ORDER-INDEPENDENT: an early token (no boundary
// registered yet) becomes a CREDIT consumed by the next push. The breakpoint policy is a pure
// function of the outstanding-boundary deque (faithful to threadFunc: ≥2 outstanding →
// breakpoint at the SECOND boundary, i.e. the GPU may run exactly one frame ahead; ≤1 → no
// breakpoint), applied idempotently after every event. No queue, no counting thread, no spin.
#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>

unsigned long g_ds_callbacks = 0;            // drawsync diag (probe /drawsync)

namespace {

constexpr u32 SM_INSTANCE   = 0x8040E1D0u;   // TDrawSyncManager::smInstance
constexpr u32 OFF_CB_BEGIN  = 0x4;           // JGadget::TVector<TDrawSyncTokenRange>.begin
constexpr u32 OFF_CB_END    = 0x8;
constexpr u32 OFF_FLAGS     = 0x34C;         // u16 mFlags
constexpr u32 GX_FLUSH           = 0x8035d8f0u;
constexpr u32 GX_ENABLE_BREAKPT  = 0x8035bbf8u;
constexpr u32 GX_DISABLE_BREAKPT = 0x8035bc90u;

// RECURSIVE mutex: calling a guest GX function (GXEnableBreakPt) delivers pending boundary
// interrupts, which drain the PE token ring, which re-enters ov_drawsync_token on the SAME
// host thread — a plain mutex self-deadlocked there (freeze_20260612_022319). Nested events
// only mutate state; the OUTERMOST apply loop re-evaluates the policy afterwards (g_applying).
std::recursive_mutex g_mtx;
std::deque<u32> g_boundaries;     // outstanding frame-end boundaries (guest FIFO addresses)
int  g_credits = 0;               // tokens that arrived before their boundary push
bool g_bp_on = false;             // last applied breakpoint state
u32  g_bp_at = 0;
bool g_applying = false;          // policy application in progress (single active guest thread)
unsigned long g_retires = 0, g_pushes = 0, g_credited = 0;

bool dbg() { static int v = -1; if (v < 0) v = getenv("SUNBRIGHT_DBG_DS") ? 1 : 0; return v; }

// Apply the breakpoint policy from current state (idempotent; faithful to threadFunc:
// ≥2 outstanding → breakpoint at the second boundary, else none). Loops until the applied
// state matches the desired state, since each guest call can deliver nested token events
// that change the deque.
void apply_policy(CPUState& cpu) {
    if (g_applying) return;        // outermost applier will re-evaluate after its guest call
    g_applying = true;
    for (int guard = 0; guard < 16; guard++) {
        const bool want_on = g_boundaries.size() >= 2;
        const u32 want_at = want_on ? g_boundaries[1] : 0;
        if (want_on == g_bp_on && (!want_on || want_at == g_bp_at)) break;
        if (want_on) {
            cpu.gpr[3] = want_at;
            call_ppc(cpu, GX_ENABLE_BREAKPT);
        } else {
            call_ppc(cpu, GX_DISABLE_BREAKPT);
        }
        g_bp_on = want_on; g_bp_at = want_at;
    }
    g_applying = false;
}

void retire_one(CPUState& cpu) {
    if (!g_boundaries.empty()) { g_boundaries.pop_front(); g_retires++; }
    else { g_credits++; g_credited++; }
    apply_policy(cpu);
}

}  // namespace

// Native TDrawSyncManager::drawSyncCallback — the GX token callback (registered global
// 0x8040EA18 → 0x802A9318; we own that entry). Token 0 = frame retire; ranged tokens run
// their registered guest callback (virtual: *(vtable+8)) then ALSO retire, exactly as
// drawSyncCallbackSub does. Tokens outside every range (and not 0) are ignored, as original.
SUNBRIGHT_OVERRIDE(ov_drawsync_token, 0x802a9318u) {
    const u16 tok = (u16)cpu.gpr[3];
    g_ds_callbacks++;
    const u32 inst = MEM_R32(SM_INSTANCE);
    if (!inst) return;
    const u16 flags = (u16)MEM_R16(inst + OFF_FLAGS);
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    if (tok == 0) {
        if (!(flags & 2)) retire_one(cpu);
        return;
    }
    for (u32 e = MEM_R32(inst + OFF_CB_BEGIN); e + 8 <= MEM_R32(inst + OFF_CB_END); e += 8) {
        const u16 lo = (u16)MEM_R16(e), hi = (u16)MEM_R16(e + 2);
        const u32 cb = MEM_R32(e + 4);
        if (!cb || tok < lo || tok > hi) continue;
        const u32 method = MEM_R32(MEM_R32(cb) + 8);   // ->drawSyncCallback(u16), first virtual
        cpu.gpr[3] = cb;
        cpu.gpr[4] = tok;
        call_ppc(cpu, method);
        if (!(flags & 2)) retire_one(cpu);
        return;
    }
}

// Native TDrawSyncManager::pushBreakPoint (0x802a9020): flush the CPU FIFO, record the write
// pointer as this frame's end boundary (consuming a credit if its token already arrived),
// apply the policy. Replaces GXFlush + GXGetFifoPtrs + OSSendMessage.
SUNBRIGHT_OVERRIDE(ov_drawsync_pushBreakPoint, 0x802a9020u) {
    const u32 inst = cpu.gpr[3];
    if (inst && (MEM_R16(inst + OFF_FLAGS) & 3)) return;
    call_ppc(cpu, GX_FLUSH);
    extern u32 sunbright_cp_write_ptr();   // dolphin_hook.cpp (live CP write pointer | 0x80000000)
    const u32 wp = sunbright_cp_write_ptr();
    if (!wp) return;
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    g_pushes++;
    if (g_credits > 0) g_credits--;        // frame already completed before we registered it
    else g_boundaries.push_back(wp);
    apply_policy(cpu);
    if (dbg() && g_pushes % 256 == 0)
        fprintf(stderr, "[ds-native] pushes=%lu retires=%lu credits_used=%lu outstanding=%zu bp=%s@%08x\n",
                g_pushes, g_retires, g_credited, g_boundaries.size(), g_bp_on ? "on" : "off", g_bp_at);
}
