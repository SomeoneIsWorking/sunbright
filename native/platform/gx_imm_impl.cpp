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

#include <dolphin/gx.h>      // GXColor etc. (gx_state.h uses them)
#include "gx_state.h"
#include "gx_imm_xform.h"
#include <cstdio>
#include <cstdlib>

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
bool g_consumed = true;        // start true so the first GXBegin clears the (empty) buffer

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
struct PrimRec { int prim, nv; bool textured; int fmt, w, h;
                 float xmn, xmx, ymn, ymx, umn, umx, vmn, vmx; };
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
        if (sameTex && last.vstart + last.vcount == start) { last.vcount += count; return; }
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
static void finalize_prim(void) {
    if (!g_in_begin) return;
    g_in_begin = false;
    if (g_prim_dbg_at && !g_prim_dbg_done && !g_prim_verts.empty()) {
        PrimRec r{}; r.prim = g_prim; r.nv = (int)g_prim_verts.size();
        r.textured = (g_prim_has_uv && g_texSnap.textured);
        r.fmt = g_texSnap.fmt; r.w = g_texSnap.w; r.h = g_texSnap.h;
        r.xmn = r.ymn = r.umn = r.vmn = 1e30f; r.xmx = r.ymx = r.umx = r.vmx = -1e30f;
        for (auto& v : g_prim_verts) {
            if (v.x<r.xmn)r.xmn=v.x; if (v.x>r.xmx)r.xmx=v.x;
            if (v.y<r.ymn)r.ymn=v.y; if (v.y>r.ymx)r.ymx=v.y;
            if (v.u<r.umn)r.umn=v.u; if (v.u>r.umx)r.umx=v.u;
            if (v.v<r.vmn)r.vmn=v.v; if (v.v>r.vmx)r.vmx=v.v;
        }
        g_prim_recs.push_back(r);
    }
    const unsigned start = (unsigned)g_frame_tris.size();
    sb::render::imm_triangulate(g_prim, g_prim_verts.data(),
                                (int)g_prim_verts.size(), g_frame_tris);
    const unsigned count = (unsigned)g_frame_tris.size() - start;
    // Textured only if the prim actually emitted texcoords AND a texture was bound: a
    // stale boundTex from an earlier window can't texture-ize the (texcoord-less) gradient.
    SbImmBatch tex = (g_prim_has_uv && g_texSnap.textured) ? g_texSnap : SbImmBatch{};
    push_batch(start, count, tex);
}

void sb_gx_imm_begin(int prim, int vtxfmt, int nverts) {
    if (g_consumed) { g_frame_tris.clear(); g_batches.clear(); g_consumed = false; }
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
            std::fprintf(stderr, "  p%zu prim=%#x nv=%d tex=%d fmt=0x%x %dx%d ndcX[%.3f,%.3f] "
                         "ndcY[%.3f,%.3f] uv[%.3f,%.3f;%.3f,%.3f]\n",
                         i, r.prim, r.nv, r.textured, r.fmt, r.w, r.h,
                         r.xmn, r.xmx, r.ymn, r.ymx, r.umn, r.umx, r.vmn, r.vmx);
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
