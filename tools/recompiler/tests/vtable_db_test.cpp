// MAKE-OR-BREAK gate for the offset-0 virtual-dispatch-on-handle routing (handoff step 1):
// the vtable-slot DB read DIRECTLY from the real guest vtable bytes must match the decomp
// header's declared virtual order, so recognition can map a guest vtable byte-offset to the
// right host method. Anchored on J3DModel (the first verifiable J3D calc slice).
//
// Ground truth (verified by hand-disassembling the ctor + dumping the DOL vtable, see
// scratch/handoff.md): J3DModel's ctor stores vptr 0x803e115c (CodeWarrior 8-byte header of two
// zero words), and the method slots are
//   +0x08 update   +0x0C entry   +0x10 calc   +0x14 viewCalc   +0x18 ~J3DModel
//
// Needs the extracted DOL (machine-local, gitignored). SKIPs cleanly (exit 0) if absent.
#include "../dol_parser.h"
#include "../vtable_db.h"
#include "../decomp_parse.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, msg) do { ++g_checks; if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s\n", (msg)); ++g_fail; } } while (0)

int main() {
    const char* dol_env = std::getenv("SUNBRIGHT_DOL");
    std::string dol_path = dol_env ? dol_env : "scratch/bin/sms.dol";
    std::ifstream df(dol_path, std::ios::binary);
    if (!df) {
        std::printf("vtable_db_test: SKIP (no DOL at %s; set SUNBRIGHT_DOL or extract it)\n",
                    dol_path.c_str());
        return 0;
    }
    std::vector<u8> bytes((std::istreambuf_iterator<char>(df)), std::istreambuf_iterator<char>());
    DOL dol = DOL::from_bytes(bytes);

    VTableDB db = build_vtable_db({"J3DModel"}, dol, "reference/sms_gmse01_funcs.txt");

    auto it = db.tables.find("J3DModel");
    CHECK(it != db.tables.end(), "J3DModel vtable located");
    if (it == db.tables.end()) { std::fprintf(stderr, "%d/%d (DB miss)\n", g_checks - g_fail, g_checks); return 1; }
    const VTable& vt = it->second;

    CHECK(vt.vptr == 0x803e115cu, "J3DModel stored vptr == 0x803e115c");
    CHECK(vt.header_size == 8, "CodeWarrior primary-vtable header is 8 bytes (offset-to-top + RTTI)");

    // The slot map read from the DOL, by byte offset.
    struct Exp { int off; int slot; const char* method; u32 target; };
    const Exp exp[] = {
        { 0x08, 0, "update",   0x802de9c0 },
        { 0x0C, 1, "entry",    0x802dedc8 },
        { 0x10, 2, "calc",     0x802debc4 },
        { 0x14, 3, "viewCalc", 0x802deeb8 },
        { 0x18, 4, "~J3DModel",0x802ddea0 },
    };
    for (const auto& e : exp) {
        const VSlot* s = vt.find_offset(e.off);
        CHECK(s != nullptr, "slot present at expected byte offset");
        if (!s) continue;
        CHECK(s->slot_index == e.slot, "slot index matches");
        CHECK(s->method == e.method, "slot method name matches the decomp virtual");
        CHECK(s->target == e.target, "slot target == the known method guest address");
        CHECK(s->defining_class == "J3DModel", "slot defining class == J3DModel");
    }
    // calc, the first verifiable slice, must be reachable by name too.
    const VSlot* calc = vt.find_method("calc");
    CHECK(calc && calc->byte_off == 0x10, "find_method(\"calc\") -> +0x10");

    // Cross-check: the decomp header's declared virtual order == the DOL slot order (J3DModel is a
    // root polymorphic class, so its declared virtuals ARE the whole slot run).
    ParsedType pt = parse_decomp_file(
        "reference/sms/include/JSystem/J3D/J3DGraphAnimator/J3DModel.hpp", "J3DModel");
    CHECK(pt.found, "J3DModel.hpp parsed");
    const char* want[] = {"update", "entry", "calc", "viewCalc", "~J3DModel"};
    CHECK(pt.virtuals.size() == 5, "header declares exactly 5 virtuals");
    for (size_t k = 0; k < pt.virtuals.size() && k < 5; ++k)
        CHECK(pt.virtuals[k] == want[k], "header virtual order matches the vtable slot order");

    // The four `void f()` virtuals are safely host-dispatchable; the destructor is NOT.
    CHECK(pt.simple_virtuals.count("calc") && pt.simple_virtuals.count("update") &&
          pt.simple_virtuals.count("entry") && pt.simple_virtuals.count("viewCalc"),
          "calc/update/entry/viewCalc are zero-arg void -> host-dispatchable");
    CHECK(pt.simple_virtuals.count("~J3DModel") == 0, "destructor is NOT a simple virtual");
    CHECK(pt.simple_virtuals.size() == 4, "exactly four simple virtuals");

    std::printf("vtable_db_test: %d/%d checks passed\n", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
