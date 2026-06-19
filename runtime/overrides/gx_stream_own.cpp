// GX stream assembler sync points — see runtime/gx_stream.h.
//
// The assembler holds gather-pipe bytes host-side; the guest's own pipeline sync
// points are where they must be decoded (synchronous OpcodeDecoder):
//   - GXFlush (0x8035d8f0): the GC contract is "after GXFlush the GPU can see
//     everything written so far" — every guest wait (drawsync pushBreakPoint,
//     GXDrawDone, PE-token polls) flushes first, so flushing here preserves all
//     ordering the game can observe.
//   - GXCopyDisp (0x8035ecec): the display copy = the frame boundary. Arms the
//     assembler on first sight (boot-time pipe writes from JIT-routed GX init
//     must never interleave with held bytes) and rotates the per-frame capture.
#include "../overrides.h"
#include "../gx_stream.h"
#include <cstdio>

extern void ngx_note_efb_copy(bool is_disp, u32 dest, u32 clear);   // ngx_j3d_shape.cpp: epoch tracking
extern u8 mem_r8(u32 ea);   // memory_bridge.cpp

// ── Blend-mode + copy-gamma capture (Delfino-wash hunt: find a darkening pass / settle copy gamma) ──
// The Delfino ground renders ~0.45x too bright in ngx; per-fragment material state is proven identical
// (see /shapeat). A uniform 0.45x darkening must be a SUBTRACT/multiply blend pass OR the EFB copy gamma.
// This tees GXSetBlendMode (histogram of distinct type/src/dst configs + counts) and the copy gamma.
// GX_BM_SUBTRACT(3) or BLEND with dst=SRCCLR(2) = a darkening blend. copy gamma!=1.0 = copy darkens.
namespace {
struct BlendCfg { unsigned char type, src, dst, op; unsigned long cnt; unsigned long lr; };
BlendCfg g_blend[32]; int g_blend_n = 0;
unsigned long g_blend_calls = 0; unsigned char g_copy_gamma = 0xFF; unsigned long g_copy_gamma_sets = 0;
// Distinct CALLER (LR) addresses of the DARKENING blend setters (subtract / dst=SRCCLR) — /fn names them.
unsigned long g_dark_lr[16]; unsigned long g_dark_lr_cnt[16]; int g_dark_lr_n = 0;
}
extern "C" int sb_gx_blend_dump(char* out, int cap) {
    static const char* TY[] = {"NONE","BLEND","LOGIC","SUBTRACT"};
    static const char* BF[] = {"ZERO","ONE","SRCCLR","INVSRCCLR","SRCA","INVSRCA","DSTA","INVDSTA"};
    static const char* GM[] = {"1.0","1.7","2.2"};
    int n = snprintf(out, cap, "gxblend: calls=%lu distinct=%d  copyGamma=%s (sets=%lu)\n",
                     g_blend_calls, g_blend_n, g_copy_gamma <= 2 ? GM[g_copy_gamma] : "?", g_copy_gamma_sets);
    for (int i = 0; i < g_blend_n && n < cap - 80; i++) {
        const BlendCfg& b = g_blend[i];
        n += snprintf(out + n, cap - n, "  %-9s src=%-9s dst=%-9s op=%u  cnt=%-6lu lr=%08lx%s\n",
            b.type < 4 ? TY[b.type] : "?", b.src < 8 ? BF[b.src] : "?", b.dst < 8 ? BF[b.dst] : "?",
            b.op, b.cnt, b.lr, (b.type == 3 || (b.type == 1 && b.dst == 2)) ? "   <== DARKENS" : "");
    }
    n += snprintf(out + n, cap - n, "  DARKENING-setter callers (LR; /fn?a=) :\n");
    for (int i = 0; i < g_dark_lr_n && n < cap - 60; i++)
        n += snprintf(out + n, cap - n, "    lr=%08lx cnt=%lu\n", g_dark_lr[i], g_dark_lr_cnt[i]);
    return n;
}

namespace {

extern "C" void func_8035d8f0(CPUState&);   // GXFlush
extern "C" void func_8035ecec(CPUState&);   // GXCopyDisp

// Run the original GX setter after the capture tee (matches ngx_super in ngx_j3d_shape.cpp): under
// pure-JIT no-recomp there is no recomp body and the GX side effect is discarded (ngx owns rendering),
// so capture-only. These are write-only setters; ngx reads blend from the J3D PE object model, not here.
static void gx_super(CPUState& cpu, u32 addr) {
    if (sunbright_purejit_mode()) return;            // no-recomp: capture-only (ngx owns rendering)
    if (RecompFunc o = recomp_raw(addr)) o(cpu);     // recomp mode (dead under no-recomp)
}

// GXSetBlendMode(type, src_factor, dst_factor, op). Capture distinct configs only when ngx is on
// (the game issues identical GX calls in NGX_PRESENT=0/1, so the NGX_PRESENT=1 capture is faithful).
SUNBRIGHT_OVERRIDE(ov_gx_blendmode, 0x80361dd0u) {
    const unsigned char ty = (unsigned char)cpu.gpr[3], sf = (unsigned char)cpu.gpr[4],
                        df = (unsigned char)cpu.gpr[5], op = (unsigned char)cpu.gpr[6];
    g_blend_calls++;
    int i = 0; for (; i < g_blend_n; i++) if (g_blend[i].type==ty && g_blend[i].src==sf && g_blend[i].dst==df) break;
    if (i == g_blend_n && g_blend_n < 32) g_blend[g_blend_n++] = {ty,sf,df,op,0,(unsigned long)cpu.lr};
    if (i < 32) { g_blend[i].cnt++; g_blend[i].lr = (unsigned long)cpu.lr; }
    if (ty == 3 || (ty == 1 && df == 2)) {           // DARKENING setter — record distinct callers
        const unsigned long lr = (unsigned long)cpu.lr;
        int j = 0; for (; j < g_dark_lr_n; j++) if (g_dark_lr[j] == lr) break;
        if (j == g_dark_lr_n && g_dark_lr_n < 16) { g_dark_lr[g_dark_lr_n] = lr; g_dark_lr_cnt[g_dark_lr_n++] = 0; }
        if (j < 16) g_dark_lr_cnt[j]++;
    }
    gx_super(cpu, 0x80361dd0u);
}

SUNBRIGHT_OVERRIDE(ov_gx_copygamma, 0x8035ecd0u) {
    g_copy_gamma = (unsigned char)cpu.gpr[3]; g_copy_gamma_sets++;
    gx_super(cpu, 0x8035ecd0u);
}

// GXSetCopyFilter(GXBool aa, u8 sample_pattern[12][2], GXBool vf, const u8 vfilter[7])  @0x8035eaa8
// The EFB->XFB copy applies the 7 vertical-tap vfilter: (sum of taps)>>6. Taps summing to >64 BRIGHTEN
// the copy. ngx presents its RT directly (no copy filter), so a >64 sum = ngx is darker by 64/sum.
// Capture vfilter[7] + sum to test the "wash = missing copy-filter brightening" hypothesis (0.80=64/80).
namespace { unsigned char g_vfilter[7] = {0}; unsigned g_vfilter_sum = 0; unsigned char g_vf_on = 0; unsigned long g_vf_sets = 0; }
extern "C" int sb_gx_copyfilter_dump(char* out, int cap) {
    return snprintf(out, cap, "copyfilter: vf=%u sets=%lu vfilter=[%u %u %u %u %u %u %u] sum=%u (==64 no change; >64 brightens by sum/64=%.3f)\n",
        g_vf_on, g_vf_sets, g_vfilter[0],g_vfilter[1],g_vfilter[2],g_vfilter[3],g_vfilter[4],g_vfilter[5],g_vfilter[6],
        g_vfilter_sum, g_vfilter_sum/64.0);
}
SUNBRIGHT_OVERRIDE(ov_gx_copyfilter, 0x8035eaa8u) {
    g_vf_on = (unsigned char)cpu.gpr[5]; g_vf_sets++;
    const u32 vfp = cpu.gpr[6];
    if (vfp >= 0x80000000u && vfp < 0x81800000u) {
        unsigned s = 0;
        for (int i = 0; i < 7; i++) { g_vfilter[i] = mem_r8(vfp + i); s += g_vfilter[i]; }
        g_vfilter_sum = s;
    }
    gx_super(cpu, 0x8035eaa8u);
}

SUNBRIGHT_OVERRIDE(ov_gxs_flush, 0x8035d8f0u) {
    func_8035d8f0(cpu);          // guest body: writes the 32-byte alignment pad
    gxs_flush("gxflush");
}

SUNBRIGHT_OVERRIDE(ov_gxs_copydisp, 0x8035ececu) {
    const u32 dd = cpu.gpr[3], dc = cpu.gpr[4];   // GXCopyDisp(dest, clear)
    func_8035ecec(cpu);          // guest body: emits the EFB→XFB copy commands
    ngx_note_efb_copy(/*is_disp=*/true, dd, dc);   // this epoch's passes went to the DISPLAY
    gxs_arm();
    gxs_frame_boundary();
}

}  // namespace
