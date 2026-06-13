// REPL-traced observer (SUNBRIGHT_DBG_LOGO) for the GC boot-logo state machine, to resolve why
// native skips creating the TMarDirector (see docs/native_threading.md). Records per-call state into
// the REPL trace ring (read via /tracelog) instead of env-gated stderr — keeps execution-trace data
// in the REPL. OBSERVES (super-calls the real body), so behaviour is unchanged.
//   802a6398 = director-creator state machine (this=803e9700; state byte [this+8]).
//   802a5f50 = per-iteration driver, RETURNS the next state in r3.
//   8034c374 = the state-2 gate whose return decides advance-to-create (r29=3) vs not.
#include "../overrides.h"
#include <cstdlib>
#include <cstdio>

#ifdef HAVE_DOLPHIN_CORE
extern "C" void func_802a6398(CPUState& cpu);
extern "C" void func_802a5f50(CPUState& cpu);
extern "C" void sb_trace(const char* tag, u32 a, u32 b, u32 c, u32 d);
extern u32 mem_r32(u32 ea);

static bool on() { static const bool e = getenv("SUNBRIGHT_DBG_LOGO") != nullptr; return e; }
// Mirror to stderr too: the REPL /tracelog ring is lost when the process aborts (the macOS
// func_802a5f50 null+0x64 fault at gameplay entry), so the last states must reach the crash log.
static void logo_log(const char* tag, u32 self, u32 state, u32 dir, u32 phase) {
    sb_trace(tag, self, state, dir, phase);
    if (on()) fprintf(stderr, "[dbg_logo] %-9s this=%08x state=%08x director=%08x phase=%08x\n",
                      tag, self, state, dir, phase);
}

// 802a6398: director-creator. Trace state-in (byte [this+8]) and director [this+4] each call.
SUNBRIGHT_OVERRIDE(dbg_logo_creator, 0x802a6398u) {
    const u32 self = cpu.gpr[3];
    if (on()) logo_log("creat.in", self, mem_r32(self + 8), mem_r32(self + 4), mem_r32(0x8040e190));
    func_802a6398(cpu);
    if (on()) logo_log("creat.out", self, mem_r32(self + 8), mem_r32(self + 4), mem_r32(0x8040e190));
}

// 802a5f50: per-iteration driver. r3 in = this; r3 out = next state. Trace in/out + director + phase.
SUNBRIGHT_OVERRIDE(dbg_logo_driver, 0x802a5f50u) {
    const u32 self = cpu.gpr[3];
    const u32 st_in = mem_r32(self + 8), dir = mem_r32(self + 4), ph = mem_r32(0x8040e190);
    if (on()) logo_log("drv.in", self, st_in, dir, ph);
    func_802a5f50(cpu);
    if (on()) logo_log("drv.ret", self, cpu.gpr[3], mem_r32(self + 4), mem_r32(0x8040e190));
}
#endif
