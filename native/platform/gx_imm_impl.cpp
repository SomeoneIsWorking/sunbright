// gx_imm_impl.cpp — native GX IMMEDIATE-MODE capture (SLICE 2 of renderer-attach).
//
// The GameCube immediate-mode draw API (GXBegin / GXPosition* / GXColor* / GXTexCoord* /
// GXEnd) streams vertices into the write-gather FIFO. The native renderer has no FIFO and
// reads the J3D OBJECT MODEL for scene geometry — but 2D/HUD content (the fader, the
// GC-logo overlay, J2D windows, font glyphs, file-select panes) is drawn ONLY through this
// immediate API, so the object-model path misses it. We capture it here instead: GXVert.h
// routes the immediate writers to the sb_gx_imm_* hooks below (under SMS_NATIVE_PLATFORM),
// which build native vertices, transform them through the captured GXState projection +
// position matrix (gx_imm_xform.h, a pure unit-tested function), triangulate, and hand the
// resulting Vulkan-NDC triangle list — grouped into per-texture BATCHES — to the present
// layer (sms_boot_present.cpp).
//
// Textured 2D (file-select windows/pictures/text): a primitive is TEXTURED iff it emitted
// texcoords AND a texmap-0 was bound (GXLoadTexObj) at GXBegin. We snapshot the bound GX
// texture descriptor (gx_state boundTex[0]) into the batch; the present layer decodes it
// (sb_tex_decode) and samples it × the vertex colour (GX_MODULATE). Untextured prims (the
// gradient, the solid window fill) form a colour-only batch (texture==none).
//
// This is NOT a FIFO emulator: it captures the game's drawn quads at the API seam and
// re-projects them, the sanctioned "rebuild as a PC game" path (see CLAUDE.md ground rules).
//
// Threading: the engine draws and presents on ONE thread (VIWaitForRetrace -> present fires
// synchronously at frame end). No locking: the frame accumulates during the frame; present
// consumes it. We clear lazily on the first GXBegin AFTER a present consumed, so several
// presents within one frame (waitForRetrace loops) render the same accumulated frame.

#include <execinfo.h>        // backtrace / backtrace_symbols (SB_IMM_TRACE)
#include <cstdlib>           // free
#include <algorithm>         // std::min/max (imm bbox)
#include <dolphin/gx.h>      // GXColor etc. (gx_state.h uses them)
#include "gx_state.h"
#include "gx_imm_xform.h"
#include "tex_decode.h"      // sb_tex_decode / sb_tex_pad_* (SB_IMM_PRIM_DBG texture dump)
#include "ngx_render_data.h" // NgxTevState — the full TEV combiner carried per imm batch
#include "gx_fifo.h"          // GX_ORACLE FIFO recorder — DRAW opcode + vertex-payload encoding
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <memory>

using sb::platform::gx::state;
using sb::render::SbImmVtx;
using sb::render::SbImmRawVtx;
using sb::render::SbImmBatch;

namespace {

// GX enum constants used here (avoid pulling the whole header dependency in).
constexpr int kVA_CLR0 = 11;   // GX_VA_CLR0
constexpr int kVA_TEX0 = 13;   // GX_VA_TEX0
constexpr int kAttrDirect = 1; // GX_DIRECT

// GX color-index (paletted) formats need a TLUT: C4(0x8) C8(0x9) C14X2(0xA).
inline bool fmt_is_paletted(int fmt) { return fmt == 0x8 || fmt == 0x9 || fmt == 0xA; }

// --- accumulated frame: flat triangle list + per-texture batches (consumed by present) ---
std::vector<SbImmVtx>   g_frame_tris;
std::vector<SbImmBatch> g_batches;
// Per-frame TEV-combiner store. One NgxTevState per distinct imm draw; batches point into
// it. A vector OF unique_ptr (not a deque/vector-of-value) so (a) the default ctor allocates
// NOTHING at static-init time — a std::deque allocates its map in its ctor and that ran
// before the game OS/heap was up, faulting; and (b) push_back never invalidates the held
// pointers (each NgxTevState is its own heap node).
//
// POOL, not clear()+new each frame: a global operator-new override routes these into the JKR
// solid scene heap, whose free() is a NO-OP (solid heaps reclaim only via freeAll). So
// g_tevstore.clear() + a fresh `new NgxTevState` per GXBegin LEAKED one node per imm draw per
// frame into the solid heap — it filled over time and, once the NPC population had already
// saturated the scene heap, EVERY per-frame draw OOM'd → host-overflow flood (859k lines) and
// the plaza map collapsed. Instead keep the allocated nodes and reuse them: reset g_tevstore_used
// to 0 each frame and overwrite existing nodes; only `new` when the high-water mark grows. This
// caps NgxTevState allocations at the per-frame peak (no per-frame leak); pointers stay stable.
std::vector<std::unique_ptr<NgxTevState>> g_tevstore;
size_t g_tevstore_used = 0;     // nodes handed out this frame (pool high-water reuse)
NgxTevState g_tevSnap;          // the TEV captured at the current GXBegin
bool g_consumed = true;        // start true so the first GXBegin clears the (empty) buffer

// Build an NgxTevState from the live GX TEV registers (state().tev). The GXState TEV block
// is stored in the SAME bit layout NgxTevStage consumes (see gx_state.h), so this is a
// near-direct field copy — the immediate-mode counterpart of sb_build_tev_state (J3D path).
inline void snapshot_tev() {
    auto& g = state();
    NgxTevState& st = g_tevSnap;
    st = NgxTevState{};
    int ns = g.numTevStages; if (ns < 1) ns = 1; if (ns > 16) ns = 16;
    st.num_stages = (uint8_t)ns;
    for (int s = 0; s < ns; ++s) {
        st.stage[s].color_env  = g.tev.colorEnv[s];
        st.stage[s].alpha_env  = g.tev.alphaEnv[s];
        st.stage[s].texcoord   = g.tev.texcoord[s];
        st.stage[s].texmap     = g.tev.texmap[s];
        st.stage[s].color_chan = g.tev.colorChan[s];
        st.stage[s].kcsel      = g.tev.kcsel[s];
        st.stage[s].kasel      = g.tev.kasel[s];
    }
    for (int c = 0; c < 4; ++c) for (int k = 0; k < 4; ++k) {
        st.tev_color[c][k] = g.tev.tevColor[c][k];
        st.kcolor[c][k]    = g.tev.kColor[c][k];
    }
    for (int t = 0; t < 4; ++t) st.swap_table[t] = g.tev.swapTable[t];
    // 2D overlays: depth off (drawn on top), faithful blend is carried separately on the
    // batch. The PE alpha-test still matters (J2DPicture's alpha path), but J2D leaves it
    // ALWAYS, so the default (no discard) is correct here.
    st.pe = NgxPEState{};
    st.pe.z_test = 0; st.pe.z_write = 0;
    if (const char* d = std::getenv("SB_IMM_CHAN_DBG"); d && d[0] && d[0] != '0') {
        static long s_n = 0;
        if (s_n < 40) { ++s_n;
            int cc = g.tev.colorChan[0] & 1;
            const auto& ch = g.chan[cc];
            bool clr0_direct = g.immVtxDesc[kVA_CLR0] == kAttrDirect;
            std::fprintf(stderr,
                "[imm-chan] c0env=%08x colorChan=%u clr0_direct=%d ctrl=%08x matSrc=%d en=%d "
                "matColor=(%u,%u,%u,%u) ambColor=(%u,%u,%u,%u)\n",
                g.tev.colorEnv[0], g.tev.colorChan[0], clr0_direct, ch.ctrl, (int)(ch.ctrl & 1),
                (int)((ch.ctrl >> 1) & 1),
                ch.matColor.r, ch.matColor.g, ch.matColor.b, ch.matColor.a,
                ch.ambColor.r, ch.ambColor.g, ch.ambColor.b, ch.ambColor.a);
        }
    }
    // st.key is left 0: the present layer keys the pipeline by the hash of the generated
    // fragment string (matching the J3D path), so no struct hash is needed here.
}

// --- in-progress primitive (between GXBegin and GXEnd) ---
bool  g_in_begin = false;
int   g_prim = 0;
int   g_prim_nverts = 0;       // GXBegin's declared vertex count (0 = unknown → rely on GXEnd)
std::vector<SbImmVtx> g_prim_verts;
bool  g_prim_has_uv = false;   // a GXTexCoord* fired this prim
int   g_tc_phase = 0;          // 1-component texcoord pairing: 0 = expecting S, 1 = T

// projection/position/viewport snapshot taken at GXBegin (state is set before the draw).
int   g_projType = 1;
float g_projMtx[6] = {0};
float g_posMtx[3][4] = {{0}};
float g_vp[6] = {0, 0, 640, 480, 0, 1};

// bound TEX0 vertex-attr fmt (for integer texcoord dequant) + bound texmap-0 descriptor.
int   g_tcType = 2 /*GX_U16*/, g_tcFrac = 15;   // J2D default (J2DGrafContext)
SbImmBatch g_texSnap;          // bound texture for the current prim (textured iff .textured)

// blend state (GXSetBlendMode) captured live at GXBegin — faithful per-prim blend instead of
// the present layer's old hardcoded SRCALPHA/INVSRCALPHA (which drew alpha-less RGB565 sprites,
// e.g. the file-select shine sparkles, as opaque squares).
signed char g_blendType = 1, g_blendSrc = 4, g_blendDst = 5;  // GX_BM_BLEND/SRCALPHA/INVSRCALPHA
bool g_colorUpdate = true;  // GXSetColorUpdate: false = write no colour (alpha-plane-only pass)
bool g_alphaUpdate = true;  // GXSetAlphaUpdate: false = write no dst-alpha
bool g_dstAlphaForce = false;  // GXSetDstAlpha enable: force the written alpha to a constant
unsigned char g_dstAlphaVal = 0;  // the forced constant (SMS only ever forces 0)

// current immediate-mode colour (GXColor*/GXParam sets it; applied to the last vertex).
float g_cr = 1, g_cg = 1, g_cb = 1, g_ca = 1;

const bool g_dbg = [] { const char* e = std::getenv("SB_GX_IMM_DBG"); return e && e[0] && e[0] != '0'; }();
int g_dbg_left = 12;

// SB_IMM_PRIM_DBG=N: dump EVERY finalized primitive of the FIRST frame whose prim count >= N —
// prim type, vert count, textured fmt+dims, NDC bbox, UV bbox. The way to find a mis-shaped 2D
// draw (e.g. the slot-row "@" starbursts: a stray vertex makes the NDC/UV bbox blow out, the
// radiating-spike signature). The settled file-select has the most prims (banner + 3 slots +
// glyphs + window borders), so a threshold ~50 fires on it, not on a sparse early frame. Per-prim
// records accumulate during the frame; printed once (then disabled) + cleared at take.
const int g_prim_dbg_at = [] { const char* e = std::getenv("SB_IMM_PRIM_DBG"); return e && e[0] ? std::atoi(e) : 0; }();
bool g_prim_dbg_done = false;
struct PrimRec { int prim, nv; bool textured; int fmt, w, h; int bt, bs, bd;
                 float xmn, xmx, ymn, ymx, umn, umx, vmn, vmx;
                 const void* image; const void* tlut; int tlutfmt;
                 int projType; float pm0, pm1, pm2, pm3; float tx, ty, tz; };
std::vector<PrimRec> g_prim_recs;

void snapshot_state() {
    auto& g = state();
    g_projType = (int)g.projType;
    for (int i = 0; i < 6; ++i) g_projMtx[i] = g.projMtx[i];
    const u32 slot = g.currentMtx / 3;
    const auto& m = g.posMtx[slot < 64 ? slot : 0];
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) g_posMtx[r][c] = m[r][c];
    g_vp[0] = g.vpLeft; g_vp[1] = g.vpTop; g_vp[2] = g.vpWd; g_vp[3] = g.vpHt;
    g_vp[4] = g.vpNearz; g_vp[5] = g.vpFarz;
    g_blendType = (signed char)g.blendType;
    g_blendSrc  = (signed char)g.blendSrc;
    g_blendDst  = (signed char)g.blendDst;
    g_colorUpdate = (g.colorUpdate != 0);
    g_alphaUpdate = (g.alphaUpdate != 0);
    g_dstAlphaForce = (g.dstAlphaEnable != 0);
    g_dstAlphaVal = g.dstAlpha;
}

// Snapshot the bound texmap-0 into g_texSnap. textured = TEX0 is a DIRECT attr (texcoords
// expected) AND a texture object is bound. Resolve the palette for CI formats from the GX
// TLUT table. The actual textured/untextured decision is finalized at GXEnd (needs the
// "did this prim emit texcoords" signal too, so a stale binding can't textured-ize the
// gradient, which sets TEX0=GX_NONE and never calls GXTexCoord).
void snapshot_texture(int vtxfmt) {
    auto& g = state();
    if (vtxfmt >= 0 && vtxfmt < 8) {
        const auto& f = g.vtxAttrFmt[vtxfmt][kVA_TEX0];
        g_tcType = f.type; g_tcFrac = f.frac;
    }
    g_texSnap = SbImmBatch{};
    const auto& bt = g.boundTex[0];
    const bool tex0direct = g.immVtxDesc[kVA_TEX0] == kAttrDirect;
    if (!tex0direct || !bt.valid || !bt.image) return;
    g_texSnap.textured = true;
    g_texSnap.image = bt.image;
    g_texSnap.w = bt.w; g_texSnap.h = bt.h;
    g_texSnap.fmt = bt.fmt; g_texSnap.wrapS = bt.wrapS; g_texSnap.wrapT = bt.wrapT;
    g_texSnap.linear = (bt.magFilt == 1 /*GX_LINEAR*/) ? 1 : 0;
    g_texSnap.tlut = nullptr; g_texSnap.tlutfmt = 0;
    if (fmt_is_paletted(bt.fmt) && bt.tlutName < 20) {
        const auto& tl = g.tlut[bt.tlutName];
        if (tl.valid) { g_texSnap.tlut = tl.lut; g_texSnap.tlutfmt = tl.fmt; }
    }
}

// Append the just-triangulated verts as a batch; coalesce with the previous batch when it
// shares the same texture binding (4 window-corner quads → one batch per border texture).
void push_batch(unsigned start, unsigned count, const SbImmBatch& tex) {
    if (!count) return;
    if (!g_batches.empty()) {
        SbImmBatch& last = g_batches.back();
        const bool sameTex = (last.textured == tex.textured) &&
                             (!tex.textured || (last.image == tex.image && last.w == tex.w &&
                                                last.h == tex.h && last.fmt == tex.fmt));
        const bool sameBlend = last.blendType == tex.blendType &&
                               last.blendSrc == tex.blendSrc && last.blendDst == tex.blendDst;
        // Only coalesce prims that share the SAME combiner — two same-texture draws with
        // different TEV (e.g. a ramp picture then a plain modulate) must stay separate.
        const bool sameTev = last.tev == tex.tev ||
            (last.tev && tex.tev &&
             std::memcmp(last.tev, tex.tev, sizeof(NgxTevState)) == 0);
        if (sameTex && sameBlend && sameTev && last.colorUpdate == tex.colorUpdate &&
            last.alphaUpdate == tex.alphaUpdate && last.dstAlphaForce == tex.dstAlphaForce &&
            last.dstAlphaVal == tex.dstAlphaVal &&
            last.vstart + last.vcount == start) { last.vcount += count; return; }
    }
    SbImmBatch b = tex;
    b.vstart = start; b.vcount = count;
    g_batches.push_back(b);
}

} // namespace

extern "C" {

// ---- capture hooks (called from GXVert.h immediate writers) ----------------
// Finalize the in-progress primitive: triangulate, classify textured, push the batch.
// Called by GXEnd, OR auto-fired once the primitive has received its full GXBegin vertex
// count. On real GC hardware a primitive auto-terminates after `nverts` (GXEnd is a no-op),
// so some draws (JUTResFont glyph quads) legitimately never call GXEnd — without the
// vertex-count auto-flush their batch would be silently dropped (the file-select banner /
// "Select data" + slot-label glyphs were invisible for exactly this reason).
// SB_IMM_TRACE: backtrace the native caller of each GXBegin (see sb_gx_imm_begin).
static void* g_imm_caller[6];
static int   g_imm_caller_n = 0;

// Encode this primitive as a GC DRAW opcode + vertex payload into sb::gxfifo,
// so the GX_ORACLE sink's OpcodeDecoder drives Dolphin's real render pipeline.
// Uses a fixed vertex layout the SDK header redirect knows (position 3xf32 +
// color0 4xf32 direct + texcoord0 2xf32 direct, matching SbImmVtx byte-for-byte
// = 36 bytes). CP registers are programmed once per frame to describe this
// format under VAT slot 0.
static void emit_fifo_draw() {
    if (!sb::gxfifo::enabled() || g_prim_verts.empty()) return;

    // One-time CP setup per frame: vertex descriptor + attribute-format table.
    static bool s_cp_set_this_frame = false;
    // Reset happens implicitly at oracle_present's sb::gxfifo::reset_frame() —
    // we re-emit CP setup on first draw of the next frame.
    if (!s_cp_set_this_frame) {
        s_cp_set_this_frame = true;
        // VCD_LO (CP reg 0x50): pos=direct(0x01<<9), color0=direct(0x01<<13).
        u32 vcd_lo = (1u << 9) | (1u << 13);
        sb::gxfifo::cp_write(0x50, vcd_lo);
        // VCD_HI (CP reg 0x60): texcoord0=direct (0x01<<0).
        u32 vcd_hi = 0x1;
        sb::gxfifo::cp_write(0x60, vcd_hi);
        // VAT slot 0 (CP regs 0x70-0x72). Pack position(3xF32), color0(RGBA8),
        // texcoord0(2xF32). Details of the field packing come from Dolphin's
        // CPMemory.h VAT bit layout — using 0 defaults means the game's actual
        // attributes are only correctly parsed once the setter emits VAT for
        // each format. First-cut: position=count3 type=4(F32) frac=0.
        // Full VAT encoding is next arc.
    }
    // Fake next-frame reset — the FIFO buffer is cleared on present, so on the
    // next present the sink will call sb_gx_emit_full_state() again which
    // triggers re-setup here via the sentinel. Set the flag every present so
    // this is correct; the flag reset happens in sb_gx_imm_frame_begin below.

    // DRAW opcode = base | (vtxfmt & 7). SMS immediate mode uses vtxfmt=0.
    u8 opcode = 0;
    switch (g_prim) {
        case 0x80: opcode = 0x80; break;   // QUADS
        case 0x90: opcode = 0x90; break;   // TRIANGLES
        case 0x98: opcode = 0x98; break;   // TRIANGLE_STRIP
        case 0xA0: opcode = 0xA0; break;   // TRIANGLE_FAN
        case 0xA8: opcode = 0xA8; break;   // LINES
        case 0xB0: opcode = 0xB0; break;   // LINE_STRIP
        case 0xB8: opcode = 0xB8; break;   // POINTS
        default:   return;                  // unknown primitive
    }
    const u16 nverts = (u16)g_prim_verts.size();
    sb::gxfifo::write_u8(opcode);
    sb::gxfifo::write_u16_be(nverts);
    // Vertex payload: position (3xF32) + color0 (4xF32 → 4xU8 RGBA) + texcoord0 (2xF32).
    // Layout MUST match VAT slot 0 above; for the first cut we emit F32 across the
    // board and rely on Dolphin's default VAT (mostly F32) to accept it.
    for (const auto& v : g_prim_verts) {
        sb::gxfifo::write_f32_be(v.x);
        sb::gxfifo::write_f32_be(v.y);
        sb::gxfifo::write_f32_be(v.z);
        sb::gxfifo::write_u8((u8)(v.r * 255.0f));
        sb::gxfifo::write_u8((u8)(v.g * 255.0f));
        sb::gxfifo::write_u8((u8)(v.b * 255.0f));
        sb::gxfifo::write_u8((u8)(v.a * 255.0f));
        sb::gxfifo::write_f32_be(v.u);
        sb::gxfifo::write_f32_be(v.v);
    }
}
// Reset the CP-set-this-frame gate at frame boundary — called from
// sb_boot_capture_frame_begin (once per present).
extern "C" void sb_gx_imm_fifo_frame_begin(void);

static void finalize_prim(void) {
    if (!g_in_begin) return;
    // Emit the DRAW opcode + vertex payload into sb::gxfifo BEFORE we mark the
    // primitive done (otherwise g_prim_verts.clear() below would drop them).
    emit_fifo_draw();
    g_in_begin = false;
    if (g_prim_dbg_at && !g_prim_dbg_done && !g_prim_verts.empty()) {
        PrimRec r{}; r.prim = g_prim; r.nv = (int)g_prim_verts.size();
        r.textured = (g_prim_has_uv && g_texSnap.textured);
        r.fmt = g_texSnap.fmt; r.w = g_texSnap.w; r.h = g_texSnap.h;
        r.image = g_texSnap.image; r.tlut = g_texSnap.tlut; r.tlutfmt = g_texSnap.tlutfmt;
        r.bt = g_blendType; r.bs = g_blendSrc; r.bd = g_blendDst;
        r.projType = g_projType;
        r.pm0 = g_projMtx[0]; r.pm1 = g_projMtx[1]; r.pm2 = g_projMtx[2]; r.pm3 = g_projMtx[3];
        r.tx = g_posMtx[0][3]; r.ty = g_posMtx[1][3]; r.tz = g_posMtx[2][3];
        r.xmn = r.ymn = r.umn = r.vmn = 1e30f; r.xmx = r.ymx = r.umx = r.vmx = -1e30f;
        for (auto& v : g_prim_verts) {
            r.xmn = std::min(r.xmn, v.x); r.xmx = std::max(r.xmx, v.x);
            r.ymn = std::min(r.ymn, v.y); r.ymx = std::max(r.ymx, v.y);
            r.umn = std::min(r.umn, v.u); r.umx = std::max(r.umx, v.u);
            r.vmn = std::min(r.vmn, v.v); r.vmx = std::max(r.vmx, v.v);
        }
        g_prim_recs.push_back(r);
    }
    // SB_IMM_TRACE: report the native caller of a full-width screen-edge textured quad
    // (the white letterbox-bar signature: textured, |x|~1, |y|>=~0.7), once.
    static bool s_traced = false;
    if (!s_traced && std::getenv("SB_IMM_TRACE") && (g_prim_has_uv && g_texSnap.textured) && !g_prim_verts.empty()) {
        float xmn=1e30f,xmx=-1e30f,ymn=1e30f,ymx=-1e30f;
        for (auto& v : g_prim_verts){ if(v.x<xmn)xmn=v.x; if(v.x>xmx)xmx=v.x; if(v.y<ymn)ymn=v.y; if(v.y>ymx)ymx=v.y; }
        if (xmn < -0.95f && xmx > 0.95f && (ymn < -0.7f || ymx > 0.7f) && (ymx-ymn) < 0.6f) {
            s_traced = true;
            std::fprintf(stderr, "[imm-trace] BAR prim nv=%d x[%.2f,%.2f] y[%.2f,%.2f] tex=%dx%d fmt=0x%x blend=%d/%d/%d caller:\n",
                         (int)g_prim_verts.size(), xmn,xmx,ymn,ymx, g_texSnap.w,g_texSnap.h,g_texSnap.fmt,
                         g_blendType,g_blendSrc,g_blendDst);
            char** syms = backtrace_symbols(g_imm_caller, g_imm_caller_n);
            if (syms) { for (int i=0;i<g_imm_caller_n;++i) std::fprintf(stderr, "    %s\n", syms[i]); free(syms); }
            auto& gs = state();
            std::fprintf(stderr, "    TEV nstages: tevColor C0(black)=%d,%d,%d,%d C1(white)=%d,%d,%d,%d CPREV=%d,%d,%d,%d\n",
                gs.tev.tevColor[1][0],gs.tev.tevColor[1][1],gs.tev.tevColor[1][2],gs.tev.tevColor[1][3],
                gs.tev.tevColor[2][0],gs.tev.tevColor[2][1],gs.tev.tevColor[2][2],gs.tev.tevColor[2][3],
                gs.tev.tevColor[0][0],gs.tev.tevColor[0][1],gs.tev.tevColor[0][2],gs.tev.tevColor[0][3]);
            for (int st=0; st<4; ++st)
                std::fprintf(stderr, "    stage%d colorEnv=%08x alphaEnv=%08x texmap=%u chan=%u kc=%u\n",
                    st, gs.tev.colorEnv[st], gs.tev.alphaEnv[st], gs.tev.texmap[st], gs.tev.colorChan[st], gs.tev.kcsel[st]);
            // 8x8 texture content (intensity) via the snapshot.
            if (g_texSnap.image && g_texSnap.w<=16 && g_texSnap.h<=16) {
                std::fprintf(stderr, "    tex %dx%d fmt=0x%x first bytes:", g_texSnap.w,g_texSnap.h,g_texSnap.fmt);
                const uint8_t* p=(const uint8_t*)g_texSnap.image;
                for (int k=0;k<16;++k) std::fprintf(stderr, " %02x", p[k]);
                std::fprintf(stderr, "\n");
            }
        }
    }
    // SB_IMM_TRACE_SOLID: backtrace the native caller of a FULLSCREEN UNTEXTURED ADDITIVE
    // quad (ib0 signature: the white ONE/ONE wash that covers the plaza). Catches the
    // persistent 2D overlay the wipe doesn't account for.
    static bool s_traced_solid = false;
    if (!s_traced_solid && std::getenv("SB_IMM_TRACE_SOLID") && !g_prim_has_uv &&
        g_blendType == 1 && g_blendSrc == 1 && g_blendDst == 1 && !g_prim_verts.empty()) {
        float xmn=1e30f,xmx=-1e30f,ymn=1e30f,ymx=-1e30f;
        for (auto& v : g_prim_verts){ if(v.x<xmn)xmn=v.x; if(v.x>xmx)xmx=v.x; if(v.y<ymn)ymn=v.y; if(v.y>ymx)ymx=v.y; }
        if (xmn < -1.0f && xmx > 1.0f && ymn < -1.0f && ymx > 1.0f) {
            s_traced_solid = true;
            std::fprintf(stderr, "[imm-trace] SOLID additive fullscreen prim nv=%d x[%.2f,%.2f] y[%.2f,%.2f] "
                         "rgba=(%.2f,%.2f,%.2f,%.2f) blend=%d/%d/%d caller:\n",
                         (int)g_prim_verts.size(), xmn,xmx,ymn,ymx,
                         g_prim_verts[0].r,g_prim_verts[0].g,g_prim_verts[0].b,g_prim_verts[0].a,
                         g_blendType,g_blendSrc,g_blendDst);
            char** syms = backtrace_symbols(g_imm_caller, g_imm_caller_n);
            if (syms) { for (int i=0;i<g_imm_caller_n;++i) std::fprintf(stderr, "    %s\n", syms[i]); free(syms); }
        }
    }
    const unsigned start = (unsigned)g_frame_tris.size();
    sb::render::imm_triangulate(g_prim, g_prim_verts.data(),
                                (int)g_prim_verts.size(), g_frame_tris);
    const unsigned count = (unsigned)g_frame_tris.size() - start;
    // Textured only if the prim actually emitted texcoords AND a texture was bound: a
    // stale boundTex from an earlier window can't texture-ize the (texcoord-less) gradient.
    SbImmBatch tex = (g_prim_has_uv && g_texSnap.textured) ? g_texSnap : SbImmBatch{};
    tex.blendType = g_blendType; tex.blendSrc = g_blendSrc; tex.blendDst = g_blendDst;
    tex.colorUpdate = g_colorUpdate;
    tex.alphaUpdate = g_alphaUpdate;
    tex.dstAlphaForce = g_dstAlphaForce;
    tex.dstAlphaVal = g_dstAlphaVal;
    // Carry the full TEV combiner captured at this prim's GXBegin (store in the per-frame
    // deque so the pointer is stable until present consumes). The present layer generates
    // the real combiner fragment from it (honors J2DPicture's black/white intensity ramp).
    // Reuse a pooled node (no per-frame solid-heap leak — see g_tevstore decl). Grow only at
    // the high-water mark; otherwise overwrite the existing node in place (pointer stays stable).
    if (g_tevstore_used >= g_tevstore.size())
        g_tevstore.push_back(std::unique_ptr<NgxTevState>(new NgxTevState(g_tevSnap)));
    else
        *g_tevstore[g_tevstore_used] = g_tevSnap;
    tex.tev = g_tevstore[g_tevstore_used].get();
    ++g_tevstore_used;
    push_batch(start, count, tex);
}

void sb_gx_imm_begin(int prim, int vtxfmt, int nverts) {
    static const bool s_trace = [](){
        const char* a = std::getenv("SB_IMM_TRACE"); const char* b = std::getenv("SB_IMM_TRACE_SOLID");
        return (a && a[0] && a[0] != '0') || (b && b[0] && b[0] != '0'); }();
    if (s_trace) g_imm_caller_n = backtrace(g_imm_caller, 6);
    if (g_consumed) { g_frame_tris.clear(); g_batches.clear(); g_tevstore_used = 0; g_consumed = false; }
    // A previous primitive that omitted the (HW no-op) GXEnd — e.g. JUTResFont glyph quads —
    // is flushed HERE, at the next GXBegin, with ALL its vertex attributes intact. (Finalizing
    // at the nverts-th GXPosition was wrong: on GC a vertex carries pos THEN colour THEN
    // texcoord, so the last vertex's colour/texcoord arrive AFTER its position — flushing on
    // the position dropped the glyph's top-left corner colour+UV → uv=(0,0), half-smeared
    // glyphs that read as faint. The primitive completes after nverts whole vertices, not
    // nverts positions, so we defer the flush to the next prim/end/present.)
    if (g_in_begin) finalize_prim();
    g_in_begin = true;
    g_prim = prim;
    g_prim_nverts = nverts;
    g_prim_verts.clear();
    g_prim_has_uv = false;
    g_tc_phase = 0;
    snapshot_state();
    snapshot_texture(vtxfmt);
    snapshot_tev();
}

void sb_gx_imm_pos(float x, float y, float z) {
    if (!g_in_begin) return;
    SbImmRawVtx raw{ x, y, z, g_cr, g_cg, g_cb, g_ca, 0.0f, 0.0f };
    SbImmVtx p = sb::render::imm_project(raw, g_projType, g_projMtx, g_posMtx, g_vp);
    g_prim_verts.push_back(p);
    g_tc_phase = 0;   // each new vertex restarts 1-component (S,T) pairing
    if (g_dbg && g_dbg_left > 0) {
        --g_dbg_left;
        std::fprintf(stderr, "[gx_imm] prim=%#x pos(%.1f,%.1f,%.1f) -> ndc(%.3f,%.3f,%.3f) "
                     "tex=%d fmt=0x%x %dx%d proj=%d\n",
                     g_prim, x, y, z, p.x, p.y, p.z, g_texSnap.textured,
                     g_texSnap.fmt, g_texSnap.w, g_texSnap.h, g_projType);
    }
    // NOTE: no auto-terminate here. A primitive that omits GXEnd is flushed at the next
    // GXBegin / GXEnd / present-take, AFTER its final vertex's colour+texcoord arrive (the
    // GC vertex order is pos→colour→texcoord). Flushing on the nverts-th position dropped the
    // last vertex's attributes (the faint-glyph bug).
}

// GXColor* sets the colour for the vertex JUST submitted (GX order is pos then colour) and
// becomes the running colour for any subsequent position with no colour of its own.
void sb_gx_imm_color_rgba(unsigned r, unsigned g, unsigned b, unsigned a) {
    g_cr = r / 255.0f; g_cg = g / 255.0f; g_cb = b / 255.0f; g_ca = a / 255.0f;
    if (g_in_begin && !g_prim_verts.empty()) {
        auto& v = g_prim_verts.back();
        v.r = g_cr; v.g = g_cg; v.b = g_cb; v.a = g_ca;
    }
}
void sb_gx_imm_color_u32(unsigned c) {   // packed 0xRRGGBBAA
    sb_gx_imm_color_rgba((c >> 24) & 0xff, (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
}
// J2DWindow border edges write the vertex colour via GXParam1s32(-1) (= 0xFFFFFFFF white).
// Inside a GXBegin that's the DIRECT CLR0 attribute; elsewhere it's a raw FIFO word (ignored).
void sb_gx_imm_param_color_s32(int v) {
    if (g_in_begin && state().immVtxDesc[kVA_CLR0] == kAttrDirect)
        sb_gx_imm_color_u32((unsigned)v);
}

// ---- texcoord hooks --------------------------------------------------------
static void set_uv(float u, float v) {
    if (!g_in_begin || g_prim_verts.empty()) return;
    g_prim_verts.back().u = u; g_prim_verts.back().v = v;
    g_prim_has_uv = true;
}
void sb_gx_imm_texcoord_f2(float s, float t) { set_uv(s, t); }
void sb_gx_imm_texcoord_i2(unsigned s, unsigned t) {
    set_uv(sb::render::imm_texcoord_scale(s, g_tcType, g_tcFrac, 16),
           sb::render::imm_texcoord_scale(t, g_tcType, g_tcFrac, 16));
}
void sb_gx_imm_texcoord_f1(float val) {
    if (!g_in_begin || g_prim_verts.empty()) return;
    if (g_tc_phase == 0) { g_prim_verts.back().u = val; g_tc_phase = 1; }
    else                 { g_prim_verts.back().v = val; g_tc_phase = 0; }
    g_prim_has_uv = true;
}
void sb_gx_imm_texcoord_i1(unsigned bits) {
    sb_gx_imm_texcoord_f1(sb::render::imm_texcoord_scale(bits, g_tcType, g_tcFrac, 16));
}

void sb_gx_imm_end(void) {
    // No-op if the primitive already auto-finalized at its declared vertex count.
    finalize_prim();
}

// ---- present bridge --------------------------------------------------------
// Legacy flat take: the whole frame as one untextured triangle list (Vulkan NDC + RGBA).
int sb_gx_imm_take(const SbImmVtx** out) {
    if (g_in_begin) finalize_prim();   // flush a trailing GXEnd-less prim (last glyph of frame)
    if (out) *out = g_frame_tris.data();
    g_consumed = true;
    return (int)g_frame_tris.size();
}

// Batch take: the flat vertex list + per-texture batches. Marks the buffer consumed so the
// next GXBegin starts a fresh frame. Returns the vertex count; *verts -> SbImmVtx[count].
int sb_gx_imm_take_batches(const SbImmVtx** verts, const SbImmBatch** batches, int* nbatch) {
    if (g_in_begin) finalize_prim();   // flush a trailing GXEnd-less prim (last glyph of frame)
    if (g_prim_dbg_at && !g_prim_dbg_done && (int)g_prim_recs.size() >= g_prim_dbg_at) {
        g_prim_dbg_done = true;
        std::fprintf(stderr, "[imm-prim] frame with %zu prims (>= %d):\n", g_prim_recs.size(), g_prim_dbg_at);
        for (size_t i = 0; i < g_prim_recs.size(); ++i) {
            const PrimRec& r = g_prim_recs[i];
            std::fprintf(stderr, "  p%zu prim=%#x nv=%d tex=%d fmt=0x%x %dx%d blend=%d/%d/%d "
                         "ndcX[%.3f,%.3f] ndcY[%.3f,%.3f] uv[%.3f,%.3f;%.3f,%.3f] img=%p\n",
                         i, r.prim, r.nv, r.textured, r.fmt, r.w, r.h, r.bt, r.bs, r.bd,
                         r.xmn, r.xmx, r.ymn, r.ymx, r.umn, r.umx, r.vmn, r.vmx, r.image);
            std::fprintf(stderr, "       projT=%d pm[%.4f,%.4f,%.4f,%.4f] pos_t=(%.1f,%.1f,%.1f)\n",
                         r.projType, r.pm0, r.pm1, r.pm2, r.pm3, r.tx, r.ty, r.tz);
            // Decode each textured prim to a PPM so the actual sprite content is visible
            // (the slot-row "@" starbursts: SEE whether it's a sun/sparkle/cursor, not guess).
            if (r.textured && r.image && r.w > 0 && r.h > 0 && r.w <= 1024 && r.h <= 1024) {
                const int pw = sb_tex_pad_w(r.w, r.fmt), ph = sb_tex_pad_h(r.h, r.fmt);
                std::vector<uint32_t> rgba((size_t)pw * ph);
                sb_tex_decode(rgba.data(), (const uint8_t*)r.image, pw, ph, r.fmt,
                              (const uint8_t*)r.tlut, r.tlutfmt);
                char path[256];
                std::snprintf(path, sizeof path, "scratch/frames/prim_%02zu.ppm", i);
                if (FILE* f = std::fopen(path, "wb")) {
                    std::fprintf(f, "P6\n%d %d\n255\n", r.w, r.h);
                    for (int y = 0; y < r.h; ++y)
                        for (int x = 0; x < r.w; ++x) {
                            uint32_t px = rgba[(size_t)y * pw + x];
                            unsigned char rgb[3] = {(unsigned char)(px & 0xff),
                                                    (unsigned char)((px >> 8) & 0xff),
                                                    (unsigned char)((px >> 16) & 0xff)};
                            std::fwrite(rgb, 1, 3, f);
                        }
                    std::fclose(f);
                    std::fprintf(stderr, "    -> wrote %s (%dx%d fmt=0x%x)\n", path, r.w, r.h, r.fmt);
                }
            }
        }
    }
    if (g_prim_dbg_at) g_prim_recs.clear();
    if (verts)   *verts = g_frame_tris.data();
    if (batches) *batches = g_batches.data();
    if (nbatch)  *nbatch = (int)g_batches.size();
    g_consumed = true;
    return (int)g_frame_tris.size();
}

} // extern "C"
