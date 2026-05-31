#pragma once
#include <cstdint>
#include <string>

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using s16 = int16_t;
using s32 = int32_t;

// Every PowerPC 750CL (Gekko) instruction decoded into a flat struct.
// Fixed 32-bit instruction width.

enum class PPCOp {
    // Integer arithmetic
    ADD, ADDO, ADDI, ADDIS, ADDIC, ADDC, ADDCO, ADDE, ADDEO,
    ADDI_DOT, SUBFIC,
    SUBF, SUBFO, SUBFC, SUBFCO, SUBFE, SUBFEO,
    SUBFME, SUBFMEO, SUBFZE, SUBFZEO,
    ADDME, ADDMEO, ADDZE, ADDZEO,
    MULLI, MULLW, MULLWO, MULHW, MULHWU,
    DIVW, DIVWO, DIVWU, DIVWUO,
    NEG, NEGO,
    // Integer compare
    CMP, CMPI, CMPL, CMPLI,
    // Logical
    AND, ANDI_DOT, ANDIS_DOT, ANDC,
    OR, ORI, ORIS, ORC,
    XOR, XORI, XORIS, XORC,
    NOR, NAND, EQV,
    EXTSB, EXTSH,
    CNTLZW,
    // Rotate/shift
    RLWINM, RLWIMI, RLWNM,
    SLW, SRW, SRAW, SRAWI,
    // Branch
    B, BL, BA, BLA,           // unconditional
    BC, BCL, BCA, BCLA,       // conditional
    BCLR, BCLRL,              // branch to LR
    BCCTR, BCCTRL,            // branch to CTR
    // CR ops
    CRAND, CRANDC, CROR, CRORC, CRXOR, CRNOR, CRNAND, CREQV,
    MCRF, MCRXR,
    // SPR moves
    MFSPR, MTSPR, MFCR, MTCRF, MFMSR, MTMSR,
    MFTB,  // time base
    // Load integer
    LBZ, LBZU, LBZX, LBZUX,
    LHZ, LHZU, LHZX, LHZUX,
    LHA, LHAU, LHAX, LHAUX,
    LWZ, LWZU, LWZX, LWZUX,
    LMW,
    LSWI, LSWX,
    LWARX,  // load word and reserve (atomic)
    // Store integer
    STB, STBU, STBX, STBUX,
    STH, STHU, STHX, STHUX,
    STW, STWU, STWX, STWUX,
    STMW,
    STSWI, STSWX,
    STWBRX, STHBRX, LWBRX,
    STWCX,  // store word conditional (atomic)
    DCBST,  // data cache block store
    // Load/store FP
    LFS, LFSU, LFSX, LFSUX,
    LFD, LFDU, LFDX, LFDUX,
    STFS, STFSU, STFSX, STFSUX,
    STFD, STFDU, STFDX, STFDUX,
    STFIWX,
    // FP double ops (opcode 63)
    FADD, FSUB, FMUL, FDIV,
    FMADD, FMSUB, FNMADD, FNMSUB,
    FABS, FNABS, FNEG, FMR,
    FRSP, FCTIW, FCTIWZ,
    FCMPU, FCMPO,
    FRES, FRSQRTE,
    MFFS, MTFSB0, MTFSB1, MTFSF, MTFSFI,
    MCRFS,  // move to CR from FPSCR
    FSEL,
    // FP single ops (opcode 59)
    FADDS, FSUBS, FMULS, FDIVS,
    FMADDS, FMSUBS, FNMADDS, FNMSUBS,
    FRESS,
    // Paired singles (opcode 4)
    PS_ADD, PS_SUB, PS_MUL, PS_DIV,
    PS_MADD, PS_MSUB, PS_NMADD, PS_NMSUB,
    PS_ABS, PS_NABS, PS_NEG, PS_MR,
    PS_SUM0, PS_SUM1,
    PS_MULS0, PS_MULS1,       // multiply by ps0/ps1 of fC (Gekko-specific)
    PS_MADDS0, PS_MADDS1,     // madd using ps0/ps1 of fC (Gekko-specific)
    PS_RES, PS_RSQRTE, PS_SEL,
    PS_CMPU0, PS_CMPU1, PS_CMPO0, PS_CMPO1,
    PS_MERGE00, PS_MERGE01, PS_MERGE10, PS_MERGE11,
    PSQ_L, PSQ_LU, PSQ_LX, PSQ_LUX,
    PSQ_ST, PSQ_STU, PSQ_STX, PSQ_STUX,
    // System
    SC, RFI, ISYNC, SYNC, EIEIO,
    // Cache
    DCBT, DCBTST, DCBF, DCBI, DCBZ, ICBI,
    // Misc
    TW, TWI,
    ECIWX, ECOWX,
    MFSR, MTSR, MFSRIN, MTSRIN,
    TLBIE, TLBSYNC,

    UNKNOWN,
};

struct PPCInstr {
    u32    raw;       // Original 32-bit word (big-endian in ROM, decoded as LE)
    u32    pc;        // Address in GC address space
    PPCOp  op;

    // Decoded fields (not all valid for every op)
    u8  rS, rD, rA, rB, rC;   // Register operands
    u8  crfD, crfS;            // CR field operands
    u8  bi, bo;                // Branch condition
    s16 d;                     // Displacement / signed immediate
    u16 uimm;                  // Unsigned immediate
    s32 simm;                  // Sign-extended immediate
    u32 target;                // Branch target (absolute or relative)
    u8  mb, me;                // Rotate mask begin/end
    u8  sh;                    // Shift amount
    u16 spr;                   // SPR number (raw, decode: spr[4:0]<<5|spr[9:5])
    u8  tbr;                   // Time base register
    u8  crl_mask;              // MTCRF field mask
    u8  fm;                    // MTFSF field mask
    u8  nb;                    // LSWI/STSWI count
    u8  to;                    // Trap condition
    u8  wi;                    // MTFSFI immediate
    u8  w;                     // psq_l/st W bit (1=load one ps, 0=load two)
    u8  i_gqr;                 // psq_l/st GQR index

    bool aa;   // absolute address
    bool lk;   // link (update LR)
    bool rc;   // record (update CR)
    bool oe;   // overflow enable

    std::string mnemonic() const;
};

// Decode a single 32-bit PPC instruction word.
PPCInstr decode(u32 word, u32 pc);

// True if this instruction can end a basic block.
bool is_branch(const PPCInstr& i);

// True if this is an unconditional, non-linking branch (basic block terminator with no fallthrough).
bool is_unconditional_branch(const PPCInstr& i);

// Resolved branch target, or 0 if indirect/unknown.
u32 branch_target(const PPCInstr& i);

// Decode the SPR number from a MFSPR/MTSPR instruction (bits are swapped in the encoding).
inline u16 decode_spr(u16 raw) { return ((raw & 0x1F) << 5) | ((raw >> 5) & 0x1F); }

// Well-known SPR numbers
enum SPR : u16 {
    SPR_XER   = 1,
    SPR_LR    = 8,
    SPR_CTR   = 9,
    SPR_DSISR = 18,
    SPR_DAR   = 19,
    SPR_DEC   = 22,
    SPR_SDR1  = 25,
    SPR_SRR0  = 26,
    SPR_SRR1  = 27,
    SPR_TBL_R = 268,
    SPR_TBU_R = 269,
    SPR_GQR0  = 912,
    SPR_GQR7  = 919,
    SPR_HID0  = 1008,
    SPR_HID2  = 920,
};
