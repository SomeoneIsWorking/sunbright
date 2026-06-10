#include "func_collect.h"
#include <cstring>
#include <map>
#include <algorithm>

std::vector<PPCInstr> collect_function(const uint8_t* data, uint32_t base, size_t size,
                                       uint32_t faddr, uint32_t fend, bool cfg) {
    std::vector<PPCInstr> instrs;
    auto read_word = [&](uint32_t a) -> uint32_t {
        uint32_t w_be;
        std::memcpy(&w_be, data + (a - base), 4);
        return __builtin_bswap32(w_be);
    };
    const uint32_t end_cap = base + (uint32_t)size;
    if (fend > end_cap) fend = end_cap;

    if (cfg) {
        // Full-CFG collection: walk every reachable block within [faddr, fend).
        std::map<uint32_t, PPCInstr> by_addr;
        std::unordered_set<uint32_t> seen;
        std::vector<uint32_t> work{faddr};
        while (!work.empty()) {
            uint32_t a = work.back(); work.pop_back();
            while (a >= faddr && a < fend && !seen.count(a)) {
                seen.insert(a);
                PPCInstr ins = decode(read_word(a), a);
                by_addr[a] = ins;
                if (!ins.lk) { uint32_t t = branch_target(ins); if (t >= faddr && t < fend) work.push_back(t); }
                if (is_unconditional_branch(ins)) break;
                a += 4;
            }
        }
        instrs.reserve(by_addr.size());
        for (auto& kv : by_addr) instrs.push_back(kv.second);
    } else {
        // LINEAR collection: straight through the function body [faddr, fend). An
        // unconditional branch ENDS collection only when it actually LEAVES the function:
        // a tail call (direct target outside [faddr, fend)) or an indirect/return branch
        // (blr/bctr/rfi → branch_target 0, i.e. not in range). An unconditional branch to
        // an address INSIDE the function is an INTERNAL jump — a loop back-edge, or a
        // forward jump to the loop's condition test — and must NOT truncate collection.
        // Truncating there emitted the rest of the body as a mid-function tail_ppc → JIT
        // handoff, which corrupts state non-deterministically. That cut initAllCheckData
        // at `b 0x801915c4` (its grid-populate loop), so the collision grid base was never
        // stored → NULL TBGCheckList → wild reads in addAfterPreNode/update on level load.
        //
        // SECOND truncation class (same symptom): a `blr`/tail EXIT that an EARLIER forward branch
        // JUMPS OVER is also NOT the function end — e.g. a switch where one case ends in `blr` at X
        // and a later case is reached via `bc <X+4>`. func_80337ccc (__va_arg): `blr` @80337d14 is
        // jumped over by `bc 0x80337d18` @80337cf8, so collection stopped at 80337d14 and the rest
        // (80337d18..) became a mid-function tail_ppc → JIT handoff. Under a recomp `bl` caller that
        // handoff siglongjmp-unwinds the caller's C frame and drops its non-volatiles (the boot
        // endRendering→vsnprintf→__va_arg r31 clobber). Fix: track the furthest in-function forward
        // branch target and only treat an exit as the end when nothing branches past it.
        uint32_t max_internal_target = faddr;
        for (uint32_t a = faddr; a < fend; a += 4) {
            instrs.push_back(decode(read_word(a), a));
            const PPCInstr& ins = instrs.back();
            // Record any in-function forward branch target (unconditional via branch_target(),
            // conditional via BC's .target — branch_target() returns 0 for BC).
            uint32_t ut = branch_target(ins);
            if (ut >= faddr && ut < fend && ut > max_internal_target) max_internal_target = ut;
            if (ins.op == PPCOp::BC && ins.target >= faddr && ins.target < fend
                && ins.target > max_internal_target) max_internal_target = ins.target;
            if (is_unconditional_branch(ins)) {
                uint32_t t = branch_target(ins);
                const bool internal = (t >= faddr && t < fend);
                // Break only at a real exit (tail call / blr / bctr / rfi) that nothing jumps past.
                if (!internal && a >= max_internal_target) break;
            }
        }
    }
    return instrs;
}

uint32_t next_func_boundary(uint32_t faddr, const std::vector<uint32_t>& real_funcs, uint32_t cap) {
    auto it = std::upper_bound(real_funcs.begin(), real_funcs.end(), faddr);
    uint32_t fend = (it != real_funcs.end()) ? *it : cap;
    return std::min(fend, cap);
}

std::unordered_set<uint32_t> intra_branch_targets(const std::vector<PPCInstr>& instrs, uint32_t faddr) {
    std::unordered_set<uint32_t> bt;
    const uint32_t func_end = instrs.empty() ? faddr : instrs.back().pc + 4;
    for (const auto& instr : instrs) {
        uint32_t tgt = branch_target(instr);
        if (tgt != 0 && tgt >= faddr && tgt < func_end) bt.insert(tgt);
        if (instr.op == PPCOp::BC && instr.target != 0 && instr.target >= faddr && instr.target < func_end)
            bt.insert(instr.target);
    }
    return bt;
}

std::unordered_set<uint32_t> jumptable_targets(
    const std::vector<PPCInstr>& instrs, uint32_t faddr, uint32_t fend,
    const std::function<bool(uint32_t, uint32_t&)>& read_word) {
    std::unordered_set<uint32_t> out;
    for (size_t k = 0; k < instrs.size(); ++k) {
        if (instrs[k].op != PPCOp::BCCTR || instrs[k].lk) continue;   // computed/tail `bctr` only
        const int lo_j = (int)k - 24 < 0 ? 0 : (int)k - 24;           // short same-region back-scan
        auto stop = [](const PPCInstr& q) { return is_unconditional_branch(q); };

        // 1) `mtctr rCtr` — the register feeding CTR
        int ctr_reg = -1;
        for (int j = (int)k - 1; j >= lo_j; --j) {
            const PPCInstr& q = instrs[j];
            if (q.op == PPCOp::MTSPR && decode_spr(q.spr) == SPR_CTR) { ctr_reg = q.rS; break; }
            if (stop(q)) break;
        }
        if (ctr_reg < 0) continue;

        // 2) the table load into rCtr: `lwzx rCtr,base,index` (indexed) or `lwz rCtr,disp(base)`
        int base_reg = -1; bool indexed = false; int32_t disp = 0;
        for (int j = (int)k - 1; j >= lo_j; --j) {
            const PPCInstr& q = instrs[j];
            if (q.op == PPCOp::LWZX && q.rD == ctr_reg) { base_reg = q.rA; indexed = true;  break; }
            if (q.op == PPCOp::LWZ  && q.rD == ctr_reg) { base_reg = q.rA; disp = q.d; indexed = false; break; }
            if (stop(q)) break;
        }
        if (base_reg < 0) continue;

        // 3) materialize the table base register at the load: forward constant propagation over
        //    the whole function body. The base is often built in the PROLOGUE (lis rX; addi
        //    rBase,rX,lo — note the cross-register addi), dozens of instructions and several
        //    branches before the bctr (TCardManager::cmdLoop 802b16b0: r31 = 0x803DF9C8 set at
        //    +0x14, bctr at +0x5c after an unconditional `b` — the old 24-instruction back-scan
        //    requiring `addi rD,rD` never saw it, the bctr emitted as tail_ppc, and the card
        //    worker thread died on the mid-body JIT handoff = the dead file-select menu).
        //    Linear scan is sound here because we only track constant-forming ops and invalidate
        //    on everything else; a base register is in practice written exactly once.
        uint32_t known[32] = {0}; bool valid[32] = {false};
        // index of the table-load instruction (we want base_reg's value just before it)
        int load_idx = -1;
        for (int j = (int)k - 1; j >= lo_j; --j) {
            const PPCInstr& q = instrs[j];
            if ((q.op == PPCOp::LWZX || q.op == PPCOp::LWZ) && q.rD == ctr_reg) { load_idx = j; break; }
        }
        if (load_idx < 0) continue;
        for (int j = 0; j < load_idx; ++j) {
            const PPCInstr& q = instrs[j];
            switch (q.op) {
            case PPCOp::ADDIS:   // lis / addis
                if (q.rA == 0) { known[q.rD] = (uint32_t)q.uimm << 16; valid[q.rD] = true; }
                else if (valid[q.rA]) { known[q.rD] = known[q.rA] + ((uint32_t)q.uimm << 16); valid[q.rD] = true; }
                else valid[q.rD] = false;
                break;
            case PPCOp::ADDI:    // li / addi
                if (q.rA == 0) { known[q.rD] = (uint32_t)q.simm; valid[q.rD] = true; }
                else if (valid[q.rA]) { known[q.rD] = known[q.rA] + (uint32_t)q.simm; valid[q.rD] = true; }
                else valid[q.rD] = false;
                break;
            case PPCOp::ORI:     // ori rA,rS,uimm (dest is rA)
                if (valid[q.rS]) { known[q.rA] = known[q.rS] | q.uimm; valid[q.rA] = true; }
                else valid[q.rA] = false;
                break;
            case PPCOp::ORIS:
                if (valid[q.rS]) { known[q.rA] = known[q.rS] | ((uint32_t)q.uimm << 16); valid[q.rA] = true; }
                else valid[q.rA] = false;
                break;
            // No GPR written — constants survive.
            case PPCOp::CMP: case PPCOp::CMPI: case PPCOp::CMPL: case PPCOp::CMPLI:
            case PPCOp::STB: case PPCOp::STBX: case PPCOp::STH: case PPCOp::STHX:
            case PPCOp::STW: case PPCOp::STWX: case PPCOp::STMW:
            case PPCOp::STWBRX: case PPCOp::STHBRX: case PPCOp::STWCX:
            case PPCOp::STFS: case PPCOp::STFSX: case PPCOp::STFD: case PPCOp::STFDX:
            case PPCOp::STFIWX: case PPCOp::DCBST:
            case PPCOp::MTSPR: case PPCOp::MTCRF: case PPCOp::MTFSB0: case PPCOp::MTFSB1:
            case PPCOp::MTFSF: case PPCOp::MTFSFI:
            case PPCOp::B: case PPCOp::BA: case PPCOp::BC: case PPCOp::BCA:
            case PPCOp::BCLR: case PPCOp::BCCTR:
            case PPCOp::CRAND: case PPCOp::CRANDC: case PPCOp::CROR: case PPCOp::CRORC:
            case PPCOp::CRXOR: case PPCOp::CRNOR: case PPCOp::CRNAND: case PPCOp::CREQV:
            case PPCOp::MCRF: case PPCOp::MCRXR: case PPCOp::MCRFS:
            case PPCOp::FCMPU: case PPCOp::FCMPO:
                break;
            // Calls clobber the volatile GPRs.
            case PPCOp::BL: case PPCOp::BLA: case PPCOp::BCL: case PPCOp::BCLA:
            case PPCOp::BCLRL: case PPCOp::BCCTRL:
                valid[0] = false;
                for (int r = 3; r <= 12; ++r) valid[r] = false;
                break;
            // Multi-register loads: give up on everything.
            case PPCOp::LMW: case PPCOp::LSWI: case PPCOp::LSWX:
                for (int r = 0; r < 32; ++r) valid[r] = false;
                break;
            default:
                // Anything else conservatively invalidates the registers it could write
                // (rD for loads/arithmetic, rA for logical/rotate dest and update forms).
                valid[q.rD & 31] = false;
                valid[q.rA & 31] = false;
                break;
            }
        }
        if (base_reg > 31 || !valid[base_reg]) continue;
        const uint32_t table = known[base_reg] + (indexed ? 0u : (uint32_t)disp);

        // 4) bound: the nearest preceding `cmpli idx,N` gives the max index ⇒ N+1 entries
        int count = -1;
        for (int j = (int)k - 1; j >= lo_j; --j) {
            const PPCInstr& q = instrs[j];
            if (q.op == PPCOp::CMPLI) { count = (int)q.uimm + 1; break; }
            if (stop(q)) break;
        }

        // 5) read entries; keep the ones that land inside this function's body. With a known count
        //    read exactly that many; otherwise read until the first word that is not a main-RAM
        //    code pointer (a contiguous table of .text pointers).
        //    The constant-propagated base makes a wrong-but-plausible table address possible in
        //    principle, so validate hard: every entry up to the known count must be a code pointer;
        //    one bad word rejects the whole table (better a tail_ppc than wrong labels).
        const int limit = (count > 0 && count <= 256) ? count : 64;
        std::unordered_set<uint32_t> cand;
        bool reject = false;
        for (int e = 0; e < limit; ++e) {
            uint32_t w;
            if (!read_word(table + (uint32_t)e * 4, w)) { reject = count > 0; break; }
            if (w >= faddr && w < fend) cand.insert(w);
            else if (w < 0x80003000u || w >= 0x81800000u) { reject = count > 0; if (count < 0) break; }
        }
        if (!reject) out.insert(cand.begin(), cand.end());
    }
    return out;
}
