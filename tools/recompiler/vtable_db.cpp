#include "vtable_db.h"
#include "ppc_decoder.h"
#include "func_sig.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>

// See vtable_db.h. DOL-anchored construction: ctor disasm -> vptr -> slots -> symbols.

namespace {

// `<hexaddr> <mangled>` symbol file -> addr->name, plus a sorted addr list (function bounds).
struct SymTab {
    std::map<u32, std::string> by_addr;
    std::vector<u32>           addrs;          // sorted unique function starts
};

SymTab load_symbols(const std::string& path) {
    SymTab t;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string addr_s, name;
        if (!(ls >> addr_s >> name)) continue;
        u32 a;
        try { a = (u32)std::stoul(addr_s, nullptr, 16); } catch (...) { continue; }
        t.by_addr[a] = name;
        t.addrs.push_back(a);
    }
    std::sort(t.addrs.begin(), t.addrs.end());
    t.addrs.erase(std::unique(t.addrs.begin(), t.addrs.end()), t.addrs.end());
    return t;
}

// The first function start strictly after `addr` (the body upper bound), or addr+0x1000 cap.
u32 next_boundary(const SymTab& st, u32 addr) {
    auto it = std::upper_bound(st.addrs.begin(), st.addrs.end(), addr);
    u32 cap = addr + 0x1000;
    return (it == st.addrs.end()) ? cap : std::min(*it, cap);
}

bool is_code_ptr(const DOL& dol, u32 addr) {
    if (addr < 0x80000000u || (addr & 3)) return false;
    const DOLSection* s = dol.section_at(addr);
    return s && s->is_text;
}

// Disassemble the constructor at `ctor` and return the vptr it stores at 0(this) — the LAST
// such store wins (most-derived vtable, after any inlined base-ctor vptr store). 0 if none.
u32 vptr_from_ctor(const DOL& dol, const SymTab& st, u32 ctor) {
    const u32 end = next_boundary(st, ctor);
    std::array<u32, 32>  imm{};        // tracked immediate value per GPR
    std::array<bool, 32> imm_ok{};     // is imm[r] a known constant?
    std::array<bool, 32> is_this{};    // does GPR r alias the incoming `this` (r3 @ entry)?
    is_this[3] = true;
    u32 vptr = 0;
    for (u32 pc = ctor; pc < end; pc += 4) {
        PPCInstr i = decode(dol.read_u32(pc), pc);
        auto setimm = [&](int r, u32 v) { if (r >= 0 && r < 32) { imm[r] = v; imm_ok[r] = true; } };
        auto clrimm = [&](int r) { if (r >= 0 && r < 32) imm_ok[r] = false; };
        switch (i.op) {
        case PPCOp::ADDIS:                                  // lis rD / addis rD,rA
            if (i.rA == 0) setimm(i.rD, (u32)(i.simm << 16));
            else if (imm_ok[i.rA]) setimm(i.rD, imm[i.rA] + (u32)(i.simm << 16));
            else clrimm(i.rD);
            is_this[i.rD] = false;
            break;
        case PPCOp::ADDI:                                   // li / addi rD,rA,SIMM
            if (i.rA == 0) setimm(i.rD, (u32)i.simm);
            else if (imm_ok[i.rA]) setimm(i.rD, imm[i.rA] + (u32)i.simm);
            else clrimm(i.rD);
            is_this[i.rD] = (i.rA != 0 && i.simm == 0 && is_this[i.rA]);  // ptr copy
            break;
        case PPCOp::ORI:                                    // ori rA,rS,UIMM
            if (imm_ok[i.rS]) setimm(i.rA, imm[i.rS] | i.uimm); else clrimm(i.rA);
            is_this[i.rA] = false;
            break;
        case PPCOp::OR:                                     // mr rA,rS == or rA,rS,rS
            if (i.rS == i.rB) { is_this[i.rA] = is_this[i.rS];
                                if (imm_ok[i.rS]) setimm(i.rA, imm[i.rS]); else clrimm(i.rA); }
            else { is_this[i.rA] = false; clrimm(i.rA); }
            break;
        case PPCOp::STW:
            if (i.d == 0 && i.rA < 32 && is_this[i.rA] && imm_ok[i.rS])
                vptr = imm[i.rS];                           // store vptr to 0(this)
            break;
        default: {
            // any other op that defines a GPR invalidates our tracking of it
            // (calls clobber volatiles; conservatively clear r3..r12 on a link branch)
            if (i.lk && (i.op == PPCOp::B || i.op == PPCOp::BC ||
                         i.op == PPCOp::BCCTR || i.op == PPCOp::BCLR)) {
                for (int r = 3; r <= 12; ++r) { imm_ok[r] = false; is_this[r] = false; }
            }
            break;
        }
        }
    }
    return vptr;
}

// Read the vtable at `vptr`: skip the CodeWarrior header (leading non-code-pointer words,
// the offset-to-top + RTTI), then the consecutive function-pointer slots.
VTable read_vtable(const DOL& dol, const SymTab& st, const std::string& type, u32 vptr) {
    VTable vt;
    vt.type = type;
    vt.vptr = vptr;
    // header: skip leading non-code words (cap 4 to stay sane)
    int hdr = 0;
    while (hdr < 4 && !is_code_ptr(dol, dol.read_u32(vptr + hdr * 4))) ++hdr;
    if (hdr == 4) return vt;                                // no method run found -> not a vtable
    vt.header_size = hdr * 4;
    for (int k = 0; k < 256; ++k) {
        u32 off = (u32)vt.header_size + (u32)k * 4;
        u32 tgt = dol.read_u32(vptr + off);
        if (!is_code_ptr(dol, tgt)) break;                 // run terminator (0 / data / pad)
        VSlot s;
        s.byte_off   = (int)off;
        s.slot_index = k;
        s.target     = tgt;
        auto it = st.by_addr.find(tgt);
        if (it != st.by_addr.end()) {
            s.mangled = it->second;
            FuncSig sig = demangle_signature(it->second);
            if (sig.ok && sig.is_method) {
                s.defining_class = sig.class_leaf;
                s.method = (sig.method_name == "__dt") ? ("~" + sig.class_leaf)
                                                       : sig.method_name;
            }
        }
        vt.slots.push_back(s);
    }
    vt.found = !vt.slots.empty();
    return vt;
}

// Find a constructor address for `type` (leaf): a symbol whose member name is "__ct" and whose
// class leaf == type. Prefer the LATER (higher-addr) ctor — irrelevant for the vptr, but
// deterministic. Returns 0 if none.
u32 find_ctor(const SymTab& st, const std::string& type) {
    u32 best = 0;
    for (const auto& [addr, name] : st.by_addr) {
        if (name.compare(0, 4, "__ct") != 0) continue;
        FuncSig sig = demangle_signature(name);
        if (sig.ok && sig.is_method && sig.method_name == "__ct" && sig.class_leaf == type)
            best = addr;                                    // map is addr-ordered -> last = highest
    }
    return best;
}

}  // namespace

VTableDB build_vtable_db(const std::vector<std::string>& active_types,
                         const DOL& dol,
                         const std::string& funcs_txt_path) {
    VTableDB db;
    SymTab st = load_symbols(funcs_txt_path);
    for (const auto& type : active_types) {
        u32 ctor = find_ctor(st, type);
        if (!ctor) continue;
        u32 vptr = vptr_from_ctor(dol, st, ctor);
        if (!vptr) continue;
        VTable vt = read_vtable(dol, st, type, vptr);
        if (vt.found) db.tables[type] = std::move(vt);
    }
    return db;
}
