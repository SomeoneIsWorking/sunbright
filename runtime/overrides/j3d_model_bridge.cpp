// =============================================================================
// runtime/overrides/j3d_model_bridge.cpp — STEP 1 of owning J3D as a port/
// subsystem (docs/re_notes/j3d_subsystem_ownership_plan.md "Execution order" #1).
//
// The recompiled game holds J3D objects as 32-bit engine HANDLES (J3DModelData
// from the load bridge, J3DModel from the construction factory) and calls the
// J3D engine API by address. These overrides catch those calls, resolve the
// handle(s) to host pointers, and route to the PC-native port engine
// (port/bridge/j3d_bridge.cpp) — the host object graph stays inside port/.
//
// ⚠ STALE / INCOMPATIBLE WITH generated-virt (2026-06-15) — see scratch/handoff.md "BLOCKER (b)".
// This was written for the field-flip era where the recompiler rewrote `new J3DModel(...)` into
// `sbnew_J3DModel()` (raw host alloc -> handle) so gpr[3] arrived as a HANDLE. That rewrite was
// REMOVED with the field-flip (41aaa69). In the current (no-flip) generated-virt, `new J3DModel`
// is plain guest code: operator new -> GUEST RAM ptr -> bl __ct__J3DModel, and the holder stores
// the operator-new BUFFER (not the ctor return). So `sb_eng_host(gpr[3])` below gets a guest ptr,
// not a handle, and returns null. FIX REQUIRED: a flip-free construction->handle emission that
// intercepts the ALLOCATION (object-identity find_alloc_sites), not the ctor return — handoff NEXT #1.
//
// This TU includes NO decomp/port headers — the port object is opaque (void*);
// it references only the extern "C" port seam + the runtime handle table.
//
// ── KNOWN LIMITATION (honest) ────────────────────────────────────────────────
// J3DModel::calc/viewCalc are VIRTUAL. A game caller that dispatches virtually
// (`lwz r12,0(this); lwz r12,off(r12); bctrl`) loads the vtable from the HANDLE
// token (0x9xxxxxxx) and faults in the memory bridge BEFORE the call resolves —
// the unsolved "virtual dispatch at offset-0 on a flipped handle" recompiler
// problem (docs/ARCHITECTURE_TARGET.md function-call half). These overrides are
// correct for DIRECT `bl <addr>` callers; full game-driven calc verification is
// gated on the offset-0 host-vtable routing enhancement.
//
// ── BUILD COUPLING ───────────────────────────────────────────────────────────
// Returning/consuming handles is only correct when the recompiled callers were
// generated with SUNBRIGHT_ENGINE_TYPES="J3DModelData J3DModel". Gated OFF behind
// SB_FLIP_J3D so the default binary is unchanged; the override registration and
// the engine-types recompile land together (build-time coupling, not a runtime
// toggle).
// =============================================================================
#include "overrides.h"
#include "intrinsics.h"   // call_ppc, sb_eng_handle, sb_eng_host

#include <cstdint>

extern "C" {
void* sbport_j3dmodel_ctor(void* self, void* md, uint32_t model_flag,
                           uint32_t mtx_buffer_flag);
void  sbport_j3dmodel_entryModelData(void* self, void* md, uint32_t model_flag,
                                     uint32_t mtx_buffer_flag);
void  sbport_j3dmodel_calc(void* self);
void  sbport_j3dmodel_viewCalc(void* self);
}

#ifdef SB_FLIP_J3D

// __ct__8J3DModelFP12J3DModelDataUlUl @0x802dde2c
//   r3 = J3DModel handle (from sbnew_J3DModel, raw buffer), r4 = J3DModelData
//   handle, r5 = model_flag, r6 = mtx_buffer_flag. Returns `this` in r3.
SUNBRIGHT_OVERRIDE(ov_j3dmodel_ctor, 0x802dde2c) {
	void* self = sb_eng_host(cpu.gpr[3]);
	void* md   = sb_eng_host(cpu.gpr[4]);
	sbport_j3dmodel_ctor(self, md, cpu.gpr[5], cpu.gpr[6]);
	// r3 stays the handle (CodeWarrior ctors return `this`; the host object is
	// addressed by the same handle).
	call_ppc(cpu, cpu.lr);
}

// entryModelData__8J3DModelFP12J3DModelDataUlUl @0x802ddf90
SUNBRIGHT_OVERRIDE(ov_j3dmodel_entryModelData, 0x802ddf90) {
	void* self = sb_eng_host(cpu.gpr[3]);
	void* md   = sb_eng_host(cpu.gpr[4]);
	sbport_j3dmodel_entryModelData(self, md, cpu.gpr[5], cpu.gpr[6]);
	call_ppc(cpu, cpu.lr);
}

// calc__8J3DModelFv @0x802debc4
SUNBRIGHT_OVERRIDE(ov_j3dmodel_calc, 0x802debc4) {
	sbport_j3dmodel_calc(sb_eng_host(cpu.gpr[3]));
	call_ppc(cpu, cpu.lr);
}

// viewCalc__8J3DModelFv @0x802deeb8
SUNBRIGHT_OVERRIDE(ov_j3dmodel_viewCalc, 0x802deeb8) {
	sbport_j3dmodel_viewCalc(sb_eng_host(cpu.gpr[3]));
	call_ppc(cpu, cpu.lr);
}

#endif  // SB_FLIP_J3D
