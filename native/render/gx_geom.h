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
                 uint8_t wrap_s = 1, wrap_t = 1; } tex[8];
    uint8_t z_test = 1, z_func = 3 /*GX_LEQUAL*/, z_write = 1;
    uint8_t blend_mode = 0, src_factor = 1, dst_factor = 0;   // GX blend (0=none)
};

} // namespace sb::render
