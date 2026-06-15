// =============================================================================
// runtime/overrides/j3d_loader_bridge.cpp — the runtime half of the FIRST
// tailored-recomp subsystem flip (J3DModelData).
//
// Catches the recompiled game's call to J3DModelLoaderDataBase::load (guest
// 0x802e6f00, a STATIC method: r3=BMD guest addr, r4=flags) and routes it to the
// PC-native PORT loader (port/bridge/j3d_bridge.cpp), returning the host object
// as a 32-bit engine HANDLE. From that point the recompiled J3DModelData*
// consumers (built with SUNBRIGHT_ENGINE_TYPES=J3DModelData) read the HOST object
// through sb_eng_host(handle) instead of guest RAM.
//
// This TU includes NO decomp/port headers — the port object is opaque (void*).
// It only references the extern "C" seam, which pulls the J3DModelData loader
// closure from the smsport_bridge/core/pal/assets archives at link.
//
// ── BUILD COUPLING (read before enabling) ────────────────────────────────────
// Returning a handle is ONLY correct when the recompiled callers were generated
// with SUNBRIGHT_ENGINE_TYPES=J3DModelData (so they treat the result as a handle,
// not a guest pointer). Until that recompile is the active generated/ set, this
// override is gated OFF behind SB_FLIP_J3D so the default binary is
// unchanged. Enable the flip by building the runtime with -DSB_FLIP_J3D
// AND regenerating generated/ with the engine type set — the two land together.
// (Build-time coupling of two halves of one translation, NOT a runtime toggle.)
// =============================================================================
#include "overrides.h"
#include "intrinsics.h"   // call_ppc, sb_eng_handle, sb_guest_to_host

#include <cstdint>

extern "C" void* sbport_j3d_load(const void* be_bmd, uint32_t flags);

#ifdef SB_FLIP_J3D
// J3DModelLoaderDataBase::load(const void* data, u32 flags) @ 0x802e6f00
SUNBRIGHT_OVERRIDE(ov_j3d_model_load, 0x802e6f00) {
	u32   guest_bmd = cpu.gpr[3];
	u32   flags     = cpu.gpr[4];
	void* host_bmd  = sb_guest_to_host(guest_bmd);
	void* md        = sbport_j3d_load(host_bmd, flags);
	cpu.gpr[3] = md ? sb_eng_handle(md) : 0u;
	call_ppc(cpu, cpu.lr);
}
#else
// Flip gated off: the override is not registered and the port loader closure is
// not pulled (the default binary is byte-for-byte unchanged). `sbport_j3d_load`
// stays declared (above) so enabling the flip is a one-define change.
#endif
