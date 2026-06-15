// Runtime gate: the REAL port loader (J3DModelLoaderDataBase::loadMaterialTable)
// consuming a big-endian .bmt made host-readable by the bmt_swap layer, building a
// J3DMaterialTable natively. Mirrors bmd_load_run for the standalone material-table
// load path (the .bmt loader that managers/actors call — distinct from the .bmd
// model load). Exercises: BE bytes -> bmt_swap_to_host (MAT3+TEX1) -> the pristine
// decomp loadMaterialTable reading host-endian values through the LP64-corrected
// block structs -> J3DMaterialFactory material construction.
//
// Real .bmt files are copyrighted and NOT committed. Loads every *.bmt found in
// $SUNBRIGHT_BMT_DIR (default scratch/bmt) and asserts a non-null table with sane
// counts; if the directory is absent/empty it SKIPS (exit 0). Drop .bmt files in
// scratch/bmt to activate locally (scan a scene archive with scratch/bmt/scan_bmt).
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DMaterialAttach.hpp>
#include "bmt_swap.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <dirent.h>

extern "C" void sb_heap_bringup();
using namespace smsport::assets;

static int g_fail = 0, g_loaded = 0;

static void load_one(const std::string& path) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return;
	std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> be(n);
	if (std::fread(be.data(), 1, n, f) != (size_t)n) { std::fclose(f); return; }
	std::fclose(f);

	std::vector<uint8_t> host;
	BmtSwapResult r = bmt_swap_to_host(be.data(), n, host);
	const char* name = path.c_str();
	if (!r.ok || !r.all_covered) {
		std::printf("[bmt_load_run] FAIL: %s swap incomplete (ok=%d covered=%u/%u err=%s)\n",
		            name, r.ok, r.blocks_covered, r.block_num, r.error ? r.error : "-");
		g_fail = 1; return;
	}
	J3DMaterialTable* mt = J3DModelLoaderDataBase::loadMaterialTable(host.data());
	if (!mt) { std::printf("[bmt_load_run] FAIL: %s loadMaterialTable() returned NULL\n", name); g_fail = 1; return; }
	u16 mn = mt->getMaterialNum();
	bool sane = mn > 0 && mn < 4096 && mt->mMaterials != nullptr;
	std::printf("[bmt_load_run] %s materials=%u texture=%p %s\n",
	            name, mn, (void*)mt->getTexture(), sane ? "ok" : "*** SUSPICIOUS");
	if (!sane) g_fail = 1; else g_loaded++;
}

int main() {
	std::setbuf(stdout, nullptr);
	sb_heap_bringup();

	const char* env = std::getenv("SUNBRIGHT_BMT_DIR");
	std::string dir = env ? env : "scratch/bmt";
	DIR* d = opendir(dir.c_str());
	if (!d) {
		std::printf("[bmt_load_run] SKIP: no BMT dir '%s' (set SUNBRIGHT_BMT_DIR or drop *.bmt there)\n",
		            dir.c_str());
		return 0;
	}
	struct dirent* e;
	while ((e = readdir(d)) != nullptr) {
		std::string nm = e->d_name;
		if (nm.size() > 4 && nm.substr(nm.size() - 4) == ".bmt")
			load_one(dir + "/" + nm);
	}
	closedir(d);

	if (g_loaded == 0)
		std::printf("[bmt_load_run] SKIP: no *.bmt files in '%s'\n", dir.c_str());
	else
		std::printf("[bmt_load_run] loaded %d table(s)%s\n", g_loaded, g_fail ? " (WITH FAILURES)" : "");
	return g_fail;
}
