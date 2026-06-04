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

    // 1. An unconditional branch to an address INSIDE the function (a forward `b` over a block) is
    //    an internal jump and must NOT truncate linear collection. Linear collects the whole
    //    [faddr,fend) body (incl. the unreachable +8 as dead code); CFG collects only the reachable
    //    blocks. Both turn the in-function branch into a goto, never a tail_ppc.
    //    Layout:  +0 addi ; +4 b->+12 ; +8 addi(unreachable) ; +12 addi ; +16 blr
    {
        std::vector<uint32_t> w = {
            enc_addi(3,3,1),                 // +0
            enc_b(B+4, B+12, false),         // +4  b -> +12 (internal; skips +8)
            enc_addi(4,4,1),                 // +8  unreachable
            enc_addi(5,5,1),                 // +12 branch target
            BLR,                             // +16
        };
        Built lin = build(B, w, /*cfg=*/false);
        CHECK(lin.n_instrs == 5, "linear collects the whole body (internal branch does NOT truncate)");
        CHECK(has(lin.code, "goto lbl_8010000c"), "linear: in-function branch becomes a goto");
        CHECK(!has(lin.code, "tail_ppc"), "linear: no tail_ppc for an in-function branch");

        Built cfg = build(B, w, /*cfg=*/true);
        // CFG follows the b to +12, collecting +0,+4,+12,+16 (not the unreachable +8).
        CHECK(cfg.n_instrs == 4, "CFG collects all reachable blocks across the branch");
        CHECK(has(cfg.code, "goto lbl_8010000c"), "CFG: in-function branch becomes a goto");
        CHECK(!has(cfg.code, "tail_ppc"), "CFG: no tail_ppc for an in-function branch");
        CHECK(has(cfg.code, "return;"), "CFG: function ends in a return (blr)");
    }

    // 2. THE drawFullSet REGRESSION: a bl to a callee in a block AFTER an internal unconditional
    //    branch. The internal-branch truncation bug used to drop it (call never emitted → unreachable
    //    by overrides). Now linear keeps collecting past the internal `b`, so BOTH linear and CFG
    //    emit call_ppc(callee). (This is the HUD quad-emitter bug AND the initAllCheckData class.)
    {
        const uint32_t callee = 0x802cd2ecu;
        std::vector<uint32_t> w = {
            enc_bc(B+0, B+12, 4, 0),         // +0  bc -> +12 (forward, conditional)
            enc_addi(3,3,1),                 // +4
            enc_b(B+8, B+16, false),         // +8  b -> +16 (internal — used to truncate here)
            enc_addi(4,4,1),                 // +12 (bc target)
            enc_b(B+16, callee, true),       // +16 bl callee   ← formerly in the dropped tail
            BLR,                             // +20
        };
        Built lin = build(B, w, false);
        CHECK(has(lin.code, "call_ppc(cpu, 0x802cd2ecu)"),
              "linear no longer truncates at the internal branch → emits the later bl (the fix)");

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

    // 5. THE initAllCheckData REGRESSION (the level-load NULL-collision-list crash). A function
    //    whose body is a loop entered by a forward `b` to the loop-condition test — the classic
    //    "jump to condition, condition branches back to body" layout the compiler emits. Linear
    //    collection USED to truncate at that forward `b`, emitting `tail_ppc(loop_cond)` and handing
    //    the whole grid-populate loop to a mid-function JIT handoff that corrupted state. The body
    //    must stay in the recompiled function: the forward `b` is a goto, the back-edge is a goto,
    //    and there is NO tail_ppc.
    //    Layout: +0 head ; +4 b->+16 (to cond) ; +8/+12 loop body ; +16 bc->+8 (back-edge) ; +20 blr
    {
        std::vector<uint32_t> w = {
            enc_addi(3,3,1),                 // +0  head
            enc_b(B+4, B+16, false),         // +4  b -> +16 (forward, to the loop condition)
            enc_addi(4,4,1),                 // +8  loop body (back-edge target)
            enc_addi(5,5,1),                 // +12 loop body
            enc_bc(B+16, B+8, 12, 0),        // +16 bc -> +8 (loop condition, back-edge)
            BLR,                             // +20 epilogue
        };
        Built lin = build(B, w, /*cfg=*/false);
        CHECK(lin.n_instrs == 6, "initAllCheckData pattern: linear collects the whole loop function");
        CHECK(!has(lin.code, "tail_ppc"),
              "initAllCheckData fix: the loop body is recompiled, NOT a mid-function JIT handoff");
        CHECK(has(lin.code, "goto lbl_80100010"), "forward `b` to the loop condition is a goto");
        CHECK(has(lin.code, "goto lbl_80100008"), "loop back-edge is a goto");
    }

    // 6. fend must come from REAL function boundaries, not pointer-discovered interior labels.
    //    A discovered label between two real functions must collect to the next REAL boundary
    //    (so it's a valid alternate entry), and must NOT shrink the preceding function's fend.
    //    (The JAIBasic audio crash: checkInitDataFile re-truncated at a discovered interior label.)
    {
        const uint32_t cap = 0x80400000u;
        // Two real functions: checkInitDataFile [0x80300f30, 0x803017b0) and the next one.
        std::vector<uint32_t> real = { 0x80300f30u, 0x803017b0u };
        // A real function: fend = the next real boundary.
        CHECK(next_func_boundary(0x80300f30u, real, cap) == 0x803017b0u,
              "real function fend = next real boundary");
        // A discovered interior label inside [0x80300f30, 0x803017b0): fend = next real boundary,
        // NOT the label itself or some nearer discovered entry → collects to the function end.
        CHECK(next_func_boundary(0x80300fa0u, real, cap) == 0x803017b0u,
              "discovered interior label collects to the END of its containing function");
        // Past the last real boundary → capped at section end.
        CHECK(next_func_boundary(0x803017b0u, real, cap) == cap,
              "last function fend = section end (cap)");
    }

    std::printf("recomp_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
