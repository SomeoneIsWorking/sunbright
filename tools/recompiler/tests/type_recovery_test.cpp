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
static uint32_t enc_lhz (int rd, int ra, int16_t d)   { return (40u<<26)|(rd<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_stw (int rs, int ra, int16_t d)   { return (36u<<26)|(rs<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_lfs (int frd,int ra, int16_t d)   { return (48u<<26)|(frd<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_bl  (uint32_t from,uint32_t to)   { int32_t dl=(int32_t)(to-from); return (18u<<26)|((uint32_t)dl&0x03fffffc)|1u; }
static uint32_t enc_b   (uint32_t from,uint32_t to)   { int32_t dl=(int32_t)(to-from); return (18u<<26)|((uint32_t)dl&0x03fffffc); }
// bc bo,bi,target — bo=12 (branch if CR[bi] set), conditional (no link).
static uint32_t enc_bc  (uint32_t from,uint32_t to)   { int16_t d=(int16_t)(to-from); return (16u<<26)|(12u<<21)|(0u<<16)|((uint16_t)d & 0xFFFCu); }
static uint32_t enc_mtctr(int rs)                     { return (31u<<26)|(rs<<21)|(0x120u<<11)|(467u<<1); } // mtspr CTR,rS
static uint32_t enc_mtlr (int rs)                     { return (31u<<26)|(rs<<21)|(0x100u<<11)|(467u<<1); } // mtspr LR,rS
static constexpr uint32_t BCTRL = (19u<<26)|(20u<<21)|(528u<<1)|1u;  // bctrl (bo=20 always)
static constexpr uint32_t BCLRL = (19u<<26)|(20u<<21)|(16u<<1)|1u;   // bclrl (bo=20 always)
static constexpr uint32_t BLR = 0x4e800020u;

static std::vector<PPCInstr> collect(uint32_t base, const std::vector<uint32_t>& w) {
    std::vector<uint8_t> b(w.size()*4);
    for (size_t k=0;k<w.size();++k){ uint32_t be=__builtin_bswap32(w[k]); std::memcpy(&b[k*4],&be,4); }
    return collect_function(b.data(), base, b.size(), base, base+(uint32_t)w.size()*4, /*cfg=*/false);
}
// Full-CFG collection (follows branches) — for the merge/branch/loop cases.
static std::vector<PPCInstr> collect_cfg(uint32_t base, const std::vector<uint32_t>& w) {
    std::vector<uint8_t> b(w.size()*4);
    for (size_t k=0;k<w.size();++k){ uint32_t be=__builtin_bswap32(w[k]); std::memcpy(&b[k*4],&be,4); }
    return collect_function(b.data(), base, b.size(), base, base+(uint32_t)w.size()*4, /*cfg=*/true);
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
        auto ef = recover_eng_fields(ins, B, db, intra_branch_targets(ins, B), {});
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
        auto ef = recover_eng_fields(ins, B, db, intra_branch_targets(ins, B), {});
        const EngField* f = field_at_idx(ins, ef, 3);   // the lfs
        CHECK(f && f->member=="mFov", "type survives a stack spill/reload of `this` across a call");
    }

    // 3. Volatile clobber: a field read off r3 AFTER a bl (with NO reload) must NOT be typed
    //    — r3 was clobbered, so emitting a host access would be wrong.
    //    bl X ; lfs f1,8(r3) ; blr
    {
        std::vector<uint32_t> w = { enc_bl(B+0, 0x80009000u), enc_lfs(1,3,8), BLR };
        auto ins = collect(B, w);
        auto ef = recover_eng_fields(ins, B, db, intra_branch_targets(ins, B), {});
        CHECK(field_at_idx(ins, ef, 1) == nullptr, "clobbered volatile is NOT typed after a call (no false host access)");
    }

    // 4. Nested pointer chase: load mNext (a Cam*), then read a field through it.
    //    lwz r4,4(r3) ; lfs f1,8(r4) ; blr  → both sites typed; the chain keeps the type.
    {
        std::vector<uint32_t> w = { enc_lwz(4,3,4), enc_lfs(1,4,8), BLR };
        auto ins = collect(B, w);
        auto ef = recover_eng_fields(ins, B, db, intra_branch_targets(ins, B), {});
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
        auto ef = recover_eng_fields(ins, B, db, intra_branch_targets(ins, B), {});
        CHECK(field_at_idx(ins, ef, 1) == nullptr, "interior address (addi nonzero) does not carry the object type");
    }

    // 6. mr copy preserves the type: mr r30,r3 ; lfs f1,12(r30) ; blr  → typed Cam::mFlags? no,
    //    offset 12 is mFlags (int) but read via lfs is unusual; use offset 8 (mFov) to stay clean.
    {
        std::vector<uint32_t> w = { enc_or(30,3,3), enc_lfs(1,30,8), BLR };
        auto ins = collect(B, w);
        auto ef = recover_eng_fields(ins, B, db, intra_branch_targets(ins, B), {});
        const EngField* f = field_at_idx(ins, ef, 1);
        CHECK(f && f->member=="mFov", "mr copy of `this` keeps the engine type");
    }

    // 6b. HOLDER pattern (SUNBRIGHT_HOLDER_TYPES): a GAME actor type that HOLDS an engine
    //     handle in a member field. The holder ("Actor", != the engine type) is in the DB only
    //     so the engine type propagates: `lwz r3,0x44(this:Actor)` loads the J3DModelData* member
    //     and types r3, so the following `lhz r4,0x24(r3)` is a typed J3DModelData::mMaterialNum
    //     read (routed by main.cpp because the engine type is in field_types; the Actor's OWN
    //     reads are NOT routed because Actor is not in field_types — gating is in main.cpp).
    //     This is the mechanism that fixed TShimmer::load's inlined getMaterialNum (handoff step 2b).
    {
        TypeDB hdb;
        hdb.layouts["Actor"].fields = { { 0x44, FieldDesc{ "unkMD", "MD" } } };  // engine-ptr member
        hdb.layouts["MD"].fields    = { { 0x24, FieldDesc{ "mMaterialNum", "" } } };
        hdb.signatures[B] = { { 3, "Actor" } };                                  // load__Actor(this)
        std::vector<uint32_t> w = { enc_lwz(3,3,0x44), enc_lhz(4,3,0x24), BLR };
        auto ins = collect(B, w);
        auto ef = recover_eng_fields(ins, B, hdb, intra_branch_targets(ins, B), {});
        const EngField* fh = field_at_idx(ins, ef, 0);   // lwz unkMD (the holder member)
        const EngField* fm = field_at_idx(ins, ef, 1);   // lhz through the propagated MD
        CHECK(fh && fh->type_cname=="Actor" && fh->member=="unkMD",
              "holder member load is typed (Actor::unkMD)");
        CHECK(fm && fm->type_cname=="MD" && fm->member=="mMaterialNum",
              "engine-field read THROUGH a holder member is typed (MD::mMaterialNum)");
    }

    // find the EngField recovered at an absolute pc
    auto field_at_pc = [&](const std::map<u32,EngField>& m, u32 pc) -> const EngField* {
        auto it = m.find(pc); return it == m.end() ? nullptr : &it->second;
    };

    // 7. Diamond CFG, both arms agree: r30 = `this` on each path; the field read at the JOIN
    //    is typed because the MEET of the two predecessors keeps Cam.
    //      +0  bc  -> L1(+12)        +4 mr r30,r3 ; +8 b JOIN(+16)
    //      +12 mr r30,r3 (L1)        +16 lfs f1,8(r30) (JOIN) ; +20 blr
    {
        std::vector<uint32_t> w = {
            enc_bc(B+0, B+12), enc_or(30,3,3), enc_b(B+8, B+16),
            enc_or(30,3,3), enc_lfs(1,30,8), BLR,
        };
        auto ins = collect_cfg(B, w);
        auto ef = recover_eng_fields(ins, B, db, intra_branch_targets(ins, B), {});
        const EngField* f = field_at_pc(ef, B+16);
        CHECK(f && f->member=="mFov", "merge: field typed when both CFG paths agree on the type");
    }

    // 8. Diamond CFG, arms DISAGREE: one path has r30=`this`, the other clears it. The MEET
    //    drops to unknown, so the JOIN field read is a (sound) MISS — exactly what stops a
    //    wrong type being carried across an edge (the bug the single linear pass could hit).
    //      +0  bc -> L1(+12)         +4 mr r30,r3 ; +8 b JOIN(+16)
    //      +12 addi r30,r0,0 (L1)    +16 lfs f1,8(r30) (JOIN) ; +20 blr
    {
        std::vector<uint32_t> w = {
            enc_bc(B+0, B+12), enc_or(30,3,3), enc_b(B+8, B+16),
            enc_addi(30,0,0), enc_lfs(1,30,8), BLR,
        };
        auto ins = collect_cfg(B, w);
        auto ef = recover_eng_fields(ins, B, db, intra_branch_targets(ins, B), {});
        CHECK(field_at_pc(ef, B+16) == nullptr, "merge: disagreeing paths -> sound MISS, no wrong type carried");
    }

    // 9. Loop (back-edge): the type established before the loop survives across iterations
    //    (the fixpoint converges; the back-edge predecessor doesn't clobber r30).
    //      +0 mr r30,r3 ; +4 lfs f1,8(r30) (LOOP) ; +8 bc -> LOOP(+4) ; +12 blr
    {
        std::vector<uint32_t> w = {
            enc_or(30,3,3), enc_lfs(1,30,8), enc_bc(B+8, B+4), BLR,
        };
        auto ins = collect_cfg(B, w);
        auto ef = recover_eng_fields(ins, B, db, intra_branch_targets(ins, B), {});
        const EngField* f = field_at_pc(ef, B+4);
        CHECK(f && f->member=="mFov", "loop: type established before the loop survives the back-edge");
    }

    // ── OBJECT-IDENTITY back-typing (docs/re_notes/object_identity.md) ────────────────────
    // A register used as `this` of a known engine ctor/method IS that engine type, and the type
    // propagates BACKWARD to its definition — typing inlined ctor writes that PRECEDE the call and
    // flagging the construction site. Opt in via the raw_allocators + alloc_sites params.
    TypeDB tdb;
    tdb.layouts["Tex"].fields = {
        { 8,    FieldDesc{ "mFov",        "" } },
        { 0x28, FieldDesc{ "mEmbPalette", "" } },
    };
    const uint32_t ALLOC = 0x80200000u;   // a raw allocator (operator new)
    const uint32_t CTOR  = 0x80210000u;   // out-of-line ctor:  __ct__Tex(this, ...)
    const uint32_t STORE = 0x80220000u;   // a method:          storeTIMG(this, ...)
    tdb.signatures[CTOR]  = { { 3, "Tex" } };
    tdb.signatures[STORE] = { { 3, "Tex" } };
    std::unordered_set<u32> raws = { ALLOC };

    // 10. Pattern A — heap new, OUT-OF-LINE ctor, WITH the real null-check merge. The store into
    //     the container field uses the SAVED operator-new result, so identity must attach at the
    //     ALLOCATION not the ctor return; and the `if (p==null) skip` branch must NOT defeat
    //     recognition (it broke a backward demand analysis — the forward origin pass survives it).
    //       li r3,0x54 ; bl ALLOC ; mr. r29,r3 ; beq +24 ; mr r3,r29 ; bl CTOR ;
    //       (merge) stw r29,0x10(r30) ; blr
    {
        std::vector<uint32_t> w = {
            enc_addi(3,0,0x54), enc_bl(B+4, ALLOC), enc_or(29,3,3),
            enc_bc(B+12, B+24), enc_or(3,29,29), enc_bl(B+20, CTOR),
            enc_stw(29,30,0x10), BLR,
        };
        auto ins = collect_cfg(B, w);
        std::map<u32,std::string> sites;
        recover_eng_fields(ins, B, tdb, intra_branch_targets(ins, B), {}, nullptr, &raws, &sites);
        CHECK(sites.count(B+4) && sites[B+4]=="Tex",
              "Pattern A: operator-new flagged as Tex alloc, surviving the null-check merge");
    }

    // 11. Pattern B — heap new, INLINED ctor (the handoff CRUX). The inlined field write that
    //     PRECEDES the revealing method call must be typed, and the alloc site flagged.
    //       bl ALLOC ; stw r0,0x28(r3) ; bl STORE ; blr
    {
        std::vector<uint32_t> w = {
            enc_bl(B+0, ALLOC), enc_stw(0,3,0x28), enc_bl(B+8, STORE), BLR,
        };
        auto ins = collect(B, w);
        std::map<u32,std::string> sites;
        auto ef = recover_eng_fields(ins, B, tdb, intra_branch_targets(ins, B), {}, nullptr, &raws, &sites);
        const EngField* f = field_at_pc(ef, B+4);   // the inlined stw mEmbPalette
        CHECK(f && f->type_cname=="Tex" && f->member=="mEmbPalette",
              "Pattern B: inlined ctor write BEFORE the call is back-typed (Tex::mEmbPalette)");
        CHECK(sites.count(B+0) && sites[B+0]=="Tex",
              "Pattern B: operator-new site flagged as engine alloc of Tex (inlined ctor)");
    }

    // 12. Pattern C — STACK temporary. `this` is an interior stack address (addi r3,r1,off); the
    //     write off it before the method call is typed and the addi flagged as a stack-temp ctor.
    //       addi r3,r1,0x28 ; stw r0,8(r3) ; bl STORE ; blr
    {
        std::vector<uint32_t> w = {
            enc_addi(3,1,0x28), enc_stw(0,3,8), enc_bl(B+8, STORE), BLR,
        };
        auto ins = collect(B, w);
        std::map<u32,std::string> sites;
        auto ef = recover_eng_fields(ins, B, tdb, intra_branch_targets(ins, B), {}, nullptr, &raws, &sites);
        const EngField* f = field_at_pc(ef, B+4);   // stw off the stack temp
        CHECK(f && f->member=="mFov", "Pattern C: write off a stack-temp engine object is typed (Tex::mFov)");
        CHECK(sites.count(B+0) && sites[B+0]=="Tex",
              "Pattern C: interior-stack addi flagged as a stack-temp engine ctor of Tex");
    }

    // 13. Negative: a raw-allocator result NOT used as an engine `this` must NOT be flagged
    //     (operator new is generic — only engine constructions are rewritten).
    //       bl ALLOC ; stw r3,0x10(r30) ; blr   (result stored as a plain pointer, no ctor)
    {
        std::vector<uint32_t> w = { enc_bl(B+0, ALLOC), enc_stw(3,30,0x10), BLR };
        auto ins = collect(B, w);
        std::map<u32,std::string> sites;
        recover_eng_fields(ins, B, tdb, intra_branch_targets(ins, B), {}, nullptr, &raws, &sites);
        CHECK(sites.empty(), "Negative: non-engine operator-new result is not flagged as an engine alloc");
    }

    // ── MOST-DERIVED-TYPE recognition (object_identity.md SCALE VALIDATION) ────────────────
    // A polymorphic subclass (D : Tex) is constructed via `new D` = `li r3,sizeof(D); bl new;
    // <inlined ctor incl. the appended-vtable store>; bl Tex_method(base)`. The first engine call
    // takes `this` as the BASE Tex, so signature resolution alone under-reports the type as Tex —
    // and the inlined vtable write at the appended offset becomes a recovery GAP. The `li` size is
    // ground truth: with `sizes`+`bases` in the DB, recognition upgrades Tex -> D by size match,
    // mapping the appended-vtable offset (no gap) and flagging D's polymorphic construction.
    TypeDB pdb;
    pdb.layouts["Tex"].fields = {
        { 8,    FieldDesc{ "mFov",        "" } },
        { 0x28, FieldDesc{ "mEmbPalette", "" } },
    };
    // D : Tex appends a vtable pointer at the end (CodeWarrior: virtuals introduced over a
    // non-polymorphic base go at offset == base size). D's layout = Tex's fields + the vptr@0x54.
    pdb.layouts["D"].fields = pdb.layouts["Tex"].fields;
    pdb.layouts["D"].fields[0x54] = FieldDesc{ "__vtbl", "" };
    pdb.sizes = { { "Tex", 0x54 }, { "D", 0x58 } };
    pdb.bases = { { "D", "Tex" } };
    pdb.signatures[STORE] = { { 3, "Tex" } };   // storeTIMG(this : Tex)
    std::unordered_set<u32> praws = { ALLOC };

    // 14. `new D` (subclass) recognized as D by the `li 0x58` size, even though the only engine
    //     call is a Tex method. The inlined appended-vtable write @0x54 is then a MAPPED field of D
    //     (not a gap), and the alloc site is flagged as D.
    //       li r3,0x58 ; bl ALLOC ; stw r0,0x28(r3) ; stw r4,0x54(r3) ; bl STORE(this=r3) ; blr
    {
        std::vector<uint32_t> w = {
            enc_addi(3,0,0x58), enc_bl(B+4, ALLOC), enc_stw(0,3,0x28),
            enc_stw(4,3,0x54), enc_bl(B+16, STORE), BLR,
        };
        auto ins = collect(B, w);
        std::vector<u32> gaps;
        std::map<u32,std::string> sites;
        auto ef = recover_eng_fields(ins, B, pdb, intra_branch_targets(ins, B), {}, &gaps, &praws, &sites);
        CHECK(sites.count(B+4) && sites[B+4]=="D",
              "most-derived: `new D` flagged as D (subclass) via the li 0x58 size, not the base Tex");
        const EngField* vt = field_at_pc(ef, B+12);  // the appended-vtable store @0x54
        CHECK(vt && vt->type_cname=="D" && vt->member=="__vtbl",
              "most-derived: D's appended-vtable write @0x54 is a MAPPED field of D (gap cleared)");
        CHECK(gaps.empty(), "most-derived: no recovery gap once the subclass D is recognized");
    }

    // 15. `new Tex` (the base itself): `li 0x54` matches the base size, no subclass upgrade.
    //       li r3,0x54 ; bl ALLOC ; stw r0,0x28(r3) ; bl STORE ; blr
    {
        std::vector<uint32_t> w = {
            enc_addi(3,0,0x54), enc_bl(B+4, ALLOC), enc_stw(0,3,0x28), enc_bl(B+12, STORE), BLR,
        };
        auto ins = collect(B, w);
        std::map<u32,std::string> sites;
        recover_eng_fields(ins, B, pdb, intra_branch_targets(ins, B), {}, nullptr, &praws, &sites);
        CHECK(sites.count(B+4) && sites[B+4]=="Tex",
              "most-derived: base size matches the base type -> no spurious subclass upgrade");
    }

    // 16. Unknown size (no `li`, e.g. size came from a register): stays the signature type Tex.
    //       bl ALLOC ; stw r0,0x28(r3) ; bl STORE ; blr
    {
        std::vector<uint32_t> w = { enc_bl(B+0, ALLOC), enc_stw(0,3,0x28), enc_bl(B+8, STORE), BLR };
        auto ins = collect(B, w);
        std::map<u32,std::string> sites;
        recover_eng_fields(ins, B, pdb, intra_branch_targets(ins, B), {}, nullptr, &praws, &sites);
        CHECK(sites.count(B+0) && sites[B+0]=="Tex",
              "most-derived: unknown allocation size keeps the signature type (honest fallback)");
    }

    // ── VIRTUAL-CALL recognition (offset-0 dispatch) ─────────────────────────────────────────
    // 17. The canonical virtual-call chain on an engine handle: lwz vt,0(this); lwz m,N(vt);
    //     mtctr m; bctrl  ->  record {bctrl pc -> (Cam, N)}. N=0x10 (slot 2, e.g. J3DModel::calc).
    {
        std::vector<uint32_t> w = {
            enc_lwz(12, 3, 0),       // lwz r12,0(r3)      vtable of Cam
            enc_lwz(12, 12, 0x10),   // lwz r12,0x10(r12)  method slot
            enc_mtctr(12),           // mtctr r12
            BCTRL,                   // bctrl
            BLR,
        };
        auto ins = collect(B, w);
        std::map<u32, VCall> vc;
        recover_eng_fields(ins, B, db, intra_branch_targets(ins, B), {},
                           nullptr, nullptr, nullptr, &vc);
        auto it = vc.find(ins[3].pc);
        CHECK(it != vc.end() && it->second.type == "Cam" && it->second.vtbl_off == 0x10,
              "virtual-call chain recognized -> (Cam, 0x10)");
        CHECK(vc.size() == 1, "exactly one virtual call recognized");
    }

    // 18. A NON-virtual indirect call (CTR loaded from a plain field, not a vtable chain) is NOT
    //     recognized. lwz r12,8(r3) is Cam::mFov (a scalar field), not a vtable; mtctr/bctrl off it
    //     must produce no VCall.
    {
        std::vector<uint32_t> w = {
            enc_lwz(12, 3, 8),       // lwz r12,8(r3)   scalar field, not offset 0
            enc_mtctr(12),
            BCTRL,
            BLR,
        };
        auto ins = collect(B, w);
        std::map<u32, VCall> vc;
        recover_eng_fields(ins, B, db, intra_branch_targets(ins, B), {},
                           nullptr, nullptr, nullptr, &vc);
        CHECK(vc.empty(), "non-vtable indirect call is not mis-recognized as virtual");
    }

    // 19. Base register NOT an engine type -> no recognition (lwz 0(rA) off an untyped reg).
    {
        std::vector<uint32_t> w = {
            enc_lwz(4, 1, 0x20),     // r4 = some stack value (untyped)
            enc_lwz(12, 4, 0),       // lwz r12,0(r4)
            enc_lwz(12, 12, 0xC),
            enc_mtctr(12),
            BCTRL,
            BLR,
        };
        auto ins = collect(B, w);
        std::map<u32, VCall> vc;
        recover_eng_fields(ins, B, db, intra_branch_targets(ins, B), {},
                           nullptr, nullptr, nullptr, &vc);
        CHECK(vc.empty(), "untyped base -> no virtual-call recognition");
    }

    // 20. RETURN-TYPE seeding: `bl getCam` (a function returning Cam*) types r3, so the following
    //     virtual-call chain on r3 is recognized — the dominant `getModel()->virtual()` pattern.
    {
        const uint32_t GETCAM = 0x80130000u;
        TypeDB rdb = db;                       // reuse Cam layout
        rdb.signatures.clear();                // NO signature seed: the type comes only from the return
        rdb.return_types[GETCAM] = "Cam";
        std::vector<uint32_t> w = {
            enc_bl(B+0, GETCAM),     // r3 = getCam()  -> Cam*
            enc_lwz(12, 3, 0),       // lwz r12,0(r3)      vtable
            enc_lwz(12, 12, 0x10),   // lwz r12,0x10(r12)  method
            enc_mtctr(12),
            BCTRL,
            BLR,
        };
        auto ins = collect(B, w);
        std::map<u32, VCall> vc;
        recover_eng_fields(ins, B, rdb, intra_branch_targets(ins, B), {},
                           nullptr, nullptr, nullptr, &vc);
        auto it = vc.find(ins[4].pc);
        CHECK(it != vc.end() && it->second.type == "Cam" && it->second.vtbl_off == 0x10,
              "return-typed base (getCam()->virtual()) is recognized -> (Cam, 0x10)");
    }

    // 21. The REAL CodeWarrior GameCube virtual-call form is `mtlr m; bclrl` (NOT mtctr/bctrl) —
    //     verified on M3UModel::perform 0x80237930 (unk8->calc(): lwz r12,0(r3); lwz r12,0x10(r12);
    //     mtlr r12; bclrl). Recognize the LR/bclrl chain too.
    {
        std::vector<uint32_t> w = {
            enc_lwz(12, 3, 0),       // lwz r12,0(r3)      vtable of Cam
            enc_lwz(12, 12, 0x10),   // lwz r12,0x10(r12)  method
            enc_mtlr(12),            // mtlr r12
            BCLRL,                   // bclrl
            BLR,
        };
        auto ins = collect(B, w);
        std::map<u32, VCall> vc;
        recover_eng_fields(ins, B, db, intra_branch_targets(ins, B), {},
                           nullptr, nullptr, nullptr, &vc);
        auto it = vc.find(ins[3].pc);
        CHECK(it != vc.end() && it->second.type == "Cam" && it->second.vtbl_off == 0x10,
              "mtlr;bclrl virtual-call chain recognized -> (Cam, 0x10)");
    }

    std::printf("type_recovery_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
