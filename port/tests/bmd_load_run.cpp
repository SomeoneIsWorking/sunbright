// Runtime test: the REAL port J3D model loader (J3DModelLoaderDataBase::load)
// consuming a big-endian BMD made host-readable by the bmd_swap layer, then
// building a J3DModelData natively. This is the first-flip loader gate
// (docs/re_notes/first_flip_endianness.md) — it exercises the whole chain:
//   BE asset bytes -> bmd_swap_to_host (8 blocks) -> the pristine decomp loader
//   reading host-endian values through the LP64-corrected (port/compat shadow)
//   block structs -> J3DJoint/J3DShape/J3DMaterial construction -> makeHierarchy
//   -> makeVcdVatCmd (GD display-list build via the faithful GDInitGDLObj).
//
// Real BMDs are copyrighted and NOT committed. The test loads every *.bmd found
// in $SUNBRIGHT_BMD_DIR (default scratch/bmd) and asserts a non-null model with
// sane counts; if the directory is absent/empty it SKIPS (exit 0) — same pattern
// as the recompiler's coverage_real_test. Drop BMDs in scratch/bmd to activate
// it locally (e.g. via `sunbright-jingle --extract /data/common.szs scratch/bmd`).
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include "bmd_swap.h"

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
	BmdSwapResult r = bmd_swap_to_host(be.data(), n, host);
	const char* name = path.c_str();
	if (!r.ok || !r.all_covered) {
		std::printf("[bmd_load_run] FAIL: %s swap incomplete (ok=%d covered=%u/%u)\n",
		            name, r.ok, r.blocks_covered, r.block_num);
		g_fail = 1; return;
	}
	J3DModelData* md = J3DModelLoaderDataBase::load(host.data(), 0);
	if (!md) { std::printf("[bmd_load_run] FAIL: %s load() returned NULL\n", name); g_fail = 1; return; }
	u16 jn = md->getJointNum(), sn = md->getShapeNum(), mn = md->getMaterialNum();
	bool sane = jn > 0 && jn < 1024 && sn > 0 && sn < 4096 && mn > 0 && mn < 4096;
	std::printf("[bmd_load_run] %s joints=%u shapes=%u materials=%u %s\n",
	            name, jn, sn, mn, sane ? "ok" : "*** SUSPICIOUS COUNTS");
	if (!sane) g_fail = 1; else g_loaded++;
}

int main() {
	std::setbuf(stdout, nullptr);
	sb_heap_bringup();

	const char* env = std::getenv("SUNBRIGHT_BMD_DIR");
	std::string dir = env ? env : "scratch/bmd";
	DIR* d = opendir(dir.c_str());
	if (!d) {
		std::printf("[bmd_load_run] SKIP: no BMD dir '%s' (set SUNBRIGHT_BMD_DIR or drop *.bmd there)\n",
		            dir.c_str());
		return 0;
	}
	struct dirent* e;
	while ((e = readdir(d)) != nullptr) {
		std::string nm = e->d_name;
		if (nm.size() > 4 && nm.substr(nm.size() - 4) == ".bmd")
			load_one(dir + "/" + nm);
	}
	closedir(d);

	if (g_loaded == 0)
		std::printf("[bmd_load_run] SKIP: no *.bmd files in '%s'\n", dir.c_str());
	else
		std::printf("[bmd_load_run] loaded %d model(s)%s\n", g_loaded, g_fail ? " (WITH FAILURES)" : "");
	return g_fail;
}
