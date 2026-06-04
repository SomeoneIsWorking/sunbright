// Recompiler unit tests. Built as `sunbright-recomp-test`; run via ctest or directly.
//
// Motivation: the recompiler's structural behaviour (which instructions belong to a function, and
// therefore whether a branch becomes a goto / call_ppc / tail_ppc) repeatedly broke assumptions
// during the HUD work — e.g. "force-CFG makes drawFullSet whole so it call_ppc's the quad emitter".
// These tests pin that behaviour so it can be ASSERTED instead of dump-and-guessed.
//
// Approach: hand-assemble tiny PPC functions as big-endian words, run them through the SAME
// collection (func_collect) + emission (CEmitter) the recompiler uses, and assert on the result.

#include "../ppc_decoder.h"
#include "../c_emitter.h"
#include "../func_collect.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg) do { ++g_checks; if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); ++g_failures; } } while (0)

// ── PPC encoders (just the few ops the tests need) ──────────────────────────────────────────────
static uint32_t enc_addi(int rt, int ra, int16_t si) { return (14u<<26)|(rt<<21)|(ra<<16)|(uint16_t)si; }
static uint32_t enc_b(uint32_t from, uint32_t to, bool lk, bool aa=false) {
    int32_t d = (int32_t)(to - from); return (18u<<26)|((uint32_t)d & 0x03fffffc)|(aa?2:0)|(lk?1:0);
}
static uint32_t enc_bc(uint32_t from, uint32_t to, int bo, int bi) {  // conditional branch
    int32_t d = (int32_t)(to - from); return (16u<<26)|(bo<<21)|(bi<<16)|((uint32_t)d & 0x0000fffc);
}
static constexpr uint32_t BLR = 0x4e800020u;   // blr
static constexpr uint32_t NOP = 0x60000000u;   // ori 0,0,0

// Lay out words at `base`, big-endian, and run collection + emission.
struct Built { std::vector<PPCInstr> instrs; std::string code; size_t n_instrs; };
static Built build(uint32_t base, const std::vector<uint32_t>& words, bool cfg) {
    std::vector<uint8_t> data(words.size()*4);
    for (size_t i = 0; i < words.size(); ++i) {
        uint32_t be = __builtin_bswap32(words[i]);
        std::memcpy(&data[i*4], &be, 4);
    }
    uint32_t fend = base + (uint32_t)words.size()*4;
    Built b;
    b.instrs = collect_function(data.data(), base, data.size(), base, fend, cfg);
    b.n_instrs = b.instrs.size();
    EmitContext ctx; ctx.func_addr = base; ctx.instrs = b.instrs;
    ctx.branch_targets = intra_branch_targets(b.instrs, base);
    std::ostringstream ss; CEmitter em(ss); em.emit_function(ctx);
    b.code = ss.str();
    return b;
}
static bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

int main() {
    const uint32_t B = 0x80100000u;

    // 1. Linear collection truncates at the first unconditional branch; CFG collects the whole body.
    //    Layout:  +0 addi ; +4 b->+12 ; +8 addi(dead-on-linear) ; +12 addi ; +16 blr
    {
        std::vector<uint32_t> w = {
            enc_addi(3,3,1),                 // +0
            enc_b(B+4, B+12, false),         // +4  b -> +12 (skips +8)
            enc_addi(4,4,1),                 // +8  (reached only via fallthrough-less CFG? no — unreachable)
            enc_addi(5,5,1),                 // +12 branch target
            BLR,                             // +16
        };
        Built lin = build(B, w, /*cfg=*/false);
        CHECK(lin.n_instrs == 2, "linear stops at the unconditional branch (addi + b)");

        Built cfg = build(B, w, /*cfg=*/true);
        // CFG follows the b to +12, collecting +0,+4,+12,+16 (not the unreachable +8).
        CHECK(cfg.n_instrs == 4, "CFG collects all reachable blocks across the branch");
        CHECK(has(cfg.code, "goto lbl_8010000c"), "CFG: in-function branch becomes a goto");
        CHECK(!has(cfg.code, "tail_ppc"), "CFG: no tail_ppc for an in-function branch");
        CHECK(has(cfg.code, "return;"), "CFG: function ends in a return (blr)");
    }

    // 2. THE drawFullSet REGRESSION: a bl to a callee that lives in a block AFTER an unconditional
    //    branch. Linear truncates before it (so the call is never emitted → unreachable by overrides);
    //    CFG reaches it and emits call_ppc(callee). This is exactly the HUD quad-emitter bug.
    {
        const uint32_t callee = 0x802cd2ecu;
        std::vector<uint32_t> w = {
            enc_bc(B+0, B+12, 4, 0),         // +0  bc -> +12 (forward, conditional)
            enc_addi(3,3,1),                 // +4
            enc_b(B+8, B+16, false),         // +8  b -> +16 (unconditional — linear stops here)
            enc_addi(4,4,1),                 // +12 (bc target)
            enc_b(B+16, callee, true),       // +16 bl callee   ← in the dropped tail
            BLR,                             // +20
        };
        Built lin = build(B, w, false);
        CHECK(!has(lin.code, "call_ppc(cpu, 0x802cd2ecu)"),
              "linear truncation drops the bl to the callee (the bug)");

        Built cfg = build(B, w, true);
        CHECK(has(cfg.code, "call_ppc(cpu, 0x802cd2ecu)"),
              "force-CFG emits call_ppc to the callee (the fix)");
    }

    // 3. Branch emission forms: bl → call_ppc; blr → return; b to OUTSIDE the function → tail_ppc.
    {
        const uint32_t callee = 0x80123454u, outside = 0x80200000u;   // branch targets are 4-aligned
        std::vector<uint32_t> w = {
            enc_b(B+0, callee, true),        // +0  bl callee
            BLR,                             // +4  blr
        };
        Built b = build(B, w, false);
        CHECK(has(b.code, "call_ppc(cpu, 0x80123454u)"), "bl emits call_ppc");
        CHECK(has(b.code, "cpu.lr = 0x80100004u"),       "bl sets lr to the return address");
        CHECK(has(b.code, "return;"),                    "blr emits a C return");

        std::vector<uint32_t> w2 = { enc_b(B+0, outside, false) };   // b to outside the function
        Built b2 = build(B, w2, false);
        CHECK(has(b2.code, "tail_ppc(cpu, 0x80200000u)"), "b leaving the function emits tail_ppc");
        CHECK(!has(b2.code, "goto"),                      "out-of-function branch is not a goto");
    }

    // 4. Conditional backward branch (a loop) is an intra-function goto under linear collection too.
    {
        std::vector<uint32_t> w = {
            enc_addi(3,3,-1),                // +0  loop top
            enc_bc(B+4, B+0, 4, 0),          // +4  bc -> +0 (backward, conditional)
            BLR,                             // +8
        };
        Built b = build(B, w, false);
        CHECK(has(b.code, "goto lbl_80100000"), "backward conditional branch is a goto");
    }

    std::printf("recomp_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
