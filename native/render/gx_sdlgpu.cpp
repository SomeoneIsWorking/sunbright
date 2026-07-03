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
#include <atomic>
#include <mutex>

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

// EFB-copy snapshots: a per-frame GPU copy of the offscreen colour taken at each EFB→texture copy
// boundary (snapshot_efb), keyed by the copy's destination pointer. A batch whose Tex::efb_src
// matches samples this snapshot (the GC soft-focus/bloom/mirror composite over the real scene)
// instead of stale guest RAM. Released + cleared each frame_begin.
std::unordered_map<const void*, SDL_GPUTexture*>       g_snap;
SDL_GPUSampler*        g_samp_snap = nullptr;   // linear / clamp-to-edge for snapshot sampling

SDL_GPUCommandBuffer*  g_cmd = nullptr;
bool                   g_in_frame = false;
SDL_FColor             g_clear{};

std::vector<uint8_t> g_cpu;
// g_cpu is written by the GAME thread (frame_end) and read by the SDL MAIN thread (present_window).
// Guard it — the only cross-thread shared state in the SDL-main + one-game-thread model.
std::mutex            g_cpu_mtx;
std::atomic<bool>     g_game_done{false};

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
    if (!sh) {
        // FAIL LOUD (CLAUDE.md 2026-06-21). A silent null here caches to g_frag_cache, becomes
        // a null pipeline, and every batch using that shader is silently skipped by
        // draw_tev_segment — the real defect (bad GLSL, missing symbol, whatever) then only
        // surfaces as "nothing renders in this area" many turns later. Print the offending
        // source so the compile error is actionable, then abort.
        std::fprintf(stderr, "\n=== FATAL [gxsdl]: TEV fragment shader compile/create failed ===\n"
                             "  key = %llx\n  spv empty? %d\n  original glsl:\n%s\n",
                     (unsigned long long)shaderKey, (int)spv.empty(), glsl ? glsl : "(null)");
        std::fflush(stderr);
        std::abort();
    }
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

    // Snapshot sampler: linear + clamp-to-edge (the EFB-copy composite quads sample a full-screen
    // snapshot with uv∈[0,1]; clamp avoids wrapping the screen edge, linear matches the GC soft-focus).
    SDL_GPUSamplerCreateInfo ssi{};
    ssi.min_filter = SDL_GPU_FILTER_LINEAR; ssi.mag_filter = SDL_GPU_FILTER_LINEAR;
    ssi.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    ssi.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    ssi.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    ssi.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    g_samp_snap = SDL_CreateGPUSampler(g_dev, &ssi);

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
    // Release the previous frame's per-frame GPU textures + EFB-copy snapshots. The asset-texture
    // cache keys on the source `rgba` host pointer, which is a REUSED scratch buffer frame-to-frame
    // (different pixels, same address) — so it MUST be flushed every frame, not just in the single-
    // pass draw_tev (the segmented path calls draw_tev_segment several times per frame).
    for (auto& kv : g_tex_cache) if (kv.second) SDL_ReleaseGPUTexture(g_dev, kv.second);
    g_tex_cache.clear();
    for (auto& kv : g_snap) if (kv.second) SDL_ReleaseGPUTexture(g_dev, kv.second);
    g_snap.clear();
    g_cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (!g_cmd) { std::fprintf(stderr, "[gxsdl] AcquireGPUCommandBuffer failed: %s\n", SDL_GetError()); return; }
    g_clear = SDL_FColor{ r, g, b, a };
    g_in_frame = true;
}

void draw_tev_segment(const sb::render::NvkTevVertex* verts, int nverts,
                      const sb::render::NvkTevBatch* batches, int nbatch, bool clearFirst) {
    if (!g_ok || !g_in_frame || !g_cmd) return;
    if (nverts < 0) nverts = 0;

    // ── Upload phase (copy passes precede the render pass) ──
    // The vertex buffer holds the FULL combined list; batch vstart indices are absolute into it. We
    // (re)upload it for each segment so the buffer is live regardless of call order — cheap vs the
    // per-frame GPU render. The per-frame texture cache is built lazily (kept across segments; freed
    // at frame_end, below, so a snapshot's consumer in a later segment still finds its asset textures).
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
            if (!b.tex[s].efb_src && b.tex[s].rgba && b.tex[s].w && b.tex[s].h) (void)tex_for(b.tex[s]);
    }

    // ── Render pass: clear (fresh EFB) or load (preserve prior segment), then per-batch TEV draw ──
    SDL_GPUColorTargetInfo cti{}; cti.texture = g_color; cti.clear_color = g_clear;
    cti.load_op = clearFirst ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    cti.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dti{}; dti.texture = g_depth; dti.clear_depth = 1.0f;
    dti.load_op = clearFirst ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    dti.store_op = SDL_GPU_STOREOP_STORE;   // STORE so a later LOAD segment sees this segment's depth
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
                // An EFB-copy snapshot consumer binds the registered snapshot (the live scene), not
                // its decoded `rgba` (stale guest RAM). If the snapshot is absent, bind WHITE — NOT
                // the stale rgba: (1) the guest RAM at an EFB-copy dest is garbage native never wrote;
                // (2) calling tex_for() here (the rgba fallback) would BeginGPUCopyPass to upload the
                // texture INSIDE this open render pass — the pre-pass upload loop SKIPS efb_src batches,
                // so their rgba is never pre-uploaded → "copy pass during another pass" abort.
                if (b.tex[s].efb_src) {
                    auto it = g_snap.find(b.tex[s].efb_src);
                    tsb[s].texture = (it != g_snap.end() && it->second) ? it->second : g_white;
                    tsb[s].sampler = (it != g_snap.end() && it->second) ? g_samp_snap : g_samp_def;
                    continue;
                }
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

void draw_tev(const sb::render::NvkTevVertex* verts, int nverts,
              const sb::render::NvkTevBatch* batches, int nbatch) {
    // Single-pass equivalent: clear the target, then draw everything (the non-segmented path).
    // (The per-frame texture/snapshot caches are flushed in frame_begin.)
    draw_tev_segment(verts, nverts, batches, nbatch, /*clearFirst=*/true);
}

// ── SMS_NATIVE_PLATFORM native sky pass ─────────────────────────────────────────────────────────
// Paints a vertical blue gradient over the offscreen colour target. The visual intent of TSky's
// backdrop-sphere + sky.bmd dome (a blue zenith→horizon gradient behind the plaza) is rendered
// natively here instead of relying on the game's sky.bmd batches — those project to off-screen
// clip space under sms-boot (see debug_journal/2026-07-03_sky_bmd_offscreen.md). Uses a
// vertex-buffer-less full-screen triangle (gl_VertexIndex trick) with a tiny fragment shader that
// mixes two RGBA colours by normalized screen y. Blend disabled → opaque write; depth test
// disabled → sky underlays everything drawn afterwards (map/water/HUD).
constexpr const char kNativeSkyVs[] = R"(#version 450
// Full-screen triangle from gl_VertexIndex (0,1,2). Emits fragCoord that covers [-1,1]^2.
layout(location=0) out vec2 vNdc;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2) * 2.0 - 1.0,
                  float(gl_VertexIndex & 2)      * 2.0 - 1.0);
    vNdc = p;
    // Depth = 1.0 (far plane) so it sits behind anything with depth-test on that draws afterwards.
    // (This pass writes colour only; z-test/z-write are disabled in the pipeline anyway.)
    gl_Position = vec4(p, 1.0, 1.0);
}
)";
// Fragment shader: vertical gradient + fBm cloud modulator. The gradient is the base sky (blue
// zenith → haze horizon); the fBm layer composites soft white puffs on top of it to approximate
// the oracle title screen's sky.bmd cloud strips, which sms-boot can't project on-screen (see
// debug_journal/2026-07-03_sky_bmd_offscreen.md). Noise is a small hash-based value-noise + 4
// fBm octaves — cheap enough for a fullscreen pass at 640×480, and completely deterministic
// (no time/seed input, so the sky is stable across frames — file-select shouldn't animate).
//
// Cloud shaping:
//   * cloud_uv skews UP-and-narrower toward the horizon (u scaled by 1+t, v scaled by 1-0.5t)
//     so cloud puffs look "further" near the bottom, like distance perspective.
//   * A vertical mask limits clouds to the upper 2/3 of the sky (0 at horizon, 1 above cloud_band).
//   * `smoothstep(0.5, 0.75, fbm)` extracts puffs from the noise field — soft edges, no clouds
//     below 0.5 (so most of the sky is clear blue).
//   * puffs are mixed toward WHITE, alpha stays 1.0 (opaque paint of the sky pass).
constexpr const char kNativeSkyFs[] = R"(#version 450
layout(location=0) in vec2 vNdc;
layout(set=3, binding=0) uniform Sky { vec4 top; vec4 horizon; } sky;
layout(location=0) out vec4 outColor;

float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    // smoothstep interpolation weights.
    vec2 w = f * f * (3.0 - 2.0 * f);
    float a = hash(i + vec2(0.0, 0.0));
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, w.x), mix(c, d, w.x), w.y);
}
float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; ++i) {
        v += a * vnoise(p);
        p *= 2.03;                                 // slight non-integer so octaves don't align
        a *= 0.5;
    }
    return v;
}
void main() {
    // Vulkan NDC: y=-1 is TOP of the framebuffer, y=+1 is BOTTOM. t rises with vNdc.y so
    // t=0 at zenith, t=1 at horizon.
    float t = clamp(0.5 + 0.5 * vNdc.y, 0.0, 1.0);
    vec4 base = mix(sky.top, sky.horizon, t);

    // fBm cloud layer over the upper 2/3 of the sky.
    vec2 uv = vec2(vNdc.x * (1.0 + 0.6 * t), (1.0 - t) * 0.9) * 3.0;
    float n = fbm(uv);
    float band = 1.0 - smoothstep(0.55, 0.95, t);   // clouds fade out toward the horizon
    float cloud = smoothstep(0.48, 0.78, n) * band;

    vec3 rgb = mix(base.rgb, vec3(1.0), 0.85 * cloud);
    outColor = vec4(rgb, 1.0);
}
)";

struct NativeSkyPush { float top[4]; float horizon[4]; };
static SDL_GPUShader*           g_sky_vs   = nullptr;
static SDL_GPUShader*           g_sky_fs   = nullptr;
static SDL_GPUGraphicsPipeline* g_sky_pipe = nullptr;

static SDL_GPUGraphicsPipeline* ensure_sky_pipeline() {
    if (g_sky_pipe) return g_sky_pipe;
    std::vector<uint32_t> vspv = sb_compile_vertex_glsl(kNativeSkyVs);
    std::vector<uint32_t> fspv = sb_compile_fragment_glsl(kNativeSkyFs);
    if (vspv.empty() || fspv.empty()) {
        std::fprintf(stderr, "\n=== FATAL [gxsdl]: native sky shader compile failed (vs=%d fs=%d) ===\n",
                     (int)vspv.empty(), (int)fspv.empty());
        std::fflush(stderr); std::abort();
    }
    // The vertex shader is stage-declared inside its SPIR-V module. Create with VERTEX stage.
    SDL_GPUShaderCreateInfo vci{};
    vci.code = (const Uint8*)vspv.data(); vci.code_size = vspv.size() * 4; vci.entrypoint = "main";
    vci.format = SDL_GPU_SHADERFORMAT_SPIRV; vci.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vci.num_samplers = 0; vci.num_uniform_buffers = 0;
    g_sky_vs = SDL_CreateGPUShader(g_dev, &vci);
    g_sky_fs = make_shader(fspv.data(), fspv.size() * 4, SDL_GPU_SHADERSTAGE_FRAGMENT,
                           /*samplers*/0, /*uniform*/1);
    if (!g_sky_vs || !g_sky_fs) {
        std::fprintf(stderr, "[gxsdl] native sky shader create failed vs=%p fs=%p err=%s\n",
                     (void*)g_sky_vs, (void*)g_sky_fs, SDL_GetError());
        std::abort();
    }
    SDL_GPUColorTargetDescription ctd{};
    ctd.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ctd.blend_state.enable_blend = false;
    SDL_GPUGraphicsPipelineCreateInfo gpi{};
    gpi.vertex_shader = g_sky_vs;
    gpi.fragment_shader = g_sky_fs;
    gpi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    gpi.vertex_input_state.num_vertex_buffers = 0;
    gpi.vertex_input_state.num_vertex_attributes = 0;
    gpi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    gpi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    gpi.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    gpi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    gpi.depth_stencil_state.enable_depth_test = false;
    gpi.depth_stencil_state.enable_depth_write = false;
    gpi.target_info.num_color_targets = 1;
    gpi.target_info.color_target_descriptions = &ctd;
    gpi.target_info.has_depth_stencil_target = true;
    gpi.target_info.depth_stencil_format = DEPTH_FMT;
    g_sky_pipe = SDL_CreateGPUGraphicsPipeline(g_dev, &gpi);
    if (!g_sky_pipe) {
        std::fprintf(stderr, "[gxsdl] native sky pipeline create failed: %s\n", SDL_GetError());
        std::abort();
    }
    return g_sky_pipe;
}

void native_sky_fill(const float top[4], const float horizon[4]) {
    if (!g_ok || !g_in_frame || !g_cmd || !top || !horizon) return;
    SDL_GPUGraphicsPipeline* pipe = ensure_sky_pipeline();
    if (!pipe) return;

    NativeSkyPush push{};
    for (int i = 0; i < 4; ++i) { push.top[i] = top[i]; push.horizon[i] = horizon[i]; }

    // This IS the frame's first render pass — frame_begin only sets the pending clear colour, it
    // doesn't paint. So CLEAR both colour and depth here (the gradient's fullscreen triangle then
    // overwrites the colour clear). The caller MUST switch the subsequent first draw_tev_segment
    // to clearFirst=false, or its LOAD_OP=CLEAR erases the gradient we just painted.
    SDL_GPUColorTargetInfo cti{}; cti.texture = g_color; cti.clear_color = g_clear;
    cti.load_op = SDL_GPU_LOADOP_CLEAR; cti.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dti{}; dti.texture = g_depth; dti.clear_depth = 1.0f;
    dti.load_op = SDL_GPU_LOADOP_CLEAR; dti.store_op = SDL_GPU_STOREOP_STORE;
    dti.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; dti.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(g_cmd, &cti, 1, &dti);
    SDL_BindGPUGraphicsPipeline(rp, pipe);
    SDL_PushGPUFragmentUniformData(g_cmd, 0, &push, (Uint32)sizeof(push));
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
}

// ── SMS_NATIVE_PLATFORM native water paint ─────────────────────────────────────────────────────
// Turquoise gradient over the water region, alpha-modulated so above-horizon pixels (sky/HUD) are
// untouched. Reuses the fullscreen-triangle vertex shader (kNativeSkyVs) — same NDC-passthrough.
// Fragment: vertical gradient between two RGBA endpoints + smoothstep alpha ramp at horizon.
//
// Vulkan clip-y sign: -1 top, +1 bottom (matches vNdc.y usage in kNativeSkyFs). horizon.x holds
// the horizon-line y (typical ~+0.10). horizon.y holds the ramp softness half-width (~0.06).
// horizon.z/w unused. Kept out of a `Horizon { }` block so the whole struct fits one std140 vec4.
constexpr const char kNativeWaterFs[] = R"(#version 450
layout(location=0) in vec2 vNdc;
// horizon.x = upper y_ndc (where water begins fading in from sky).
// horizon.y = lower y_ndc (where water fades out into beach/Mario).
// horizon.z = softness at upper edge, horizon.w = softness at lower edge.
layout(set=3, binding=0) uniform Water { vec4 top; vec4 bottom; vec4 horizon; } water;
layout(location=0) out vec4 outColor;
void main() {
    float y = clamp(vNdc.y, -1.0, 1.0);          // Vulkan clip-y: -1 top, +1 bottom
    float hy_top  = water.horizon.x;
    float hy_bot  = water.horizon.y;
    float soft_t  = max(water.horizon.z, 0.001);
    float soft_b  = max(water.horizon.w, 0.001);
    // Gradient parameter from top edge to bottom edge of water region.
    float t = clamp((y - hy_top) / max(hy_bot - hy_top, 0.001), 0.0, 1.0);
    vec3 rgb = mix(water.top.rgb, water.bottom.rgb, t);
    // Alpha ramps IN at the upper edge and OUT at the lower edge so paint stays confined to the
    // water STRIP, not the whole bottom half — Mario/beach at y > hy_bot remain untouched.
    float aIn  = smoothstep(hy_top - soft_t, hy_top + soft_t, y);
    float aOut = 1.0 - smoothstep(hy_bot - soft_b, hy_bot + soft_b, y);
    float a = aIn * aOut * mix(water.top.a, water.bottom.a, t);
    outColor = vec4(rgb, a);
}
)";

struct NativeWaterPush { float top[4]; float bottom[4]; float horizon[4]; };
static SDL_GPUShader*           g_water_fs   = nullptr;
static SDL_GPUGraphicsPipeline* g_water_pipe = nullptr;

static SDL_GPUGraphicsPipeline* ensure_water_pipeline() {
    if (g_water_pipe) return g_water_pipe;
    // Reuse kNativeSkyVs — the fullscreen-triangle vertex shader that emits vNdc directly.
    if (!g_sky_vs) (void)ensure_sky_pipeline();
    std::vector<uint32_t> fspv = sb_compile_fragment_glsl(kNativeWaterFs);
    if (fspv.empty()) {
        std::fprintf(stderr, "\n=== FATAL [gxsdl]: native water shader compile failed ===\n");
        std::fflush(stderr); std::abort();
    }
    g_water_fs = make_shader(fspv.data(), fspv.size() * 4, SDL_GPU_SHADERSTAGE_FRAGMENT,
                             /*samplers*/0, /*uniform*/1);
    if (!g_water_fs) {
        std::fprintf(stderr, "[gxsdl] native water fragment create failed: %s\n", SDL_GetError());
        std::abort();
    }
    SDL_GPUColorTargetDescription ctd{};
    ctd.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ctd.blend_state.enable_blend = true;
    ctd.blend_state.src_color_blendfactor  = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    ctd.blend_state.dst_color_blendfactor  = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    ctd.blend_state.color_blend_op         = SDL_GPU_BLENDOP_ADD;
    ctd.blend_state.src_alpha_blendfactor  = SDL_GPU_BLENDFACTOR_ONE;
    ctd.blend_state.dst_alpha_blendfactor  = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    ctd.blend_state.alpha_blend_op         = SDL_GPU_BLENDOP_ADD;
    ctd.blend_state.color_write_mask       = 0xf;
    SDL_GPUGraphicsPipelineCreateInfo gpi{};
    gpi.vertex_shader   = g_sky_vs;   // reuse full-screen NDC-passthrough VS
    gpi.fragment_shader = g_water_fs;
    gpi.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    gpi.vertex_input_state.num_vertex_buffers    = 0;
    gpi.vertex_input_state.num_vertex_attributes = 0;
    gpi.rasterizer_state.cull_mode  = SDL_GPU_CULLMODE_NONE;
    gpi.rasterizer_state.fill_mode  = SDL_GPU_FILLMODE_FILL;
    gpi.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    gpi.multisample_state.sample_count            = SDL_GPU_SAMPLECOUNT_1;
    gpi.depth_stencil_state.enable_depth_test     = false;
    gpi.depth_stencil_state.enable_depth_write    = false;
    gpi.target_info.num_color_targets       = 1;
    gpi.target_info.color_target_descriptions = &ctd;
    gpi.target_info.has_depth_stencil_target  = true;
    gpi.target_info.depth_stencil_format      = DEPTH_FMT;
    g_water_pipe = SDL_CreateGPUGraphicsPipeline(g_dev, &gpi);
    if (!g_water_pipe) {
        std::fprintf(stderr, "[gxsdl] native water pipeline create failed: %s\n", SDL_GetError());
        std::abort();
    }
    return g_water_pipe;
}

// ── SMS_NATIVE_PLATFORM cloud pass ─────────────────────────────────────────────────────────────
// Port of sky.bmd cloud strip (shaderKey 0x7bc0841d). RE'd on 2026-07-03:
//   * 2 TEV stages, both sampling the SAME 8×8 I4 texture with different tex-gens (offset UVs).
//     Stage 0 = RASC × TEX(uv0), stage 1 = CPREV × TEX(uv1). Since RASC = matColor white,
//     the color output = TEX(uv0) × TEX(uv1).
//   * Blend = SRC_ALPHA / ONE (additive). Alpha comes from TEXA → same intensity pattern.
//   * 8×8 texel mean = 0.48 → contribution mean = 0.48² ≈ 0.23 additive white per pixel.
// Native fragment analytically models TEX × TEX by multiplying two offset value-noise fields
// at ~8-cell scale (matches the 8-pixel texel period on a repeat-wrapped strip). Density is
// scaled by the region mask so clouds fade into the horizon and sit only in the upper sky.
// ── Byte-exact cloud-strip FRAGMENT SHADER (in-stream swap) ─────────────────────────────────────
// This fragment is patched into the sky.bmd cloud-strip batches (shaderKey hi32 == 0x7bc0841d)
// via NvkTevBatch::fragGlsl in sms_boot_present.cpp. The batch keeps its real mesh (40 vertices
// with per-vertex TEX0/TEX1 UVs produced by the material's texgens — see sb_texgen_uv), its
// material's PE state (blend SRC_ALPHA/ONE, depth test/write), and its position in the batch
// stream (DrawBuf AfterIndirect Xlu, after the dome, before HUD/palm). We only replace the
// per-pixel colour computation with the RE'd math: sample the byte-exact 8×8 I4 pattern at
// vUV[0] and vUV[1], multiply, output white × (t0×t1) with same as alpha (matches TEV chain
// stage0 = RASC × TEX0.rgba, stage1 = CPREV × TEX1.rgba, RASC = matC0 = white).
//
// Uses the standard TEV vertex-shader interface (see native/render/shaders/tev.vert): vUV[0..7],
// vColor, vColor1. No textures bound (empty samplers) — we don't sample tex[8], we sample the
// embedded const array; the standard batch pipeline still binds 8 samplers via g_white so the
// descriptor set matches, but they're not read.
constexpr const char kCloudStripFragGlsl[] = R"(#version 450
layout(location=0) in vec4 vColor;
layout(location=1) in vec2 vUV[8];
layout(location=9) in vec4 vColor1;
layout(location=0) out vec4 o;
layout(set=0, binding=0) uniform sampler2D tex[8];
layout(push_constant) uniform Mat { ivec4 kcolor[4]; ivec4 tevreg[4]; } m;

// The RE'd 8×8 I4 texel intensities (0..1). See [cloudmat] probe dump in commit 5423fe5.
const float kCloudTex[64] = float[64](
    0.000, 0.000, 0.000, 0.000, 1.000, 1.000, 1.000, 0.400,
    0.000, 0.000, 0.000, 0.000, 1.000, 1.000, 1.000, 1.000,
    0.000, 0.000, 0.000, 0.000, 1.000, 1.000, 1.000, 0.400,
    0.000, 0.000, 0.000, 1.000, 0.000, 1.000, 1.000, 1.000,
    1.000, 1.000, 1.000, 0.000, 1.000, 0.000, 0.000, 0.000,
    1.000, 1.000, 1.000, 1.000, 0.000, 0.000, 0.000, 0.000,
    1.000, 1.000, 1.000, 1.000, 0.000, 0.000, 0.000, 0.000,
    1.000, 1.000, 1.000, 1.000, 0.000, 0.000, 0.000, 0.000
);

// Bilinear sample of the 8×8 pattern with REPEAT wrap (matches material's wrap=1 + GX_LINEAR).
// uv is in NORMALISED texture coords (0..1) — same convention as the sampler2D API and as the
// texgen output (J3D texgen matrices are authored in normalised uv space).
float sampleTex(vec2 uv) {
    vec2 tex = uv * 8.0;                // to texel coords
    tex = mod(tex + 800.0, 8.0);        // REPEAT wrap
    vec2 i0 = floor(tex);
    vec2 f  = tex - i0;
    ivec2 a = ivec2(i0);
    ivec2 b = ivec2(mod(i0 + 1.0, 8.0));
    float s00 = kCloudTex[a.y * 8 + a.x];
    float s10 = kCloudTex[a.y * 8 + b.x];
    float s01 = kCloudTex[b.y * 8 + a.x];
    float s11 = kCloudTex[b.y * 8 + b.x];
    float sx0 = mix(s00, s10, f.x);
    float sx1 = mix(s01, s11, f.x);
    return mix(sx0, sx1, f.y);
}

void main() {
    // RE'd TEV combine collapsed to a single expression:
    //   stage0.rgb = TEX(uv0).rgb  (RASC=1, a=0, d=0)
    //   stage1.rgb = stage0.rgb × TEX(uv1).rgb  (CPREV × TEXC)
    //   final.rgb  = TEX(uv0).rgb × TEX(uv1).rgb
    // Since the 8×8 is I4, R=G=B=A=intensity. Final.rgb = white × (t0 * t1) where the RGB is
    // 1 and the alpha channel carries t0*t1 for the SRC_ALPHA / ONE additive blend.
    float t0 = sampleTex(vUV[0]);
    float t1 = sampleTex(vUV[1]);
    float k  = t0 * t1;
    o = vec4(1.0, 1.0, 1.0, k);
    // Reference unused vars so glslang doesn't optimise out the shader interface (which would
    // break the pipeline's descriptor-set contract with the batch's bound state).
    if (m.kcolor[0].w == -12345678) o.r += vColor.r * 1e-30 + vColor1.r * 1e-30 + texture(tex[0], vec2(0)).r * 1e-30;
}
)";


void native_water_fill(const float top[4], const float bottom[4], float horizon_ndc_y) {
    if (!g_ok || !g_in_frame || !g_cmd || !top || !bottom) return;
    SDL_GPUGraphicsPipeline* pipe = ensure_water_pipeline();
    if (!pipe) return;

    NativeWaterPush push{};
    for (int i = 0; i < 4; ++i) { push.top[i] = top[i]; push.bottom[i] = bottom[i]; }
    // horizon_ndc_y is the STRIP TOP (where paint fades in from sky). Bottom fades out at
    // horizon_ndc_y + 0.40 (empirical: oracle water ends where beach/Mario start ~y_ndc +0.70).
    // Strip: horizon_ndc_y (top) to horizon_ndc_y + 0.50 (bottom). Below the strip is beach floor
    // where Mario stands — scene batches draw the floor tiles / shore terrain there; painting
    // through would tint them turquoise (visible defect). Sampling oracle: water spans y_frac
    // 0.60..0.85 = y_ndc +0.20..+0.70; caller sets top to +0.20, strip stops at +0.70.
    push.horizon[0] = horizon_ndc_y;              // upper edge (fade in at horizon)
    push.horizon[1] = horizon_ndc_y + 0.60f;      // lower edge (fade out at shore)
    push.horizon[2] = 0.06f;                      // upper softness
    push.horizon[3] = 0.06f;                      // lower softness

    // Paint over the current color target — preserve existing content (LOAD), no depth needed.
    SDL_GPUColorTargetInfo cti{}; cti.texture = g_color;
    cti.load_op = SDL_GPU_LOADOP_LOAD; cti.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(g_cmd, &cti, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(rp, pipe);
    SDL_PushGPUFragmentUniformData(g_cmd, 0, &push, (Uint32)sizeof(push));
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
}

// ── SMS_NATIVE_PLATFORM zzz sleep bubbles ───────────────────────────────────────────────────────
// Screen-space blue-tinted "Z" glyphs painted over the final composite. Vertex-buffer-less draw
// of 6 verts/quad, up to 8 quads (instanced). Push constants carry per-quad (x, y, half, alpha).
// Fragment shader draws a soft-edged "Z" (three thick strokes) on a translucent rounded card so
// the intent (rising zzz bubbles above sleeping Mario) reads at a glance.
constexpr const char kNativeZzzVs[] = R"(#version 450
layout(location=0) out vec2 vUV;
layout(location=1) out float vAlpha;
layout(set=1, binding=0) uniform Zzz { vec4 quads[8]; int count; } zzz;
void main() {
    int qi = gl_VertexIndex / 6;
    int vi = gl_VertexIndex % 6;
    // Two-triangle quad (0,1,2)(0,2,3) → corners (-1,-1)(+1,-1)(+1,+1)(-1,+1).
    vec2 corners[6] = vec2[6](
        vec2(-1.0,-1.0), vec2(+1.0,-1.0), vec2(+1.0,+1.0),
        vec2(-1.0,-1.0), vec2(+1.0,+1.0), vec2(-1.0,+1.0)
    );
    vec2 c = corners[vi];
    vec4 q = zzz.quads[qi];               // x, y, half, alpha
    float hsz = q.z;                       // half-size in NDC (`half` is a reserved GLSL word).
    // Vulkan clip-y: -1 top, +1 bottom. UV (0..1) top-left origin — flip Y to match glyph draw.
    vec2 pos = vec2(q.x + c.x * hsz, q.y + c.y * hsz);
    vUV = vec2(0.5 + 0.5 * c.x, 0.5 - 0.5 * c.y);
    vAlpha = q.w;
    // Cull the excess instances (past count) by collapsing them to a point.
    if (qi >= zzz.count) { pos = vec2(2.0, 2.0); }
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";

// Fragment: soft rounded card + a stylized "Z" glyph (3 strokes).
constexpr const char kNativeZzzFs[] = R"(#version 450
layout(location=0) in vec2 vUV;
layout(location=1) in float vAlpha;
layout(location=0) out vec4 outColor;

float strokeDist(vec2 uv, vec2 a, vec2 b, float w) {
    vec2 pa = uv - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    float d = length(pa - ba * h);
    return smoothstep(w, w * 0.5, d);
}
void main() {
    // uv in [0,1]. Center at (0.5,0.5).
    vec2 uv = vUV;
    vec2 p  = uv - vec2(0.5);
    float r = length(p);
    // Soft card fade — rounded square-ish; falls off past radius 0.45.
    float card = smoothstep(0.50, 0.35, r);
    // "Z" glyph: top bar, diagonal, bottom bar. All in UV space.
    const float w = 0.09;
    float z1 = strokeDist(uv, vec2(0.25, 0.28), vec2(0.75, 0.28), w);
    float z2 = strokeDist(uv, vec2(0.72, 0.30), vec2(0.28, 0.70), w);
    float z3 = strokeDist(uv, vec2(0.25, 0.72), vec2(0.75, 0.72), w);
    float glyph = clamp(z1 + z2 + z3, 0.0, 1.0);

    // Blue-tinted card + white glyph, alpha modulated by per-quad alpha and card fade.
    vec3 cardCol  = vec3(0.35, 0.55, 0.95);
    vec3 glyphCol = vec3(1.00, 1.00, 1.00);
    vec3 rgb = mix(cardCol, glyphCol, glyph);
    float a = vAlpha * (card * 0.75 + glyph * 0.9);
    outColor = vec4(rgb, a);
}
)";

struct NativeZzzPush { float quads[8][4]; int count; int _pad0; int _pad1; int _pad2; };
static SDL_GPUShader*           g_zzz_vs   = nullptr;
static SDL_GPUShader*           g_zzz_fs   = nullptr;
static SDL_GPUGraphicsPipeline* g_zzz_pipe = nullptr;

static SDL_GPUGraphicsPipeline* ensure_zzz_pipeline() {
    if (g_zzz_pipe) return g_zzz_pipe;
    std::vector<uint32_t> vspv = sb_compile_vertex_glsl(kNativeZzzVs);
    std::vector<uint32_t> fspv = sb_compile_fragment_glsl(kNativeZzzFs);
    if (vspv.empty() || fspv.empty()) {
        std::fprintf(stderr, "\n=== FATAL [gxsdl]: native zzz shader compile failed (vs=%d fs=%d) ===\n",
                     (int)vspv.empty(), (int)fspv.empty());
        std::fflush(stderr); std::abort();
    }
    SDL_GPUShaderCreateInfo vci{};
    vci.code = (const Uint8*)vspv.data(); vci.code_size = vspv.size() * 4; vci.entrypoint = "main";
    vci.format = SDL_GPU_SHADERFORMAT_SPIRV; vci.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vci.num_samplers = 0; vci.num_uniform_buffers = 1;
    g_zzz_vs = SDL_CreateGPUShader(g_dev, &vci);
    g_zzz_fs = make_shader(fspv.data(), fspv.size() * 4, SDL_GPU_SHADERSTAGE_FRAGMENT,
                           /*samplers*/0, /*uniform*/0);
    if (!g_zzz_vs || !g_zzz_fs) {
        std::fprintf(stderr, "\n=== FATAL [gxsdl]: native zzz shader create vs=%p fs=%p err=%s ===\n",
                     (void*)g_zzz_vs, (void*)g_zzz_fs, SDL_GetError());
        std::abort();
    }
    SDL_GPUColorTargetDescription ctd{};
    ctd.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ctd.blend_state.enable_blend = true;
    ctd.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    ctd.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    ctd.blend_state.color_blend_op        = SDL_GPU_BLENDOP_ADD;
    ctd.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    ctd.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    ctd.blend_state.alpha_blend_op        = SDL_GPU_BLENDOP_ADD;
    SDL_GPUGraphicsPipelineCreateInfo gpi{};
    gpi.vertex_shader = g_zzz_vs;
    gpi.fragment_shader = g_zzz_fs;
    gpi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    gpi.vertex_input_state.num_vertex_buffers = 0;
    gpi.vertex_input_state.num_vertex_attributes = 0;
    gpi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    gpi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    gpi.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    gpi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    gpi.depth_stencil_state.enable_depth_test = false;
    gpi.depth_stencil_state.enable_depth_write = false;
    gpi.target_info.num_color_targets = 1;
    gpi.target_info.color_target_descriptions = &ctd;
    gpi.target_info.has_depth_stencil_target = true;
    gpi.target_info.depth_stencil_format = DEPTH_FMT;
    g_zzz_pipe = SDL_CreateGPUGraphicsPipeline(g_dev, &gpi);
    if (!g_zzz_pipe) {
        std::fprintf(stderr, "\n=== FATAL [gxsdl]: native zzz pipeline create failed: %s ===\n",
                     SDL_GetError());
        std::abort();
    }
    return g_zzz_pipe;
}

void native_zzz_paint(const float quads[][4], int count) {
    if (!g_ok || !g_in_frame || !g_cmd || !quads || count <= 0) return;
    if (count > 8) count = 8;
    SDL_GPUGraphicsPipeline* pipe = ensure_zzz_pipeline();
    if (!pipe) return;

    NativeZzzPush push{};
    for (int i = 0; i < count; ++i) {
        push.quads[i][0] = quads[i][0];
        push.quads[i][1] = quads[i][1];
        push.quads[i][2] = quads[i][2];
        push.quads[i][3] = quads[i][3];
    }
    push.count = count;

    SDL_GPUColorTargetInfo cti{}; cti.texture = g_color; cti.clear_color = g_clear;
    cti.load_op = SDL_GPU_LOADOP_LOAD; cti.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dti{}; dti.texture = g_depth; dti.clear_depth = 1.0f;
    dti.load_op = SDL_GPU_LOADOP_LOAD; dti.store_op = SDL_GPU_STOREOP_STORE;
    dti.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; dti.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(g_cmd, &cti, 1, &dti);
    SDL_BindGPUGraphicsPipeline(rp, pipe);
    SDL_PushGPUVertexUniformData(g_cmd, 0, &push, (Uint32)sizeof(push));
    SDL_DrawGPUPrimitives(rp, count * 6, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
}

void snapshot_efb(const void* key) {
    if (!g_ok || !g_in_frame || !g_cmd || !key) return;
    // Reuse an existing snapshot texture for this key (a key repeats only across frames, where
    // frame_begin already released it; within a frame each dest is distinct). Create one sized to
    // the offscreen target and GPU-copy g_color into it (no CPU round-trip).
    SDL_GPUTextureCreateInfo tci{};
    tci.type = SDL_GPU_TEXTURETYPE_2D; tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    tci.width = (Uint32)g_w; tci.height = (Uint32)g_h; tci.layer_count_or_depth = 1;
    tci.num_levels = 1; tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* snap = SDL_CreateGPUTexture(g_dev, &tci);
    if (!snap) { std::fprintf(stderr, "[gxsdl] snapshot texture create failed: %s\n", SDL_GetError()); return; }
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(g_cmd);
    SDL_GPUTextureLocation s{}; s.texture = g_color;
    SDL_GPUTextureLocation d{}; d.texture = snap;
    SDL_CopyGPUTextureToTexture(cp, &s, &d, (Uint32)g_w, (Uint32)g_h, 1, false);
    SDL_EndGPUCopyPass(cp);
    g_snap[key] = snap;
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
        // winding is irrelevant; depth unaffected). Lock g_cpu — the SDL main thread reads it.
        const uint8_t* src = (const uint8_t*)mapped;
        const size_t row = (size_t)g_w * 4;
        std::lock_guard<std::mutex> lk(g_cpu_mtx);
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

// ── Live window present (SB_WINDOW=1) — runs on the SDL MAIN thread ───────────────────────────────
// Architecture (user directive 2026-06-30): MAIN thread = SDL (window + events + present), ONE
// separate GAME thread runs the decomp and renders the offscreen frame into g_cpu. present_window()
// is called ONLY from the SDL main thread (boot.cpp's window loop) — never the game thread.
//
// CRITICAL (learned the hard way): presenting on the game/scheduler thread is what crashed/deadlocked
// everything — claiming a window on g_dev (the offscreen render device) spun up Vulkan WSI threads and
// wedged the cooperative scheduler's sb_sched_drain_until_idle; and an SDL_Renderer present on the game
// thread hit X11/DRI3 from the wrong thread → SIGSEGV. With present on the SDL main thread and a plain
// SOFTWARE SDL_Renderer (its own backend; g_dev untouched), windowing is single-threaded + safe. g_cpu
// is the one shared buffer (game writes in frame_end, main reads here) — guarded by g_cpu_mtx.
SDL_Window*   g_window   = nullptr;
SDL_Renderer* g_wr       = nullptr;
SDL_Texture*  g_wtex     = nullptr;
bool          g_window_off = false;   // window creation failed → stop trying
bool          g_quit      = false;

bool window_should_quit() { return g_quit; }

void present_window() {
    if (!g_ok || g_window_off) return;
    if (!g_window) {
        g_window = SDL_CreateWindow("sms-boot (native PC renderer)", g_w * 2, g_h * 2,
                                    SDL_WINDOW_RESIZABLE);
        if (!g_window) { std::fprintf(stderr, "[gxsdl] SDL_CreateWindow failed: %s\n", SDL_GetError());
                         g_window_off = true; return; }
        // Force the SOFTWARE renderer: a hardware (Vulkan/GL) SDL_Renderer fights the SDL3-GPU Vulkan
        // device (g_dev) for the same GPU and crashes inside SDL. Software blits via X — trivial at
        // 640×480→window and fully decoupled from the render device. (The heavy 3D render is still GPU.)
        g_wr = SDL_CreateRenderer(g_window, "software");
        if (!g_wr) { std::fprintf(stderr, "[gxsdl] SDL_CreateRenderer failed: %s\n", SDL_GetError());
                     SDL_DestroyWindow(g_window); g_window = nullptr; g_window_off = true; return; }
        SDL_SetRenderVSync(g_wr, 0);   // non-blocking present — must not stall the scheduler runner
        g_wtex = SDL_CreateTexture(g_wr, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, g_w, g_h);
        std::fprintf(stderr, "[gxsdl] live window up (%dx%d, SDL_Renderer)\n", g_w * 2, g_h * 2);
    }
    // Pump events (responsiveness + clean quit).
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) g_quit = true;
        else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_ESCAPE) g_quit = true;
    }
    if (!g_wtex) return;
    {   // lock the read against the game thread's frame_end write
        std::lock_guard<std::mutex> lk(g_cpu_mtx);
        if (!g_cpu.empty()) SDL_UpdateTexture(g_wtex, nullptr, g_cpu.data(), g_w * 4);
    }
    SDL_RenderClear(g_wr);
    SDL_RenderTexture(g_wr, g_wtex, nullptr, nullptr);   // scales to the window
    SDL_RenderPresent(g_wr);
}

// SB_WINDOW=1 → live-inspection window mode. Read once.
bool window_mode() {
    static int v = -1;
    if (v < 0) { const char* e = std::getenv("SB_WINDOW"); v = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return v != 0;
}

// Called from the SDL MAIN thread BEFORE the game thread starts, so SDL video is initialised on the
// thread that will own the window + event pump (SDL's windowing is main-thread-affine).
void window_preinit() {
    if (!SDL_WasInit(SDL_INIT_VIDEO)) SDL_InitSubSystem(SDL_INIT_VIDEO);
}

void mark_game_done() { g_game_done.store(true, std::memory_order_release); }
bool game_done()      { return g_game_done.load(std::memory_order_acquire); }

// Exported for the shader-swap in sms_boot_present.cpp — points to the byte-exact fragment
// text; the pipeline cache keys on kCloudStripFragKey so a distinct pipeline is built for
// the swapped batches (never colliding with the material's original TEV-generated key).
const char* const kCloudStripFragGlsl_ptr = kCloudStripFragGlsl;
const uint64_t     kCloudStripFragKey     = 0x7bc0841dc10d0100ull;   // distinct sentinel

} // namespace sb::gxsdl
