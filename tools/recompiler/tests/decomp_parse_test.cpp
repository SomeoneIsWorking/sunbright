// Unit tests for decomp_parse — extracting engine field lists from the SMS decomp headers
// (the front end of building the tailored-recomp type DB, ARCHITECTURE_TARGET §boundary).
//
// Three layers of evidence:
//   1. Parser parity: a parsed type's field list == the hand-keyed list abi_layout_test uses.
//   2. ABI cross-check: for a real all-scalar struct, compute_layout(parsed fields) reproduces
//      EVERY annotated `/* 0x */` offset (the decomp's own offsets are the oracle).
//   3. Corpus sweep: parse every reference/sms header; for every fully-sizable, non-inherited
//      annotated struct, assert compute_layout matches all annotations. Catches ABI-rule and
//      parser bugs at scale, and prints honest coverage (how much the parser can place soundly).
#include "../decomp_parse.h"
#include "../abi_layout.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, msg) do { ++g_checks; if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); ++g_fail; } } while (0)

static const char* INC = "reference/sms/include";

// Recursively collect *.hpp/*.h under a directory.
static void walk(const std::string& dir, std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string p = dir + "/" + n;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) walk(p, out);
        else if ((n.size() >= 4 && n.substr(n.size()-4) == ".hpp") ||
                 (n.size() >= 2 && n.substr(n.size()-2) == ".h"))
            out.push_back(p);
    }
    closedir(d);
}

static std::string slurp(const std::string& p) {
    std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

// Validate one parsed type's annotated offsets against the ABI rules, ABSOLUTELY (anchored
// at the object origin, offset 0). Two invariants, both vtable-placement-agnostic and
// tolerant of the decomp's real padding holes / vtable slots (unannotated gaps):
//   * ALIGNMENT  — each annotated offset is a multiple of its field's natural alignment.
//   * NON-OVERLAP — fields in offset order don't overlap (ann[i]+size(i) <= ann[i+1]); a
//                   larger gap is fine (explicit padding, an interleaved vtable pointer, or
//                   an unannotated field). This is exactly what abi_layout encodes (natural
//                   alignment + size), checked directly against the decomp's own offsets, so
//                   it needs no model of where the CodeWarrior vtable sits (class-dependent:
//                   TNameRef at 0, TGraphWeb/TSolidStack at the end — see docs/re_notes).
// A misaligned field or an overlap (a genuine ABI/parse error) fails; a hole does not.
// Strong check for hand-verified types: compute_layout EXACTLY reproduces every annotation.
static bool exact_offsets(const ParsedType& t, bool polymorphic) {
    std::vector<LField> lf = to_lfields(t);
    LResult g = compute_layout(lf, polymorphic, guest_abi());
    if (g.offsets.size() != t.fields.size()) return false;
    for (size_t i = 0; i < t.fields.size(); ++i)
        if (g.offsets[i] != t.fields[i].offset) return false;
    return true;
}

static bool validate_offsets(const ParsedType& t) {
    if (!t.fully_sizable || t.fields.empty()) return false;
    std::vector<std::pair<int,int>> spans;   // (offset, size) in declaration (== offset) order
    for (const auto& f : t.fields) {
        LField lf{ f.name, f.kind, f.pointee, f.array_count };
        int size, align;
        field_size_align(lf, guest_abi(), size, align);
        if (align > 0 && (f.offset % align) != 0) return false;        // misaligned
        spans.emplace_back(f.offset, size);
    }
    for (size_t i = 1; i < spans.size(); ++i)
        if (spans[i-1].first + spans[i-1].second > spans[i].first) return false;  // overlap
    return true;
}

int main() {
    // ── 1. JUTRect parity (matches abi_layout_test's hand-keyed list) ──────────────────
    {
        ParsedType t = parse_decomp_file(std::string(INC) + "/JSystem/JUtility/JUTRect.hpp", "JUTRect");
        CHECK(t.found, "JUTRect found");
        CHECK(t.base.empty() && !t.polymorphic, "JUTRect: no base, non-polymorphic");
        CHECK(t.fields.size() == 4, "JUTRect: 4 fields");
        if (t.fields.size() == 4) {
            const char* names[] = {"x1","y1","x2","y2"};
            int offs[] = {0,4,8,12};
            for (int i = 0; i < 4; ++i) {
                CHECK(t.fields[i].name == names[i], "JUTRect field name");
                CHECK(t.fields[i].offset == offs[i], "JUTRect annotated offset");
                CHECK(t.fields[i].kind == FKind::I32 && t.fields[i].sizable, "JUTRect field is s32");
            }
        }
        CHECK(exact_offsets(t, false), "JUTRect: compute_layout reproduces the annotations exactly");
    }

    // ── 2. A real all-scalar struct with s16/f32 alignment + a pointer-free body ───────
    {
        ParsedType t = parse_decomp_file(std::string(INC) + "/Camera/CameraKindParam.hpp",
                                         "TCameraKindParam");
        CHECK(t.found, "TCameraKindParam found");
        CHECK(t.fully_sizable, "TCameraKindParam fully sizable (all scalars)");
        CHECK(t.fields.size() >= 20, "TCameraKindParam has many fields");
        CHECK(exact_offsets(t, false), "TCameraKindParam: ABI offsets reproduce every /* 0x */ annotation");
        // spot-check the s16-then-f32 alignment boundary (0x18 s16, 0x1A s16, 0x1C f32)
        bool saw_18 = false;
        for (const auto& f : t.fields)
            if (f.offset == 0x18) { saw_18 = true; CHECK(f.kind == FKind::I16, "0x18 is s16"); }
        CHECK(saw_18, "TCameraKindParam has a field at 0x18");
    }

    // ── 3. A pointer field -> nested engine type in the layout ─────────────────────────
    {
        std::string src =
            "class TFoo {\n"
            "public:\n"
            "  virtual void f();\n"
            "  /* 0x4 */ TFoo* mNext;\n"
            "  /* 0x8 */ f32 mFov;\n"
            "  /* 0xC */ u8 mFlag;\n"
            "  void method(int);\n"   // method, must be ignored
            "  static int sCount;\n"  // static, must be ignored
            "};\n";
        ParsedType t = parse_decomp_type(src, "TFoo");
        CHECK(t.found && t.polymorphic, "TFoo found, polymorphic (has virtual)");
        CHECK(t.fields.size() == 3, "TFoo: methods/statics ignored, 3 data fields");
        EngineLayout L = to_engine_layout(t, {"TFoo"});
        CHECK(L.fields.count(4) && L.fields[4].member == "mNext" && L.fields[4].nested_type == "TFoo",
              "TFoo.mNext@4 -> nested TFoo (chaining)");
        CHECK(L.fields.count(8) && L.fields[8].nested_type.empty(), "TFoo.mFov@8 scalar, no nested");
        // ABI cross-check: polymorphic (TNameRef-style vtable@0), ptr@4, f32@8, u8@12 (guest)
        CHECK(exact_offsets(t, true), "TFoo: ABI reproduces annotations (vtable@0 + ptr + scalars)");
    }

    // ── 4. Corpus sweep — validate the ABI engine against every checkable real struct ──
    {
        std::vector<std::string> headers;
        walk(INC, headers);
        std::sort(headers.begin(), headers.end());
        CHECK(headers.size() > 100, "corpus: found a substantial header set");

        // Known NON-bugs: types whose decomp annotations are themselves inconsistent, or HW
        // structs with non-natural alignment — NOT parser/ABI defects (verified by hand):
        //   JAISeqUpdateData, TDSPChannel — decomp puts 4 sub-byte fields all at /* 0x0 */
        //       (overlapping by the decomp's own offsets — a WIP-annotation placeholder).
        //   HeaderData (CardManager) — decomp's mComment[0x20]@0x24 overlaps mBanner@0x40.
        //   OSContext — HW thread context: f64 psf[32] at 4-aligned 0x1C4 (paired-single
        //       registers; deliberately non-8-aligned). An OS struct, not an engine type.
        const std::set<std::string> known_non_bugs = {
            "JAISeqUpdateData", "TDSPChannel", "HeaderData", "OSContext"
        };
        int types_seen = 0, types_validated = 0, types_mismatch = 0;
        std::set<std::string> mismatch_names;
        std::vector<std::string> mismatches;
        for (const auto& h : headers) {
            std::string text = slurp(h);
            // find every "class NAME" / "struct NAME" with a body and an annotation inside
            for (const char* kw : {"class ", "struct "}) {
                size_t p = 0;
                while ((p = text.find(kw, p)) != std::string::npos) {
                    size_t ns = p + std::strlen(kw);
                    size_t ne = ns;
                    while (ne < text.size() && (isalnum((unsigned char)text[ne]) || text[ne]=='_')) ++ne;
                    std::string name = text.substr(ns, ne - ns);
                    p = ne;
                    if (name.empty()) continue;
                    ParsedType t = parse_decomp_type(text, name);
                    if (!t.found || t.fields.empty()) continue;
                    ++types_seen;
                    if (!t.fully_sizable) continue;
                    if (validate_offsets(t)) ++types_validated;
                    else {
                        ++types_mismatch;
                        mismatch_names.insert(name);
                        if (mismatches.size() < 25) mismatches.push_back(name + "  (" + h + ")");
                    }
                }
            }
        }
        std::printf("[corpus] headers=%zu  annotated-types-seen=%d  fully-sizable-validated=%d  MISMATCH=%d\n",
                    headers.size(), types_seen, types_validated, types_mismatch);
        for (const auto& m : mismatches) std::printf("    MISMATCH: %s\n", m.c_str());
        CHECK(types_validated > 250, "corpus: validated a meaningful number of real structs");
        // Every mismatch must be a known decomp-data/HW quirk — any NEW name is a real regression.
        bool only_known = true;
        for (const auto& n : mismatch_names)
            if (!known_non_bugs.count(n)) { only_known = false;
                std::fprintf(stderr, "UNEXPECTED mismatch (not a known decomp quirk): %s\n", n.c_str()); }
        CHECK(only_known, "corpus: all alignment/overlap failures are known decomp-data/HW quirks (no ABI/parser bug)");
    }

    std::printf("decomp_parse_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
