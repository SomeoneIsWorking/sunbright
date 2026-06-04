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
        for (uint32_t a = faddr; a < fend; a += 4) {
            instrs.push_back(decode(read_word(a), a));
            if (is_unconditional_branch(instrs.back())) {
                uint32_t t = branch_target(instrs.back());
                const bool internal = (t >= faddr && t < fend);
                if (!internal) break;   // tail call / blr / bctr / rfi → real function exit
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
