// Generator for the OBJECT-IDENTITY slice (the engine-CONSTRUCTION half of the game<->engine
// boundary — docs/re_notes/object_identity.md), Pattern B (heap `new` with an INLINED ctor).
// Mirrors field_slice_gen: runs ONE hand-assembled "game" constructor through the SAME decoder +
// collection + seeded type-recovery + emitter the production recompiler uses, twice:
//
//   * ORACLE  (no eng_fields / alloc_sites) -> guest-layout: `operator new` is a real call, the
//                                              inlined ctor writes go to the raw guest buffer.
//   * TAILORED (recovery with raw_allocators) -> host-native: `operator new` is rewritten to
//                                              cpu.gpr[3] = sb_eng_alloc<EngineTex>(), and the
//                                              inlined ctor writes hit the HOST object's members.
//
//   +0  bl  operator_new   ; r3 = new EngineTex             (rewritten in TAILORED)
//   +4  li  r0, 64
//   +8  stw r0, 0x3c(r3)   ; inlined ctor: mWidth = 64      (host member write in TAILORED)
//   +12 li  r0, 0
//   +16 stw r0, 0x28(r3)   ; inlined ctor: mEmbPalette = 0
//   +20 stw r3, 8(r1)      ; spill `this` across the call
//   +24 bl  eng_touch      ; engine method -> REVEALS the type to recovery (no-op at runtime)
//   +28 lwz r3, 8(r1)      ; reload `this` (the object identity) into r3
//   +32 blr
//
// construct_slice_test.cpp compiles both bodies and proves the TAILORED host object ends up with
// the SAME field values as the ORACLE guest buffer.
#include "../../tools/recompiler/ppc_decoder.h"
#include "../../tools/recompiler/c_emitter.h"
#include "../../tools/recompiler/func_collect.h"
#include "../../tools/recompiler/type_recovery.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

static uint32_t enc_addi(int rd, int ra, int16_t si) { return (14u<<26)|(rd<<21)|(ra<<16)|(uint16_t)si; }
static uint32_t enc_stw (int rs, int ra, int16_t d)  { return (36u<<26)|(rs<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_lwz (int rd, int ra, int16_t d)  { return (32u<<26)|(rd<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_bl  (uint32_t from, uint32_t to) { int32_t dl=(int32_t)(to-from); return (18u<<26)|((uint32_t)dl & 0x03fffffc)|1u; }
static constexpr uint32_t BLR = 0x4e800020u;

static constexpr uint32_t OPNEW     = 0x80009100u;   // operator new (raw allocator)
static constexpr uint32_t ENG_TOUCH = 0x80009200u;   // an EngineTex method (type-revealing, no-op)

static std::vector<uint32_t> game_words(uint32_t base) {
    return {
        enc_bl(base + 0,  OPNEW),       // +0   r3 = operator new(...)
        enc_addi(0, 0, 64),             // +4   r0 = 64
        enc_stw(0, 3, 0x3c),            // +8   this->mWidth = 64
        enc_addi(0, 0, 0),              // +12  r0 = 0
        enc_stw(0, 3, 0x28),            // +16  this->mEmbPalette = 0
        enc_stw(3, 1, 8),               // +20  spill this
        enc_bl(base + 24, ENG_TOUCH),   // +24  eng_touch(this)  (reveals type)
        enc_lwz(3, 1, 8),               // +28  reload this
        BLR,                            // +32
    };
}

// EngineTex layout: GUEST offsets from real-JUTTexture-style displacements (mEmbPalette@0x28,
// mWidth@0x3c). The host struct puts them elsewhere (see the test) so a guest-offset bug is caught.
static TypeDB make_db() {
    TypeDB db;
    db.layouts["EngineTex"].fields = {
        { 0x28, FieldDesc{ "mEmbPalette", "" } },
        { 0x3c, FieldDesc{ "mWidth",      "" } },
    };
    db.signatures[ENG_TOUCH] = { { 3, "EngineTex" } };   // eng_touch(this=EngineTex*)
    return db;
}

static std::string emit_one(uint32_t base, bool tailored) {
    std::vector<uint32_t> w = game_words(base);
    std::vector<uint8_t> bytes(w.size() * 4);
    for (size_t k = 0; k < w.size(); ++k) {
        uint32_t be = __builtin_bswap32(w[k]);
        std::memcpy(&bytes[k * 4], &be, 4);
    }
    uint32_t fend = base + (uint32_t)w.size() * 4;

    EmitContext ctx;
    ctx.func_addr = base;
    ctx.instrs = collect_function(bytes.data(), base, bytes.size(), base, fend, /*cfg=*/false);
    ctx.branch_targets = intra_branch_targets(ctx.instrs, base);
    if (tailored) {
        TypeDB db = make_db();
        std::unordered_set<u32> raws = { OPNEW };
        ctx.eng_fields = recover_eng_fields(ctx.instrs, base, db, ctx.branch_targets,
                                            /*jumptable=*/{}, nullptr, &raws, &ctx.alloc_sites);
    }
    std::ostringstream ss;
    CEmitter em(ss);
    em.emit_function(ctx);
    return ss.str();
}

int main(int argc, char** argv) {
    const char* dir = (argc > 1) ? argv[1] : "scratch/port";
    std::string oracle   = emit_one(0x80010000u, /*tailored=*/false);
    std::string tailored = emit_one(0x80020000u, /*tailored=*/true);
    auto write = [&](const std::string& name, const std::string& body) {
        std::string path = std::string(dir) + "/" + name;
        std::ofstream f(path);
        f << "// AUTO-GENERATED by construct_slice_gen — object-identity slice. DO NOT EDIT.\n" << body;
        std::printf("[construct_slice_gen] wrote %s (%zu bytes)\n", path.c_str(), body.size());
    };
    write("construct_oracle.inc",   oracle);
    write("construct_tailored.inc", tailored);
    std::printf("\n----- ORACLE (guest-layout) -----\n%s\n", oracle.c_str());
    std::printf("----- TAILORED (host-native construction) -----\n%s\n", tailored.c_str());
    return 0;
}
