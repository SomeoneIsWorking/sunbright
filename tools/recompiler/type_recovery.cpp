#include "type_recovery.h"

#include <array>
#include <deque>
#include <unordered_map>

// Seeded type recovery — see type_recovery.h for the rationale and the SHARP EDGE note.
//
// This is a forward dataflow analysis over the function's control-flow graph (built from the
// branch-target / jump-table sets). The lattice value carried at each program point is:
//   reg_type[r]   : engine type name held in GPR r ("" = not an engine pointer)
//   frame_slots[d]: engine type spilled to the local frame at displacement d off r1
//                   (the dominant real pattern: the compiler saves `this` to the stack in the
//                   prologue and reloads it into a non-volatile after a call).
// At a control-flow JOIN the incoming states are MET: a register keeps its type only when
// every predecessor agrees, otherwise it drops to unknown (a frame slot likewise). This is the
// sound treatment of branches/merges/loops the earlier single straight-line pass lacked — a
// disagreement now yields a (detectable) MISS rather than silently carrying a wrong type across
// an edge. Analysis runs at instruction granularity (each instruction is a CFG node), iterated
// to a fixpoint; field sites are then recorded in a final pass from each instruction's stable
// entry state.

namespace {

struct State {
    std::array<std::string, 32> reg;        // GPR -> engine type ("" = none)
    std::map<int, std::string>  frame;      // r1 displacement -> engine type
    bool defined = false;                   // has this point received any incoming state yet?

    bool operator==(const State& o) const { return reg == o.reg && frame == o.frame; }
};

// Meet `in` into `acc` (acc = acc ⊓ in). First contribution copies; later ones intersect.
void meet_into(State& acc, const State& in) {
    if (!in.defined) return;
    if (!acc.defined) { acc = in; acc.defined = true; return; }
    for (int r = 0; r < 32; ++r)
        if (acc.reg[r] != in.reg[r]) acc.reg[r].clear();      // disagree -> unknown
    std::map<int, std::string> merged;
    for (const auto& [d, ty] : acc.frame) {
        auto it = in.frame.find(d);
        if (it != in.frame.end() && it->second == ty) merged.emplace(d, ty);
    }
    acc.frame.swap(merged);
}

// Which GPR (if any) this op overwrites with a value we don't model as type-preserving.
// (Type-PRESERVING/CONSUMING ops are handled explicitly in `apply`.)
int clobbered_gpr(const PPCInstr& i) {
    switch (i.op) {
    case PPCOp::ADDIS: case PPCOp::ADD: case PPCOp::ADDO: case PPCOp::ADDC:
    case PPCOp::ADDIC: case PPCOp::ADDE: case PPCOp::ADDME: case PPCOp::ADDZE:
    case PPCOp::SUBF: case PPCOp::SUBFIC: case PPCOp::SUBFC: case PPCOp::SUBFE:
    case PPCOp::SUBFZE: case PPCOp::SUBFME: case PPCOp::NEG:
    case PPCOp::MULLI: case PPCOp::MULLW: case PPCOp::MULHW: case PPCOp::MULHWU:
    case PPCOp::DIVW: case PPCOp::DIVWU: case PPCOp::MFSPR: case PPCOp::MFCR:
    case PPCOp::LBZU: case PPCOp::LHZU: case PPCOp::LWZU: case PPCOp::LHAU:
    case PPCOp::LBZX: case PPCOp::LHZX: case PPCOp::LWZX: case PPCOp::LHAX:
    case PPCOp::MFTB:
        return i.rD;
    case PPCOp::AND: case PPCOp::ANDI_DOT: case PPCOp::ANDIS_DOT: case PPCOp::ANDC:
    case PPCOp::ORI: case PPCOp::ORIS: case PPCOp::ORC: case PPCOp::XOR:
    case PPCOp::XORI: case PPCOp::XORIS: case PPCOp::NOR: case PPCOp::NAND:
    case PPCOp::EQV: case PPCOp::EXTSB: case PPCOp::EXTSH: case PPCOp::CNTLZW:
    case PPCOp::RLWINM: case PPCOp::RLWIMI: case PPCOp::RLWNM:
    case PPCOp::SLW: case PPCOp::SRW: case PPCOp::SRAW: case PPCOp::SRAWI:
        return i.rA;
    default:
        return -1;
    }
}

bool is_call(const PPCInstr& i) {
    return i.lk && (i.op == PPCOp::B || i.op == PPCOp::BC ||
                    i.op == PPCOp::BCCTR || i.op == PPCOp::BCLR);
}

// Apply instruction `i` to state `s`. If `out` is non-null, record any typed field site; if
// `gaps` is non-null, record a "typed base, unmapped offset" miss (the dangerous case).
void apply(const PPCInstr& i, State& s, const TypeDB& db,
           std::map<u32, EngField>* out, std::vector<u32>* gaps) {
    auto field_at = [&](int base_reg, int disp) -> std::string {
        const std::string& ty = s.reg[base_reg];
        if (ty.empty()) return "";
        auto L = db.layouts.find(ty);
        if (L == db.layouts.end()) return "";
        auto f = L->second.fields.find(disp);
        if (f == L->second.fields.end()) {                     // typed base, unmapped offset (gap)
            if (gaps) gaps->push_back(i.pc);
            return "";
        }
        if (out) (*out)[i.pc] = EngField{ ty, f->second.member, f->second.nested_type };
        return f->second.nested_type;                          // nested engine ptr -> chaining
    };

    switch (i.op) {
    case PPCOp::OR:                                            // mr rD,rS == or rA,rS,rS
        s.reg[i.rA] = (i.rS == i.rB) ? s.reg[i.rS] : std::string();
        break;
    case PPCOp::ADDI:                                          // addi rD,rA,0 = ptr copy
        s.reg[i.rD] = (i.rA != 0 && i.simm == 0) ? s.reg[i.rA] : std::string();
        break;

    case PPCOp::STW:
        if (i.rA == 1) {                                       // spill to local frame
            if (s.reg[i.rS].empty()) s.frame.erase((int)i.d);
            else s.frame[(int)i.d] = s.reg[i.rS];
        } else field_at(i.rA, (int)i.d);
        break;
    case PPCOp::LWZ:
        if (i.rA == 1) {                                       // reload from local frame
            auto it = s.frame.find((int)i.d);
            s.reg[i.rD] = (it == s.frame.end()) ? std::string() : it->second;
        } else s.reg[i.rD] = field_at(i.rA, (int)i.d);
        break;

    case PPCOp::LBZ: case PPCOp::LHZ: case PPCOp::LHA:
        field_at(i.rA, (int)i.d);
        s.reg[i.rD].clear();                                   // narrow scalar: never a ptr
        break;
    case PPCOp::STB: case PPCOp::STH:
        if (i.rA != 1) field_at(i.rA, (int)i.d);
        break;

    case PPCOp::LFS: case PPCOp::LFD:
        field_at(i.rA, (int)i.d);
        break;
    case PPCOp::STFS: case PPCOp::STFD:
        if (i.rA != 1) field_at(i.rA, (int)i.d);
        break;

    default:
        if (is_call(i))                                        // calls clobber EABI volatiles
            for (int r = 3; r <= 12; r++) s.reg[r].clear();
        else {
            int rc = clobbered_gpr(i);
            if (rc >= 0) s.reg[rc].clear();
        }
        break;
    }
}

}  // namespace

std::map<u32, EngField> recover_eng_fields(const std::vector<PPCInstr>& instrs,
                                           u32 func_addr, const TypeDB& db,
                                           const std::unordered_set<u32>& branch_targets,
                                           const std::unordered_set<u32>& jumptable_targets,
                                           std::vector<u32>* unmapped) {
    std::map<u32, EngField> out;
    const int n = (int)instrs.size();
    if (n == 0) return out;

    // pc -> instruction index, for resolving branch targets.
    std::unordered_map<u32, int> idx_of;
    idx_of.reserve(n * 2);
    for (int k = 0; k < n; ++k) idx_of[instrs[k].pc] = k;

    // Successor edges (CFG), mirroring the emitter's branch lowering.
    std::vector<std::vector<int>> succ(n);
    auto add_target = [&](u32 tgt, std::vector<int>& v) {
        auto it = idx_of.find(tgt);
        if (it != idx_of.end()) v.push_back(it->second);
    };
    for (int k = 0; k < n; ++k) {
        const PPCInstr& i = instrs[k];
        std::vector<int>& v = succ[k];
        bool fallthrough = (k + 1 < n);
        switch (i.op) {
        case PPCOp::B:
            if (i.lk) { if (fallthrough) v.push_back(k + 1); }       // bl: call returns -> fall
            else if (branch_targets.count(i.target)) add_target(i.target, v);  // b intra
            // else: tail/return out of the function -> no intra successor
            break;
        case PPCOp::BC:
            if (i.lk) { if (fallthrough) v.push_back(k + 1); }       // bcl: call -> fall
            else {                                                    // conditional: both edges
                if (branch_targets.count(i.target)) add_target(i.target, v);
                bool always = (i.bo & 0x14) == 0x14;                  // bo ignores CTR and CR
                if (fallthrough && !always) v.push_back(k + 1);
            }
            break;
        case PPCOp::BCLR:
            if (i.lk) { if (fallthrough) v.push_back(k + 1); }        // blrl: call -> fall
            else if (i.bo != 0x14 && fallthrough) v.push_back(k + 1); // cond blr: fall remains
            // unconditional blr: return -> no successor
            break;
        case PPCOp::BCCTR:
            if (i.lk) { if (fallthrough) v.push_back(k + 1); }        // bctrl: call -> fall
            else if (!jumptable_targets.empty())                      // bctr switch
                for (u32 t : jumptable_targets) add_target(t, v);
            // else: external computed tail branch -> no successor
            break;
        default:
            if (fallthrough) v.push_back(k + 1);
            break;
        }
    }

    // Predecessors.
    std::vector<std::vector<int>> pred(n);
    for (int k = 0; k < n; ++k) for (int t : succ[k]) pred[t].push_back(k);

    // Seed the entry state from the decomp signature (EABI args r3..r10).
    State seed;
    seed.defined = true;
    auto sig = db.signatures.find(func_addr);
    if (sig != db.signatures.end())
        for (const auto& [idx, ty] : sig->second)
            if (idx >= 0 && idx < 32) seed.reg[idx] = ty;

    std::vector<State> in(n), outs(n);

    // Forward dataflow fixpoint.
    std::deque<int> work;
    std::vector<char> queued(n, 0);
    in[0] = seed;
    outs[0] = seed; apply(instrs[0], outs[0], db, nullptr, nullptr);
    work.push_back(0); queued[0] = 1;
    // ensure every node gets visited even if unreachable from a clean entry chain
    for (int k = 1; k < n; ++k) { work.push_back(k); queued[k] = 1; }

    while (!work.empty()) {
        int k = work.front(); work.pop_front(); queued[k] = 0;

        State nin;
        if (k == 0) { nin = seed; }
        else { for (int p : pred[k]) meet_into(nin, outs[p]); }
        if (!nin.defined) continue;                            // not yet reached

        if (in[k].defined && nin == in[k]) continue;           // no change
        in[k] = nin;

        State nout = nin;
        apply(instrs[k], nout, db, nullptr, nullptr);
        nout.defined = true;
        outs[k] = nout;

        for (int t : succ[k]) if (!queued[t]) { work.push_back(t); queued[t] = 1; }
    }

    // Final pass: record field sites from each instruction's stable entry state.
    for (int k = 0; k < n; ++k) {
        if (!in[k].defined) continue;                          // unreachable: nothing to record
        State s = in[k];
        apply(instrs[k], s, db, &out, unmapped);
    }
    return out;
}
