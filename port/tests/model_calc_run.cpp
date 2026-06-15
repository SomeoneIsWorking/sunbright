// Runtime test: construct a PC-native J3DModel from a loaded J3DModelData and run
// the GX-FREE per-frame calc path natively (no Dolphin, no recompiler). This is
// the port-side foundation for the STEP 1 J3DModel bridge
// (docs/re_notes/j3d_subsystem_ownership_plan.md): before wiring the recomp→bridge
// dispatch, prove the port engine can build a J3DModel and compute its joint /
// node matrices entirely on the host.
//
// Chain: BE BMD bytes -> bmd_swap_to_host -> J3DModelLoaderDataBase::load ->
//   J3DModel(modelData, 0, 1) ctor (initialize + entryModelData allocations) ->
//   J3DModel::calc() (j3dSys/vertexBuffer frameInit + mtxCalc recursiveCalc) ->
//   dump mNodeMatrices.
//
// calc() is matrix math only (the decomp J3DModel::calc touches no GX); the few
// GX symbols pulled by the construction closure resolve to port/pal/gx/gx_stub.cpp
// no-ops. A non-crashing run with finite node matrices proves the slice is sound.
//
// Real BMDs are copyrighted and NOT committed: loads every *.bmd in
// $SUNBRIGHT_BMD_DIR (default scratch/bmd), SKIPs (exit 0) if none — same pattern
// as bmd_load_run.
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/JMath.hpp>
#include "bmd_swap.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <dirent.h>

extern "C" void sb_heap_bringup();
using namespace smsport::assets;

static int g_fail = 0, g_calced = 0;

static bool finite_mtx(MtxPtr m) {
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 4; ++c)
			if (!std::isfinite(m[r][c])) return false;
	return true;
}

static void calc_one(const std::string& path) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return;
	std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> be(n);
	if (std::fread(be.data(), 1, n, f) != (size_t)n) { std::fclose(f); return; }
	std::fclose(f);
	const char* name = path.c_str();

	std::vector<uint8_t> host;
	BmdSwapResult r = bmd_swap_to_host(be.data(), n, host);
	if (!r.ok || !r.all_covered) {
		std::printf("[model_calc_run] FAIL: %s swap incomplete\n", name);
		g_fail = 1; return;
	}
	J3DModelData* md = J3DModelLoaderDataBase::load(host.data(), 0);
	if (!md) { std::printf("[model_calc_run] FAIL: %s load NULL\n", name); g_fail = 1; return; }

	// Construct the model. flags=0, mtx_buffer_flag=1 (allocate one draw-mtx slot
	// per buffer — the common single-view case).
	J3DModel* model = new J3DModel(md, 0, 1);
	if (!model || model->getModelData() != md) {
		std::printf("[model_calc_run] FAIL: %s ctor/entryModelData bad\n", name);
		g_fail = 1; return;
	}

	model->calc();

	// Validate every node matrix is finite (the joint hierarchy was walked).
	u16 jn = md->getJointNum();
	int bad = 0;
	for (u16 i = 0; i < jn; ++i) {
		MtxPtr nm = model->getAnmMtx(i);   // mNodeMatrices[i]
		if (!nm || !finite_mtx(nm)) ++bad;
	}
	std::printf("[model_calc_run] %s joints=%u nodeMtx_bad=%d %s\n",
	            name, jn, bad, bad ? "*** NON-FINITE" : "ok");
	if (bad) g_fail = 1; else g_calced++;
}

int main() {
	std::setbuf(stdout, nullptr);
	sb_heap_bringup();
	// Engine global init the game does at boot (Application.cpp:390). The joint
	// transform math (J3DGetTranslateRotateMtx -> JMASSin/Cos) reads jmaSinTable.
	JMANewSinTable(0xC);

	const char* env = std::getenv("SUNBRIGHT_BMD_DIR");
	std::string dir = env ? env : "scratch/bmd";
	DIR* d = opendir(dir.c_str());
	if (!d) {
		std::printf("[model_calc_run] SKIP: no BMD dir '%s'\n", dir.c_str());
		return 0;
	}
	struct dirent* e;
	while ((e = readdir(d)) != nullptr) {
		std::string nm = e->d_name;
		if (nm.size() > 4 && nm.substr(nm.size() - 4) == ".bmd")
			calc_one(dir + "/" + nm);
	}
	closedir(d);

	if (g_calced == 0)
		std::printf("[model_calc_run] SKIP: no *.bmd in '%s'\n", dir.c_str());
	else
		std::printf("[model_calc_run] calc'd %d model(s)%s\n", g_calced, g_fail ? " (FAILURES)" : "");
	return g_fail;
}
