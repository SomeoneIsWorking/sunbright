// Unit tests for type_db_build — composing a full engine-type layout across the single-
// inheritance base chain, resolved automatically from the decomp headers.
#include "../type_db_build.h"
#include "../decomp_parse.h"

#include <cstdio>
#include <set>
#include <string>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, msg) do { ++g_checks; if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s\n", (msg)); ++g_fail; } } while (0)

static const char* INC = "reference/sms/include";

static const FieldDesc* at(const EngineLayout& L, int off) {
    auto it = L.fields.find(off); return it == L.fields.end() ? nullptr : &it->second;
}

int main() {
    auto index = index_headers(INC);
    std::printf("[type_db_build] indexed %zu type definitions\n", index.size());
    CHECK(index.size() > 500, "header index found many type definitions");
    CHECK(index.count("TNameRef") && index.count("TViewObj") && index.count("TViewport"),
          "index located the JDrama view hierarchy");

    // TViewObj : TNameRef — composed layout carries the base's mName@4, mKeyCode@8 AND own unkC@C.
    {
        std::vector<std::string> unresolved;
        EngineLayout L = compose_layout("TViewObj", index, {}, &unresolved);
        const FieldDesc* a = at(L, 0x4);
        const FieldDesc* b = at(L, 0x8);
        const FieldDesc* c = at(L, 0xC);
        CHECK(a && a->member == "mName",   "TViewObj: inherited TNameRef::mName at 0x4");
        CHECK(b && b->member == "mKeyCode","TViewObj: inherited TNameRef::mKeyCode at 0x8");
        CHECK(c && c->member == "unkC",    "TViewObj: own unkC at 0xC");
    }

    // TViewport : TViewObj : TNameRef — three-level chain composed into one layout. unk10 is an
    // embedded TRect(:JUTRect) so it expands to unk10.x1@0x10 (recursive embedded composition);
    // unk20 is a scalar.
    {
        EngineLayout L = compose_layout("TViewport", index, {}, nullptr);
        CHECK(at(L, 0x4)  && at(L, 0x4)->member  == "mName",     "TViewport: base mName@4");
        CHECK(at(L, 0xC)  && at(L, 0xC)->member  == "unkC",      "TViewport: TViewObj unkC@C");
        CHECK(at(L, 0x10) && at(L, 0x10)->member == "unk10.x1",  "TViewport: embedded TRect unk10.x1@0x10");
        CHECK(at(L, 0x20) && at(L, 0x20)->member == "unk20",     "TViewport: own scalar unk20@0x20");
    }

    // build_type_db end-to-end for a no-base type: layout + signatures both populated.
    {
        auto r = build_type_db({ "TCameraMarioData" }, INC, "reference/sms_gmse01_funcs.txt");
        CHECK(r.missing_types.empty(), "build_type_db: TCameraMarioData resolved");
        CHECK(r.db.layouts.count("TCameraMarioData"), "build_type_db: layout present");
        const auto& L = r.db.layouts["TCameraMarioData"];
        CHECK(at(L, 0x10) && at(L, 0x10)->member == "unk10", "build_type_db: unk10@0x10 in layout");
        // embedded value type: unk0 is JGeometry::TVec3<f32> -> expanded to unk0.x/y/z at 0/4/8
        CHECK(at(L, 0x0) && at(L, 0x0)->member == "unk0.x", "embedded TVec3: unk0.x at 0x0");
        CHECK(at(L, 0x4) && at(L, 0x4)->member == "unk0.y", "embedded TVec3: unk0.y at 0x4");
        CHECK(at(L, 0x8) && at(L, 0x8)->member == "unk0.z", "embedded TVec3: unk0.z at 0x8");
        // signatures: the isMario* / calcAndSetMarioData methods seed this=TCameraMarioData in r3
        bool seeded = false;
        for (const auto& [addr, m] : r.db.signatures) {
            auto it = m.find(3);
            if (it != m.end() && it->second == "TCameraMarioData") { seeded = true; break; }
        }
        CHECK(seeded, "build_type_db: at least one method seeds this=TCameraMarioData in r3");
    }

    std::printf("type_db_build_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
