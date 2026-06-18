#pragma once
#include "cpu_state.h"

// Hook into Dolphin's JIT interface to redirect compiled blocks to our
// statically-recompiled native functions.
//
// Mechanism:
//   1. Load the recompiled shared library (generated/libsms_recomp.so)
//   2. Patch JitInterface::Compile to check g_recomp_table first
//   3. If found: call native function (after translating Dolphin PowerPC::State to CPUState)
//   4. If not found: fall through to Dolphin's normal JIT

// Call once after Dolphin Core is initialized and before starting emulation.
bool dolphin_hook_install(const char* recomp_lib_path);

// Uninstall hook (called on shutdown).
void dolphin_hook_uninstall();

// Build the direct-mapped dispatch table from the linked recomp table. Must be
// called once at startup so call_ppc resolves intra-recomp bl/returns directly
// (otherwise every call bounces to Dolphin's JIT with a full register-file copy).
void recomp_build_dispatch();

// Translate Dolphin's internal CPU state to our CPUState struct and back.
// Used at every recompiled function call boundary.
#ifdef HAVE_DOLPHIN_CORE
#  include "Core/PowerPC/PowerPC.h"
#  include <csetjmp>
void dolphin_state_to_cpu(const PowerPC::PowerPCState& src, CPUState& dst);
void cpu_to_dolphin_state(const CPUState& src, PowerPC::PowerPCState& dst);

// C-call model: install the longjmp target a tail-branch into non-recomp code uses
// to unwind the recomp C stack. Returns the previous slot (restore it when done).
sigjmp_buf* sunbright_set_tail_jmp(sigjmp_buf* j);

// Native threading: adopt the calling host thread (Dolphin's EmuThread / CPU thread)
// as nthr guest thread 0 and install the context-switch hooks. Idempotent; call from
// the guest-entry path so it runs exactly once, on the right thread.
void sunbright_adopt_cpu_thread();

// Takeover-time adoption of all GC threads created before interception (lazy; idempotent). Builds
// the nthr registry for the pre-existing threads; inert until the native primitives schedule them.
void sunbright_adopt_all_gc_threads();

// Run a recompiled function tree to completion on the current native C stack, handling
// both exits: a normal C return (top-level blr) commits cpu→ppc and sets pc=lr; a
// tail-branch into non-recomp code siglongjmps back having already committed ppc. Shared
// by SunbrightBridge::Run (JIT entry) and a guest host-thread body (native threading), so
// both use the identical tail-jmp logic. `fn` is the recomp body for `cpu.pc`.
void sunbright_run_recomp_tree(CPUState& cpu, void (*fn)(CPUState&));

// Reentrant run-original-under-Dolphin-JIT primitive (purejit engine seam — see dolphin_hook.cpp).
// sb_bypass_once_check: consulted by IsRecompiled — one-shot "JIT the original at this addr".
// sb_consume_explicit_next_pc: consulted by the Run epilogue — resume at the original, not cpu.lr.
bool sb_bypass_once_check(u32 pc);
bool sb_consume_explicit_next_pc(u32& pc_out);

// Native-threading runtime (host-thread guest threads + guest↔host mapping). Called by the
// native OS primitives in native_os.cpp. See dolphin_hook.cpp.
void nthrt_spawn_guest(u32 os_thread, u32 entry, u32 param, u32 stack, int prio);  // OSCreateThread
// Recomp RAM poll loop (e.g. the GC OS idle loop on RunQueueBits): advance CoreTiming and deliver
// a pending interrupt + run its ISR, so an ISR can set the polled flag. See dolphin_hook.cpp.
void sunbright_poll_yield();
void sunbright_dispatch_profile_note(u32 pc);   // SUNBRIGHT_DISPATCH_PROFILE histogram

void nthrt_make_ready(u32 os_thread);   // OSResumeThread / OSWakeupThread (mark Ready)
void nthrt_bind_current(u32 os_thread); // map the running thread under its authoritative OSThread*
void nthrt_block_current();             // OSSleepThread (park current until woken)
// Frame barrier: resume once nothing else is Ready, or after deadline_us of host time (<=0 =
// unbounded). Bounded matches hardware: the retrace interrupt can't be starved by a yield-spin.
void nthrt_block_drain(CPUState* caller = nullptr, long deadline_us = 0);
void nthrt_yield_current(CPUState* yielder = nullptr);  // OSYieldThread: yield token, stay Ready;
void sunbright_wait_vi_field(CPUState& cpu);  // force ONE VI field of emulated time (native IRQ dispatch)
bool sunbright_drawsync_recover(CPUState& cpu); // idle: recover a PE-coalesced draw-sync token (sms_drawsync_native)
                                        // if nothing else runnable, run the GC idle thread (advance
                                        // CoreTiming + deliver device IRQs) on the yielder's context
#endif
