// Generator for the TAILORED-RECOMP de-risk slice (the field-access half of the
// game<->engine boundary — see docs/ARCHITECTURE_TARGET.md). Now driven by the REAL
// seeded type-recovery pass (tools/recompiler/type_recovery.cpp), not a hand stub.
//
// It hand-assembles ONE realistic recompiled "game accessor" and runs it through the
// SAME decoder + collection + recovery + emitter the production recompiler uses, twice:
//
//   * ORACLE  (eng_fields empty)  -> raw guest-layout emission (MEM_R*/MEM_W*, big-endian)
//                                    = what today's recompiler / Dolphin produces.
//   * TAILORED (eng_fields from recovery) -> host-native emission: field accesses baked
//                                    as direct host-struct member access of real
//                                    `port/`-style engine objects.
//
// The function exercises the patterns that BREAK naive propagation and the ones the first
// slice left open:
//   +0  stw  r3, 8(r1)    ; spill `this` to the local frame (prologue-save pattern)
//   +4  lwz  r4, 4(r3)    ; r4 = this->mNext   (a NESTED engine pointer -> a HANDLE)
//   +8  lfs  f1, 8(r4)    ; f1 = this->mNext->mFov   (CHAINED field read through it)
//   +12 bl   eng_scale    ; f1 *= 2   (engine call through the boundary; clobbers volatiles)
//   +16 lwz  r31, 8(r1)   ; reload `this` from the frame (survives the call)
//   +20 stfs f1, 8(r31)   ; this->mFov = f1   (field write via the reloaded pointer)
//   +24 blr
//
// field_slice_test.cpp compiles both bodies and proves TAILORED == ORACLE end-to-end.
#include "../../tools/recompiler/ppc_decoder.h"
#include "../../tools/recompiler/c_emitter.h"
#include "../../tools/recompiler/func_collect.h"
#include "../../tools/recompiler/type_recovery.h"
#include "../../tools/recompiler/type_db.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// ── PPC encoders (only what the slice needs) ────────────────────────────────────────
static uint32_t enc_stw (int rs, int ra, int16_t d) { return (36u<<26)|(rs<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_lwz (int rd, int ra, int16_t d) { return (32u<<26)|(rd<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_lfs (int frd,int ra, int16_t d) { return (48u<<26)|(frd<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_stfs(int frs,int ra, int16_t d) { return (52u<<26)|(frs<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_bl  (uint32_t from, uint32_t to) { int32_t dl=(int32_t)(to-from); return (18u<<26)|((uint32_t)dl & 0x03fffffc)|1u; }
static constexpr uint32_t BLR = 0x4e800020u;

// The engine fn the game calls through the boundary (bridged in the test). Layout-agnostic
// scalar so ORACLE and TAILORED invoke the SAME host code.
static constexpr uint32_t ENG_SCALE = 0x80009000u;

static std::vector<uint32_t> game_words(uint32_t base) {
    return {
        enc_stw(3, 1, 8),               // +0   spill this
        enc_lwz(4, 3, 4),               // +4   r4 = this->mNext  (engine ptr -> handle)
        enc_lfs(1, 4, 8),               // +8   f1 = this->mNext->mFov
        enc_bl(base + 12, ENG_SCALE),   // +12  f1 = eng_scale(f1)
        enc_lwz(31, 1, 8),              // +16  reload this
        enc_stfs(1, 31, 8),             // +20  this->mFov = f1
        BLR,                            // +24
    };
}

// The host engine layout + the accessor's signature, seeded as the real recompiler would
// from the decomp headers + symbol map. Type names ARE the host C++ struct names. The
// guest field offsets are COMPUTED by abi_layout (GameCube ABI) from the field list — not
// hand-entered — via build_engine_layout: a polymorphic EngineCam yields mNext@4, mFov@8,
// mFlags@12 (guest), matching the host struct's mNext@8/mFov@16/mFlags@20 by name.
static TypeDB make_db(uint32_t base) {
    TypeDB db;
    db.layouts["EngineCam"] = build_engine_layout({
        { "mNext",  FKind::Ptr, "EngineCam" },   // nested engine pointer
        { "mFov",   FKind::F32, ""          },
        { "mFlags", FKind::I32, ""          },
    }, /*polymorphic=*/true);
    db.signatures[base] = { { 3, "EngineCam" } };    // this in r3
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
    if (tailored)
        ctx.eng_fields = recover_eng_fields(ctx.instrs, base, make_db(base),
                                            ctx.branch_targets, /*jumptable=*/{});

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
        f << "// AUTO-GENERATED by field_slice_gen — the de-risk slice. DO NOT EDIT.\n" << body;
        std::printf("[field_slice_gen] wrote %s (%zu bytes)\n", path.c_str(), body.size());
    };
    write("slice_oracle.inc",   oracle);
    write("slice_tailored.inc", tailored);

    std::printf("\n----- ORACLE (guest-layout) -----\n%s\n", oracle.c_str());
    std::printf("----- TAILORED (host-native, recovered) -----\n%s\n", tailored.c_str());
    return 0;
}
