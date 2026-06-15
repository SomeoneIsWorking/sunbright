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
#include "bmd_swap.h"

#include <cstdint>
#include <vector>

extern "C" void sb_heap_bringup();   // port/pal/heap/heap_init.cpp (idempotent)

using namespace smsport::assets;

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

	// The port loader allocates its aligned tables from the current JKRHeap
	// (the `new (align) T[]` placement form). Ensure a host-backed heap is up.
	sb_heap_bringup();

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

}  // extern "C"
