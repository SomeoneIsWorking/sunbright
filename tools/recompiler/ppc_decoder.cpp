#include "ppc_decoder.h"
#include <cassert>

// Field extraction helpers
#define FIELD(w, hi, lo) (((w) >> (lo)) & ((1u << ((hi)-(lo)+1)) - 1))
#define BIT(w, n)        (((w) >> (n)) & 1)

static s16 sext16(u32 v) { return (s16)(u16)v; }
static s32 sext26(u32 v) { return (s32)(v << 6) >> 6; }  // 26-bit sign extend
static s32 sext16_i(u32 v) { return (s32)(s16)v; }

PPCInstr decode(u32 word, u32 pc) {
    PPCInstr i{};
    i.raw = word;
    i.pc  = pc;
    i.op  = PPCOp::UNKNOWN;

    u32 op6  = FIELD(word, 31, 26);
    u32 xop  = FIELD(word, 10, 1);   // X-form / XO-form extended opcode (10 bits)
    u32 axo  = FIELD(word, 5,  1);   // A-form XO (5 bits) — opcodes 4/59/63 FP/PS
    u32 xop9 = FIELD(word, 10, 2);   // legacy; only used for opcode-4 indexed PS

    // Common field extraction
    i.rD  = FIELD(word, 25, 21);
    i.rS  = FIELD(word, 25, 21);  // rS == rD slot
    i.rA  = FIELD(word, 20, 16);
    i.rB  = FIELD(word, 15, 11);
    i.rC  = FIELD(word, 10, 6);
    i.rc  = BIT(word, 0);
    i.oe  = BIT(word, 10);
    i.lk  = BIT(word, 0);
    i.aa  = BIT(word, 1);
    i.d   = sext16(FIELD(word, 15, 0));
    i.uimm = FIELD(word, 15, 0);
    i.simm = sext16_i(FIELD(word, 15, 0));
    i.crfD = FIELD(word, 25, 23);
    i.crfS = FIELD(word, 20, 18);
    i.spr  = FIELD(word, 20, 11);
    i.mb   = FIELD(word, 10, 6);
    i.me   = FIELD(word, 5,  1);
    i.sh   = FIELD(word, 15, 11);
    i.bi   = FIELD(word, 20, 16);
    i.bo   = FIELD(word, 25, 21);
    i.fm   = FIELD(word, 24, 17);
    i.nb   = FIELD(word, 15, 11);
    i.to   = FIELD(word, 25, 21);
    i.wi   = FIELD(word, 15, 12);
    i.w    = BIT(word, 15);
    i.i_gqr = FIELD(word, 12, 10);

    // CR move/logic use crfD in bits [25:23]
    i.crl_mask = FIELD(word, 19, 12);

    switch (op6) {
    // ── Opcode 3: twi
    case 3:  i.op = PPCOp::TWI; break;

    // ── Opcode 4: Paired singles + AltiVec (we only handle PS on GC)
    case 4: {
        u32 xop10 = FIELD(word, 10, 1);
        switch (xop10) {
        case 0:   i.op = PPCOp::PS_CMPU0; break;
        case 32:  i.op = PPCOp::PS_CMPO0; break;
        case 64:  i.op = PPCOp::PS_CMPU1; break;
        case 96:  i.op = PPCOp::PS_CMPO1; break;
        case 528: i.op = PPCOp::PS_MERGE00; break;
        case 560: i.op = PPCOp::PS_MERGE01; break;
        case 592: i.op = PPCOp::PS_MERGE10; break;
        case 624: i.op = PPCOp::PS_MERGE11; break;
        case 6:   i.op = PPCOp::PSQ_LX;  break;
        case 7:   i.op = PPCOp::PSQ_STX; break;
        case 38:  i.op = PPCOp::PSQ_LUX; break;
        case 39:  i.op = PPCOp::PSQ_STUX; break;
        default:
            // PS A-form — XO is bits [5:1]
            switch (axo) {
            case 10:  i.op = PPCOp::PS_SUM0;   break;
            case 11:  i.op = PPCOp::PS_SUM1;   break;
            case 12:  i.op = PPCOp::PS_MULS0;  break;
            case 13:  i.op = PPCOp::PS_MULS1;  break;
            case 14:  i.op = PPCOp::PS_MADDS0; break;
            case 15:  i.op = PPCOp::PS_MADDS1; break;
            case 18:  i.op = PPCOp::PS_DIV;    break;
            case 20:  i.op = PPCOp::PS_SUB;    break;
            case 21:  i.op = PPCOp::PS_ADD;    break;
            case 23:  i.op = PPCOp::PS_SEL;    break;
            case 24:  i.op = PPCOp::PS_RES;    break;
            case 25:  i.op = PPCOp::PS_MUL;    break;
            case 26:  i.op = PPCOp::PS_RSQRTE; break;
            case 28:  i.op = PPCOp::PS_MSUB;   break;
            case 29:  i.op = PPCOp::PS_MADD;   break;
            case 30:  i.op = PPCOp::PS_NMSUB;  break;
            case 31:  i.op = PPCOp::PS_NMADD;  break;
            }
            // XO-form PS (bits[10:6] extended)
            switch (xop10) {
            case 40:  i.op = PPCOp::PS_NEG;    break;
            case 72:  i.op = PPCOp::PS_MR;     break;
            case 136: i.op = PPCOp::PS_NABS;   break;
            case 264: i.op = PPCOp::PS_ABS;    break;
            // Opcode 4 XO=1014 is dcbz_l, NOT a paired-single op: it zeroes the 32-byte
            // cache block containing EA, the locked-cache counterpart of dcbz (opcode 31,
            // same XO). Decoding it as ps_sum0 turned a memory clear into a float add, so
            // any buffer the game zeroes this way kept its previous contents. Our locked
            // cache is backed storage at 0xE0000000, so the plain dcbz semantics are right.
            case 1014:i.op = PPCOp::DCBZ;      break;
            }
            // sum0/sum1 special cases — bits[10:1]
            if (xop10 == 10) i.op = PPCOp::PS_SUM0;
            if (xop10 == 11) i.op = PPCOp::PS_SUM1;
        }
        break;
    }

    // ── Opcode 7: mulli
    case 7: i.op = PPCOp::MULLI; break;

    // ── Opcode 8: subfic
    case 8: i.op = PPCOp::SUBFIC; break;

    // ── Opcode 10: cmpli
    case 10: i.op = PPCOp::CMPLI; break;

    // ── Opcode 11: cmpi
    case 11: i.op = PPCOp::CMPI; break;

    // ── Opcode 12: addic (immediate, rA + SIMM)
    case 12: i.op = PPCOp::ADDIC; break;

    // ── Opcode 13: addic. (addic + CR0 update)
    case 13: i.op = PPCOp::ADDIC; i.rc = 1; break;

    // ── Opcode 14: addi (li when rA==0)
    case 14: i.op = PPCOp::ADDI; break;

    // ── Opcode 15: addis (lis when rA==0)
    case 15: i.op = PPCOp::ADDIS; break;

    // ── Opcode 16: bc/bcl/bca/bcla
    case 16: {
        i.lk = BIT(word, 0);
        i.aa = BIT(word, 1);
        s32 bd = sext16(FIELD(word, 15, 2) << 2);
        i.target = i.aa ? (u32)bd : (pc + (u32)bd);
        i.op = PPCOp::BC;
        break;
    }

    // ── Opcode 17: sc
    case 17: i.op = PPCOp::SC; break;

    // ── Opcode 18: b/bl/ba/bla
    case 18: {
        i.lk = BIT(word, 0);
        i.aa = BIT(word, 1);
        s32 li = sext26(FIELD(word, 25, 2) << 2);
        i.target = i.aa ? (u32)li : (pc + (u32)li);
        i.op = PPCOp::B;
        break;
    }

    // ── Opcode 19: branch + CR ops
    case 19: {
        i.lk = BIT(word, 0);
        switch (xop) {
        case 0:   i.op = PPCOp::MCRF;    break;
        case 16:  i.op = PPCOp::BCLR;    break;
        case 33:  i.op = PPCOp::CRNOR;   break;
        case 50:  i.op = PPCOp::RFI;     break;
        case 129: i.op = PPCOp::CRANDC;  break;
        case 150: i.op = PPCOp::ISYNC;   break;
        case 193: i.op = PPCOp::CRXOR;   break;
        case 225: i.op = PPCOp::CRNAND;  break;
        case 257: i.op = PPCOp::CRAND;   break;
        case 289: i.op = PPCOp::CREQV;   break;
        case 417: i.op = PPCOp::CRORC;   break;
        case 449: i.op = PPCOp::CROR;    break;
        case 528: i.op = PPCOp::BCCTR;   break;
        }
        break;
    }

    // ── Opcode 20: rlwimi
    case 20: i.op = PPCOp::RLWIMI; break;

    // ── Opcode 21: rlwinm
    case 21: i.op = PPCOp::RLWINM; break;

    // ── Opcode 23: rlwnm
    case 23: i.op = PPCOp::RLWNM; break;

    // ── Opcode 24: ori
    case 24: i.op = PPCOp::ORI; break;

    // ── Opcode 25: oris
    case 25: i.op = PPCOp::ORIS; break;

    // ── Opcode 26: xori
    case 26: i.op = PPCOp::XORI; break;

    // ── Opcode 27: xoris
    case 27: i.op = PPCOp::XORIS; break;

    // ── Opcode 28: andi.
    case 28: i.op = PPCOp::ANDI_DOT; break;

    // ── Opcode 29: andis.
    case 29: i.op = PPCOp::ANDIS_DOT; break;

    // ── Opcode 31: Integer extended
    case 31: {
        u32 xop1 = FIELD(word, 10, 1);
        switch (xop1) {
        case 0:   i.op = PPCOp::CMP;     break;
        case 4:   i.op = PPCOp::TW;      break;
        case 8:   i.op = i.oe ? PPCOp::SUBFC  : PPCOp::SUBFC;   break;
        case 10:  i.op = PPCOp::ADDC;    break;
        case 11:  i.op = PPCOp::MULHWU;  break;
        case 19:  i.op = PPCOp::MFCR;    break;
        case 20:  i.op = PPCOp::LWARX;   break;  // not in enum, treat as LWZ for now
        case 23:  i.op = PPCOp::LWZX;    break;
        case 24:  i.op = PPCOp::SLW;     break;
        case 26:  i.op = PPCOp::CNTLZW;  break;
        case 28:  i.op = PPCOp::AND;     break;
        case 32:  i.op = PPCOp::CMPL;    break;
        case 40:  i.op = PPCOp::SUBF;    break;
        case 54:  i.op = PPCOp::DCBST;   break;
        case 55:  i.op = PPCOp::LWZUX;   break;
        case 60:  i.op = PPCOp::ANDC;    break;
        case 75:  i.op = PPCOp::MULHW;   break;
        case 83:  i.op = PPCOp::MFMSR;   break;
        case 86:  i.op = PPCOp::DCBF;    break;
        case 87:  i.op = PPCOp::LBZX;    break;
        case 104: i.op = PPCOp::NEG;     break;
        case 119: i.op = PPCOp::LBZUX;   break;
        case 124: i.op = PPCOp::NOR;     break;
        case 136: i.op = PPCOp::SUBFE;   break;
        case 138: i.op = PPCOp::ADDE;    break;
        case 144: i.op = PPCOp::MTCRF;   break;
        case 146: i.op = PPCOp::MTMSR;   break;
        case 150: i.op = PPCOp::STWCX;   break;  // not in enum, treat as STWX
        case 151: i.op = PPCOp::STWX;    break;
        case 183: i.op = PPCOp::STWUX;   break;
        case 200: i.op = PPCOp::SUBFZE;  break;
        case 202: i.op = PPCOp::ADDZE;   break;
        case 215: i.op = PPCOp::STBX;    break;
        case 232: i.op = PPCOp::SUBFME;  break;
        case 234: i.op = PPCOp::ADDME;   break;
        case 235: i.op = PPCOp::MULLW;   break;
        case 247: i.op = PPCOp::STBUX;   break;
        case 266: i.op = PPCOp::ADD;     break;
        case 278: i.op = PPCOp::DCBT;    break;
        case 279: i.op = PPCOp::LHZX;    break;
        case 284: i.op = PPCOp::EQV;     break;
        case 306: i.op = PPCOp::TLBIE;   break;
        case 310: i.op = PPCOp::ECIWX;   break;
        case 311: i.op = PPCOp::LHZUX;   break;
        case 316: i.op = PPCOp::XOR;     break;
        case 339: i.op = PPCOp::MFSPR;   break;
        case 343: i.op = PPCOp::LHAX;    break;
        case 371: i.op = PPCOp::MFTB;    break;
        case 375: i.op = PPCOp::LHAUX;   break;
        case 407: i.op = PPCOp::STHX;    break;
        case 412: i.op = PPCOp::ORC;     break;
        case 438: i.op = PPCOp::ECOWX;   break;
        case 439: i.op = PPCOp::STHUX;   break;
        case 444: i.op = PPCOp::OR;      break;
        case 459: i.op = PPCOp::DIVWU;   break;
        case 467: i.op = PPCOp::MTSPR;   break;
        case 470: i.op = PPCOp::DCBI;    break;
        case 476: i.op = PPCOp::NAND;    break;
        case 491: i.op = PPCOp::DIVW;    break;
        case 512: i.op = PPCOp::MCRXR;   break;
        case 533: i.op = PPCOp::LSWX;    break;
        case 534: i.op = PPCOp::LWBRX;   break;
        case 535: i.op = PPCOp::LFSX;    break;
        case 536: i.op = PPCOp::SRW;     break;
        case 566: i.op = PPCOp::TLBSYNC; break;
        case 567: i.op = PPCOp::LFSUX;   break;
        case 595: i.op = PPCOp::MFSR;    break;
        case 597: i.op = PPCOp::LSWI;    break;
        case 598: i.op = PPCOp::SYNC;    break;
        case 599: i.op = PPCOp::LFDX;    break;
        case 631: i.op = PPCOp::LFDUX;   break;
        case 659: i.op = PPCOp::MFSRIN;  break;
        case 661: i.op = PPCOp::STSWX;   break;
        case 662: i.op = PPCOp::STWBRX;  break;
        case 663: i.op = PPCOp::STFSX;   break;
        case 695: i.op = PPCOp::STFSUX;  break;
        case 725: i.op = PPCOp::STSWI;   break;
        case 727: i.op = PPCOp::STFDX;   break;
        case 759: i.op = PPCOp::STFDUX;  break;
        case 787: i.op = PPCOp::MTSR;    break;
        case 790: i.op = PPCOp::STHBRX;  break;
        case 792: i.op = PPCOp::SRAW;    break;
        case 824: i.op = PPCOp::SRAWI;   break;
        case 851: i.op = PPCOp::MTSRIN;  break;
        case 854: i.op = PPCOp::EIEIO;   break;
        case 922: i.op = PPCOp::EXTSH;   break;
        case 954: i.op = PPCOp::EXTSB;   break;
        case 982: i.op = PPCOp::ICBI;    break;
        case 983: i.op = PPCOp::STFIWX;  break;
        case 1014: i.op = PPCOp::DCBZ;   break;
        // OE variants
        case 8+512:   i.op = PPCOp::SUBFCO;  break;
        case 10+512:  i.op = PPCOp::ADDCO;   break;
        case 40+512:  i.op = PPCOp::SUBFO;   break;
        case 104+512: i.op = PPCOp::NEGO;    break;
        case 136+512: i.op = PPCOp::SUBFEO;  break;
        case 138+512: i.op = PPCOp::ADDEO;   break;
        case 200+512: i.op = PPCOp::SUBFZEO; break;
        case 202+512: i.op = PPCOp::ADDZEO;  break;
        case 232+512: i.op = PPCOp::SUBFMEO; break;
        case 234+512: i.op = PPCOp::ADDMEO;  break;
        case 235+512: i.op = PPCOp::MULLWO;  break;
        case 266+512: i.op = PPCOp::ADDO;    break;
        case 459+512: i.op = PPCOp::DIVWUO;  break;
        case 491+512: i.op = PPCOp::DIVWO;   break;
        }
        break;
    }

    // ── Load/store integer (opcodes 32–47)
    case 32: i.op = PPCOp::LWZ;  break;
    case 33: i.op = PPCOp::LWZU; break;
    case 34: i.op = PPCOp::LBZ;  break;
    case 35: i.op = PPCOp::LBZU; break;
    case 36: i.op = PPCOp::STW;  break;
    case 37: i.op = PPCOp::STWU; break;
    case 38: i.op = PPCOp::STB;  break;
    case 39: i.op = PPCOp::STBU; break;
    case 40: i.op = PPCOp::LHZ;  break;
    case 41: i.op = PPCOp::LHZU; break;
    case 42: i.op = PPCOp::LHA;  break;
    case 43: i.op = PPCOp::LHAU; break;
    case 44: i.op = PPCOp::STH;  break;
    case 45: i.op = PPCOp::STHU; break;
    case 46: i.op = PPCOp::LMW;  break;
    case 47: i.op = PPCOp::STMW; break;

    // ── Load/store FP (opcodes 48–55)
    case 48: i.op = PPCOp::LFS;  break;
    case 49: i.op = PPCOp::LFSU; break;
    case 50: i.op = PPCOp::LFD;  break;
    case 51: i.op = PPCOp::LFDU; break;
    case 52: i.op = PPCOp::STFS; break;
    case 53: i.op = PPCOp::STFSU; break;
    case 54: i.op = PPCOp::STFD; break;
    case 55: i.op = PPCOp::STFDU; break;

    // ── Opcode 56/57/60/61: psq_l/lu/st/stu
    case 56: {
        // psq_l: [56][rD][rA][W][I][d]
        i.w     = BIT(word, 15);
        i.i_gqr = FIELD(word, 14, 12);
        i.d     = sext16(FIELD(word, 11, 0));
        i.op    = PPCOp::PSQ_L;
        break;
    }
    case 57: {
        i.w     = BIT(word, 15);
        i.i_gqr = FIELD(word, 14, 12);
        i.d     = sext16(FIELD(word, 11, 0));
        i.op    = PPCOp::PSQ_LU;
        break;
    }
    case 60: {
        i.w     = BIT(word, 15);
        i.i_gqr = FIELD(word, 14, 12);
        i.d     = sext16(FIELD(word, 11, 0));
        i.op    = PPCOp::PSQ_ST;
        break;
    }
    case 61: {
        i.w     = BIT(word, 15);
        i.i_gqr = FIELD(word, 14, 12);
        i.d     = sext16(FIELD(word, 11, 0));
        i.op    = PPCOp::PSQ_STU;
        break;
    }

    // ── Opcode 59: FP single arithmetic (all A-form; XO = bits [5:1])
    case 59: {
        switch (axo) {
        case 18: i.op = PPCOp::FDIVS;   break;
        case 20: i.op = PPCOp::FSUBS;   break;
        case 21: i.op = PPCOp::FADDS;   break;
        case 24: i.op = PPCOp::FRESS;   break;
        case 25: i.op = PPCOp::FMULS;   break;
        case 28: i.op = PPCOp::FMSUBS;  break;
        case 29: i.op = PPCOp::FMADDS;  break;
        case 30: i.op = PPCOp::FNMSUBS; break;
        case 31: i.op = PPCOp::FNMADDS; break;
        }
        break;
    }

    // ── Opcode 63: FP double arithmetic
    case 63: {
        switch (xop) {
        case 0:   i.op = PPCOp::FCMPU;   break;
        case 12:  i.op = PPCOp::FRSP;    break;
        case 14:  i.op = PPCOp::FCTIW;   break;
        case 15:  i.op = PPCOp::FCTIWZ;  break;
        case 32:  i.op = PPCOp::FCMPO;   break;
        case 38:  i.op = PPCOp::MTFSB1;  break;
        case 40:  i.op = PPCOp::FNEG;    break;
        case 64:  i.op = PPCOp::MCRFS;   break;
        case 70:  i.op = PPCOp::MTFSB0;  break;
        case 72:  i.op = PPCOp::FMR;     break;
        case 134: i.op = PPCOp::MTFSFI;  break;
        case 136: i.op = PPCOp::FNABS;   break;
        case 264: i.op = PPCOp::FABS;    break;
        case 583: i.op = PPCOp::MFFS;    break;
        case 711: i.op = PPCOp::MTFSF;   break;
        case 814: i.op = PPCOp::FRES;    break;
        case 846: i.op = PPCOp::FRSQRTE; break;
        default:
            // A-form FP double — XO is bits [5:1]
            switch (axo) {
            case 18: i.op = PPCOp::FDIV;    break;
            case 20: i.op = PPCOp::FSUB;    break;
            case 21: i.op = PPCOp::FADD;    break;
            case 23: i.op = PPCOp::FSEL;    break;
            case 25: i.op = PPCOp::FMUL;    break;
            case 26: i.op = PPCOp::FRSQRTE; break;
            case 28: i.op = PPCOp::FMSUB;   break;
            case 29: i.op = PPCOp::FMADD;   break;
            case 30: i.op = PPCOp::FNMSUB;  break;
            case 31: i.op = PPCOp::FNMADD;  break;
            }
        }
        break;
    }

    default: break;
    }

    return i;
}

bool is_branch(const PPCInstr& i) {
    switch (i.op) {
    case PPCOp::B:
    case PPCOp::BC:
    case PPCOp::BCLR: case PPCOp::BCLRL:
    case PPCOp::BCCTR: case PPCOp::BCCTRL:
    case PPCOp::RFI:
        return true;
    default:
        return false;
    }
}

bool is_unconditional_branch(const PPCInstr& i) {
    if (i.op == PPCOp::B && !i.lk) return true;
    if (i.op == PPCOp::BCLR && !i.lk && i.bo == 0x14) return true;  // always blr
    if (i.op == PPCOp::BCCTR && !i.lk && i.bo == 0x14) return true; // always bctr
    if (i.op == PPCOp::RFI) return true;
    return false;
}

u32 branch_target(const PPCInstr& i) {
    if (i.op == PPCOp::B || i.op == PPCOp::BC)
        return i.target;
    return 0;  // indirect
}
