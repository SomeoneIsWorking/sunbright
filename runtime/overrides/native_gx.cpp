// Native GX SDK writers — slice 1: the TEV hot set (~25M dispatched calls/run,
// docs/decomp/runtime_coverage.md tier 1).
//
// These are PC-native ports of the GameCube GX SDK functions that build GPU
// commands into the write-gather FIFO (0xCC008000 → memory_bridge → Dolphin
// GPFifo) and maintain the CPU-side __GXData state block (guest global; pointer
// lives in SDA at r13-0x72F8).
//
// DESIGN CONTRACT — byte-identical output:
//   * Every override must produce EXACTLY the same FIFO byte stream as the guest
//     function it replaces (the 0x61 GX_LOAD_BP_REG byte followed by the big-endian
//     32-bit BP register value, via the gather pipe), and the same __GXData bytes.
//     The main session verifies this with a FIFO shadow harness that diffs the
//     bytes both paths produce.
//   * To make that diffable without replaying, the pure register-packing logic is
//     factored into standalone pack_*() functions (no guest-memory access, no
//     side effects); the overrides are thin glue: read state word → pack → write
//     state word → emit FIFO bytes → bpSent=0 (+ extras per function).
//
// Sources of truth (all three cross-checked per function — see
// docs/decomp/gx_writers.md for the VERIFIED bitfield tables):
//   * DOL disassembly (sunbright-recomp --disasm) of GMSE01 at the addresses below.
//   * reference/sms/src/dolphin/gx/GXTev.c + GXBump.c (decomp; matches the disasm
//     instruction-for-instruction on every function ported here).
//   * Dolphin externals/dolphin BPMemory.h register names (the map, cited in docs).
//
// Dispatch caveat (same as every override): overrides fire on dispatch entries
// (call_ppc / JIT entry / our own native call sites). CORRECTION (main session):
// under the current call model EVERY emitted bl/bctrl dispatches through call_ppc →
// recomp_lookup → overrides — full coverage, no extra routing needed.

#include <cstdio>
#include <cstdlib>

#include "../cpu_state.h"
#include "../overrides.h"
#include "../intrinsics.h"

namespace {

// ── Guest layout constants (disasm-verified, see docs/decomp/gx_writers.md) ──
constexpr u32 kGxPtrSdaOff = 0x72F8;      // gx = *(u32*)(r13 - 0x72F8)
constexpr u32 kWGPipe      = 0xCC008000;  // write-gather pipe (lis 0xCC01; -0x8000)
constexpr u8  kLoadBPReg   = 0x61;        // GX_LOAD_BP_REG command byte

// __GXData offsets (struct __GXData_struct, reference/sms/src/dolphin/gx/__gx.h;
// every offset below confirmed in the GMSE01 disasm)
constexpr u32 kOffBpSent     = 0x002;     // u16, set to 0 after every BP write
constexpr u32 kOffTref       = 0x100;     // u32 tref[8]      (BP TREF 0x28+i)
constexpr u32 kOffTevc       = 0x130;     // u32 tevc[16]     (BP TEV_COLOR_ENV 0xC0+2i)
constexpr u32 kOffTeva       = 0x170;     // u32 teva[16]     (BP TEV_ALPHA_ENV 0xC1+2i)
constexpr u32 kOffTevKsel    = 0x1B0;     // u32 tevKsel[8]   (BP TEV_KSEL 0xF6+i)
constexpr u32 kOffTexmapId   = 0x49C;     // u32 texmapId[16]
constexpr u32 kOffTcoordEnab = 0x4E0;     // u32 bitmask: stage has a live texcoord
constexpr u32 kOffDirtyState = 0x4F4;     // u32, TevOrder sets bit 0

inline u32 gx_ptr(CPUState& cpu) { return sb_r32(cpu.gpr[13] - kGxPtrSdaOff); }

// SET_REG_FIELD equivalent: replace `size` bits at `shift` in `reg` with `val`.
inline u32 set_field(u32 reg, u32 size, u32 shift, u32 val) {
    const u32 mask = ((size >= 32 ? 0u : (1u << size)) - 1u) << shift;
    return (reg & ~mask) | ((val << shift) & mask);
}

// Emit one BP register load into the gather pipe: 0x61 byte + 32-bit value.
// sb_w8/sb_w32 on 0xCC008000 route through memory_bridge → GPFifo::Write8/Write32,
// the exact same path the recompiled guest stores take.
inline void fifo_bp(u32 reg) {
    sb_w8(kWGPipe, kLoadBPReg);
    sb_w32(kWGPipe, reg);
}

// Common tail of every TEV writer: store the updated shadow word, emit it to the
// FIFO, clear gx->bpSent (guest order: stw shadow; stb 0x61; stw value; sth 0).
inline void commit_bp(u32 gx, u32 word_ea, u32 reg) {
    sb_w32(word_ea, reg);
    fifo_bp(reg);
    sb_w16(gx + kOffBpSent, 0);
}

} // namespace

// ───────────────────────── pure packing (harness-diffable) ─────────────────────────
// External linkage on purpose: the FIFO-shadow harness compares these values
// against the guest-produced words without replaying the overrides.
// All field positions VERIFIED against the GMSE01 disasm and named per Dolphin
// BPMemory.h in docs/decomp/gx_writers.md.
namespace native_gx {
using ::set_field;

// BP TEV_COLOR_ENV (0xC0+2*stage): a/b/c/d selectors. High bits (op/bias/scale/
// clamp/dest + the BP address byte) are preserved from the shadow word.
u32 pack_tev_color_in(u32 old, u32 a, u32 b, u32 c, u32 d) {
    old = set_field(old, 4, 12, a);
    old = set_field(old, 4, 8,  b);
    old = set_field(old, 4, 4,  c);
    old = set_field(old, 4, 0,  d);
    return old;
}

// BP TEV_ALPHA_ENV (0xC1+2*stage): a/b/c/d selectors (3 bits each; bits 0-3 are
// the rswap/tswap fields owned by GXSetTevSwapMode).
u32 pack_tev_alpha_in(u32 old, u32 a, u32 b, u32 c, u32 d) {
    old = set_field(old, 3, 13, a);
    old = set_field(old, 3, 10, b);
    old = set_field(old, 3, 7,  c);
    old = set_field(old, 3, 4,  d);
    return old;
}

// Shared op/bias/scale/clamp/dest packing for TEV_COLOR_ENV and TEV_ALPHA_ENV
// (identical field layout; GXSetTevColorOp and GXSetTevAlphaOp differ only in
// which shadow array they touch). For compare ops (op >= GX_TEV_COMP_R8_GT == 8):
// scale field holds (op>>1)&3 (the comparison kind) and bias is forced to 3.
u32 pack_tev_op(u32 old, u32 op, u32 bias, u32 scale, u32 clamp, u32 out_reg) {
    old = set_field(old, 1, 18, op & 1);
    if (op <= 1) {                              // GX_TEV_ADD / GX_TEV_SUB
        old = set_field(old, 2, 20, scale);
        old = set_field(old, 2, 16, bias);
    } else {                                    // compare modes
        old = set_field(old, 2, 20, (op >> 1) & 3);
        old = set_field(old, 2, 16, 3);
    }
    old = set_field(old, 1, 19, clamp & 0xFF);
    old = set_field(old, 2, 22, out_reg);
    return old;
}

// BP TEV_ALPHA_ENV low bits: rswap (2@0) / tswap (2@2).
u32 pack_tev_swap_mode(u32 old, u32 ras_sel, u32 tex_sel) {
    old = set_field(old, 2, 0, ras_sel);
    old = set_field(old, 2, 2, tex_sel);
    return old;
}

// BP TEV_KSEL (0xF6 + stage/2): kcsel at 5@4 (even stage) / 5@14 (odd).
u32 pack_tev_kcsel(u32 old, u32 stage_odd, u32 sel) {
    return stage_odd ? set_field(old, 5, 14, sel) : set_field(old, 5, 4, sel);
}
// kasel at 5@9 (even stage) / 5@19 (odd).
u32 pack_tev_kasel(u32 old, u32 stage_odd, u32 sel) {
    return stage_odd ? set_field(old, 5, 19, sel) : set_field(old, 5, 9, sel);
}

// BP IND_CMD (0x10 + tev_stage): built from zero, NOT shadowed in __GXData.
u32 pack_tev_indirect(u32 tev_stage, u32 ind_stage, u32 format, u32 bias_sel,
                      u32 matrix_sel, u32 wrap_s, u32 wrap_t, u32 add_prev,
                      u32 utc_lod, u32 alpha_sel) {
    u32 reg = 0;
    reg = set_field(reg, 2, 0,  ind_stage);
    reg = set_field(reg, 2, 2,  format);
    reg = set_field(reg, 3, 4,  bias_sel);
    reg = set_field(reg, 2, 7,  alpha_sel);
    reg = set_field(reg, 4, 9,  matrix_sel);
    reg = set_field(reg, 3, 13, wrap_s);
    reg = set_field(reg, 3, 16, wrap_t);
    reg = set_field(reg, 1, 19, utc_lod);
    reg = set_field(reg, 1, 20, add_prev);
    reg = set_field(reg, 8, 24, tev_stage + 16);
    return reg;
}

// GXSetTevOrder channel-id → TREF rasterized-color field (GXTev.c c2r[], guest
// table at 0x803E9390). GX_COLOR_NULL (0xFF) maps to 7 before indexing.
constexpr u32 c2r[] = { 0, 1, 0, 1, 0, 1, 7, 5, 6 };

// BP TREF (0x28 + stage/2): each register packs two stages, fields offset by 12.
u32 pack_tref(u32 old, u32 stage_odd, u32 tmap, u32 tcoord, u32 ras, u32 enable) {
    const u32 s = stage_odd ? 12 : 0;
    old = set_field(old, 3, 0 + s, tmap);
    old = set_field(old, 3, 3 + s, tcoord);
    old = set_field(old, 3, 7 + s, ras);
    old = set_field(old, 1, 6 + s, enable);
    return old;
}

} // namespace native_gx

namespace {
using namespace native_gx;

// ── activation gate + verification shadow ───────────────────────────────────
// These overrides are OPT-IN until FIFO/state-verified by the main session:
//   SUNBRIGHT_NATIVE_GX=1  — native packers live (guest body not run)
//   SUNBRIGHT_GX_SHADOW=1  — guest body runs (authoritative) AND the native pack
//                            value is compared against the guest's shadow-state
//                            word (ea) — the verification harness.
//   neither                — pure pass-through to the guest body (no behavior change).
static bool gx_native_on() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("SUNBRIGHT_NATIVE_GX"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v;
}
static bool gx_shadow_on() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("SUNBRIGHT_GX_SHADOW"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v;
}
// Returns true when the GUEST handled the call (override must return immediately).
static bool gx_guard(CPUState& cpu, u32 addr, u32 shadow_ea, u32 native_val, const char* nm) {
    if (gx_native_on()) return false;
    if (RecompFunc orig = recomp_raw(addr)) {
        CPUState c2 = cpu;
        orig(c2);
        if (gx_shadow_on() && shadow_ea) {
            static unsigned long bad = 0;
            const u32 g = sb_r32(shadow_ea);
            if (g != native_val && bad++ < 16)
                fprintf(stderr, "[gxshadow] %s MISMATCH native=%08x guest=%08x ea=%08x\n",
                        nm, native_val, g, shadow_ea);
        }
    }
    return true;
}


// ───────────────────────────────── overrides ─────────────────────────────────

// 0x8036128c GXSetTevColorIn(stage, a, b, c, d)
SUNBRIGHT_OVERRIDE(ov_GXSetTevColorIn, 0x8036128cu) {
    const u32 gx = gx_ptr(cpu);
    const u32 ea = gx + kOffTevc + cpu.gpr[3] * 4;
    const u32 v = pack_tev_color_in(sb_r32(ea), cpu.gpr[4], cpu.gpr[5],
                                    cpu.gpr[6], cpu.gpr[7]);
    if (gx_guard(cpu, 0x8036128cu, ea, v, "TevColorIn")) return;
    commit_bp(gx, ea, v);
}

// 0x8036130c GXSetTevAlphaIn(stage, a, b, c, d)
SUNBRIGHT_OVERRIDE(ov_GXSetTevAlphaIn, 0x8036130cu) {
    const u32 gx = gx_ptr(cpu);
    const u32 ea = gx + kOffTeva + cpu.gpr[3] * 4;
    const u32 v = pack_tev_alpha_in(sb_r32(ea), cpu.gpr[4], cpu.gpr[5],
                                    cpu.gpr[6], cpu.gpr[7]);
    if (gx_guard(cpu, 0x8036130cu, ea, v, "TevAlphaIn")) return;
    commit_bp(gx, ea, v);
}

// 0x80361390 GXSetTevColorOp(stage, op, bias, scale, clamp, out_reg)
SUNBRIGHT_OVERRIDE(ov_GXSetTevColorOp, 0x80361390u) {
    const u32 gx = gx_ptr(cpu);
    const u32 ea = gx + kOffTevc + cpu.gpr[3] * 4;
    const u32 v = pack_tev_op(sb_r32(ea), cpu.gpr[4], cpu.gpr[5],
                              cpu.gpr[6], cpu.gpr[7], cpu.gpr[8]);
    if (gx_guard(cpu, 0x80361390u, ea, v, "TevColorOp")) return;
    commit_bp(gx, ea, v);
}

// 0x80361450 GXSetTevAlphaOp(stage, op, bias, scale, clamp, out_reg)
SUNBRIGHT_OVERRIDE(ov_GXSetTevAlphaOp, 0x80361450u) {
    const u32 gx = gx_ptr(cpu);
    const u32 ea = gx + kOffTeva + cpu.gpr[3] * 4;
    const u32 v = pack_tev_op(sb_r32(ea), cpu.gpr[4], cpu.gpr[5],
                              cpu.gpr[6], cpu.gpr[7], cpu.gpr[8]);
    if (gx_guard(cpu, 0x80361450u, ea, v, "TevAlphaOp")) return;
    commit_bp(gx, ea, v);
}

// 0x80361744 GXSetTevSwapMode(stage, ras_sel, tex_sel)
SUNBRIGHT_OVERRIDE(ov_GXSetTevSwapMode, 0x80361744u) {
    const u32 gx = gx_ptr(cpu);
    const u32 ea = gx + kOffTeva + cpu.gpr[3] * 4;
    const u32 v = pack_tev_swap_mode(sb_r32(ea), cpu.gpr[4], cpu.gpr[5]);
    if (gx_guard(cpu, 0x80361744u, ea, v, "TevSwapMode")) return;
    commit_bp(gx, ea, v);
}

// 0x8036166c GXSetTevKColorSel(stage, sel)
SUNBRIGHT_OVERRIDE(ov_GXSetTevKColorSel, 0x8036166cu) {
    const u32 gx    = gx_ptr(cpu);
    const u32 stage = cpu.gpr[3];
    const u32 ea    = gx + kOffTevKsel + (stage >> 1) * 4;
    const u32 v = pack_tev_kcsel(sb_r32(ea), stage & 1, cpu.gpr[4]);
    if (gx_guard(cpu, 0x8036166cu, ea, v, "TevKColorSel")) return;
    commit_bp(gx, ea, v);
}

// 0x803616d8 GXSetTevKAlphaSel(stage, sel)
SUNBRIGHT_OVERRIDE(ov_GXSetTevKAlphaSel, 0x803616d8u) {
    const u32 gx    = gx_ptr(cpu);
    const u32 stage = cpu.gpr[3];
    const u32 ea    = gx + kOffTevKsel + (stage >> 1) * 4;
    const u32 v = pack_tev_kasel(sb_r32(ea), stage & 1, cpu.gpr[4]);
    if (gx_guard(cpu, 0x803616d8u, ea, v, "TevKAlphaSel")) return;
    commit_bp(gx, ea, v);
}

// 0x80360a18 GXSetTevIndirect(tev_stage, ind_stage, format, bias_sel, matrix_sel,
//                             wrap_s, wrap_t, add_prev, utc_lod, alpha_sel)
// Args 9/10 arrive on the guest stack (CW EABI: 9th arg word at caller-SP+8,
// 10th at +0xC; utc_lod is a GXBool byte in the low byte of its slot — the guest
// reads lbz 0xB(SP)). No shadow word: the register is built from zero and only
// the FIFO + bpSent are touched (disasm-verified).
SUNBRIGHT_OVERRIDE(ov_GXSetTevIndirect, 0x80360a18u) {
    const u32 gx       = gx_ptr(cpu);
    const u32 sp       = cpu.gpr[1];
    const u32 utc_lod  = sb_r8(sp + 0xB);
    const u32 alphasel = sb_r32(sp + 0xC);
    if (gx_guard(cpu, 0x80360a18u, 0, 0, "TevIndirect")) return;   // no shadow word
    fifo_bp(pack_tev_indirect(cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6],
                              cpu.gpr[7], cpu.gpr[8], cpu.gpr[9], cpu.gpr[10],
                              utc_lod, alphasel));
    sb_w16(gx + kOffBpSent, 0);
}

// 0x80361910 GXSetTevOrder(stage, coord, map, color)
SUNBRIGHT_OVERRIDE(ov_GXSetTevOrder, 0x80361910u) {
    const u32 gx    = gx_ptr(cpu);
    const u32 stage = cpu.gpr[3];
    const u32 coord = cpu.gpr[4];
    const u32 map   = cpu.gpr[5];
    const u32 color = cpu.gpr[6];

    const u32 tref_ea = gx + kOffTref + (stage >> 1) * 4;

    u32 tmap = map & ~0x100u;
    if (tmap >= 8) tmap = 0;                        // >= GX_MAX_TEXMAP → GX_TEXMAP0

    u32 tcoord;
    const u32 enab = sb_r32(gx + kOffTcoordEnab);
    if (coord >= 8) {                               // >= GX_MAX_TEXCOORD
        sb_w32(gx + kOffTcoordEnab, enab & ~(1u << stage));
        tcoord = 0;
    } else {
        sb_w32(gx + kOffTcoordEnab, enab | (1u << stage));
        tcoord = coord;
    }

    const u32 ras    = (color == 0xFF) ? 7u : c2r[color];   // GX_COLOR_NULL → 7
    const u32 enable = (map != 0xFF && !(map & 0x100)) ? 1u : 0u;
    const u32 v = pack_tref(sb_r32(tref_ea), stage & 1, tmap, tcoord, ras, enable);
    if (gx_guard(cpu, 0x80361910u, tref_ea, v, "TevOrder")) return;
    sb_w32(gx + kOffTexmapId + stage * 4, map);     // full id, incl. the 0x100 disable bit
    commit_bp(gx, tref_ea, v);
    sb_w32(gx + kOffDirtyState, sb_r32(gx + kOffDirtyState) | 1u);
}

} // namespace
