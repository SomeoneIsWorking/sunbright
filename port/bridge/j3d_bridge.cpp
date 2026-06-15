// =============================================================================
// port/bridge/j3d_bridge.cpp — C-ABI seam between the recompiled game (sunbright
// runtime) and the PC-native PORT J3D model loader.
//
// This is the FIRST tailored-recomp subsystem flip (docs/ARCHITECTURE_TARGET.md,
// first-flip = J3DModelData per docs/re_notes/first_flip_endianness.md). The
// recompiled game calls J3DModelLoaderDataBase::load(guestBMD, flags) at guest
// 0x802e6f00; the runtime override (runtime/overrides/j3d_loader_bridge.cpp)
// catches it and routes here. We:
//   1. byteswap the big-endian guest BMD into a host-endian copy (bmd_swap), and
//   2. run the PRISTINE port decomp loader on it, producing a host J3DModelData.
// The host object is returned to the runtime, which hands the game a 32-bit
// handle (sb_eng_handle); the recompiled J3DModelData* consumers (built with
// SUNBRIGHT_ENGINE_TYPES=J3DModelData) read it host-side via sb_eng_host.
//
// Compiled INSIDE the port build (smsport_bridge) so it sees the decomp headers
// with the compat shims + LP64 block-struct shadows. It exposes ONLY a plain
// extern "C" surface (opaque void* / scalars), so the runtime override TU can
// call it WITHOUT pulling any decomp header into the Dolphin-based runtime build.
//
// Lifetime: the loader stores pointers INTO the swapped file image (vertex/joint/
// material tables are referenced in place), so the host-endian copy must outlive
// the J3DModelData. For this loader-managed slice the model is long-lived, so the
// copy is intentionally leaked (held by a heap-allocated vector). A future teardown
// path would free both together.
// =============================================================================
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/JMath.hpp>
#include "bmd_swap.h"

#include <cstdint>
#include <vector>

extern "C" void sb_heap_bringup();   // port/pal/heap/heap_init.cpp (idempotent)

using namespace smsport::assets;

// Idempotent J3D engine global init. The game does this once at boot
// (System/Application.cpp:390 -> JMANewSinTable(0xC)); the joint transform math
// in J3DModel::calc (J3DGetTranslateRotateMtx -> JMASSin/JMASCos) reads the
// global jmaSinTable, which is null until this runs. Called from every bridge
// entry point that can be the first J3D activity in a run (load + construct).
static void sb_j3d_bringup() {
	static bool done = false;
	if (done)
		return;
	sb_heap_bringup();      // JMANewSinTable allocates the table from the JKRHeap
	JMANewSinTable(0xC);    // 12-bit sin table, matching the game
	done = true;
}

// Big-endian J3D file header: magic[4], type[4], fileLength@0x08 (BE u32).
static inline uint32_t be32(const uint8_t* p) {
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
	       (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

extern "C" {

// Load a big-endian guest BMD (host pointer to the bytes in guest RAM) into a
// host-native J3DModelData. Returns the host pointer (as void*) or nullptr on
// failure (null/short/unswappable input). `flags` is the guest load flags (r4).
void* sbport_j3d_load(const void* be_bmd, uint32_t flags) {
	if (!be_bmd)
		return nullptr;

	// Ensure the host JKRHeap (loader allocates aligned tables from it) + J3D
	// engine globals (sin table) are up. Idempotent.
	sb_j3d_bringup();

	const uint8_t* p = static_cast<const uint8_t*>(be_bmd);
	uint32_t len = be32(p + 8);             // J3D fileLength
	if (len < 0x20)
		return nullptr;

	// Held for the model's lifetime (loader references the image in place).
	std::vector<uint8_t>* host = new std::vector<uint8_t>();
	BmdSwapResult r = bmd_swap_to_host(p, len, *host);
	if (!r.ok || !r.all_covered) {
		delete host;
		return nullptr;
	}

	J3DModelData* md = J3DModelLoaderDataBase::load(host->data(), flags);
	if (!md)
		delete host;   // swap copy unused; loader produced nothing
	return md;
}

// Field/method accessors for oracle verification of the flipped slice. The
// J3DModelData* is the host pointer (the runtime resolves the handle before
// calling these via the bridge thunk).
uint16_t sbport_j3dmodeldata_getJointNum(void* md) {
	return md ? static_cast<J3DModelData*>(md)->getJointNum() : 0;
}
uint16_t sbport_j3dmodeldata_getShapeNum(void* md) {
	return md ? static_cast<J3DModelData*>(md)->getShapeNum() : 0;
}
uint16_t sbport_j3dmodeldata_getMaterialNum(void* md) {
	return md ? static_cast<J3DModelData*>(md)->getMaterialNum() : 0;
}

// ── J3DModel: construct + per-frame calc bridge (STEP 1) ─────────────────────
// J3DModel is POLYMORPHIC with an OUT-OF-LINE ctor (guest __ct__8J3DModelFP12-
// J3DModelDataUlUl @0x802dde2c). The recompiler's object-identity path rewrites
// the game's `new J3DModel(...)` into `sbnew_J3DModel()` (a raw host
// `::operator new(sizeof(J3DModel))` + handle, vtable UNSET), then the inlined/
// bridged ctor initializes it. Here the ctor is bridged: placement-new the real
// host object onto that buffer so the host C++ vtable + initialize() +
// entryModelData() run. `self` is the raw host buffer (handle already resolved
// by the runtime override); `md` is the host J3DModelData.
//
// Construction touches the joint transform math via calc only, but bring the
// engine up here too so a construct-before-any-load run is sound. Returns `self`
// (CodeWarrior ctors return `this` in r3; the override keeps the handle).
void* sbport_j3dmodel_ctor(void* self, void* md, uint32_t model_flag,
                           uint32_t mtx_buffer_flag) {
	if (!self)
		return self;
	sb_j3d_bringup();
	new (self) J3DModel(static_cast<J3DModelData*>(md), model_flag, mtx_buffer_flag);
	return self;
}

// J3DModel::entryModelData @0x802ddf90 — used when the game default-constructs
// then entries the model data separately. Idempotent w.r.t. the ctor path
// (which already entries) only if called on a fresh model; mirrors the guest.
void sbport_j3dmodel_entryModelData(void* self, void* md, uint32_t model_flag,
                                    uint32_t mtx_buffer_flag) {
	if (!self)
		return;
	sb_j3d_bringup();
	static_cast<J3DModel*>(self)->entryModelData(
	    static_cast<J3DModelData*>(md), model_flag, mtx_buffer_flag);
}

// J3DModel::calc @0x802debc4 — the GX-free per-frame matrix calc (joint/weight/
// normal matrices). `self` is the host J3DModel (handle resolved by override).
void sbport_j3dmodel_calc(void* self) {
	if (self)
		static_cast<J3DModel*>(self)->calc();
}

// J3DModel::viewCalc @0x802deeb8 — view-space draw/normal matrices (also GX-free;
// the GX submission is in entry/draw, not here). Needs j3dSys view matrix set by
// the caller's render setup; bridged for the calc cluster completeness.
void sbport_j3dmodel_viewCalc(void* self) {
	if (self)
		static_cast<J3DModel*>(self)->viewCalc();
}

// Oracle-verification reader: copy node matrix [idx] (the calc output) out to a
// caller-provided 12-float (3x4) buffer. Returns 0 on success, 1 on bad args.
int sbport_j3dmodel_getNodeMtx(void* self, uint32_t idx, float out12[12]) {
	if (!self || !out12)
		return 1;
	J3DModel* m = static_cast<J3DModel*>(self);
	if (idx >= m->getModelData()->getJointNum())
		return 1;
	MtxPtr nm = m->getAnmMtx((int)idx);
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 4; ++c)
			out12[r * 4 + c] = nm[r][c];
	return 0;
}

}  // extern "C"
