// Generator for the OBJECT-IDENTITY slice (engine-CONSTRUCTION half of the boundary —
// docs/re_notes/object_identity.md). Mirrors field_slice_gen: hand-assembled "game" code through
// the SAME decoder + collection + seeded type-recovery + emitter the production recompiler uses,
// emitted twice per world: ORACLE (guest-layout, no recovery) and TAILORED (host-native).
//
// Two heap-construction patterns, each as its own function(s):
//
//   Pattern B — heap `new` with an INLINED ctor (one function):
//     +0  bl operator_new   ; r3 = new EngineTex   (rewritten in TAILORED -> sb_eng_alloc)
//     +4  li r0,64 ; +8 stw r0,0x3c(r3)   ; inlined ctor: mWidth = 64
//     +12 li r0,0  ; +16 stw r0,0x28(r3)  ; inlined ctor: mEmbPalette = 0
//     +20 stw r3,8(r1) ; +24 bl eng_touch ; +28 lwz r3,8(r1) ; +32 blr   (eng_touch reveals type)
//
//   Pattern A — heap `new` with an OUT-OF-LINE ctor (caller + a recompiled tailored ctor):
//     caller:  bl operator_new ; mr r29,r3 ; li r4,77 ; mr r3,r29 ; bl ctor ; mr r3,r29 ; blr
//     ctor:    stw r4,0x3c(r3) ; blr        ; this seeded EngineTex -> host member write
//   KEY: the out-of-line ctor needs NO special bridge for a non-polymorphic type — recompiling it
//   tailored makes its own `this` field writes host-native (object_identity.md).
//
// construct_slice_test.cpp compiles all bodies and proves each TAILORED host object ends up with
// the SAME field values as the matching ORACLE guest buffer.
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
static uint32_t enc_or  (int rd, int rs, int rb)     { return (31u<<26)|(rs<<21)|(rd<<16)|(rb<<11)|(444u<<1); }
static uint32_t enc_stw (int rs, int ra, int16_t d)  { return (36u<<26)|(rs<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_lwz (int rd, int ra, int16_t d)  { return (32u<<26)|(rd<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_bl  (uint32_t from, uint32_t to) { int32_t dl=(int32_t)(to-from); return (18u<<26)|((uint32_t)dl & 0x03fffffc)|1u; }
static constexpr uint32_t BLR = 0x4e800020u;

static constexpr uint32_t OPNEW     = 0x80009100u;   // operator new (raw allocator)
static constexpr uint32_t ENG_TOUCH = 0x80009200u;   // an EngineTex method (type-revealing, no-op)
static constexpr uint32_t CTORV     = 0x80009300u;   // a POLYMORPHIC type's out-of-line ctor (bridged)

// Per-world function addresses (oracle base 0x80010000, tailored base 0x80020000).
static uint32_t fB(uint32_t base)    { return base + 0x0000; }   // Pattern B caller
static uint32_t fAcall(uint32_t base){ return base + 0x1000; }   // Pattern A caller
static uint32_t fActor(uint32_t base){ return base + 0x2000; }   // Pattern A out-of-line ctor
static uint32_t fC(uint32_t base)    { return base + 0x3000; }   // Pattern C stack temporary
static uint32_t fAV(uint32_t base)   { return base + 0x4000; }   // Pattern A, POLYMORPHIC (bridged ctor)

static std::vector<uint32_t> patB_words(uint32_t a) {
    return {
        enc_bl(a + 0, OPNEW), enc_addi(0,0,64), enc_stw(0,3,0x3c),
        enc_addi(0,0,0), enc_stw(0,3,0x28), enc_stw(3,1,8),
        enc_bl(a + 24, ENG_TOUCH), enc_lwz(3,1,8), BLR,
    };
}
static std::vector<uint32_t> patAcall_words(uint32_t a, uint32_t ctor) {
    return {
        enc_bl(a + 0, OPNEW),    // r3 = operator new
        enc_or(29,3,3),          // mr r29,r3
        enc_addi(4,0,77),        // li r4,77  (ctor arg: width)
        enc_or(3,29,29),         // mr r3,r29 (this)
        enc_bl(a + 16, ctor),    // ctor(this=r29, 77)
        enc_or(3,29,29),         // mr r3,r29 (return the object)
        BLR,
    };
}
static std::vector<uint32_t> patActor_words() {
    return { enc_stw(4,3,0x3c), BLR };   // this->mWidth = r4
}
// Pattern A for a POLYMORPHIC type: same as patAcall but the ctor is BRIDGED (a placement-new
// wrapper that runs the real C++ ctor -> sets the host vtable). The caller is identical shape;
// only the ctor target (CTORV) is a bridge, not a recompiled function.
static std::vector<uint32_t> patAVcall_words(uint32_t a) {
    return {
        enc_bl(a + 0, OPNEW),    // r3 = operator new -> sb_eng_alloc<EngineTexV>()
        enc_or(29,3,3),          // mr r29,r3
        enc_addi(4,0,99),        // li r4,99
        enc_or(3,29,29),         // mr r3,r29 (this=handle)
        enc_bl(a + 16, CTORV),   // ctor(this, 99)  -> bridged placement-new (sets vtable)
        enc_or(3,29,29),         // mr r3,r29 (return the object)
        BLR,
    };
}
// Pattern C — a STACK temporary (interior `addi r1,off`), written then re-materialized and read
// back: the two addis for the same frame slot must share ONE host object (same handle), and the
// value is exported to the frame (untyped) so the test can compare it to the oracle.
static std::vector<uint32_t> patC_words(uint32_t a) {
    return {
        enc_addi(3,1,0x28),          // +0   this = stack temp (addi#1)
        enc_addi(0,0,88),            // +4   r0 = 88
        enc_stw(0,3,0x3c),           // +8   this->mWidth = 88   (host write off addi#1)
        enc_bl(a + 12, ENG_TOUCH),   // +12  eng_touch(this) -> reveals/flags addi#1
        enc_addi(3,1,0x28),          // +16  re-materialize this (addi#2, SAME slot)
        enc_lwz(5,3,0x3c),           // +20  r5 = this->mWidth  (host read off addi#2 -> same obj)
        enc_stw(5,1,0x100),          // +24  export to frame+0x100 (untyped, observable)
        enc_bl(a + 28, ENG_TOUCH),   // +28  eng_touch(this) -> reveals/flags addi#2
        BLR,                         // +32
    };
}

static TypeDB make_db(uint32_t ctor_addr) {
    TypeDB db;
    db.layouts["EngineTex"].fields = {
        { 0x28, FieldDesc{ "mEmbPalette", "" } },
        { 0x3c, FieldDesc{ "mWidth",      "" } },
    };
    db.layouts["EngineTexV"] = {};                       // polymorphic; no inline field access in caller
    db.signatures[ENG_TOUCH] = { { 3, "EngineTex" } };   // eng_touch(this)
    db.signatures[ctor_addr] = { { 3, "EngineTex" } };   // ctor(this, ...) — reveal + self-seed
    db.signatures[CTORV]     = { { 3, "EngineTexV" } };  // polymorphic ctor(this, ...) — flags the alloc
    return db;
}

static std::string emit_fn(uint32_t base, const std::vector<uint32_t>& w, bool tailored,
                           uint32_t ctor_addr) {
    std::vector<uint8_t> bytes(w.size() * 4);
    for (size_t k = 0; k < w.size(); ++k) { uint32_t be = __builtin_bswap32(w[k]); std::memcpy(&bytes[k*4], &be, 4); }
    EmitContext ctx;
    ctx.func_addr = base;
    ctx.instrs = collect_function(bytes.data(), base, bytes.size(), base, base + (uint32_t)w.size()*4, false);
    ctx.branch_targets = intra_branch_targets(ctx.instrs, base);
    if (tailored) {
        TypeDB db = make_db(ctor_addr);
        std::unordered_set<u32> raws = { OPNEW };
        ctx.eng_fields = recover_eng_fields(ctx.instrs, base, db, ctx.branch_targets,
                                            /*jumptable=*/{}, nullptr, &raws, &ctx.alloc_sites);
    }
    std::ostringstream ss; CEmitter em(ss); em.emit_function(ctx);
    return ss.str();
}

// All functions for one world, concatenated.
static std::string emit_world(uint32_t base, bool tailored) {
    std::string s;
    s += emit_fn(fB(base),     patB_words(fB(base)),                 tailored, fActor(base));
    s += emit_fn(fAcall(base), patAcall_words(fAcall(base), fActor(base)), tailored, fActor(base));
    s += emit_fn(fActor(base), patActor_words(),                     tailored, fActor(base));
    s += emit_fn(fC(base),     patC_words(fC(base)),                 tailored, fActor(base));
    s += emit_fn(fAV(base),    patAVcall_words(fAV(base)),           tailored, fActor(base));
    return s;
}

int main(int argc, char** argv) {
    const char* dir = (argc > 1) ? argv[1] : "scratch/port";
    std::string oracle   = emit_world(0x80010000u, /*tailored=*/false);
    std::string tailored = emit_world(0x80020000u, /*tailored=*/true);
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
