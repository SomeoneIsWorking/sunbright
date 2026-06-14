#include "type_recovery.h"

// Seeded type recovery — see type_recovery.h for the rationale and the SHARP EDGE note.
//
// State carried forward over the linear instruction stream:
//   reg_type[r]   : engine type name held in GPR r ("" = not an engine pointer)
//   frame_slots[d]: engine type spilled to the local frame at displacement d off r1
//                   (the dominant real pattern: the compiler saves `this` to the stack
//                   in the prologue and reloads it into a non-volatile after a call).
//
// This is intentionally a single forward pass with NO merge handling at control-flow
// joins — enough for the straight-line accessor shape the de-risk targets, and honest
// about its limits (branches/merges are de-risk #2's remaining coverage problem).

namespace {

// Which GPR (if any) this op overwrites, so we can invalidate a stale type when an op we
// don't model as type-preserving clobbers a register. Returns the register index, or -1.
// (Type-PRESERVING/CONSUMING ops — copies, loads, the stack spill/reload — are handled
// explicitly in the main loop and never reach here.)
int clobbered_gpr(const PPCInstr& i) {
    switch (i.op) {
    // dest = rD
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
    // dest = rA
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

}  // namespace

std::map<u32, EngField> recover_eng_fields(const std::vector<PPCInstr>& instrs,
                                           u32 func_addr, const TypeDB& db) {
    std::map<u32, EngField> out;
    std::string reg_type[32];
    std::map<int, std::string> frame_slots;

    // Seed from the decomp signature (EABI: integer/pointer args in r3..r10).
    auto sig = db.signatures.find(func_addr);
    if (sig != db.signatures.end())
        for (const auto& [idx, ty] : sig->second)
            if (idx >= 0 && idx < 32) reg_type[idx] = ty;

    // Record a typed field site, and return the field's nested-pointer type (for chaining).
    auto field_at = [&](const PPCInstr& i, int base_reg) -> std::string {
        const std::string& ty = reg_type[base_reg];
        if (ty.empty()) return "";
        auto L = db.layouts.find(ty);
        if (L == db.layouts.end()) return "";
        auto f = L->second.fields.find((int)i.d);
        if (f == L->second.fields.end()) return "";   // engine ptr but unmapped offset (coverage gap)
        out[i.pc] = EngField{ ty, f->second.member, f->second.nested_type };
        return f->second.nested_type;
    };

    for (const auto& i : instrs) {
        switch (i.op) {

        // ── register copies ──────────────────────────────────────────────────────
        case PPCOp::OR:                                  // mr rD,rS == or rA,rS,rS
            reg_type[i.rA] = (i.rS == i.rB) ? reg_type[i.rS] : std::string();
            break;
        case PPCOp::ADDI:                                // addi rD,rA,0 = pointer copy; nonzero = interior
            reg_type[i.rD] = (i.rA != 0 && i.simm == 0) ? reg_type[i.rA] : std::string();
            break;

        // ── stack spill / reload of a typed pointer (the prologue-save pattern) ───
        case PPCOp::STW:
            if (i.rA == 1) {                             // stw rS, d(r1): spill to local frame
                if (reg_type[i.rS].empty()) frame_slots.erase((int)i.d);
                else frame_slots[(int)i.d] = reg_type[i.rS];
            } else {
                field_at(i, i.rA);                       // stw to an engine field (typed write)
            }
            break;
        case PPCOp::LWZ:
            if (i.rA == 1) {                             // lwz rD, d(r1): reload from local frame
                auto s = frame_slots.find((int)i.d);
                reg_type[i.rD] = (s == frame_slots.end()) ? std::string() : s->second;
            } else {
                reg_type[i.rD] = field_at(i, i.rA);      // engine field load; nested type chains
            }
            break;

        // ── integer field reads/writes (scalar — no nested chaining) ─────────────
        case PPCOp::LBZ: case PPCOp::LHZ: case PPCOp::LHA:
            field_at(i, i.rA);
            reg_type[i.rD] = "";                          // narrow scalar: never an engine pointer
            break;
        case PPCOp::STB: case PPCOp::STH:
            if (i.rA != 1) field_at(i, i.rA);
            break;

        // ── float field reads/writes (FPR dest/src — GPR types unchanged) ────────
        case PPCOp::LFS: case PPCOp::LFD:
            field_at(i, i.rA);
            break;
        case PPCOp::STFS: case PPCOp::STFD:
            if (i.rA != 1) field_at(i, i.rA);
            break;

        // ── calls clobber the EABI volatiles (r3..r12); frame slots survive ──────
        case PPCOp::B:
            if (i.lk) for (int r = 3; r <= 12; r++) reg_type[r] = "";
            break;
        case PPCOp::BCCTR: case PPCOp::BCLR:
            if (i.lk) for (int r = 3; r <= 12; r++) reg_type[r] = "";
            break;

        // ── everything else: invalidate a clobbered register's tracked type ──────
        default: {
            int rc = clobbered_gpr(i);
            if (rc >= 0) reg_type[rc] = "";
            break;
        }
        }
    }
    return out;
}
