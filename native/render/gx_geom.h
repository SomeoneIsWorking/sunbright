// gx_geom.h — the captured GX geometry contract (renderer-agnostic POD types).
//
// These structs ARE the boundary between the GX capture layer (scene_drive / J3D walk / 2D imm)
// and the renderer (gx_sdlgpu, the SDL3 GPU backend). They were originally defined inside nvk.h
// (the retired from-scratch Vulkan renderer "nvk"); the renderer is gone, but the data contract it
// established stays — SDL3 GPU consumes exactly these. The "Nvk" name prefix is kept to avoid
// churning every call site. Vulkan-style clip space (Y-down, depth [0,1]) — SDL3 GPU NDC matches.
#pragma once
#include <cstdint>
#include <vector>

namespace sb::render {

// A vertex the native engine produces itself: NDC xyz + RGBA (0..1). z = NDC depth.
struct NvkVertex { float x, y, z; float r, g, b, a; };

// A textured vertex: NDC xyz + UV.
struct NvkTexVertex { float x, y, z; float u, v; };

// A TEV vertex: CLIP-space xyzw + both GX raster colour channels + the 8 GX texcoord UVs (texgen
// already applied on the CPU). Matches the TEV vertex shader inputs (tev.vert): vColor (color0),
// vColor1 (COLOR1A1), vUV[0..7]. 3D J3D verts carry the real perspective w; 2D/imm content w=1.
struct NvkTevVertex {
    float x, y, z;
    float w = 1.0f;
    float rgba[4];
    float rgba1[4];
    float uv[8][2];
};

// Push constants the generated TEV fragment shader reads: GX TEV konst colours and the S10 TEV
// colour registers (CPREV/C0/C1/C2). Matches `ivec4 kcolor[4]; ivec4 tevreg[4];`.
struct NvkTevPush {
    int32_t kcolor[4][4];
    int32_t tevreg[4][4];
};

struct NvkClear { float r, g, b, a; };

// One material batch: a vertex span + its generated TEV fragment shader (by key), push constants,
// up to 8 texmap textures, and GX depth/blend state. The renderer draws each batch with its own
// pipeline. (Was Nvk::NvkTevBatch — now a free struct; the renderer that owned it is retired.)
struct NvkTevBatch {
    uint32_t vstart = 0, vcount = 0;
    NvkTevPush push{};
    const char* fragGlsl = nullptr;   // sb_tev_gen_fragment(...) source (owned by caller)
    uint64_t shaderKey = 0;           // unique per distinct fragGlsl (shader/pipeline cache key)
    struct Tex { const uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
                 uint8_t linear = 0;       // MAG filter linear (else nearest)
                 uint8_t min_filter = 1;   // GX min filter (encodes mip mode)
                 uint8_t max_aniso = 0;    // GX_ANISO_1/2/4 = 0/1/2
                 uint8_t wrap_s = 1, wrap_t = 1;
                 // EFB-copy snapshot source: when non-null this texmap samples a snapshot of the
                 // framebuffer taken at the EFB→texture copy whose destination pointer == efb_src
                 // (the GC soft-focus/bloom/mirror composite), NOT the decoded `rgba`. The segmented
                 // present (sms_boot_present.cpp) registers each snapshot in the SDL3-GPU backend
                 // keyed by this pointer; the backend binds it for any batch slot carrying it. See
                 // the 2026-06-30 file-select overbright journal.
                 const void* efb_src = nullptr; } tex[8];
    uint8_t z_test = 1, z_func = 3 /*GX_LEQUAL*/, z_write = 1;
    uint8_t blend_mode = 0, src_factor = 1, dst_factor = 0;   // GX blend (0=none)
    // Pixel-engine write enables + destination-alpha (GXSetColorUpdate / GXSetAlphaUpdate /
    // GXSetDstAlpha). color_update=0 ⇒ write no RGB (alpha-plane-only pass); alpha_update=0 ⇒
    // write no A. dst_alpha_force=1 ⇒ the framebuffer ALPHA written is forced to the constant
    // dst_alpha_val (GX bypasses the TEV alpha) — SMS_FillScreenAlpha uses force=0 to clear the
    // water-volume mask. With the DST_ALPHA/INV_DST_ALPHA blend factors this is the destination-
    // alpha masking the water-volume / silhouette composites depend on.
    uint8_t color_update = 1, alpha_update = 1;
    uint8_t dst_alpha_force = 0, dst_alpha_val = 0;
    // Active TEV stage count (NgxTevState.num_stages, == GX GENMODE numtevstages+1). Carried purely
    // so the per-draw GX-state diff (SB_GXDRAW vs the oracle's SUNBRIGHT_DBG_GXDRAW) can match a
    // native batch to its oracle draw by blend/tev SIGNATURE (the engines merge differently, so an
    // index-align is impossible; signature is the renderer-neutral join key). Not used by the renderer.
    uint8_t num_stages = 0;
    // Which TMarDirector perform list this batch was captured from, under SB_OWN_GXLIST
    // (set via sb_boot_capture_set_phase before each list runs). Lets the overbright harness
    // attribute a duplicated/over-composited layer to its source pass: 0=unknown/hand-driven,
    // 1=unk40(drawbuf/light pre-pass), 2=unk38(graffiti EFB), 3=unk3C(graffiti EFB),
    // 4=mPerformListGX(MAIN), 5=mPerformListSilhouette, 6=mPerformListGXPost. The EFB pre-passes
    // (1..3) render to OFF-SCREEN EFB regions on GC (copied to textures, sampled later) — they
    // must NOT be alpha-composited into the visible framebuffer.
    uint8_t phase = 0;
    // Debug-only: the draw-buffer name active when this batch was captured (TDrawBufObj::getName(),
    // static string), for attributing a batch to its source buffer in the overbright harness. nullptr
    // if not captured from a TDrawBufObj draw (e.g. a directly-performed TViewObj). Not used by the
    // renderer.
    const char* dbgName = nullptr;

    // ── PER-BATCH LIGHTING SNAPSHOT (overbright wash cross-engine diagnostic, 2026-07-02) ──
    // Captures the raster-stage inputs at the moment this batch's shape was captured. Mirrors the
    // oracle's per-draw LightSnap in gx_parse.h (see runtime/gx_parse.h::GxFrameInfo::LightSnap):
    // same channels (RGB float in [0,1]) so a cross-engine per-shape diff pins which stage diverges.
    // Populated in sms_boot_j3d_capture.cpp from the material's chan-ctrl + amb/mat + live GX lights.
    // Values captured, not owned; consumers just read.
    uint16_t dbg_chan_ctrl = 0;      // chanCtrl[0] (light mask + amb-mat source)
    uint8_t  dbg_light_mask = 0;     // bit i = light i was valid at capture
    float    dbg_amb[3]  = {0, 0, 0};    // material's ambColor[0] normalised
    float    dbg_matc[4] = {1, 1, 1, 1}; // material's matColor[0] normalised
    float    dbg_light_pos[8][3] = {};
    float    dbg_light_col[8][3] = {};
};

} // namespace sb::render
