// Generator for the TAILORED-RECOMP de-risk slice (the field-access half of the
// game<->engine boundary — see docs/ARCHITECTURE_TARGET.md "DE-RISK FIRST").
//
// It hand-assembles ONE tiny but REAL recompiled "game function" and runs it through
// the SAME decoder + collection + emitter the production recompiler uses, twice:
//
//   * ORACLE  (eng_fields empty)  -> raw guest-layout emission: the field accesses go
//                                    through MEM_R*/MEM_W* against a big-endian guest
//                                    image, exactly like today's recompiler / Dolphin.
//   * TAILORED (eng_fields seeded) -> host-native emission: the same field accesses are
//                                    baked as direct host-struct member writes/reads of
//                                    a real `port/`-style engine object (host offset /
//                                    endianness / pointer width).
//
// The seed (eng_fields) is produced by a SMALL stand-in for seeded type recovery: the
// function's `this` type is known from its decomp signature (r3 = EngineCam*), the type
// propagates through register copies (mr/addi-0), and at each load/store the base
// register's engine type + the guest displacement select the host member from a
// per-type layout map. This stands in for the eventual decomp-header-driven recovery.
//
// Output: two C function bodies written to scratch/port/. field_slice_test.cpp compiles
// them into one program and proves TAILORED == ORACLE against the same logical object.
#include "../../tools/recompiler/ppc_decoder.h"
#include "../../tools/recompiler/c_emitter.h"
#include "../../tools/recompiler/func_collect.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// ── PPC encoders (only what the slice needs) ────────────────────────────────────────
static uint32_t enc_or  (int rd, int rs, int rb) { return (31u<<26)|(rs<<21)|(rd<<16)|(rb<<11)|(444u<<1); } // mr = or rd,rs,rs
static uint32_t enc_lfs (int frd, int ra, int16_t d) { return (48u<<26)|(frd<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_stfs(int frs, int ra, int16_t d) { return (52u<<26)|(frs<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_bl  (uint32_t from, uint32_t to) { int32_t dl=(int32_t)(to-from); return (18u<<26)|((uint32_t)dl & 0x03fffffc)|1u; }
static constexpr uint32_t BLR = 0x4e800020u;

// The engine function the game code calls through the boundary (a real host fn,
// registered via SUNBRIGHT_BRIDGE in the test). Absolute guest address (the handle the
// recompiler bakes into call_ppc); layout-agnostic scalar so ORACLE and TAILORED invoke
// the SAME host code and the ONLY observable difference under test is the field access.
static constexpr uint32_t ENG_SCALE = 0x80009000u;

// ── Per-type host layout (slice stand-in for decomp-header-derived field maps) ───────
// guest displacement -> host member name, per engine type. EngineCam's host layout has
// an 8-byte pointer member, so mFov sits at host offset 16 while its GUEST offset is 8 —
// the load-bearing divergence the slice must translate correctly.
static const std::map<std::string, std::map<int, std::string>> g_layout = {
    { "EngineCam", { { 8, "mFov" }, { 12, "mFlags" } } },
};

// ── Seeded type recovery (slice stand-in) ────────────────────────────────────────────
// Returns the eng_fields map: load/store pc -> {host type, host member} for every site
// whose base register is a known engine pointer at a mapped displacement.
static std::map<uint32_t, EngField> seed_fields(const std::vector<PPCInstr>& ins) {
    std::map<uint32_t, EngField> out;
    std::string rt[32];                 // register -> engine type name ("" = not an engine ptr)
    rt[3] = "EngineCam";                // signature seed: this in r3

    auto map_site = [&](const PPCInstr& i, int base_reg) {
        const std::string& ty = rt[base_reg];
        if (ty.empty()) return;
        auto t = g_layout.find(ty);
        if (t == g_layout.end()) return;
        auto m = t->second.find((int)i.d);
        if (m != t->second.end()) out[i.pc] = EngField{ ty, m->second };
    };

    for (const auto& i : ins) {
        switch (i.op) {
        case PPCOp::OR:                                   // mr rD,rS  (or rA,rS,rS)
            rt[i.rA] = (i.rS == i.rB) ? rt[i.rS] : std::string();
            break;
        case PPCOp::ADDI:                                 // addi rD,rA,0 = pointer copy
            rt[i.rD] = (i.rA != 0 && i.simm == 0) ? rt[i.rA] : std::string();
            break;
        case PPCOp::LFS:                                  // float field read (FPR dest, no GPR type change)
            map_site(i, i.rA);
            break;
        case PPCOp::LWZ: case PPCOp::LBZ: case PPCOp::LHZ: // integer field read
            map_site(i, i.rA);
            rt[i.rD] = "";                                // loaded scalar: type unknown
            break;
        case PPCOp::STFS: case PPCOp::STW: case PPCOp::STB: case PPCOp::STH:
            map_site(i, i.rA);                            // field write
            break;
        case PPCOp::B:
            if (i.lk) for (int r = 3; r <= 12; r++) rt[r] = "";  // bl clobbers volatiles
            break;
        default:
            // Conservative: any other op that writes rD invalidates a tracked type. The
            // slice's function uses only the copies above, so nothing else to clear here.
            break;
        }
    }
    return out;
}

// The shared game-function body, laid out at `base`:
//   +0  mr   r31, r3      ; save `this` across the call (r31 is non-volatile)
//   +4  lfs  f1, 8(r3)    ; f1 = this->mFov            [FIELD READ]
//   +8  bl   eng_scale    ; f1 = eng_scale(f1)         [ENGINE CALL through the boundary]
//   +12 stfs f1, 8(r31)   ; this->mFov = f1            [FIELD WRITE]
//   +16 blr
static std::vector<uint32_t> game_words(uint32_t base) {
    return {
        enc_or(31, 3, 3),               // +0
        enc_lfs(1, 3, 8),               // +4
        enc_bl(base + 8, ENG_SCALE),    // +8
        enc_stfs(1, 31, 8),             // +12
        BLR,                            // +16
    };
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
    if (tailored) ctx.eng_fields = seed_fields(ctx.instrs);

    std::ostringstream ss;
    CEmitter em(ss);
    em.emit_function(ctx);
    return ss.str();
}

int main(int argc, char** argv) {
    const char* dir = (argc > 1) ? argv[1] : "scratch/port";

    // ORACLE and TAILORED at distinct base addresses so their func_<addr> names don't
    // collide when compiled into the same test TU.
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

    // Echo both so the emitter's tailored output is visible in the build log.
    std::printf("\n----- ORACLE (guest-layout) -----\n%s\n", oracle.c_str());
    std::printf("----- TAILORED (host-native) -----\n%s\n", tailored.c_str());
    return 0;
}
