// STEP-0 COMPILE GATE generator (docs/re_notes/j3d_subsystem_ownership_plan.md STEP 0, Option A).
//
// Reproduces — and proves FIXED — the blocker that gated every tailored flip: a generated
// engine-types TU did not compile, because pulling the decomp host-struct headers into the
// generated TU collides with runtime/cpu_state.h (the `u64` typedef clash: uint64_t==unsigned long
// vs decomp `unsigned long long`). Option A splits the work: the generated GAME TU only CALLS
// extern accessor thunks (no host type named), and a separate port-compiled ACCESSOR TU defines
// them with the decomp headers.
//
// This generator runs a synthetic recompiled function that CONSTRUCTS + field-accesses a REAL
// engine type (JUTTexture) through the SAME emitter the production recompiler uses, and writes the
// three real artifacts exactly as tools/recompiler/main.cpp does:
//   * flip_funcs.cpp      — the GAME TU (cpu_state.h + intrinsics.h + eng_accessors.h, NO decomp)
//   * eng_accessors.h     — extern "C" decls for the thunks
//   * eng_accessors.cpp   — the thunk DEFS + #includes of the real JUTTexture/JUTPalette headers
// run_flip_compile_test.sh then compiles the GAME TU without the decomp headers and the ACCESSOR
// TU with them, and links — the end-to-end STEP-0 proof.
#include "../../tools/recompiler/ppc_decoder.h"
#include "../../tools/recompiler/c_emitter.h"
#include "../../tools/recompiler/func_collect.h"
#include "../../tools/recompiler/type_recovery.h"
#include "../../tools/recompiler/type_db_build.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

static uint32_t enc_stw (int rs, int ra, int16_t d) { return (36u<<26)|(rs<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_lwz (int rd, int ra, int16_t d) { return (32u<<26)|(rd<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_lhz (int rd, int ra, int16_t d) { return (40u<<26)|(rd<<21)|(ra<<16)|(uint16_t)d; }
static uint32_t enc_addi(int rd, int ra, int16_t si){ return (14u<<26)|(rd<<21)|(ra<<16)|(uint16_t)si; }
static uint32_t enc_bl  (uint32_t from, uint32_t to){ int32_t dl=(int32_t)(to-from); return (18u<<26)|((uint32_t)dl & 0x03fffffc)|1u; }
static constexpr uint32_t BLR = 0x4e800020u;

static constexpr uint32_t OPNEW = 0x802c3ba4u;   // operator new
static constexpr uint32_t METHOD = 0x802ca640u;  // a bridged JUTTexture method (storeTIMG)

int main(int argc, char** argv) {
    const char* dir = (argc > 1) ? argv[1] : "scratch/port";
    const uint32_t B = 0x80100000u;

    // r3 = new JUTTexture; <inlined ctor writes>; bridged method; a stack temp; field reads/writes.
    std::vector<uint32_t> w = {
        enc_bl (B+0,  OPNEW),    // +0   r3 = operator new(sizeof JUTTexture)  -> sbnew_<T>()
        enc_stw(0, 3, 0x28),     // +4   mEmbPalette = 0   (engine-ptr inlined ctor write)
        enc_bl (B+8,  METHOD),   // +8   storeTIMG(this=r3)   (bridged call_ppc)
        enc_lwz(4, 3, 0x20),     // +12  r4 = mTexInfo   (guest-data ptr -> lwzg)
        enc_lhz(5, 3, 0x3c),     // +16  r5 = mWidth     (scalar u16  -> lhz)
        enc_lwz(6, 3, 0x28),     // +20  r6 = mEmbPalette(engine ptr -> lwzp handle)
        enc_stw(4, 3, 0x24),     // +24  mTexData = r4   (guest-data ptr -> stwg)
        enc_addi(7, 1, 0x40),    // +28  r7 = &stackTemp (Pattern C -> SbDynStackObj handle)
        BLR,
    };
    std::vector<uint8_t> bytes(w.size()*4);
    for (size_t k=0;k<w.size();++k){ uint32_t be=__builtin_bswap32(w[k]); std::memcpy(&bytes[k*4],&be,4); }

    EmitContext ctx;
    ctx.func_addr = B;
    ctx.instrs = collect_function(bytes.data(), B, bytes.size(), B, B+(uint32_t)w.size()*4, /*cfg=*/false);
    ctx.branch_targets = intra_branch_targets(ctx.instrs, B);
    // Seed the recognized construction sites + typed field accesses (what type-recovery produces
    // for a real flip; hand-seeded here with the CONFIRMED JUTTexture layout so the gate needs no
    // DOL). mEmbPalette is an engine-object pointer; mTexInfo/mTexData are guest-data pointers.
    ctx.alloc_sites[B+0]  = "JUTTexture";   // heap new
    ctx.alloc_sites[B+28] = "JUTTexture";   // stack temp
    ctx.eng_fields[B+4]   = EngField{ "JUTTexture", "mEmbPalette", "JUTPalette", false };
    ctx.eng_fields[B+12]  = EngField{ "JUTTexture", "mTexInfo",    "",           true  };
    ctx.eng_fields[B+16]  = EngField{ "JUTTexture", "mWidth",      "",           false };
    ctx.eng_fields[B+20]  = EngField{ "JUTTexture", "mEmbPalette", "JUTPalette", false };
    ctx.eng_fields[B+24]  = EngField{ "JUTTexture", "mTexData",    "",           true  };

    EngAccessorTable accessors;
    std::ofstream body(std::string(dir) + "/flip_body.inc");
    { CEmitter em(body, &accessors); em.emit_function(ctx); }
    body.close();

    // The GAME TU: same include environment as generated/functions.h (NO decomp headers).
    {
        std::ofstream f(std::string(dir) + "/flip_funcs.cpp");
        f << "// AUTO-GENERATED flip-compile gate GAME TU. DO NOT EDIT.\n"
             "#include \"cpu_state.h\"\n#include \"intrinsics.h\"\n#include \"eng_accessors.h\"\n"
             "#include <cstdint>\n#include <cmath>\n\n";
        std::ifstream in(std::string(dir) + "/flip_body.inc");
        f << in.rdbuf();
    }

    // Resolve the real decomp headers for the flipped types (mirror main.cpp).
    auto built = build_type_db({ "JUTTexture", "JUTPalette" }, "reference/sms/include",
                               "reference/sms_gmse01_funcs.txt");
    std::string pre = "reference/sms/include/";
    std::set<std::string> hdrs;
    for (auto& [ty, path] : built.type_headers) {
        std::string rel = path;
        if (rel.rfind(pre, 0) == 0) rel = rel.substr(pre.size());
        if (!rel.empty()) hdrs.insert(rel);
    }

    {
        std::ofstream h(std::string(dir) + "/eng_accessors.h");
        h << "// AUTO-GENERATED. DO NOT EDIT.\n#pragma once\n#include <cstdint>\n#include <cstddef>\n\n";
        for (auto& [s, d] : accessors.by_symbol) h << d.decl << "\n";
    }
    {
        std::ofstream f(std::string(dir) + "/eng_accessors.cpp");
        f << "// AUTO-GENERATED port-compiled accessor TU. DO NOT EDIT.\n"
             "#include \"eng_accessor_rt.h\"\n";
        for (auto& rel : hdrs) f << "#include <" << rel << ">\n";
        f << "\n";
        for (auto& [s, d] : accessors.by_symbol) f << d.def << "\n";
    }

    std::printf("[flip_compile_gen] %zu accessor thunks, %zu header(s): ",
                accessors.by_symbol.size(), hdrs.size());
    for (auto& h : hdrs) std::printf("%s ", h.c_str());
    std::printf("\n");
    if (accessors.by_symbol.empty()) { std::fprintf(stderr, "ERROR: no accessors generated\n"); return 1; }
    if (hdrs.empty()) { std::fprintf(stderr, "ERROR: no decomp headers resolved for the flipped types\n"); return 1; }
    return 0;
}
