// Unit tests for type_recovery — SEEDED type recovery for the tailored-recomp boundary.
//
// Hand-assembles tiny PPC functions exercising the patterns that BREAK naive register
// propagation in real compiled code, runs them through the real decoder + collection +
// the recovery pass, and asserts which load/store sites get a host engine-field binding.
//
// Why these patterns: a real accessor doesn't keep `this` in r3 — the prologue spills it
// to the stack and reloads it into a non-volatile across calls, fields chain through
// nested object pointers, and `&obj->sub` (addi by a nonzero offset) must NOT be confused
// with the object itself. A MISS on an engine-typed site is a correctness bug in the
// tailored build (see the SHARP EDGE note in type_recovery.h), so coverage is asserted.
#include "../ppc_decoder.h"
#include "../func_collect.h"
#include "../type_recovery.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, msg) do { ++g_checks; if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); ++g_fail; } } while (0)

// ── encoders ────────────────────────────────────────────────────────────────────────
static uint32_t enc_addi(int rd, int ra, int16_t si) { return (14u<<26)|(rd<<21)|(ra<<16)|(uint16_t)si; }
static uint32_t enc_or  (int rd, int rs, int rb)      { return (31u<<26)|(rs<<21)|(rd<<16)|(rb<<11)|(444u<<1); }
static uint32_t enc_lwz (int rd, int ra, int16_t d)   { return (32u<<26)|(rd<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_stw (int rs, int ra, int16_t d)   { return (36u<<26)|(rs<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_lfs (int frd,int ra, int16_t d)   { return (48u<<26)|(frd<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_bl  (uint32_t from,uint32_t to)   { int32_t dl=(int32_t)(to-from); return (18u<<26)|((uint32_t)dl&0x03fffffc)|1u; }
static constexpr uint32_t BLR = 0x4e800020u;

static std::vector<PPCInstr> collect(uint32_t base, const std::vector<uint32_t>& w) {
    std::vector<uint8_t> b(w.size()*4);
    for (size_t k=0;k<w.size();++k){ uint32_t be=__builtin_bswap32(w[k]); std::memcpy(&b[k*4],&be,4); }
    return collect_function(b.data(), base, b.size(), base, base+(uint32_t)w.size()*4, /*cfg=*/false);
}

int main() {
    const uint32_t B = 0x80100000u;

    // Layout DB: engine type "Cam" — guest offset 4 is a NESTED Cam* pointer, 8 is mFov,
    // 12 is mFlags. Function at B takes `this` (Cam*) in r3.
    TypeDB db;
    db.layouts["Cam"].fields = {
        { 4,  FieldDesc{ "mNext",  "Cam" } },   // nested engine pointer
        { 8,  FieldDesc{ "mFov",   ""    } },
        { 12, FieldDesc{ "mFlags", ""    } },
    };
    db.signatures[B] = { { 3, "Cam" } };

    // helper: find the EngField recovered at the pc of the Nth instruction
    auto field_at_idx = [&](const std::vector<PPCInstr>& ins,
                            const std::map<u32,EngField>& m, size_t idx) -> const EngField* {
        auto it = m.find(ins[idx].pc);
        return it == m.end() ? nullptr : &it->second;
    };

    // 1. Signature seed + direct float field read: lfs f1,8(r3) is typed Cam::mFov.
    {
        std::vector<uint32_t> w = { enc_lfs(1,3,8), BLR };
        auto ins = collect(B, w);
        auto ef = recover_eng_fields(ins, B, db);
        const EngField* f = field_at_idx(ins, ef, 0);
        CHECK(f && f->type_cname=="Cam" && f->member=="mFov", "seeded `this` field read is typed (Cam::mFov)");
    }

    // 2. Stack spill/reload across a call: the prologue saves `this` and reloads it into a
    //    non-volatile, then reads a field — the field site must still be typed.
    //    stw r3,0x14(r1) ; bl X ; lwz r31,0x14(r1) ; lfs f1,8(r31) ; blr
    {
        std::vector<uint32_t> w = {
            enc_stw(3,1,0x14),                 // spill this
            enc_bl(B+4, 0x80009000u),          // call (clobbers r3..r12)
            enc_lwz(31,1,0x14),                // reload this into r31
            enc_lfs(1,31,8),                   // read this->mFov
            BLR,
        };
        auto ins = collect(B, w);
        auto ef = recover_eng_fields(ins, B, db);
        const EngField* f = field_at_idx(ins, ef, 3);   // the lfs
        CHECK(f && f->member=="mFov", "type survives a stack spill/reload of `this` across a call");
    }

    // 3. Volatile clobber: a field read off r3 AFTER a bl (with NO reload) must NOT be typed
    //    — r3 was clobbered, so emitting a host access would be wrong.
    //    bl X ; lfs f1,8(r3) ; blr
    {
        std::vector<uint32_t> w = { enc_bl(B+0, 0x80009000u), enc_lfs(1,3,8), BLR };
        auto ins = collect(B, w);
        auto ef = recover_eng_fields(ins, B, db);
        CHECK(field_at_idx(ins, ef, 1) == nullptr, "clobbered volatile is NOT typed after a call (no false host access)");
    }

    // 4. Nested pointer chase: load mNext (a Cam*), then read a field through it.
    //    lwz r4,4(r3) ; lfs f1,8(r4) ; blr  → both sites typed; the chain keeps the type.
    {
        std::vector<uint32_t> w = { enc_lwz(4,3,4), enc_lfs(1,4,8), BLR };
        auto ins = collect(B, w);
        auto ef = recover_eng_fields(ins, B, db);
        const EngField* fn = field_at_idx(ins, ef, 0);   // lwz mNext
        const EngField* ff = field_at_idx(ins, ef, 1);   // lfs through mNext
        CHECK(fn && fn->member=="mNext", "nested-pointer field load is itself typed (Cam::mNext)");
        CHECK(ff && ff->member=="mFov",  "field access THROUGH a nested engine pointer stays typed");
    }

    // 5. Interior address (addi by a nonzero offset) must NOT propagate the object type —
    //    &this->sub is not `this`, and we don't model interior typing (honest conservatism).
    //    addi r5,r3,0x10 ; lwz r0,0(r5) ; blr  → the load off r5 is NOT typed.
    {
        std::vector<uint32_t> w = { enc_addi(5,3,0x10), enc_lwz(0,5,0), BLR };
        auto ins = collect(B, w);
        auto ef = recover_eng_fields(ins, B, db);
        CHECK(field_at_idx(ins, ef, 1) == nullptr, "interior address (addi nonzero) does not carry the object type");
    }

    // 6. mr copy preserves the type: mr r30,r3 ; lfs f1,12(r30) ; blr  → typed Cam::mFlags? no,
    //    offset 12 is mFlags (int) but read via lfs is unusual; use offset 8 (mFov) to stay clean.
    {
        std::vector<uint32_t> w = { enc_or(30,3,3), enc_lfs(1,30,8), BLR };
        auto ins = collect(B, w);
        auto ef = recover_eng_fields(ins, B, db);
        const EngField* f = field_at_idx(ins, ef, 1);
        CHECK(f && f->member=="mFov", "mr copy of `this` keeps the engine type");
    }

    std::printf("type_recovery_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
