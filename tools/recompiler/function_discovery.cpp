#include "function_discovery.h"

#include <algorithm>
#include <cstring>

std::vector<u32> find_call_targets(const u8* text, u32 base_addr, u32 size) {
    std::vector<u32> targets;
    for (u32 offset = 0; offset + sizeof(u32) <= size; offset += sizeof(u32)) {
        u32 word_be;
        std::memcpy(&word_be, text + offset, sizeof(word_be));
        const u32 pc = base_addr + offset;
        const PPCInstr instr = decode(__builtin_bswap32(word_be), pc);

        if ((instr.op == PPCOp::B || instr.op == PPCOp::BC) && instr.lk) {
            targets.push_back(instr.target);
        }
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    return targets;
}

std::vector<u32> find_functions(const u8* text, u32 base_addr, u32 size) {
    std::vector<u32> functions{base_addr};
    for (const u32 target : find_call_targets(text, base_addr, size)) {
        if (target >= base_addr && target < base_addr + size) {
            functions.push_back(target);
        }
    }

    std::sort(functions.begin(), functions.end());
    functions.erase(std::unique(functions.begin(), functions.end()), functions.end());
    return functions;
}
