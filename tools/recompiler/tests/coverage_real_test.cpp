// END-TO-END COMPLETE-COVERAGE proof for the tailored-recomp field-access boundary, on REAL
// game code and a REAL engine object (the de-risk #2 gate, ARCHITECTURE_TARGET / handoff step 1).
//
// For a real recompiled accessor it runs the WHOLE auto pipeline the production recompiler would:
//   decomp header --decomp_parse--> engine layout
//   symbol        --func_sig-----> this/param type seeds
//   DOL bytes     --decode/collect-> instructions
//   --recover_eng_fields (CFG dataflow)--> host field bindings + the dangerous-miss list
// and asserts COMPLETENESS: there is NO load/store whose base register the pass types as the
// engine object but whose offset isn't a known field (a "typed base, unmapped offset" miss — the
// SHARP EDGE: that would emit a guest MEM access against a host handle = corruption). The decomp
// SOURCE is the oracle for WHICH this-fields the function touches (encoded per case below).
//
// Needs the extracted DOL (machine-local, gitignored). If absent it SKIPS cleanly (exit 0), so a
// checkout without a ROM still passes ctest; run it locally where scratch/bin/sms.dol exists.
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
#include <sstream>
#include <string>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, msg) do { ++g_checks; if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s\n", (msg)); ++g_fail; } } while (0)

// funcs.txt: sorted "<hexaddr> <mangled>" lines.
struct Sym { u32 addr; std::string name; };

int main() {
    const char* dol_env = std::getenv("SUNBRIGHT_DOL");
    std::string dol_path = dol_env ? dol_env : "scratch/bin/sms.dol";
    std::ifstream df(dol_path, std::ios::binary);
    if (!df) {
        std::printf("coverage_real_test: SKIP (no DOL at %s; set SUNBRIGHT_DOL or extract it)\n",
                    dol_path.c_str());
        return 0;
    }
    std::vector<u8> bytes((std::istreambuf_iterator<char>(df)), std::istreambuf_iterator<char>());
    DOL dol = DOL::from_bytes(bytes);

    // Load the symbol table.
    std::vector<Sym> syms;
    { std::ifstream f("reference/sms_gmse01_funcs.txt");
      std::string a, n;
      while (f >> a >> n) { try { syms.push_back({ (u32)std::stoul(a, nullptr, 16), n }); } catch (...) {} } }
    std::sort(syms.begin(), syms.end(), [](const Sym& x, const Sym& y){ return x.addr < y.addr; });
    auto find_sym = [&](const std::string& name) -> const Sym* {
        for (auto& s : syms) if (s.name == name) return &s; return nullptr;
    };
    auto fend_of = [&](u32 addr) -> u32 {
        for (auto& s : syms) if (s.addr > addr) return s.addr; return addr + 0x400;
    };

    // One real accessor to prove. `expect` = the this-field members it touches, per the decomp
    // source (the oracle). Add more cases as the engine grows.
    struct Case { const char* mangled; const char* type; std::set<std::string> expect; };
    std::vector<Case> cases = {
        // TCameraMarioData::isMarioGoDown() const — source reads only `unk10` off `this`
        // (the other derefs are the globals gpMarioPos / gpMarioOriginal, not engine-typed).
        { "isMarioGoDown__16TCameraMarioDataCFv", "TCameraMarioData", { "unk10" } },
        // TCameraMarioData::calcAndSetMarioData() — branchy (switch + if/else); the source
        // reads/writes unkC, unk10, unk14, unk18, unk1C off `this` (unk0/the TVec3 is a local).
        { "calcAndSetMarioData__16TCameraMarioDataFv", "TCameraMarioData",
          { "unkC", "unk10", "unk14", "unk18", "unk1C" } },
    };

    // Build the DB for every type the cases touch ONCE, via the real production builder
    // (auto-resolves headers + composes inheritance + builds signatures from the symbol file).
    std::set<std::string> active;
    for (const Case& c : cases) active.insert(c.type);
    auto built = build_type_db(active, "reference/sms/include", "reference/sms_gmse01_funcs.txt");
    CHECK(built.missing_types.empty(), "build_type_db resolved every case type");
    const TypeDB& db = built.db;

    int total_typed_sites = 0;

    for (const Case& c : cases) {
        const Sym* s = find_sym(c.mangled);
        CHECK(s != nullptr, c.mangled);
        if (!s) continue;
        u32 addr = s->addr, fend = fend_of(addr);

        const DOLSection* sec = dol.section_at(addr);
        CHECK(sec != nullptr, "function address is in a DOL section");
        if (!sec) continue;
        std::vector<PPCInstr> instrs =
            collect_function(sec->data.data(), sec->addr, sec->data.size(), addr, fend, /*cfg=*/true);
        CHECK(!instrs.empty(), "decoded a non-empty function body");

        // The builder must have seeded this=type in r3 for this method.
        auto sit = db.signatures.find(addr);
        CHECK(sit != db.signatures.end() && sit->second.count(3) && sit->second.at(3) == c.type,
              "build_type_db seeded `this` = engine type in r3 for this accessor");

        // Resolve any computed-bctr jump table (reads table entries from the DOL) so the CFG is
        // complete even for switch-heavy functions.
        auto read_word = [&](u32 a, u32& outw) -> bool {
            if (!dol.section_at(a)) return false; outw = dol.read_u32(a); return true; };
        auto jt = jumptable_targets(instrs, addr, fend, read_word);

        // Recover, capturing the dangerous-miss list.
        std::vector<u32> gaps;
        auto ef = recover_eng_fields(instrs, addr, db, intra_branch_targets(instrs, addr), jt, &gaps);

        // Report: disassembly with per-site annotation.
        std::printf("\n== %s  [%s]  (%08x..%08x, %zu instrs) ==\n",
                    c.mangled, c.type, addr, fend, instrs.size());
        std::set<std::string> got_members;
        for (const auto& i : instrs) {
            std::string ann;
            auto it = ef.find(i.pc);
            if (it != ef.end()) { ann = "  -> " + it->second.type_cname + "::" + it->second.member; got_members.insert(it->second.member); }
            for (u32 g : gaps) if (g == i.pc) ann = "  -> *** UNMAPPED ENGINE FIELD (miss) ***";
            std::printf("  %08x  %-28s%s\n", i.pc, i.mnemonic().c_str(), ann.c_str());
        }

        // Assertions: complete (no dangerous misses) + matches the source oracle.
        total_typed_sites += (int)ef.size();
        CHECK(gaps.empty(), "COMPLETE: no typed-base/unmapped-offset misses");
        CHECK(got_members == c.expect, "recovered this-fields match the decomp source oracle");
        if (got_members != c.expect) {
            std::printf("  expected:"); for (auto& m : c.expect) std::printf(" %s", m.c_str());
            std::printf("\n  got:     "); for (auto& m : got_members) std::printf(" %s", m.c_str());
            std::printf("\n");
        }
    }

    CHECK(total_typed_sites > 0, "the suite recovered engine-field sites on real code");
    std::printf("\ncoverage_real_test: %d checks, %d failures (%d typed sites on real code)\n",
                g_checks, g_fail, total_typed_sites);
    return g_fail ? 1 : 0;
}
