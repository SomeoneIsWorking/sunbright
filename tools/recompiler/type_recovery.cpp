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
// `gaps` is non-null, record a "typed base, unmapped offset" miss (the dangerous case). If
// `alloc_types` is non-null, an instruction whose pc is a resolved engine-CONSTRUCTION site (the
// `operator new` bl, or the interior-stack `addi`) SEEDS its result register with the constructed
// engine type — this is what carries the new object's type forward so the inlined ctor field
// writes and the container store of the handle are typed (see object_identity.md / find_alloc_sites).
void apply(const PPCInstr& i, State& s, const TypeDB& db,
           std::map<u32, EngField>* out, std::vector<u32>* gaps,
           const std::map<u32, std::string>* alloc_types = nullptr) {
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
        if (out) (*out)[i.pc] = EngField{ ty, f->second.member, f->second.nested_type, f->second.guest_ptr };
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

    // Engine-construction seed: at a resolved alloc site the result register becomes the new
    // object's engine type (overriding the clobber above), so the type flows forward from here.
    if (alloc_types) {
        auto a = alloc_types->find(i.pc);
        if (a != alloc_types->end()) {
            if (is_call(i)) s.reg[3] = a->second;              // operator new -> return in r3
            else if (i.op == PPCOp::ADDI) s.reg[i.rD] = a->second;  // interior-stack temp -> rD
        }
    }
}

// Resolved direct-call target of a linking branch (0 if indirect).
u32 call_target(const PPCInstr& i) {
    if (i.lk && i.op == PPCOp::B) return i.target;
    return 0;
}

// ── Engine-CONSTRUCTION recognition (forward "allocation origin" analysis) ────────────────────
// The type of a freshly-made engine object is not known at its allocation — it is revealed only
// when the object is first used as a known engine method's `this`/engine-arg. A purely forward
// TYPE pass therefore can't type the allocation or its inlined ctor writes; a backward DEMAND pass
// can, but its MEET is unsound across the ubiquitous `new`-then-null-check merge (demand on the
// used path is intersected away by the skip path). So we instead track, forward, each register's
// allocation ORIGIN (the pc of the `operator new` bl, or the interior-stack `addi`) — which
// survives the null-check merge cleanly because the skip path branches AROUND the use, not into it.
// When an origin-carrying register is used as an engine arg, we resolve {origin -> engine type}.
// A second forward TYPE pass then seeds those origins with their resolved type (apply's
// alloc_types), so everything downstream — inlined writes, the container store — types normally.
struct OState {
    std::array<u32, 32> reg{};            // GPR -> alloc-origin pc (0 = none)
    std::map<int, u32>  frame;            // r1 displacement -> alloc-origin pc
    bool defined = false;
    bool operator==(const OState& o) const { return reg == o.reg && frame == o.frame; }
};
void ometet_into(OState& acc, const OState& in) {            // intersect (disagree -> 0)
    if (!in.defined) return;
    if (!acc.defined) { acc = in; acc.defined = true; return; }
    for (int r = 0; r < 32; ++r) if (acc.reg[r] != in.reg[r]) acc.reg[r] = 0;
    std::map<int, u32> merged;
    for (const auto& [d, v] : acc.frame) {
        auto it = in.frame.find(d);
        if (it != in.frame.end() && it->second == v) merged.emplace(d, v);
    }
    acc.frame.swap(merged);
}
// Apply `i` to origin-state `s`. At an engine-arg use of an origin-carrying register, record
// {origin -> engine type} into `sites` (`db.signatures` gives the call target's arg types).
void apply_origin(const PPCInstr& i, OState& s, const TypeDB& db,
                  const std::unordered_set<u32>& raw_allocators,
                  std::map<u32, std::string>* sites) {
    switch (i.op) {
    case PPCOp::OR:                                            // mr rD,rS
        s.reg[i.rA] = (i.rS == i.rB) ? s.reg[i.rS] : 0u;
        break;
    case PPCOp::ADDI:
        if (i.rA == 1 && i.simm != 0) s.reg[i.rD] = i.pc;     // interior stack addr = stack-temp origin
        else if (i.rA != 0 && i.simm == 0) s.reg[i.rD] = s.reg[i.rA];  // ptr copy
        else s.reg[i.rD] = 0;
        break;
    case PPCOp::STW:
        if (i.rA == 1) { if (s.reg[i.rS]) s.frame[(int)i.d] = s.reg[i.rS]; else s.frame.erase((int)i.d); }
        break;
    case PPCOp::LWZ:
        if (i.rA == 1) { auto it = s.frame.find((int)i.d); s.reg[i.rD] = (it==s.frame.end())?0u:it->second; }
        else s.reg[i.rD] = 0;
        break;
    case PPCOp::LBZU: case PPCOp::LHZU: case PPCOp::LWZU: case PPCOp::LHAU:
    case PPCOp::LBZX: case PPCOp::LHZX: case PPCOp::LWZX: case PPCOp::LHAX:
    case PPCOp::LBZ: case PPCOp::LHZ: case PPCOp::LHA:
        s.reg[i.rD] = 0;                                       // load defines rD
        break;
    default:
        if (is_call(i)) {
            u32 tgt = call_target(i);
            if (sites && tgt) {                               // engine-arg use of an origin -> resolve
                auto sig = db.signatures.find(tgt);
                if (sig != db.signatures.end())
                    for (const auto& [idx, ty] : sig->second)
                        if (idx >= 3 && idx <= 10 && s.reg[idx]) (*sites)[s.reg[idx]] = ty;
            }
            for (int r = 3; r <= 12; r++) s.reg[r] = 0;       // volatiles clobbered
            if (tgt && raw_allocators.count(tgt)) s.reg[3] = i.pc;  // operator new -> origin in r3
        } else {
            int rc = clobbered_gpr(i);
            if (rc >= 0) s.reg[rc] = 0;
        }
        break;
    }
}

// Forward origin fixpoint over the CFG -> {construction-site pc -> engine type}.
std::map<u32, std::string> find_alloc_sites(const std::vector<PPCInstr>& instrs,
        const TypeDB& db, const std::unordered_set<u32>& raw_allocators,
        const std::vector<std::vector<int>>& succ, const std::vector<std::vector<int>>& pred) {
    const int n = (int)instrs.size();
    std::vector<OState> in(n), outs(n);
    std::deque<int> work;
    for (int k = 0; k < n; ++k) work.push_back(k);
    while (!work.empty()) {
        int k = work.front(); work.pop_front();
        OState nin;
        if (k == 0) nin.defined = true;
        else for (int p : pred[k]) ometet_into(nin, outs[p]);
        if (!nin.defined) continue;
        if (in[k].defined && nin == in[k]) continue;
        in[k] = nin;
        OState nout = nin;
        apply_origin(instrs[k], nout, db, raw_allocators, nullptr);
        nout.defined = true;
        outs[k] = nout;
        for (int t : succ[k]) work.push_back(t);
    }
    std::map<u32, std::string> sites;
    for (int k = 0; k < n; ++k) {
        if (!in[k].defined) continue;
        OState s = in[k];
        apply_origin(instrs[k], s, db, raw_allocators, &sites);
    }
    return sites;
}

}  // namespace

std::map<u32, EngField> recover_eng_fields(const std::vector<PPCInstr>& instrs,
                                           u32 func_addr, const TypeDB& db,
                                           const std::unordered_set<u32>& branch_targets,
                                           const std::unordered_set<u32>& jumptable_targets,
                                           std::vector<u32>* unmapped,
                                           const std::unordered_set<u32>* raw_allocators,
                                           std::map<u32, std::string>* alloc_sites) {
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

    // Engine-CONSTRUCTION recognition (opt-in): a forward "allocation origin" pass resolves which
    // `operator new` / interior-stack sites construct an engine object and of which type, so the
    // TYPE pass below can seed them. This is what closes the object-identity gap (the handoff CRUX:
    // an object's type is unknown at its allocation; it is revealed at the first engine-method use).
    std::map<u32, std::string> alloc_local;
    const std::map<u32, std::string>* alloc_types = nullptr;
    if (raw_allocators && !raw_allocators->empty()) {
        alloc_local = find_alloc_sites(instrs, db, *raw_allocators, succ, pred);
        alloc_types = &alloc_local;
        if (alloc_sites) *alloc_sites = alloc_local;
    }

    std::vector<State> in(n), outs(n);

    // Forward dataflow fixpoint (type lattice; seeds engine-construction sites via alloc_types).
    std::deque<int> work;
    std::vector<char> queued(n, 0);
    in[0] = seed;
    outs[0] = seed; apply(instrs[0], outs[0], db, nullptr, nullptr, alloc_types);
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
        apply(instrs[k], nout, db, nullptr, nullptr, alloc_types);
        nout.defined = true;
        outs[k] = nout;

        for (int t : succ[k]) if (!queued[t]) { work.push_back(t); queued[t] = 1; }
    }

    // Final pass: record field sites from each instruction's stable entry state. With construction
    // recognition the alloc result reg carries the new object's type forward, so its inlined ctor
    // writes type naturally.
    for (int k = 0; k < n; ++k) {
        if (!in[k].defined) continue;                          // unreachable: nothing to record
        State s = in[k];
        apply(instrs[k], s, db, &out, unmapped, alloc_types);
    }
    return out;
}
