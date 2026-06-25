// gx_sdlgpu.cpp — see gx_sdlgpu.h. GX→SDL3-GPU switch.
//   P1: headless GPU device + offscreen color target + clear + readback.
//   P2: vertex-buffer upload + a modulate pipeline (cull NONE) — geometry matched nvk.
//   P3: the REAL per-material TEV combiner. Each batch's generated GLSL-450 fragment shader
//       (sb_tev_gen_fragment, already attached as NvkTevBatch::fragGlsl) is reused — only its
//       resource bindings are remapped to SDL3 GPU's model (fragment samplers set=2, uniform
//       buffer set=3) — compiled via glslang, with per-batch blend/depth pipeline state, 8
//       texmap samplers, and the NvkTevPush uniform. NvkTevVertex feeds RAW (SDL3 GPU NDC ==
//       Vulkan NDC). This is nvk's renderTevFrame on SDL3 GPU. docs/gx_sdlgpu_switch.md.
#include "gx_sdlgpu.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include "glsl_compile.h"     // sb_compile_fragment_glsl (GLSL 450 -> SPIR-V via glslang)
#include "tev_vert_spv.h"     // tev_vert_spv[] : the shipping TEV vertex shader (SPIR-V), reused

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>

namespace sb::gxsdl {

namespace {
bool  g_init_tried = false;
bool  g_ok         = false;
int   g_w = 0, g_h = 0;

SDL_GPUDevice*         g_dev   = nullptr;
SDL_GPUTexture*        g_color = nullptr;   // offscreen color target (EFB-sized)
SDL_GPUTexture*        g_depth = nullptr;   // depth target (D32_FLOAT)
SDL_GPUTransferBuffer* g_dl    = nullptr;   // download buffer (w*h*4)

SDL_GPUBuffer*         g_vbuf     = nullptr;   // vertex buffer (grown as needed)
SDL_GPUTransferBuffer* g_vup      = nullptr;   // its upload transfer buffer
size_t                 g_vbuf_cap = 0;

SDL_GPUShader*         g_vs    = nullptr;       // tev vertex shader (created once)
SDL_GPUSampler*        g_samp_def = nullptr;    // default sampler (white / unbound texmaps)
SDL_GPUTexture*        g_white = nullptr;

// Caches: fragment shader by shaderKey, pipeline by (shaderKey + blend/depth state), sampler by
// filter/wrap; per-frame texture uploads by source pixel pointer.
std::unordered_map<uint64_t, SDL_GPUShader*>           g_frag_cache;
std::unordered_map<uint64_t, SDL_GPUGraphicsPipeline*> g_pipe_cache;
std::unordered_map<uint32_t, SDL_GPUSampler*>          g_samp_cache;
std::unordered_map<const void*, SDL_GPUTexture*>       g_tex_cache;

SDL_GPUCommandBuffer*  g_cmd = nullptr;
bool                   g_in_frame = false;
SDL_FColor             g_clear{};

std::vector<uint8_t> g_cpu;

constexpr SDL_GPUTextureFormat DEPTH_FMT = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

SDL_GPUShader* make_shader(const void* code, size_t bytes, SDL_GPUShaderStage stage,
                           Uint32 nSamplers, Uint32 nUniform) {
    SDL_GPUShaderCreateInfo ci{};
    ci.code = (const Uint8*)code; ci.code_size = bytes; ci.entrypoint = "main";
    ci.format = SDL_GPU_SHADERFORMAT_SPIRV; ci.stage = stage;
    ci.num_samplers = nSamplers; ci.num_uniform_buffers = nUniform;
    return SDL_CreateGPUShader(g_dev, &ci);
}

// GX blend factor (GXBlendFactor) → SDL_GPUBlendFactor (mirrors nvk gx_blend_factor exactly).
SDL_GPUBlendFactor sdl_blend_factor(uint8_t f, bool isSrc) {
    switch (f) {
        case 0: return SDL_GPU_BLENDFACTOR_ZERO;
        case 1: return SDL_GPU_BLENDFACTOR_ONE;
        case 2: return isSrc ? SDL_GPU_BLENDFACTOR_DST_COLOR : SDL_GPU_BLENDFACTOR_SRC_COLOR;
        case 3: return isSrc ? SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
        case 4: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        case 5: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        case 6: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
        case 7: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
        default: return isSrc ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ZERO;
    }
}

// Remap a generated TEV fragment shader (set=0 sampler ARRAY tex[8] + push_constant) onto SDL3
// GPU's binding model: fragment samplers at set=2 as 8 INDIVIDUAL samplers (SDL3 GPU binds each
// as its own slot), and the push_constant block as a set=3 uniform buffer. All tex[N] indices in
// the generator are literal constants, so the textual rewrite is exact.
std::string remap_for_sdlgpu(const char* glsl) {
    std::string s(glsl ? glsl : "");
    auto repl = [&](const std::string& from, const std::string& to) {
        for (size_t p; (p = s.find(from)) != std::string::npos; ) s.replace(p, from.size(), to);
    };
    std::string decl8;
    for (int i = 0; i < 8; ++i)
        decl8 += "layout(set=2, binding=" + std::to_string(i) + ") uniform sampler2D tex_" + std::to_string(i) + ";\n";
    repl("layout(set=0, binding=0) uniform sampler2D tex[8];\n", decl8);
    repl("layout(set=0,binding=0) uniform sampler2D tex[8];\n", decl8);
    for (int i = 0; i < 8; ++i) repl("tex[" + std::to_string(i) + "]", "tex_" + std::to_string(i));
    repl("layout(push_constant) uniform Mat", "layout(set=3, binding=0) uniform Mat");
    return s;
}

SDL_GPUShader* ensure_frag(uint64_t shaderKey, const char* glsl) {
    auto it = g_frag_cache.find(shaderKey);
    if (it != g_frag_cache.end()) return it->second;
    std::string src = remap_for_sdlgpu(glsl);
    std::vector<uint32_t> spv = sb_compile_fragment_glsl(src);
    SDL_GPUShader* sh = spv.empty() ? nullptr
        : make_shader(spv.data(), spv.size() * 4, SDL_GPU_SHADERSTAGE_FRAGMENT, /*samplers*/8, /*uniform*/1);
    if (!sh) std::fprintf(stderr, "[gxsdl] TEV frag compile/create failed (key=%llx)\n",
                          (unsigned long long)shaderKey);
    g_frag_cache.emplace(shaderKey, sh);
    return sh;
}

SDL_GPUGraphicsPipeline* ensure_pipeline(const sb::render::NvkTevBatch& b) {
    uint32_t state = (uint32_t)(b.blend_mode & 1)
                   | ((uint32_t)(b.src_factor & 15) << 1)
                   | ((uint32_t)(b.dst_factor & 15) << 5)
                   | ((uint32_t)(b.z_test & 1) << 9)
                   | ((uint32_t)(b.z_func & 7) << 10)
                   | ((uint32_t)(b.z_write & 1) << 13)
                   | ((uint32_t)(b.color_update & 1) << 14)
                   | ((uint32_t)(b.alpha_update & 1) << 15)
                   | ((uint32_t)(b.dst_alpha_force & 1) << 16);
    uint64_t key = b.shaderKey * 1099511628211ull ^ state ^ ((uint64_t)b.dst_alpha_val << 40);
    auto it = g_pipe_cache.find(key);
    if (it != g_pipe_cache.end()) return it->second;

    SDL_GPUShader* fs = ensure_frag(b.shaderKey, b.fragGlsl);
    if (!fs) { g_pipe_cache.emplace(key, nullptr); return nullptr; }

    using V = sb::render::NvkTevVertex;
    SDL_GPUVertexAttribute attrs[7] = {
        { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, (Uint32)offsetof(V, x) },
        { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, (Uint32)offsetof(V, rgba) },
        { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, (Uint32)offsetof(V, rgba1) },
        { 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, (Uint32)offsetof(V, uv[0]) },
        { 4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, (Uint32)offsetof(V, uv[2]) },
        { 5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, (Uint32)offsetof(V, uv[4]) },
        { 6, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, (Uint32)offsetof(V, uv[6]) },
    };
    SDL_GPUVertexBufferDescription vbd{};
    vbd.slot = 0; vbd.pitch = sizeof(V); vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUColorTargetDescription ctd{};
    ctd.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    auto& bs = ctd.blend_state;
    // GXSetColorUpdate / GXSetAlphaUpdate → per-channel write mask. color_update gates RGB,
    // alpha_update gates the destination ALPHA plane (the water-volume mask).
    bs.enable_color_write_mask = true;
    bs.color_write_mask = (SDL_GPUColorComponentFlags)
        ((b.color_update ? (SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B) : 0)
       | (b.alpha_update ? SDL_GPU_COLORCOMPONENT_A : 0));
    if (b.blend_mode == 1 /*GX_BM_BLEND*/ || b.dst_alpha_force) {
        bs.enable_blend = true;
        bs.src_color_blendfactor = sdl_blend_factor(b.src_factor, true);
        bs.dst_color_blendfactor = sdl_blend_factor(b.dst_factor, false);
        bs.color_blend_op = SDL_GPU_BLENDOP_ADD;
        if (b.dst_alpha_force) {
            // GXSetDstAlpha(GX_TRUE, val): the framebuffer alpha is FORCED to the constant,
            // bypassing the TEV/blend. SMS_FillScreenAlpha only ever forces 0 (clears the mask);
            // ZERO/ZERO writes 0 exactly. (A nonzero forced value would need a blend constant —
            // it does not occur on the SMS paths; assert visibility if it ever does.)
            bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            if (b.dst_alpha_val != 0)
                std::fprintf(stderr, "[gxsdl] WARN forced dst-alpha=%u unsupported (only 0); wrote 0\n",
                             b.dst_alpha_val);
        } else {
            bs.src_alpha_blendfactor = bs.src_color_blendfactor;
            bs.dst_alpha_blendfactor = bs.dst_color_blendfactor;
        }
        bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }

    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader = g_vs; pci.fragment_shader = fs;
    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pci.vertex_input_state.vertex_buffer_descriptions = &vbd;
    pci.vertex_input_state.num_vertex_buffers = 1;
    pci.vertex_input_state.vertex_attributes = attrs;
    pci.vertex_input_state.num_vertex_attributes = 7;
    pci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;       // match nvk VK_CULL_MODE_NONE
    pci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pci.rasterizer_state.enable_depth_clip = true;
    pci.depth_stencil_state.enable_depth_test = b.z_test != 0;
    pci.depth_stencil_state.enable_depth_write = b.z_write != 0;
    pci.depth_stencil_state.compare_op = (SDL_GPUCompareOp)((b.z_func <= 7 ? b.z_func : 3) + 1); // GX→SDL (+1 for INVALID=0)
    pci.target_info.color_target_descriptions = &ctd;
    pci.target_info.num_color_targets = 1;
    pci.target_info.depth_stencil_format = DEPTH_FMT;
    pci.target_info.has_depth_stencil_target = true;

    SDL_GPUGraphicsPipeline* pipe = SDL_CreateGPUGraphicsPipeline(g_dev, &pci);
    if (!pipe) std::fprintf(stderr, "[gxsdl] pipeline create failed: %s\n", SDL_GetError());
    g_pipe_cache.emplace(key, pipe);
    return pipe;
}

SDL_GPUSampler* get_sampler(const sb::render::NvkTevBatch::Tex& t) {
    // Mirror nvk getSampler: GX min-filter enum (0 NEAR,1 LINEAR,2 NEAR_MIP_NEAR,3 LIN_MIP_NEAR,
    // 4 NEAR_MIP_LIN,5 LIN_MIP_LIN). useMips = anything but pure GX_NEAR — SMS authors most ground/
    // water as GX_LINEAR with no mip chain, so we generate one ourselves (upload_texture) and treat
    // GX_LINEAR min as trilinear so the grazing shoreline minifies smoothly (vs the bilinear moire).
    bool magLin   = t.linear != 0;
    bool minLin   = (t.min_filter == 1 || t.min_filter == 3 || t.min_filter == 5);
    bool useMips  = (t.min_filter != 0);
    bool mipLin   = (t.min_filter == 1 || t.min_filter == 4 || t.min_filter == 5);
    auto toAddr = [](uint8_t w) {
        switch (w) { case 0: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
                     case 2: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
                     default: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT; }
    };
    uint32_t key = (uint32_t)magLin | ((uint32_t)minLin << 1) | ((uint32_t)useMips << 2)
                 | ((uint32_t)mipLin << 3) | ((uint32_t)(t.wrap_s & 3) << 4) | ((uint32_t)(t.wrap_t & 3) << 6);
    auto it = g_samp_cache.find(key);
    if (it != g_samp_cache.end()) return it->second;
    SDL_GPUSamplerCreateInfo sci{};
    sci.mag_filter = magLin ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    sci.min_filter = minLin ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    sci.mipmap_mode = mipLin ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sci.address_mode_u = toAddr(t.wrap_s);
    sci.address_mode_v = toAddr(t.wrap_t);
    sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sci.min_lod = 0.0f;
    sci.max_lod = useMips ? 1000.0f : 0.0f;   // unbounded LOD when mipped (else level-0 only)
    SDL_GPUSampler* s = SDL_CreateGPUSampler(g_dev, &sci);
    g_samp_cache.emplace(key, s);
    return s;
}

SDL_GPUTexture* upload_texture(const uint8_t* rgba, Uint32 w, Uint32 h) {
    // Full mip chain (nvk makeTexture parity): SMS ground/water is GX_LINEAR with no mip chain, so
    // a heavily-tiled grazing surface minifies many texels/pixel → bilinear moire. Generate mips
    // (GPU 2x linear downscale) so trilinear sampling smooths it.
    Uint32 levels = 1; for (Uint32 m = (w > h ? w : h); m > 1; m >>= 1) ++levels;
    SDL_GPUTextureCreateInfo tci{};
    tci.type = SDL_GPU_TEXTURETYPE_2D; tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (levels > 1 ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
    tci.width = w; tci.height = h; tci.layer_count_or_depth = 1; tci.num_levels = levels;
    tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* tex = SDL_CreateGPUTexture(g_dev, &tci);
    if (!tex) return nullptr;
    SDL_GPUTransferBufferCreateInfo tbci{}; tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; tbci.size = w * h * 4;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(g_dev, &tbci);
    void* m = SDL_MapGPUTransferBuffer(g_dev, tb, false);
    std::memcpy(m, rgba, (size_t)w * h * 4);
    SDL_UnmapGPUTransferBuffer(g_dev, tb);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(g_cmd);
    SDL_GPUTextureTransferInfo src{}; src.transfer_buffer = tb; src.pixels_per_row = w; src.rows_per_layer = h;
    SDL_GPUTextureRegion dst{}; dst.texture = tex; dst.w = w; dst.h = h; dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);   // level 0
    SDL_EndGPUCopyPass(cp);
    if (levels > 1) SDL_GenerateMipmapsForGPUTexture(g_cmd, tex);   // must be outside any pass
    SDL_ReleaseGPUTransferBuffer(g_dev, tb);
    return tex;
}

SDL_GPUTexture* tex_for(const sb::render::NvkTevBatch::Tex& t) {
    auto it = g_tex_cache.find(t.rgba);
    if (it != g_tex_cache.end()) return it->second;
    SDL_GPUTexture* tex = upload_texture(t.rgba, t.w, t.h);
    g_tex_cache.emplace(t.rgba, tex);
    return tex;
}

void ensure_vbuf(size_t bytes) {
    if (g_vbuf && g_vbuf_cap >= bytes) return;
    if (g_vbuf) SDL_ReleaseGPUBuffer(g_dev, g_vbuf);
    if (g_vup)  SDL_ReleaseGPUTransferBuffer(g_dev, g_vup);
    SDL_GPUBufferCreateInfo bci{}; bci.usage = SDL_GPU_BUFFERUSAGE_VERTEX; bci.size = (Uint32)bytes;
    g_vbuf = SDL_CreateGPUBuffer(g_dev, &bci);
    SDL_GPUTransferBufferCreateInfo tbci{}; tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; tbci.size = (Uint32)bytes;
    g_vup = SDL_CreateGPUTransferBuffer(g_dev, &tbci);
    g_vbuf_cap = bytes;
}
}

bool enabled() {
    // DEFAULT ON: SDL3 GPU is the GX seam's renderer (full nvk parity, P4). SB_SDLGPU=0 forces the
    // nvk path (the A/B oracle). If init() later fails in some context, the present layer falls back
    // to nvk anyway, so default-on is safe.
    static int v = -1;
    if (v < 0) { const char* e = std::getenv("SB_SDLGPU"); v = (e && e[0] == '0') ? 0 : 1; }
    return v == 1;
}

bool init(int w, int h) {
    if (g_init_tried) return g_ok && g_w == w && g_h == h;
    g_init_tried = true;

    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY") && !std::getenv("SDL_VIDEODRIVER"))
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "[gxsdl] SDL_InitSubSystem(VIDEO) failed: %s\n", SDL_GetError()); return false;
    }
    g_dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, /*debug=*/true, nullptr);
    if (!g_dev) { std::fprintf(stderr, "[gxsdl] SDL_CreateGPUDevice failed: %s\n", SDL_GetError()); return false; }

    SDL_GPUTextureCreateInfo cci{};
    cci.type = SDL_GPU_TEXTURETYPE_2D; cci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    cci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    cci.width = (Uint32)w; cci.height = (Uint32)h; cci.layer_count_or_depth = 1; cci.num_levels = 1;
    cci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    g_color = SDL_CreateGPUTexture(g_dev, &cci);
    SDL_GPUTextureCreateInfo dci = cci; dci.format = DEPTH_FMT;
    dci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    g_depth = SDL_CreateGPUTexture(g_dev, &dci);
    if (!g_color || !g_depth) { std::fprintf(stderr, "[gxsdl] target texture create failed: %s\n", SDL_GetError()); return false; }

    SDL_GPUTransferBufferCreateInfo tbci{}; tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; tbci.size = (Uint32)(w * h * 4);
    g_dl = SDL_CreateGPUTransferBuffer(g_dev, &tbci);

    g_vs = make_shader(tev_vert_spv, sizeof(tev_vert_spv), SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    if (!g_vs) { std::fprintf(stderr, "[gxsdl] vertex shader create failed: %s\n", SDL_GetError()); return false; }

    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter = SDL_GPU_FILTER_NEAREST; sci.mag_filter = SDL_GPU_FILTER_NEAREST;
    sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    g_samp_def = SDL_CreateGPUSampler(g_dev, &sci);

    // 1x1 white default for unbound texmaps.
    g_cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    const uint8_t white[4] = { 255, 255, 255, 255 };
    g_white = upload_texture(white, 1, 1);
    SDL_SubmitGPUCommandBuffer(g_cmd); g_cmd = nullptr;

    g_cpu.assign((size_t)w * h * 4, 0);
    g_w = w; g_h = h; g_ok = true;
    std::fprintf(stderr, "[gxsdl] SDL3 GPU device up (%dx%d), driver=%s\n", w, h, SDL_GetGPUDeviceDriver(g_dev));
    return true;
}

void frame_begin(float r, float g, float b, float a) {
    if (!g_ok || g_in_frame) return;
    g_cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (!g_cmd) { std::fprintf(stderr, "[gxsdl] AcquireGPUCommandBuffer failed: %s\n", SDL_GetError()); return; }
    g_clear = SDL_FColor{ r, g, b, a };
    g_in_frame = true;
}

void draw_tev(const sb::render::NvkTevVertex* verts, int nverts,
              const sb::render::NvkTevBatch* batches, int nbatch) {
    if (!g_ok || !g_in_frame || !g_cmd) return;

    for (auto& kv : g_tex_cache) if (kv.second) SDL_ReleaseGPUTexture(g_dev, kv.second);
    g_tex_cache.clear();
    if (nverts < 0) nverts = 0;

    // ── Upload phase (copy passes precede the render pass) ──
    if (nverts > 0 && verts) {
        size_t bytes = (size_t)nverts * sizeof(sb::render::NvkTevVertex);
        ensure_vbuf(bytes);
        void* m = SDL_MapGPUTransferBuffer(g_dev, g_vup, false);
        std::memcpy(m, verts, bytes);
        SDL_UnmapGPUTransferBuffer(g_dev, g_vup);
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(g_cmd);
        SDL_GPUTransferBufferLocation src{}; src.transfer_buffer = g_vup;
        SDL_GPUBufferRegion dst{}; dst.buffer = g_vbuf; dst.size = (Uint32)bytes;
        SDL_UploadToGPUBuffer(cp, &src, &dst, false);
        SDL_EndGPUCopyPass(cp);
    }
    for (int bi = 0; bi < nbatch && batches; ++bi) {
        const auto& b = batches[bi];
        if (!b.vcount) continue;
        for (int s = 0; s < 8; ++s)
            if (b.tex[s].rgba && b.tex[s].w && b.tex[s].h) (void)tex_for(b.tex[s]);
    }

    // ── Render pass: clear, then per-batch TEV draw ──
    SDL_GPUColorTargetInfo cti{}; cti.texture = g_color; cti.clear_color = g_clear;
    cti.load_op = SDL_GPU_LOADOP_CLEAR; cti.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dti{}; dti.texture = g_depth; dti.clear_depth = 1.0f;
    dti.load_op = SDL_GPU_LOADOP_CLEAR; dti.store_op = SDL_GPU_STOREOP_DONT_CARE;
    dti.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; dti.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(g_cmd, &cti, 1, &dti);

    if (nverts > 0 && verts && nbatch > 0 && batches) {
        SDL_GPUBufferBinding vb{}; vb.buffer = g_vbuf;
        SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
        for (int bi = 0; bi < nbatch; ++bi) {
            const auto& b = batches[bi];
            if (b.vcount == 0 || b.vstart >= (uint32_t)nverts) continue;
            uint32_t count = b.vcount;
            if (b.vstart + count > (uint32_t)nverts) count = (uint32_t)nverts - b.vstart;
            SDL_GPUGraphicsPipeline* pipe = ensure_pipeline(b);
            if (!pipe) continue;
            SDL_BindGPUGraphicsPipeline(rp, pipe);
            SDL_PushGPUFragmentUniformData(g_cmd, 0, &b.push, (Uint32)sizeof(b.push));
            SDL_GPUTextureSamplerBinding tsb[8];
            for (int s = 0; s < 8; ++s) {
                bool has = b.tex[s].rgba && b.tex[s].w && b.tex[s].h;
                tsb[s].texture = has ? tex_for(b.tex[s]) : g_white;
                if (!tsb[s].texture) tsb[s].texture = g_white;
                tsb[s].sampler = has ? get_sampler(b.tex[s]) : g_samp_def;
            }
            SDL_BindGPUFragmentSamplers(rp, 0, tsb, 8);
            SDL_DrawGPUPrimitives(rp, count, 1, b.vstart, 0);
        }
    }
    SDL_EndGPURenderPass(rp);
}

void frame_end() {
    if (!g_ok || !g_in_frame || !g_cmd) { g_in_frame = false; return; }
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(g_cmd);
    SDL_GPUTextureRegion reg{}; reg.texture = g_color; reg.w = (Uint32)g_w; reg.h = (Uint32)g_h; reg.d = 1;
    SDL_GPUTextureTransferInfo tti{}; tti.transfer_buffer = g_dl; tti.pixels_per_row = (Uint32)g_w; tti.rows_per_layer = (Uint32)g_h;
    SDL_DownloadFromGPUTexture(cp, &reg, &tti);
    SDL_EndGPUCopyPass(cp);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(g_cmd);
    if (fence) { SDL_WaitForGPUFences(g_dev, true, &fence, 1); SDL_ReleaseGPUFence(g_dev, fence); }
    g_cmd = nullptr;

    void* mapped = SDL_MapGPUTransferBuffer(g_dev, g_dl, false);
    if (mapped) {
        // SDL3 GPU's clip→framebuffer Y is inverted vs raw Vulkan (nvk), so the download is
        // bottom-up vs the top-left PPM/nvk convention. Flip rows on copy-out (cull is NONE so
        // winding is irrelevant; depth unaffected).
        const uint8_t* src = (const uint8_t*)mapped;
        const size_t row = (size_t)g_w * 4;
        for (int y = 0; y < g_h; ++y)
            std::memcpy(g_cpu.data() + (size_t)(g_h - 1 - y) * row, src + (size_t)y * row, row);
        SDL_UnmapGPUTransferBuffer(g_dev, g_dl);
    }
    g_in_frame = false;
}

bool readback(uint8_t* rgba, int w, int h) {
    if (!g_ok || w != g_w || h != g_h || !rgba) return false;
    std::memcpy(rgba, g_cpu.data(), (size_t)w * h * 4);
    return true;
}

} // namespace sb::gxsdl
