// sms_boot_material.h — extract a J3DMaterial's render state (TEV combiner, konst/reg
// colours, swap tables, PE block) into the renderer-ready NgxTevState, and resolve the
// material's bound textures into decoded RGBA8 images, using the NATIVE J3D C++ object
// model (clean accessors) rather than the recomp era's raw guest-pointer offsets.
//
// This is the material half of the owned native render path (the geometry half lives in
// sms_boot_j3d_capture.cpp). The captured J3DShape's material is reachable at draw time
// via j3dSys.getMatPacket()->getMaterial(); this turns it into the data the TEV shader
// generator (sb_tev_gen_fragment) and nvk's TEV pipeline consume.
#pragma once
#include "ngx_render_data.h"   // NgxTevState
#include <cstdint>
#include <vector>

class J3DMaterial;

// Build the full TEV/PE render state for `mat` into `out`. Faithful: per-stage GX
// combiner registers (color_env/alpha_env), tev/konst colour registers, konst
// selection, swap tables, alpha-compare, blend, z-mode and cull — all read from the
// material's J3DTevBlock / J3DPEBlock / J3DColorBlock via their virtual accessors.
// Returns false only if `mat` has no TEV block.
bool sb_build_tev_state(J3DMaterial* mat, NgxTevState& out);

// One resolved, decoded texture for a GX texmap slot. `rgba` is owned by the vector the
// caller passes; w/h are the logical dimensions. valid==false ⇒ that texmap is unbound
// (the renderer samples 1×1 white).
struct SbTexImage {
    int      slot = -1;          // GX texmap 0..7
    uint32_t w = 0, h = 0;       // logical dims
    std::vector<uint32_t> rgba;  // decoded RGBA8, row-major, w*h texels
    uint8_t  wrap_s = 1, wrap_t = 1;  // GX wrap (0=CLAMP 1=REPEAT 2=MIRROR)
    bool     linear = false;     // MAG filter is LINEAR (else NEAREST)
    uint8_t  min_filter = 1;     // GX min filter (0 NEAR,1 LIN,2 NEAR_MIP_NEAR,3 LIN_MIP_NEAR,4 NEAR_MIP_LIN,5 LIN_MIP_LIN)
    uint8_t  max_aniso = 0;      // GX_ANISO_1=0, _2=1, _4=2
    // Non-null when this texmap's image data IS an EFB-copy destination recorded this frame: it
    // samples a snapshot of the framebuffer (the GC soft-focus/bloom/mirror composite), not `rgba`.
    // The segmented present binds the snapshot registered under this key. (See the overbright journal.)
    const void* efb_src = nullptr;
    // The texmap's resolved ResTIMG image-data host pointer, kept so the batch-fill can RE-EVALUATE
    // efb_src LIVE each frame (the EFB-sampler status is dynamic: a material is decoded+cached once,
    // but whether its src is an EFB-copy dest depends on copies recorded across frames — composite3's
    // material is first cached before the screen-texture copy is known, so a cached efb_src is stale-null).
    const void* src_ptr = nullptr;
};

// Resolve+decode every texmap the material's TEV stages reference. `tex` is the model's
// J3DTexture table (j3dSys.getTexture()); each stage's texmap → material texNo →
// ResTIMG → decoded RGBA8. Appends one SbTexImage per distinct bound texmap to `out`.
void sb_resolve_textures(J3DMaterial* mat, void* j3dTexture, std::vector<SbTexImage>& out);
