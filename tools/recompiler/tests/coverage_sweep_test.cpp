// COVERAGE-AT-SCALE sweep for the tailored field-access recovery: for a set of active engine
// types, run recovery over EVERY function in the DOL that the type DB seeds (its methods +
// callers taking the type), and tally typed sites vs the dangerous "typed-base/unmapped-offset"
// gaps. This is the instrument for (a) measuring how complete recovery is across the real game
// and (b) choosing which subsystem to flip first (pick types that sweep CLEAN — 0 gaps).
//
// SUNBRIGHT_SWEEP_TYPES (comma/space list) overrides the default clean set. The default set is
// asserted to sweep with ZERO gaps — a real regression guard that clean scalar types stay fully
// covered across all their accessors in the shipping game. SKIPs without the gitignored DOL.
//
// Findings (2026-06-14): clean scalar types (TCameraKindParam, TCameraMarioData) = 0 gaps;
// complex actor types (TMario ~1025 gaps, TLiveActor ~212) still miss sites (embedded value
// types outside the known table, unions, arrays-of-struct) — so the FIRST subsystem flip should
// use clean types, and broad-actor coverage is the remaining "scale" work.
#include "../dol_parser.h"
#include "../ppc_decoder.h"
#include "../func_collect.h"
#include "../type_db_build.h"
#include "../type_recovery.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, msg) do { ++g_checks; if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s\n", (msg)); ++g_fail; } } while (0)

struct Sym { u32 addr; std::string name; };

int main() {
    std::ifstream df("scratch/bin/sms.dol", std::ios::binary);
    if (!df) { std::printf("coverage_sweep_test: SKIP (no scratch/bin/sms.dol)\n"); return 0; }
    std::vector<u8> bytes((std::istreambuf_iterator<char>(df)), std::istreambuf_iterator<char>());
    DOL dol = DOL::from_bytes(bytes);

    std::vector<Sym> syms;
    { std::ifstream f("reference/sms_gmse01_funcs.txt"); std::string a, n;
      while (f >> a >> n) { try { syms.push_back({ (u32)std::stoul(a, nullptr, 16), n }); } catch (...) {} } }
    std::sort(syms.begin(), syms.end(), [](const Sym& x, const Sym& y){ return x.addr < y.addr; });

    std::set<std::string> active;
    bool default_set = true;
    if (const char* e = getenv("SUNBRIGHT_SWEEP_TYPES")) {
        default_set = false;
        std::string cur;
        for (char ch : std::string(e)) {
            if (ch == ',' || ch == ' ') { if (!cur.empty()) { active.insert(cur); cur.clear(); } }
            else cur += ch;
        }
        if (!cur.empty()) active.insert(cur);
    }
    if (active.empty()) active = { "TCameraMarioData", "TCameraKindParam" };  // clean default

    auto built = build_type_db(active, "reference/sms/include", "reference/sms_gmse01_funcs.txt");
    std::printf("[sweep] %zu active types, %zu layouts, %zu funcs seeded\n",
                active.size(), built.db.layouts.size(), built.db.signatures.size());
    for (const auto& m : built.missing_types) std::printf("  MISSING type: %s\n", m.c_str());

    int funcs = 0, typed_funcs = 0, typed_sites = 0, gap_sites = 0;
    std::map<int, int> gap_off_hist;     // guest offset -> gap count (to see what's missing)
    for (size_t i = 0; i < syms.size(); ++i) {
        u32 addr = syms[i].addr, fend = (i + 1 < syms.size()) ? syms[i+1].addr : addr + 0x400;
        if (!built.db.signatures.count(addr)) continue;
        const DOLSection* sec = dol.section_at(addr);
        if (!sec || !sec->is_text) continue;
        auto instrs = collect_function(sec->data.data(), sec->addr, sec->data.size(), addr, fend, true);
        if (instrs.empty()) continue;
        auto read_word = [&](u32 a, u32& o) -> bool {
            if (!dol.section_at(a)) return false;
            o = dol.read_u32(a);
            return true;
        };
        auto jt = jumptable_targets(instrs, addr, fend, read_word);
        std::vector<u32> gaps;
        auto ef = recover_eng_fields(instrs, addr, built.db, intra_branch_targets(instrs, addr), jt, &gaps);
        ++funcs;
        if (!ef.empty()) ++typed_funcs;
        typed_sites += (int)ef.size();
        gap_sites += (int)gaps.size();
        for (u32 g : gaps) {
            for (const auto& in : instrs) if (in.pc == g) gap_off_hist[(int)in.d]++;
        }
    }
    std::printf("[sweep] %d funcs swept, %d typed>=1, %d typed sites, %d GAP sites\n",
                funcs, typed_funcs, typed_sites, gap_sites);
    if (gap_sites) {
        std::printf("  gap offsets (offset:count):");
        int shown = 0;
        for (const auto& [off, cnt] : gap_off_hist) { std::printf(" 0x%x:%d", off, cnt); if (++shown >= 20) break; }
        std::printf("\n");
    }

    CHECK(typed_sites > 0, "sweep typed engine-field sites on real code");
    if (default_set)
        CHECK(gap_sites == 0, "clean default type set sweeps with ZERO gaps across all accessors");

    std::printf("coverage_sweep_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
