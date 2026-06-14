// Unit tests for abi_layout — GameCube (guest) vs host struct field-offset computation,
// the mechanical core of building the tailored-recomp type DB.
//
// Two kinds of oracle:
//   * the DECOMP's real annotated offsets (JUTRect: x1@0,y1@4,x2@8,y2@0xC) — proves the
//     GC-ABI rules match the actual GameCube layout;
//   * the HOST compiler via offsetof — proves the host computation matches what g++/clang
//     actually produce for the port/ engine structs (so guest↔host pairing is sound).
#include "../abi_layout.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, msg) do { ++g_checks; if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); ++g_fail; } } while (0)

int main() {
    // 1. JUTRect (real decomp oracle): four s32, non-polymorphic. Offsets 0/4/8/C, size 16.
    {
        std::vector<LField> f = {
            { "x1", FKind::I32, "" }, { "y1", FKind::I32, "" },
            { "x2", FKind::I32, "" }, { "y2", FKind::I32, "" },
        };
        LResult g = compute_layout(f, /*polymorphic=*/false, guest_abi());
        CHECK(g.offsets == (std::vector<int>{0,4,8,12}), "JUTRect guest offsets match the decomp // _XX annotations");
        CHECK(g.size == 16 && g.align == 4, "JUTRect size/align");
        // No pointers -> host layout is identical.
        LResult h = compute_layout(f, false, host_abi());
        CHECK(h.offsets == g.offsets && h.size == g.size, "no-pointer struct: host == guest layout");
    }

    // 2. Alignment + pointer divergence: { u8, f64, u32, ptr }.
    //    Guest (ptr4): u8@0, f64@8, u32@16, ptr@20, size 24.
    //    Host  (ptr8): u8@0, f64@8, u32@16, ptr@24, size 32.
    {
        std::vector<LField> f = {
            { "b", FKind::U8,  "" }, { "d", FKind::F64, "" },
            { "n", FKind::U32, "" }, { "p", FKind::Ptr, "" },
        };
        LResult g = compute_layout(f, false, guest_abi());
        LResult h = compute_layout(f, false, host_abi());
        CHECK(g.offsets == (std::vector<int>{0,8,16,20}), "guest: f64 forces 8-align padding; ptr is 4 bytes");
        CHECK(g.size == 24, "guest size rounds to 8 (max align)");
        CHECK(h.offsets == (std::vector<int>{0,8,16,24}), "host: 8-byte pointer shifts past the u32");
        CHECK(h.size == 32, "host size");
        CHECK(g.offsets[3] != h.offsets[3], "the pointer field's guest and host offsets DIVERGE (the load-bearing case)");
    }

    // 3. Polymorphic engine object { vtable, ptr mNext, f32 mFov } — the field_slice's
    //    EngineCam shape. Guest: vtable@0, mNext@4, mFov@8. Host: vtable@0, mNext@8, mFov@16.
    //    This is exactly the divergence the end-to-end slice proved (mFov guest 8 / host 16).
    {
        std::vector<LField> f = {
            { "mNext", FKind::Ptr, "EngineCam" }, { "mFov", FKind::F32, "" },
        };
        LResult g = compute_layout(f, /*polymorphic=*/true, guest_abi());
        LResult h = compute_layout(f, /*polymorphic=*/true, host_abi());
        CHECK(g.offsets == (std::vector<int>{4,8}),  "EngineCam guest: mNext@4, mFov@8");
        CHECK(h.offsets == (std::vector<int>{8,16}), "EngineCam host: mNext@8, mFov@16 (matches the slice)");

        // Cross-check the HOST computation against the real host compiler.
        struct EngineCam { virtual ~EngineCam(); EngineCam* mNext; float mFov; };
        CHECK((int)offsetof(EngineCam, mNext) == h.offsets[0], "host mNext offset == offsetof (rule engine matches the compiler)");
        CHECK((int)offsetof(EngineCam, mFov)  == h.offsets[1], "host mFov offset == offsetof");
    }

    std::printf("abi_layout_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
