#include "func_collect.h"
#include <cstring>
#include <map>

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
        // Legacy LINEAR collection: straight through until the first unconditional branch.
        for (uint32_t a = faddr; a < fend; a += 4) {
            instrs.push_back(decode(read_word(a), a));
            if (is_unconditional_branch(instrs.back())) break;
        }
    }
    return instrs;
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
