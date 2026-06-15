// End-to-end STEP 1 bridge integration test: exercise the J3DModel construct +
// calc bridge through the REAL runtime engine-handle table, exactly as the
// recompiled-game override path does — minus the recompiler dispatch itself.
//
// The override path (runtime/overrides/j3d_model_bridge.cpp) is:
//   load:   sb_eng_handle(sbport_j3d_load(bmd))                 -> J3DModelData handle
//   sbnew:  sb_eng_handle(::operator new(sizeof(J3DModel)))     -> J3DModel handle (raw)
//   ctor:   sbport_j3dmodel_ctor(sb_eng_host(hModel), sb_eng_host(hData), f, m)
//   calc:   sbport_j3dmodel_calc(sb_eng_host(hModel))
//   read:   sbport_j3dmodel_getNodeMtx(sb_eng_host(hModel), i, out)
//
// This test runs that exact sequence (real sb_eng_handle/sb_eng_host round-trips,
// placement-new onto a raw operator-new buffer, calc, handle-resolved matrix read)
// and asserts the resulting node matrices are bit-identical to a DIRECT
// `new J3DModel(md,0,1); calc()` on the same model. Identical matrices prove the
// handle marshalling + placement-new-on-raw-buffer (host vtable correctly set, no
// corruption) + calc are all faithful — the runtime half of the J3D flip is sound
// for the CONSTRUCTION + DIRECT-call path. (J3DModel::calc is dispatched virtually
// by the game; that path is gated on the offset-0 host-vtable routing enhancement
// and is NOT exercised here.)
//
// SKIPs (exit 0) if no *.bmd in $SUNBRIGHT_BMD_DIR (default scratch/bmd).
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/JMath.hpp>
#include "bmd_swap.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <dirent.h>

using namespace smsport::assets;

extern "C" void sb_heap_bringup();

// Port bridge C-ABI seam (port/bridge/j3d_bridge.cpp).
extern "C" {
void* sbport_j3d_load(const void* be_bmd, uint32_t flags);
void* sbport_j3dmodel_ctor(void* self, void* md, uint32_t f, uint32_t m);
void  sbport_j3dmodel_calc(void* self);
int   sbport_j3dmodel_getNodeMtx(void* self, uint32_t idx, float out12[12]);
}

// Runtime engine-handle table (runtime/eng_handle.cpp). Declared by hand so this
// TU never includes eng_handle.h -> cpu_state.h (whose u64 typedef collides with
// the decomp headers this TU needs). The mangled symbol names are independent of
// the u32 return typedef, so `unsigned` (== uint32_t) links to the real symbols.
unsigned sb_eng_handle(void* host);
void*    sb_eng_host(unsigned handle);

static int g_fail = 0, g_ok = 0;

static void run_one(const std::string& path) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return;
	std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> be(n);
	if (std::fread(be.data(), 1, n, f) != (size_t)n) { std::fclose(f); return; }
	std::fclose(f);
	const char* name = path.c_str();

	// ── DIRECT reference path: load + construct + calc natively ──────────────
	std::vector<uint8_t> host;
	BmdSwapResult r = bmd_swap_to_host(be.data(), n, host);
	if (!r.ok || !r.all_covered) { std::printf("[j3d_bridge_run] FAIL %s swap\n", name); g_fail = 1; return; }
	J3DModelData* mdDirect = J3DModelLoaderDataBase::load(host.data(), 0);
	if (!mdDirect) { std::printf("[j3d_bridge_run] FAIL %s direct load\n", name); g_fail = 1; return; }
	J3DModel* mDirect = new J3DModel(mdDirect, 0, 1);
	mDirect->calc();
	u16 jn = mdDirect->getJointNum();

	// ── BRIDGE/HANDLE path: mirror the override sequence exactly ─────────────
	void*    mdHost  = sbport_j3d_load(be.data(), 0);   // bridge swaps internally
	if (!mdHost) { std::printf("[j3d_bridge_run] FAIL %s bridge load\n", name); g_fail = 1; return; }
	unsigned hData   = sb_eng_handle(mdHost);
	void*    buf     = ::operator new(sizeof(J3DModel)); // == generated sbnew_J3DModel()
	unsigned hModel  = sb_eng_handle(buf);
	sbport_j3dmodel_ctor(sb_eng_host(hModel), sb_eng_host(hData), 0, 1);
	sbport_j3dmodel_calc(sb_eng_host(hModel));

	// Handle round-trips must be lossless.
	if (sb_eng_host(hModel) != buf || sb_eng_host(hData) != mdHost) {
		std::printf("[j3d_bridge_run] FAIL %s handle round-trip\n", name);
		g_fail = 1; return;
	}

	// Compare every node matrix between the two paths (same model data content,
	// so the calc output must be bit-identical).
	int mism = 0;
	for (u16 i = 0; i < jn; ++i) {
		float bridgeMtx[12];
		if (sbport_j3dmodel_getNodeMtx(sb_eng_host(hModel), i, bridgeMtx)) { ++mism; continue; }
		MtxPtr dm = mDirect->getAnmMtx((int)i);
		for (int rr = 0; rr < 3; ++rr)
			for (int cc = 0; cc < 4; ++cc)
				if (std::memcmp(&dm[rr][cc], &bridgeMtx[rr * 4 + cc], sizeof(float)) != 0)
					++mism;
	}
	std::printf("[j3d_bridge_run] %s joints=%u mtx_mismatch=%d %s\n",
	            name, jn, mism, mism ? "*** MISMATCH" : "ok");
	if (mism) g_fail = 1; else g_ok++;
}

int main() {
	std::setbuf(stdout, nullptr);
	sb_heap_bringup();
	JMANewSinTable(0xC);   // engine global init (calc joint math); bridge also does this

	const char* env = std::getenv("SUNBRIGHT_BMD_DIR");
	std::string dir = env ? env : "scratch/bmd";
	DIR* d = opendir(dir.c_str());
	if (!d) { std::printf("[j3d_bridge_run] SKIP: no BMD dir '%s'\n", dir.c_str()); return 0; }
	struct dirent* e;
	while ((e = readdir(d)) != nullptr) {
		std::string nm = e->d_name;
		if (nm.size() > 4 && nm.substr(nm.size() - 4) == ".bmd")
			run_one(dir + "/" + nm);
	}
	closedir(d);

	if (g_ok == 0) std::printf("[j3d_bridge_run] SKIP: no *.bmd in '%s'\n", dir.c_str());
	else std::printf("[j3d_bridge_run] %d model(s) bridge==direct%s\n", g_ok, g_fail ? " (FAILURES)" : "");
	return g_fail;
}
