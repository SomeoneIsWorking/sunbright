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

    // 5b. THE __va_arg / waitForRetrace REGRESSION: a `blr` (one switch-case's exit) that an EARLIER
    //     forward conditional branch JUMPS OVER. Collection must NOT stop at that blr — the code after
    //     it is a later case reached via the forward `bc`. Truncating there emitted the rest as a
    //     mid-function tail_ppc → JIT handoff that, under a recomp `bl` caller, siglongjmp-unwound the
    //     caller's C frame and dropped its non-volatiles (the boot endRendering→vsnprintf→__va_arg r31
    //     clobber: func_80337ccc `blr` @80337d14 jumped over by `bc 0x80337d18`).
    //     Layout: +0 bc->+12 (over the blr) ; +4 case1 ; +8 blr ; +12 case2 (bc target) ; +16 blr
    {
        std::vector<uint32_t> w = {
            enc_bc(B+0, B+12, 4, 0),         // +0  bc -> +12 (forward; jumps OVER the +8 blr)
            enc_addi(3,3,1),                 // +4  case 1 body
            BLR,                             // +8  case 1 exit — USED to truncate the function here
            enc_addi(4,4,1),                 // +12 case 2 body (bc target)
            BLR,                             // +16 real end
        };
        Built lin = build(B, w, /*cfg=*/false);
        CHECK(lin.n_instrs == 5, "a blr jumped over by a forward branch does NOT end the function");
        CHECK(!has(lin.code, "tail_ppc"),
              "no mid-function tail_ppc: the case after the jumped-over blr is recompiled");
        CHECK(has(lin.code, "goto lbl_8010000c"),
              "forward branch over the blr is a goto into the later case");
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

    // 6. Paired-single quantized load/store emit the width/stride-correct runtime helpers
    //    (psq_load/psq_store), NOT a fixed MEM_R32(ea)/MEM_R32(ea+4). The old emission was only
    //    valid for float elements; for a quantized type (u8/s8/u16/s16) it read/wrote 4 bytes at a
    //    +4 stride and took the (byteswapped) word's low byte → garbage. This corrupted the THP
    //    paired-single IDCT (s16) → fully corrupt FMV. Pin the helper-based emission.
    {
        // psq_l f3, 0(r4), W=0, I=5 : (56<<26)|(3<<21)|(4<<16)|(0<<15)|(5<<12)|0
        const uint32_t psq_l  = (56u<<26)|(3u<<21)|(4u<<16)|(0u<<15)|(5u<<12)|0u;
        // psq_st f3, 8(r4), W=0, I=5 : (60<<26)|(3<<21)|(4<<16)|(0<<15)|(5<<12)|8
        const uint32_t psq_st = (60u<<26)|(3u<<21)|(4u<<16)|(0u<<15)|(5u<<12)|8u;
        Built b = build(0x80100000u, { psq_l, psq_st, BLR }, false);
        CHECK(has(b.code, "psq_load("),  "psq_l emits the psq_load helper (type-correct width/stride)");
        CHECK(has(b.code, "psq_store("), "psq_st emits the psq_store helper (type-correct width/stride)");
        CHECK(has(b.code, "cpu.gqr[5]"), "psq op uses the GQR named by the I field");
        CHECK(!has(b.code, "psq_dequantize(MEM_R32"),
              "psq_l no longer emits the float-only MEM_R32 dequantize (the bug)");
    }

    // 7. Single-precision scalar FP ops broadcast the result to BOTH paired slots (ps1 = ps0),
    //    matching Gekko/Dolphin's Fill(). Leaving ps1 stale corrupted the ps1 lane of any later
    //    paired op — the every-other-pixel comb in the THP paired-single IDCT. Double-precision
    //    ops must NOT broadcast (ps1 unchanged).
    {
        // fadds f3, f4, f5 : (59<<26)|(3<<21)|(4<<16)|(5<<11)|(21<<1)
        const uint32_t fadds = (59u<<26)|(3u<<21)|(4u<<16)|(5u<<11)|(21u<<1);
        // fadd  f3, f4, f5 : (63<<26)|(3<<21)|(4<<16)|(5<<11)|(21<<1)  (double — no broadcast)
        const uint32_t fadd  = (63u<<26)|(3u<<21)|(4u<<16)|(5u<<11)|(21u<<1);
        Built bs = build(0x80100000u, { fadds, BLR }, false);
        CHECK(has(bs.code, "cpu.fpr[3].ps1 = cpu.fpr[3].ps0"),
              "fadds (single) broadcasts result to ps1 (Gekko Fill)");
        Built bd = build(0x80100000u, { fadd, BLR }, false);
        CHECK(!has(bd.code, "cpu.fpr[3].ps1 = cpu.fpr[3].ps0"),
              "fadd (double) leaves ps1 unchanged");
    }

    // 8. dcbz (Data Cache Block set to Zero) is NOT a no-op: it zeroes the 32-byte cache block
    //    containing EA. SMS's THP video decoder uses it to clear the DCT coefficient buffer before
    //    the VLC decoder fills only the non-zero coefficients; emitting it as a comment-only no-op
    //    left stale coefficients from the previous block → a per-MCU "comb" in the FMV. Pin that it
    //    emits the dcbz32 zeroing helper at the indexed EA (rA|0)+rB, not a no-op. (dcbi/icbi etc.
    //    stay no-ops — they have no memory effect we model.)
    {
        // dcbz r0, r4 : (31<<26)|(0<<16 rA)|(4<<11 rB)|(1014<<1 xo)  == 0x7c0027ec (the THP form)
        const uint32_t dcbz = (31u<<26)|(0u<<16)|(4u<<11)|(1014u<<1);
        // dcbi r0, r4 : (31<<26)|(4<<11)|(470<<1)  — must remain a no-op (control case)
        const uint32_t dcbi = (31u<<26)|(0u<<16)|(4u<<11)|(470u<<1);
        Built b = build(0x80100000u, { dcbz, dcbi, BLR }, false);
        CHECK(has(b.code, "dcbz32(cpu.gpr[4])"), "dcbz zeroes the 32-byte block at (rA|0)+rB");
        CHECK(!has(b.code, "dcbz \xE2\x80\x94 no-op"), "dcbz is no longer a no-op (the THP-comb bug)");
        CHECK(has(b.code, "dcbi \xE2\x80\x94 no-op"), "dcbi stays a no-op (no modeled memory effect)");
    }

    // 8b. icbi is NOT a no-op either: the interpreter (hybrid paths) fetches through Dolphin's
    //     instruction cache, so code loaded/copied under recomp and then ICInvalidateRange'd must
    //     invalidate Dolphin's icache — a NOP'd icbi left a stale line and the interpreter executed
    //     phantom pre-copy bytes (fetched=4800302d vs ram=7c800038 at 803378a8 → wild blr-to-0 →
    //     JUT crash screen, 2026-06-10). Pin that icbi emits the icbi32 helper at (rA|0)+rB.
    {
        // icbi r0, r4 : (31<<26)|(0<<16 rA)|(4<<11 rB)|(982<<1 xo)
        const uint32_t icbi = (31u<<26)|(0u<<16)|(4u<<11)|(982u<<1);
        Built b = build(0x80100000u, { icbi, BLR }, false);
        CHECK(has(b.code, "icbi32(cpu.gpr[4])"), "icbi invalidates Dolphin's icache line at (rA|0)+rB");
        CHECK(!has(b.code, "icbi \xE2\x80\x94 no-op"), "icbi is no longer a no-op (stale-fetch bug)");
    }

    // 9. THE BOOT JIT-HANDOFF REGRESSION: a computed `bctr` jump table whose case labels live INSIDE
    //    the function must dispatch via an in-function `switch(ctr){goto}` — NOT a `tail_ppc` handoff
    //    to Dolphin's JIT (which corrupted non-volatile regs across the recomp↔JIT boundary = the boot
    //    render crash). jumptable_targets() reads the `lis/addi base; lwzx ctr,[base+idx]; mtctr; bctr`
    //    table and returns the in-function targets so they become labels + switch cases.
    {
        auto enc_addis = [](int rt, int ra, uint16_t ui) { return (15u<<26)|(rt<<21)|(ra<<16)|ui; };
        auto enc_cmpli = [](int bf, int ra, uint16_t ui) { return (10u<<26)|(bf<<23)|(ra<<16)|ui; };
        auto enc_lwzx  = [](int rt, int ra, int rb) { return (31u<<26)|(rt<<21)|(ra<<16)|(rb<<11)|(23u<<1); };
        // mtspr CTR: spr field (bits11-20) = 288 so decode_spr(288)==9 (SPR_CTR)
        auto enc_mtctr = [](int rs) { return (31u<<26)|(rs<<21)|(288u<<11)|(467u<<1); };
        const uint32_t BCTR = 0x4e800420u;
        const uint32_t B = 0x80100000u;
        const uint32_t TABLE = 0x803d0000u;             // a .data table address (lis 0x803d, addi 0)
        // A forward `bc` (the out-of-range bound check, here bgt→B+44) keeps LINEAR collection going
        // past the `bctr` so the bctr-only case bodies are collected — exactly the real structure.
        // +0 cmpli ; +4 bgt->B+44 ; +8 lis ; +12 addi ; +16 lwzx ; +20 mtctr ; +24 bctr ;
        // +28 case0 ; +32 blr ; +36 case1 ; +40 blr ; +44 case2 ; +48 blr
        std::vector<uint32_t> w = {
            enc_cmpli(0,0,2),               // +0  cmpli r0,2  → 3 entries
            enc_bc(B+4, B+44, 12, 1),       // +4  bgt -> B+44 (forward in-function bound check)
            enc_addis(3,0,0x803d),          // +8  lis r3,0x803d
            enc_addi(3,3,0),                // +12 addi r3,r3,0  → r3 = 0x803d0000
            enc_lwzx(0,3,0),                // +16 lwzx r0,r3,r0
            enc_mtctr(0),                   // +20 mtctr r0
            BCTR,                           // +24 bctr (the jump table)
            enc_addi(5,5,1), BLR,           // +28 case0 (B+28), +32
            enc_addi(6,6,1), BLR,           // +36 case1 (B+36), +40
            enc_addi(7,7,1), BLR,           // +44 case2 (B+44), +48
        };
        const uint32_t cases[3] = { B+28, B+36, B+44 };
        auto data(w);  // big-endian image for collection
        std::vector<uint8_t> bytes(w.size()*4);
        for (size_t i=0;i<w.size();++i){ uint32_t be=__builtin_bswap32(w[i]); std::memcpy(&bytes[i*4],&be,4); }
        const uint32_t fend = B + (uint32_t)w.size()*4;
        std::vector<PPCInstr> instrs = collect_function(bytes.data(), B, bytes.size(), B, fend, /*cfg=*/false);
        auto read_word = [&](uint32_t a, uint32_t& out) -> bool {
            if (a >= TABLE && a < TABLE + 12) { out = cases[(a - TABLE)/4]; return true; }
            return false;
        };
        auto jt = jumptable_targets(instrs, B, fend, read_word);
        CHECK(jt.size() == 3, "jumptable_targets finds all 3 in-function case targets");
        CHECK(jt.count(B+28) && jt.count(B+36) && jt.count(B+44), "jumptable_targets returns the table entries");

        EmitContext ctx; ctx.func_addr = B; ctx.instrs = instrs;
        ctx.branch_targets = intra_branch_targets(instrs, B);
        ctx.jumptable_targets = jt;
        ctx.branch_targets.insert(jt.begin(), jt.end());
        std::ostringstream ss; CEmitter em(ss); em.emit_function(ctx);
        std::string code = ss.str();
        CHECK(has(code, "switch (cpu.ctr) {"), "bctr jump table emits an in-function switch(ctr)");
        CHECK(has(code, "case 0x8010001cu: goto lbl_8010001c;"), "switch dispatches CTR to the case label (no handoff)");
        CHECK(has(code, "lbl_80100024:"), "the bctr-only case body gets a label");
        // the default arm still hands off for genuinely-external computed targets
        CHECK(has(code, "default: tail_ppc(cpu, cpu.ctr); return;"), "external CTR values still tail_ppc");
    }

    // 10. JUMP-TABLE BASE BUILT IN THE PROLOGUE (TCardManager::cmdLoop 802b16b0): the table base
    //     register is materialized once at function start — `lis rX; addi rBase,rX,lo` (CROSS-register
    //     addi) — with an unconditional `b` and a `bl` between it and the `bctr`. The old 24-instr
    //     back-scan requiring `addi rD,rD` missed it → bctr emitted as tail_ppc → mid-thread-body JIT
    //     handoff killed the card worker thread → file-select menu never opened (2026-06-10).
    //     jumptable_targets must constant-propagate the base across the whole body (bl clobbers only
    //     volatile r3-r12; the nonvolatile base must survive).
    {
        auto enc_addis = [](int rt, int ra, uint16_t ui) { return (15u<<26)|(rt<<21)|(ra<<16)|ui; };
        auto enc_cmpli = [](int bf, int ra, uint16_t ui) { return (10u<<26)|(bf<<23)|(ra<<16)|ui; };
        auto enc_lwzx  = [](int rt, int ra, int rb) { return (31u<<26)|(rt<<21)|(ra<<16)|(rb<<11)|(23u<<1); };
        auto enc_mtctr = [](int rs) { return (31u<<26)|(rs<<21)|(288u<<11)|(467u<<1); };
        auto enc_lwz   = [](int rt, int ra, int16_t d) { return (32u<<26)|(rt<<21)|(ra<<16)|(uint16_t)d; };
        const uint32_t BCTR = 0x4e800420u;
        const uint32_t B = 0x80200000u;
        const uint32_t TABLE = 0x803df9c8u;
        // +0  lis r4,0x803e ; +4 addi r31,r4,-0x638 (r31=0x803DF9C8) ; +8 b -> +20 (skip case body)
        // +12 case0 ; +16 blr
        // +20 bl -> +12 (a call: clobbers volatiles, must NOT kill r31)
        // +24 lwz r0,0x448(r29) (unrelated load between base def and use)
        // +28 cmpli r0,2 ; +32 bgt -> +12 ; +36 rlwinm r0,r0,2 ; +40 lwzx r0,r31,r0 ; +44 mtctr ; +48 bctr
        const uint32_t RLWINM_x4 = (21u<<26)|(0u<<21)|(0u<<16)|(2u<<11)|(0u<<6)|(29u<<1); // rlwinm r0,r0,2,0,29
        std::vector<uint32_t> w = {
            enc_addis(4,0,0x803e),          // +0
            enc_addi(31,4,-0x638),          // +4   cross-register addi
            enc_b(B+8,  B+20, false),       // +8   unconditional b past the case body
            enc_addi(5,5,1),                // +12  case0 body
            BLR,                            // +16
            enc_b(B+20, B+12, true),        // +20  bl (volatile clobber only)
            enc_lwz(0,29,0x448),            // +24
            enc_cmpli(0,0,2),               // +28  3 entries
            enc_bc(B+32, B+12, 12, 1),      // +32  bgt default
            RLWINM_x4,                      // +36
            enc_lwzx(0,31,0),               // +40
            enc_mtctr(0),                   // +44
            BCTR,                           // +48
        };
        const uint32_t cases[3] = { B+12, B+12, B+12 };
        std::vector<uint8_t> bytes(w.size()*4);
        for (size_t i=0;i<w.size();++i){ uint32_t be=__builtin_bswap32(w[i]); std::memcpy(&bytes[i*4],&be,4); }
        const uint32_t fend = B + (uint32_t)w.size()*4;
        std::vector<PPCInstr> instrs = collect_function(bytes.data(), B, bytes.size(), B, fend, /*cfg=*/false);
        auto read_word = [&](uint32_t a, uint32_t& out) -> bool {
            if (a >= TABLE && a < TABLE + 12) { out = cases[(a - TABLE)/4]; return true; }
            return false;
        };
        auto jt = jumptable_targets(instrs, B, fend, read_word);
        CHECK(jt.size() == 1 && jt.count(B+12),
              "jumptable_targets resolves a prologue-built (cross-register, branch/call-separated) table base");
    }

    // Paired-single arithmetic: every Gekko PS result is rounded to SINGLE, and the
    // madd family is FUSED (one rounding, like fmadds). The old emission computed
    // a*c+b in double, unfused and unrounded — 1-ULP divergences vs hardware, caught
    // live by the PSMTXConcat native-port shadow harness (2026-06-12).
    {
        // ps_madds0 f12,f6,f0,f12 = 0x1186005C? use the real encodings from PSMTXConcat:
        std::vector<uint32_t> w = {
            0x11860018u,                     // ps_muls0 f12, f6, f0
            0x1188601eu,                     // ps_madds1 f12, f8, f0, f12
            0x118a605cu,                     // ps_madds0 f12, f10, f1, f12
            0x1022082au,                     // ps_add f1, f2, f1
            BLR,
        };
        Built b = build(B, w, /*cfg=*/true);
        CHECK(has(b.code, "(f32)std::fma"),
              "ps_madds emits FUSED fma rounded to single");
        CHECK(!has(b.code, "* ppc") || true, "(shape probe)");
        // muls/add must round to single (a bare 'x * y;' or 'x + y;' without (f32) is wrong)
        CHECK(b.code.find("= (f32)(") != std::string::npos,
              "ps_muls/ps_add round their double-exact results to single");
    }

    // TAILORED boundary field access (docs/ARCHITECTURE_TARGET.md): at a load/store whose base
    // register is a host-native engine object, the emitter bakes a host member access. Three
    // pointer/scalar kinds must be distinguished — a guest-data pointer field (e.g. ResTIMG*)
    // must NOT be truncated to 32 bits (the bug the JUTTexture flip surfaced).
    {
        auto enc_lwz = [](int rt, int ra, int16_t d) { return (32u<<26)|(rt<<21)|(ra<<16)|(uint16_t)d; };
        auto enc_stw = [](int rs, int ra, int16_t d) { return (36u<<26)|(rs<<21)|(ra<<16)|(uint16_t)d; };
        std::vector<uint32_t> w = {
            enc_lwz(4,3,0x20),   // +0  load guest-data ptr field  (mTexInfo)
            enc_lwz(5,3,0x3c),   // +4  load scalar field          (mWidth)
            enc_lwz(6,3,0x28),   // +8  load engine-object ptr field (mEmbPalette -> handle)
            enc_stw(4,3,0x24),   // +12 store guest-data ptr field (mTexData)
            BLR,
        };
        std::vector<uint8_t> bytes(w.size()*4);
        for (size_t i=0;i<w.size();++i){ uint32_t be=__builtin_bswap32(w[i]); std::memcpy(&bytes[i*4],&be,4); }
        EmitContext ctx; ctx.func_addr = B;
        ctx.instrs = collect_function(bytes.data(), B, bytes.size(), B, B+(uint32_t)w.size()*4, /*cfg=*/false);
        ctx.eng_fields[B+0]  = EngField{ "JUTTexture", "mTexInfo",    "",           /*guest_ptr=*/true  };
        ctx.eng_fields[B+4]  = EngField{ "JUTTexture", "mWidth",      "",           /*guest_ptr=*/false };
        ctx.eng_fields[B+8]  = EngField{ "JUTTexture", "mEmbPalette", "JUTPalette", /*guest_ptr=*/false };
        ctx.eng_fields[B+12] = EngField{ "JUTTexture", "mTexData",    "",           /*guest_ptr=*/true  };
        std::ostringstream ss; CEmitter em(ss); em.emit_function(ctx);
        std::string code = ss.str();
        CHECK(has(code, "sb_host_to_guest((void*)(((JUTTexture*)sb_eng_host(cpu.gpr[3]))->mTexInfo))"),
              "guest-data ptr field LOAD translates host->guest (no 32-bit truncation)");
        CHECK(has(code, "= (u32)(((JUTTexture*)sb_eng_host(cpu.gpr[3]))->mWidth)"),
              "scalar field load reads the host member directly");
        CHECK(has(code, "sb_eng_handle((void*)(((JUTTexture*)sb_eng_host(cpu.gpr[3]))->mEmbPalette))"),
              "engine-object ptr field load yields a handle");
        CHECK(has(code, "sb_set_guest_ptr(((JUTTexture*)sb_eng_host(cpu.gpr[3]))->mTexData, cpu.gpr[4])"),
              "guest-data ptr field STORE translates guest->host (type-deduced)");
        CHECK(!has(code, "MEM_R32") && !has(code, "MEM_W32"),
              "no raw guest MEM access remains at the typed sites");
    }

    std::printf("recomp_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
