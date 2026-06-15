// Runtime gate: the REAL port loader (J3DAnmLoaderDataBase::load) consuming a
// big-endian animation file (.bck/.btk/.brk/.bpk/.btp/...) made host-readable by
// the anm_swap layer, building a J3DAnmBase subclass natively. Mirrors
// bmt_load_run for the animation load path (the loader TShimmer::load and most
// actors call). Exercises: BE bytes -> anm_swap_to_host (whole J3D1 block family)
// -> the pristine decomp J3DAnmLoaderDataBase::load reading host-endian values ->
// a J3DAnmBase with a sane frame-max.
//
// Real animation files are copyrighted and NOT committed. Loads every supported
// animation file found in $SUNBRIGHT_ANM_DIR (default scratch/bmt/anm) and asserts
// a non-null animator with a sane getFrameMax(); if the directory is absent/empty
// it SKIPS (exit 0). Populate it with scratch/bmt/scan_anm <scene.szs> --write.
#include <JSystem/J3D/J3DGraphLoader/J3DAnmLoader.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DAnimation.hpp>
#include "anm_swap.h"

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

// The animation extensions J3DAnmLoaderDataBase::load dispatches (J3D1 family).
static bool is_anm(const std::string& nm) {
	static const char* ext[] = {".bck",".btk",".brk",".bpk",".blk",".bxk",
	                            ".bca",".bpa",".btp",".bla",".bva",".bxa"};
	if (nm.size() < 4) return false;
	std::string e = nm.substr(nm.size() - 4);
	for (const char* x : ext) if (e == x) return true;
	return false;
}

static void load_one(const std::string& path) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return;
	std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> be(n);
	if (std::fread(be.data(), 1, n, f) != (size_t)n) { std::fclose(f); return; }
	std::fclose(f);

	std::vector<uint8_t> host;
	AnmSwapResult r = anm_swap_to_host(be.data(), n, host);
	const char* name = path.c_str();
	if (!r.ok || !r.all_covered) {
		std::printf("[anm_load_run] FAIL: %s swap incomplete (ok=%d covered=%u/%u type=%08x err=%s)\n",
		            name, r.ok, r.blocks_covered, r.block_num, r.file_type,
		            r.error ? r.error : "-");
		g_fail = 1; return;
	}
	J3DAnmBase* anm = J3DAnmLoaderDataBase::load(host.data());
	if (!anm) { std::printf("[anm_load_run] FAIL: %s load() returned NULL\n", name); g_fail = 1; return; }
	s16 fmax = anm->getFrameMax();
	// getFrameMax is s16; a wrong byteswap of a small value lands in the negative
	// half. Texture-scroll (.btk) loops are legitimately long (tens of thousands
	// of frames), so the only sane bound is non-negative.
	bool sane = fmax >= 0;
	std::printf("[anm_load_run] %s type=%08x frameMax=%d %s\n",
	            name, r.file_type, fmax, sane ? "ok" : "*** SUSPICIOUS");
	if (!sane) g_fail = 1; else g_loaded++;
}

int main() {
	std::setbuf(stdout, nullptr);
	sb_heap_bringup();

	const char* env = std::getenv("SUNBRIGHT_ANM_DIR");
	std::string dir = env ? env : "scratch/bmt/anm";
	DIR* d = opendir(dir.c_str());
	if (!d) {
		std::printf("[anm_load_run] SKIP: no anm dir '%s' (set SUNBRIGHT_ANM_DIR or scan_anm --write)\n",
		            dir.c_str());
		return 0;
	}
	struct dirent* e;
	while ((e = readdir(d)) != nullptr) {
		std::string nm = e->d_name;
		if (is_anm(nm)) load_one(dir + "/" + nm);
	}
	closedir(d);

	if (g_loaded == 0)
		std::printf("[anm_load_run] SKIP: no animation files in '%s'\n", dir.c_str());
	else
		std::printf("[anm_load_run] loaded %d animator(s)%s\n", g_loaded, g_fail ? " (WITH FAILURES)" : "");
	return g_fail;
}
