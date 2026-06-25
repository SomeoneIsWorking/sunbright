// gx_sdlgpu.cpp — see gx_sdlgpu.h. GX→SDL3-GPU switch.
//   P1: headless GPU device + offscreen color target + clear + readback.
//   P2: vertex-buffer upload + ONE modulate pipeline (cull NONE) drawing every batch, so the
//       captured scene geometry renders through SDL3 GPU. NvkTevVertex is fed RAW (SDL3 GPU NDC =
//       Vulkan NDC, exactly nvk's contract — no Y-flip / depth-remap). Per-material TEV combiner,
//       per-batch blend/depth, and textured filtering fidelity are P3. docs/gx_sdlgpu_switch.md.
#include "gx_sdlgpu.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include "glsl_compile.h"     // sb_compile_fragment_glsl (GLSL 450 -> SPIR-V via glslang)
#include "tev_vert_spv.h"     // tev_vert_spv[] : the shipping TEV vertex shader (SPIR-V), reused

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstddef>
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

// Vertex buffer (grown as needed) + its upload transfer buffer.
SDL_GPUBuffer*         g_vbuf     = nullptr;
SDL_GPUTransferBuffer* g_vup      = nullptr;
size_t                 g_vbuf_cap = 0;      // bytes

// P2 pipeline + shaders + default white texture + sampler.
SDL_GPUGraphicsPipeline* g_pipe = nullptr;
SDL_GPUSampler*          g_samp = nullptr;
SDL_GPUTexture*          g_white = nullptr;

// Per-frame command recording.
SDL_GPUCommandBuffer*  g_cmd = nullptr;
SDL_GPURenderPass*     g_rp  = nullptr;
bool                   g_in_frame = false;
SDL_FColor             g_clear{};

// Per-frame texture uploads (keyed by source pixel pointer; released each frame).
std::unordered_map<const void*, SDL_GPUTexture*> g_tex_cache;

std::vector<uint8_t> g_cpu;   // last downloaded frame (top-left origin RGBA8)

constexpr SDL_GPUTextureFormat DEPTH_FMT = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

SDL_GPUShader* make_shader(const void* code, size_t bytes, SDL_GPUShaderStage stage, Uint32 nSamplers) {
    SDL_GPUShaderCreateInfo ci{};
    ci.code = (const Uint8*)code; ci.code_size = bytes; ci.entrypoint = "main";
    ci.format = SDL_GPU_SHADERFORMAT_SPIRV; ci.stage = stage; ci.num_samplers = nSamplers;
    return SDL_CreateGPUShader(g_dev, &ci);
}

// Minimal modulate fragment shader (GLSL 450). SDL3 GPU mandates fragment samplers at set=2; the
// in-locations match tev.vert's outputs (loc0 color0, loc1 uv[8], loc9 color1). P3 swaps this for
// the generated per-material TEV combiner.
const char* kModFS =
    "#version 450\n"
    "layout(location=0) in vec4 vColor;\n"
    "layout(location=1) in vec2 vUV[8];\n"
    "layout(location=9) in vec4 vColor1;\n"
    "layout(set=2, binding=0) uniform sampler2D tex0;\n"
    "layout(location=0) out vec4 outColor;\n"
    "void main(){ outColor = texture(tex0, vUV[0]) * vColor; }\n";

bool build_pipeline() {
    SDL_GPUShader* vs = make_shader(tev_vert_spv, sizeof(tev_vert_spv), SDL_GPU_SHADERSTAGE_VERTEX, 0);
    if (!vs) { std::fprintf(stderr, "[gxsdl] vertex shader create failed: %s\n", SDL_GetError()); return false; }
    std::vector<uint32_t> fspv = sb_compile_fragment_glsl(kModFS);
    if (fspv.empty()) { std::fprintf(stderr, "[gxsdl] modulate FS compile failed\n"); return false; }
    SDL_GPUShader* fs = make_shader(fspv.data(), fspv.size() * 4, SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
    if (!fs) { std::fprintf(stderr, "[gxsdl] fragment shader create failed: %s\n", SDL_GetError()); return false; }

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
    ctd.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;   // P2: opaque (blend disabled)

    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader = vs; pci.fragment_shader = fs;
    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pci.vertex_input_state.vertex_buffer_descriptions = &vbd;
    pci.vertex_input_state.num_vertex_buffers = 1;
    pci.vertex_input_state.vertex_attributes = attrs;
    pci.vertex_input_state.num_vertex_attributes = 7;
    pci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;   // match nvk VK_CULL_MODE_NONE
    pci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pci.rasterizer_state.enable_depth_clip = true;            // native near/far clip (NOT clamp)
    pci.depth_stencil_state.enable_depth_test = true;
    pci.depth_stencil_state.enable_depth_write = true;
    pci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    pci.target_info.color_target_descriptions = &ctd;
    pci.target_info.num_color_targets = 1;
    pci.target_info.depth_stencil_format = DEPTH_FMT;
    pci.target_info.has_depth_stencil_target = true;

    g_pipe = SDL_CreateGPUGraphicsPipeline(g_dev, &pci);
    SDL_ReleaseGPUShader(g_dev, vs);
    SDL_ReleaseGPUShader(g_dev, fs);
    if (!g_pipe) { std::fprintf(stderr, "[gxsdl] CreateGPUGraphicsPipeline failed: %s\n", SDL_GetError()); return false; }
    return true;
}

// Upload one RGBA8 image to a fresh GPU texture (transient transfer buffer + copy pass on g_cmd).
SDL_GPUTexture* upload_texture(const uint8_t* rgba, Uint32 w, Uint32 h) {
    SDL_GPUTextureCreateInfo tci{};
    tci.type = SDL_GPU_TEXTURETYPE_2D; tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; tci.width = w; tci.height = h;
    tci.layer_count_or_depth = 1; tci.num_levels = 1; tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
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
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_ReleaseGPUTransferBuffer(g_dev, tb);   // safe to release after the copy is recorded
    return tex;
}

SDL_GPUTexture* tex_for(const sb::render::Nvk::NvkTevBatch::Tex& t) {
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
    static int v = -1;
    if (v < 0) { const char* e = std::getenv("SB_SDLGPU"); v = (e && e[0] && e[0] != '0') ? 1 : 0; }
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

    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter = SDL_GPU_FILTER_LINEAR; sci.mag_filter = SDL_GPU_FILTER_LINEAR;
    sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    g_samp = SDL_CreateGPUSampler(g_dev, &sci);

    if (!build_pipeline()) return false;

    // 1x1 white default texture for untextured batches (needs a command buffer for the upload).
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
    // The render pass (with the clear) opens in draw_tev, AFTER the upload copy passes — SDL3 GPU
    // forbids copy passes inside a render pass.
}

void draw_tev(const sb::render::NvkTevVertex* verts, int nverts,
              const sb::render::Nvk::NvkTevBatch* batches, int nbatch) {
    if (!g_ok || !g_in_frame || !g_cmd) return;

    // Release last frame's texture uploads (their source pixel pointers are reused per frame).
    for (auto& kv : g_tex_cache) if (kv.second) SDL_ReleaseGPUTexture(g_dev, kv.second);
    g_tex_cache.clear();

    if (nverts < 0) nverts = 0;

    // ── Upload phase (copy passes — must precede the render pass) ──────────────────────────
    if (nverts > 0 && verts) {
        size_t bytes = (size_t)nverts * sizeof(sb::render::NvkTevVertex);
        ensure_vbuf(bytes);
        void* m = SDL_MapGPUTransferBuffer(g_dev, g_vup, false);
        std::memcpy(m, verts, bytes);
        SDL_UnmapGPUTransferBuffer(g_dev, g_vup);
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(g_cmd);
        SDL_GPUTransferBufferLocation src{}; src.transfer_buffer = g_vup; src.offset = 0;
        SDL_GPUBufferRegion dst{}; dst.buffer = g_vbuf; dst.offset = 0; dst.size = (Uint32)bytes;
        SDL_UploadToGPUBuffer(cp, &src, &dst, false);
        SDL_EndGPUCopyPass(cp);
    }
    // Upload each batch's texture (own copy passes inside tex_for).
    for (int bi = 0; bi < nbatch && batches; ++bi) {
        const auto& b = batches[bi];
        if (b.vcount && b.tex[0].rgba && b.tex[0].w && b.tex[0].h) (void)tex_for(b.tex[0]);
    }

    // ── Render pass (clear color + depth, then draw every batch) ──────────────────────────
    SDL_GPUColorTargetInfo cti{}; cti.texture = g_color; cti.clear_color = g_clear;
    cti.load_op = SDL_GPU_LOADOP_CLEAR; cti.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dti{}; dti.texture = g_depth; dti.clear_depth = 1.0f;
    dti.load_op = SDL_GPU_LOADOP_CLEAR; dti.store_op = SDL_GPU_STOREOP_DONT_CARE;
    dti.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; dti.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    g_rp = SDL_BeginGPURenderPass(g_cmd, &cti, 1, &dti);

    if (nverts > 0 && verts && nbatch > 0 && batches) {
        SDL_BindGPUGraphicsPipeline(g_rp, g_pipe);
        SDL_GPUBufferBinding vb{}; vb.buffer = g_vbuf; vb.offset = 0;
        SDL_BindGPUVertexBuffers(g_rp, 0, &vb, 1);
        for (int bi = 0; bi < nbatch; ++bi) {
            const auto& b = batches[bi];
            if (b.vcount == 0 || b.vstart >= (uint32_t)nverts) continue;
            uint32_t count = b.vcount;
            if (b.vstart + count > (uint32_t)nverts) count = (uint32_t)nverts - b.vstart;
            SDL_GPUTexture* tex = (b.tex[0].rgba && b.tex[0].w && b.tex[0].h) ? tex_for(b.tex[0]) : g_white;
            if (!tex) tex = g_white;
            SDL_GPUTextureSamplerBinding tsb{}; tsb.texture = tex; tsb.sampler = g_samp;
            SDL_BindGPUFragmentSamplers(g_rp, 0, &tsb, 1);
            SDL_DrawGPUPrimitives(g_rp, count, 1, b.vstart, 0);
        }
    }
    SDL_EndGPURenderPass(g_rp);
    g_rp = nullptr;
}

void frame_end() {
    if (!g_ok || !g_in_frame || !g_cmd) { g_in_frame = false; return; }
    // If draw_tev never opened/closed a render pass (e.g. it early-returned), do a clear-only pass
    // so the frame still reflects the GX copy-clear.
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
        // SDL3 GPU's clip→framebuffer Y is inverted vs raw Vulkan (nvk), so the downloaded image is
        // bottom-up relative to the top-left PPM/nvk convention. Flip rows on copy-out. (Pure
        // presentation; cull is NONE so winding is irrelevant; depth is unaffected.)
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
    std::memcpy(rgba, g_cpu.data(), (size_t)w * h * 4);   // SDL3 GPU textures are top-left origin
    return true;
}

} // namespace sb::gxsdl
