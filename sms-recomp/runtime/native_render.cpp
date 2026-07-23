// native_render.cpp — the recomp's own GX renderer on SDL3 GPU (2026-07-23, user-directed).
//
// The end state: no Aurora, no Dolphin — the parsed GX stream (dev_gxfifo) rendered by code we own,
// on the SDL3 GPU API. Built INCREMENTALLY alongside aurora, which stays as the per-frame parity
// oracle until this reaches parity (CLAUDE.md RENDERER DOCTRINE, 2026-07-23).
//
// The SDL3-GPU device and the clear/target/readback plumbing are resurrected from the retired
// Path-B renderer (git 9283f44^:native/render/gx_sdlgpu.cpp), which reached real per-material TEV.
// Its BACKEND (batches -> GPU) resurrects here; its FRONTEND (GX state -> transformed verts + TEV
// shaders) has to be driven from dev_gxfifo's FIFO parse, which is the work ahead.
//
// MILESTONE LADDER (each A/B'd against aurora): [0] device + clear + readback  ->  pass-through geom
// ->  vertex transform  ->  TEV  ->  textures  ->  EFB. This file is at milestone 0.
//
//   SBR_SDLGPU=1        stand the device up and, once per frame, clear the native target to the GX
//                       copy-clear colour and read it back (proves the plumbing; nothing drawn yet).

#include "native_render.h"

#include <lucent/log.h>

#include <SDL3/SDL.h>

#include "gx_texture.h"

#include "shaders/geom_vert_spv.h"
#include "shaders/geom_frag_spv.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <vector>

namespace {

constexpr SDL_GPUTextureFormat kColorFmt = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
constexpr SDL_GPUTextureFormat kDepthFmt = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

SDL_GPUDevice*         g_dev   = nullptr;
SDL_GPUTexture*        g_color = nullptr;   // offscreen EFB-sized colour target
SDL_GPUTexture*        g_depth = nullptr;   // depth target
SDL_GPUTransferBuffer* g_dl    = nullptr;   // download staging (w*h*4)
int  g_w = 0, g_h = 0;
int  g_lastBatches = 0;
bool g_tried = false, g_ok = false;

std::vector<uint8_t> g_cpu;   // last frame read back, top-left origin RGBA8

// Geometry path (milestone 1). Vertices arrive in CLIP space already (the frontend does posMtx +
// projection on the CPU), so one pipeline serves every draw for now — no per-material state yet.
SDL_GPUShader* g_vs = nullptr;
SDL_GPUShader* g_fs = nullptr;
// One pipeline per distinct depth state. GX varies depth state per MATERIAL, and SDL3 GPU has no
// dynamic depth state, so the state has to be baked into a pipeline and the scene split into runs.
std::unordered_map<uint32_t, SDL_GPUGraphicsPipeline*> g_pipes;

struct Batch {
    SbrDepthState st;
    uint32_t first, count;
    uint64_t texKey;        // 0 = untextured (a 1x1 white texel is bound so one shader serves both)
};

// Decoded textures, keyed by the guest description. Textures are immutable for a given
// (address, format, size), so decoding once and caching is not an optimisation but a requirement:
// decoding a 1024-texel CMPR image per draw would dominate the frame.
struct Tex {
    SDL_GPUTexture* tex = nullptr;
};
std::unordered_map<uint64_t, Tex> g_texs;
std::unordered_map<uint64_t, SbrTexture> g_pendingTex;   // descriptions seen this frame
SDL_GPUSampler* g_sampler = nullptr;
size_t g_texBytes = 0;
SDL_GPUTexture* g_white = nullptr;

// SBR_TEX=1 opts INTO texture decode/upload. Default OFF: this path drives GPU allocations from
// guest data, and a defect here does not fail politely — it can take the device down for the whole
// process. It stays opt-in until it has run clean for a while.
bool textures_enabled() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_TEX");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

uint64_t tex_key(const SbrTexture& t) {
    if (!textures_enabled()) return 0;
    if (t.addr == 0 || t.width == 0 || t.height == 0) return 0;
    return (uint64_t)t.addr << 24 ^ (uint64_t)t.format << 20 ^ (uint64_t)t.width << 10 ^ t.height;
}
std::vector<Batch> g_batches;
SDL_GPUBuffer*           g_vbuf = nullptr;
SDL_GPUTransferBuffer*   g_vup  = nullptr;
size_t                   g_vcap = 0;
std::vector<SbrVertex>   g_verts;   // accumulated this frame

// The resource counts are NOT optional. SDL3 GPU builds the pipeline layout from them, not by
// reflecting the SPIR-V, so a fragment shader that declares a sampler while the create-info says
// zero produces a descriptor-set mismatch — which manifests as VK_ERROR_DEVICE_LOST, and (with
// aurora's Dawn device in the same process) surfaces on aurora's device, blaming the wrong
// subsystem. This was the actual cause of the device losses, not the texture data.
SDL_GPUShader* make_shader(const void* code, size_t bytes, SDL_GPUShaderStage stage,
                           Uint32 numSamplers) {
    SDL_GPUShaderCreateInfo ci{};
    ci.code = (const Uint8*)code;
    ci.code_size = bytes;
    ci.entrypoint = "main";
    ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    ci.stage = stage;
    ci.num_samplers = numSamplers;
    ci.num_storage_textures = 0;
    ci.num_storage_buffers = 0;
    ci.num_uniform_buffers = 0;
    return SDL_CreateGPUShader(g_dev, &ci);
}

SDL_GPUCompareOp gx_compare(uint8_t f) {
    // GXCompare -> backend compare. The transform in scene.cpp maps GC's [-1,0] clip depth to
    // [0,1] PRESERVING order (nearer stays smaller), so each GX function keeps its meaning.
    switch (f & 7) {
    case 0: return SDL_GPU_COMPAREOP_NEVER;
    case 1: return SDL_GPU_COMPAREOP_LESS;
    case 2: return SDL_GPU_COMPAREOP_EQUAL;
    case 3: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case 4: return SDL_GPU_COMPAREOP_GREATER;
    case 5: return SDL_GPU_COMPAREOP_NOT_EQUAL;
    case 6: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    default: return SDL_GPU_COMPAREOP_ALWAYS;
    }
}

uint32_t depth_key(SbrDepthState d) {
    return (uint32_t)(d.test ? 1 : 0) << 24 | (uint32_t)(d.func & 7) << 20 |
           (uint32_t)(d.write ? 1 : 0) << 16 | (uint32_t)(d.blend & 3) << 8 |
           (uint32_t)(d.srcFac & 7) << 4 | (uint32_t)(d.dstFac & 7);
}

// GXBlendFactor -> backend factor. GX names the factors in terms of the SOURCE and DESTINATION
// colours exactly as the backend does, so this is a direct mapping, not an approximation.
SDL_GPUBlendFactor gx_blend_factor(uint8_t f) {
    switch (f & 7) {
    case 0: return SDL_GPU_BLENDFACTOR_ZERO;
    case 1: return SDL_GPU_BLENDFACTOR_ONE;
    case 2: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case 3: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case 4: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case 5: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case 6: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    default: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    }
}

SDL_GPUGraphicsPipeline* pipeline_for(SbrDepthState d);

void ensure_vbuf(size_t bytes) {
    if (bytes <= g_vcap && g_vbuf != nullptr) return;
    if (g_vbuf) SDL_ReleaseGPUBuffer(g_dev, g_vbuf);
    if (g_vup)  SDL_ReleaseGPUTransferBuffer(g_dev, g_vup);
    g_vcap = bytes + (bytes / 2) + 4096;
    SDL_GPUBufferCreateInfo bci{};
    bci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bci.size = (Uint32)g_vcap;
    g_vbuf = SDL_CreateGPUBuffer(g_dev, &bci);
    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size = (Uint32)g_vcap;
    g_vup = SDL_CreateGPUTransferBuffer(g_dev, &tbci);
}

bool enabled() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_SDLGPU");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

SDL_GPUTexture* upload_rgba(const uint8_t* rgba, uint32_t w, uint32_t h) {
    SDL_GPUTextureCreateInfo ci{};
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = kColorFmt;
    ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width = w; ci.height = h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* t = SDL_CreateGPUTexture(g_dev, &ci);
    if (t == nullptr) return nullptr;

    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size = w * h * 4;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(g_dev, &tbci);
    if (void* m = SDL_MapGPUTransferBuffer(g_dev, tb, false)) {
        std::memcpy(m, rgba, (size_t)w * h * 4);
        SDL_UnmapGPUTransferBuffer(g_dev, tb);
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tb;
    src.pixels_per_row = w;
    src.rows_per_layer = h;
    SDL_GPUTextureRegion dst{};
    dst.texture = t; dst.w = w; dst.h = h; dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    // WAIT before freeing the staging buffer. Releasing it straight after submit frees memory the
    // GPU is still reading, which showed up as a VK_ERROR_DEVICE_LOST — and, because two Vulkan
    // devices share this process, the loss surfaced on AURORA's device, pointing at the wrong
    // subsystem entirely. Uploads are one-time per texture, so the wait costs nothing steady-state.
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(g_dev, true, &fence, 1);
        SDL_ReleaseGPUFence(g_dev, fence);
    }
    SDL_ReleaseGPUTransferBuffer(g_dev, tb);
    return t;
}

// Decode-and-upload on first sight of a texture description.
SDL_GPUTexture* texture_for(uint64_t key, const SbrTexture& t) {
    if (key == 0) return g_white;
    if (const auto it = g_texs.find(key); it != g_texs.end()) return it->second.tex;

    // Guest data is not trusted to size a GPU allocation. GX caps textures at 1024x1024, and an
    // uninitialised GXTexObj decodes to arbitrary dimensions.
    if (t.width == 0 || t.height == 0 || t.width > 1024 || t.height > 1024) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            lucent::error("nrender", "implausible texture {}x{} fmt {} at 0x{:08x} — not uploaded",
                          t.width, t.height, t.format, t.addr);
        }
        g_texs.emplace(key, Tex{g_white});
        return g_white;
    }
    // Hard ceiling on how much VRAM this path may claim, so a key that varies when it should not
    // degrades into white rather than exhausting the device.
    constexpr size_t kMaxTexBytes = 192u << 20;
    if (g_texBytes + (size_t)t.width * t.height * 4 > kMaxTexBytes) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            lucent::error("nrender", "texture budget of {} MB exhausted at {} textures — binding "
                                     "white from here", kMaxTexBytes >> 20, g_texs.size());
        }
        g_texs.emplace(key, Tex{g_white});
        return g_white;
    }
    g_texBytes += (size_t)t.width * t.height * 4;

    // VALIDATE before allocating. GX caps textures at 1024x1024, and a GXTexObj that has not been
    // initialised yet decodes to arbitrary dimensions — allocating on those is how this took the
    // Vulkan device down (and, with two devices in the process, the failure surfaced on aurora's).
    if (t.width == 0 || t.height == 0 || t.width > 1024 || t.height > 1024) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            lucent::error("nrender", "implausible texture {}x{} format {} at 0x{:08x} — not uploaded",
                          t.width, t.height, t.format, t.addr);
        }
        g_texs.emplace(key, Tex{g_white});
        return g_white;
    }

    std::vector<uint8_t> rgba((size_t)t.width * t.height * 4);
    SDL_GPUTexture* gt = nullptr;
    if (gx_decode_texture(t.addr, t.width, t.height, t.format, t.tlut, rgba.data())) {
        gt = upload_rgba(rgba.data(), t.width, t.height);
    } else {
        // Report an undecodable format ONCE by name. Silently binding white here would look like a
        // lighting bug rather than a missing decoder (CLAUDE.md: no silent success-shaped stubs).
        static std::unordered_map<uint32_t, bool> warned;
        if (!warned[t.format]) {
            warned[t.format] = true;
            lucent::error("nrender", "no decoder for texture format {} ({}) at 0x{:08x} {}x{}",
                          t.format, gx_texture_format_name(t.format), t.addr, t.width, t.height);
        }
        gt = g_white;
    }
    g_texs.emplace(key, Tex{gt});
    return gt;
}

SDL_GPUGraphicsPipeline* pipeline_for(SbrDepthState d) {
    const uint32_t key = depth_key(d);
    if (const auto it = g_pipes.find(key); it != g_pipes.end()) return it->second;

    SDL_GPUVertexBufferDescription vbd{};
    vbd.slot = 0;
    vbd.pitch = sizeof(SbrVertex);
    vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute va[3]{};
    va[0].location = 0; va[0].buffer_slot = 0;
    va[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[0].offset = 0;
    va[1].location = 1; va[1].buffer_slot = 0;
    va[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[1].offset = 16;
    va[2].location = 2; va[2].buffer_slot = 0;
    va[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; va[2].offset = 32;

    SDL_GPUColorTargetDescription ctd{};
    ctd.format = kColorFmt;
    // GX_BM_BLEND is the only blend mode the scene actually uses; LOGIC/SUBTRACT are left
    // unblended rather than approximated, and will announce themselves if they ever appear.
    if (d.blend == 1) {
        ctd.blend_state.enable_blend = true;
        ctd.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        ctd.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        ctd.blend_state.src_color_blendfactor = gx_blend_factor(d.srcFac);
        ctd.blend_state.dst_color_blendfactor = gx_blend_factor(d.dstFac);
        ctd.blend_state.src_alpha_blendfactor = gx_blend_factor(d.srcFac);
        ctd.blend_state.dst_alpha_blendfactor = gx_blend_factor(d.dstFac);
    }

    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader = g_vs;
    pci.fragment_shader = g_fs;
    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pci.vertex_input_state.vertex_buffer_descriptions = &vbd;
    pci.vertex_input_state.num_vertex_buffers = 1;
    pci.vertex_input_state.vertex_attributes = va;
    pci.vertex_input_state.num_vertex_attributes = 3;
    // SBR_RENDER_WIREFRAME=1 draws edges instead of filled triangles. A few large planes can
    // occlude an entire correct scene behind them, which is indistinguishable from "the scene is
    // missing" in a filled render — wireframe separates those two cases, so it is a diagnostic
    // worth keeping rather than a one-off.
    const char* wf = std::getenv("SBR_RENDER_WIREFRAME");
    pci.rasterizer_state.fill_mode = (wf != nullptr && wf[0] != '\0' && wf[0] != '0')
                                         ? SDL_GPU_FILLMODE_LINE : SDL_GPU_FILLMODE_FILL;
    pci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;   // GX cull comes with the state machine
    pci.target_info.color_target_descriptions = &ctd;
    pci.target_info.num_color_targets = 1;
    pci.target_info.depth_stencil_format = kDepthFmt;
    pci.target_info.has_depth_stencil_target = true;
    // The GAME's depth state, not an assumed one. The sky is drawn with WRITE DISABLED; honouring
    // that is what stops it stamping depth over the whole scene.
    pci.depth_stencil_state.enable_depth_test = d.test != 0;
    pci.depth_stencil_state.enable_depth_write = d.write != 0;
    pci.depth_stencil_state.compare_op = gx_compare(d.func);

    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(g_dev, &pci);
    if (p == nullptr) {
        // A pipeline that fails to compile must not be silently skipped: the batch would vanish and
        // read as a render bug (CLAUDE.md FAIL FAST).
        lucent::error("nrender", "pipeline create failed for test={} func={} write={} blend={} "
                                 "src={} dst={}: {}",
                      d.test, d.func, d.write, d.blend, d.srcFac, d.dstFac, SDL_GetError());
        std::abort();
    }
    g_pipes.emplace(key, p);
    return p;
}

} // namespace

bool sbr_render_enabled() { return enabled(); }

bool sbr_render_init(int w, int h) {
    if (g_tried) return g_ok && g_w == w && g_h == h;
    g_tried = true;

    // Aurora has already SDL_Init'd video (it owns the window); this reuses that. SDL3 GPU runs its
    // own Vulkan instance independent of aurora's Dawn, so the two devices coexist in the process.
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        lucent::error("nrender", "SDL_InitSubSystem(VIDEO) failed: {}", SDL_GetError());
        return false;
    }
    g_dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, /*debug=*/true, nullptr);
    if (g_dev == nullptr) {
        lucent::error("nrender", "SDL_CreateGPUDevice failed: {}", SDL_GetError());
        return false;
    }

    SDL_GPUTextureCreateInfo cci{};
    cci.type = SDL_GPU_TEXTURETYPE_2D;
    cci.format = kColorFmt;
    cci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    cci.width = (Uint32)w;
    cci.height = (Uint32)h;
    cci.layer_count_or_depth = 1;
    cci.num_levels = 1;
    cci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    g_color = SDL_CreateGPUTexture(g_dev, &cci);

    SDL_GPUTextureCreateInfo dci = cci;
    dci.format = kDepthFmt;
    dci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    g_depth = SDL_CreateGPUTexture(g_dev, &dci);

    if (g_color == nullptr || g_depth == nullptr) {
        lucent::error("nrender", "target texture create failed: {}", SDL_GetError());
        return false;
    }

    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbci.size = (Uint32)(w * h * 4);
    g_dl = SDL_CreateGPUTransferBuffer(g_dev, &tbci);

    // One pipeline: pass-through clip-space verts, depth test on, alpha blending off. The per-GX
    // z-mode and blend state become per-material state as the TEV milestone lands; the depth test
    // itself is not optional, because without it the last drawable submitted paints over the whole
    // scene (measured: a uniform 100%-coverage fill of one object's colour).
    g_vs = make_shader(kGeomVertSpv, sizeof(kGeomVertSpv), SDL_GPU_SHADERSTAGE_VERTEX, 0);
    g_fs = make_shader(kGeomFragSpv, sizeof(kGeomFragSpv), SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
    if (g_vs == nullptr || g_fs == nullptr) {
        lucent::error("nrender", "shader create failed: {}", SDL_GetError());
        return false;
    }
    SDL_GPUVertexBufferDescription vbd{};
    vbd.slot = 0;
    vbd.pitch = sizeof(SbrVertex);
    vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute va[3]{};
    va[0].location = 0; va[0].buffer_slot = 0;
    va[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[0].offset = 0;
    va[1].location = 1; va[1].buffer_slot = 0;
    va[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[1].offset = 16;
    va[2].location = 2; va[2].buffer_slot = 0;
    va[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; va[2].offset = 32;

    // Pipelines are created ON DEMAND per depth state (pipeline_for), not once here: GX varies
    // depth state per material and the backend cannot change it dynamically.
    g_pipes.clear();

    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter = SDL_GPU_FILTER_LINEAR;
    sci.mag_filter = SDL_GPU_FILTER_LINEAR;
    sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    // GX's default wrap for J3D materials is REPEAT; per-material wrap modes arrive with the rest
    // of the material state.
    sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    g_sampler = SDL_CreateGPUSampler(g_dev, &sci);

    // One opaque white texel, bound for untextured draws so a single shader serves both cases.
    const uint8_t white[4] = {255, 255, 255, 255};
    g_white = upload_rgba(white, 1, 1);
    if (g_sampler == nullptr || g_white == nullptr) {
        lucent::error("nrender", "sampler/white texture create failed: {}", SDL_GetError());
        return false;
    }

    g_cpu.assign((size_t)w * h * 4, 0);
    g_w = w;
    g_h = h;
    g_ok = true;
    lucent::info("nrender", "SDL3 GPU device up ({}x{}), driver={}", w, h,
                 SDL_GetGPUDeviceDriver(g_dev));
    return true;
}

namespace { SDL_FColor g_clear{}; int g_lastVerts = 0; }

void sbr_render_begin(float r, float g, float b, float a) {
    if (!g_ok) return;
    g_clear = SDL_FColor{r, g, b, a};
    g_verts.clear();
    g_batches.clear();
}

void sbr_render_tris(const SbrVertex* verts, int count, SbrDepthState depth, SbrTexture tex) {
    if (!g_ok || verts == nullptr || count < 3) return;
    count -= count % 3;
    g_verts.insert(g_verts.end(), verts, verts + count);
    // Merge into the previous run when the state is unchanged, so honouring per-material depth
    // costs draws only where the state actually changes.
    const uint32_t first = (uint32_t)(g_verts.size() - (size_t)count);
    const uint64_t tk = tex_key(tex);
    if (!g_batches.empty() && depth_key(g_batches.back().st) == depth_key(depth) &&
        g_batches.back().texKey == tk &&
        g_batches.back().first + g_batches.back().count == first) {
        g_batches.back().count += (uint32_t)count;
    } else {
        g_batches.push_back(Batch{depth, first, (uint32_t)count, tk});
        g_pendingTex[tk] = tex;
    }
}

// Upload the frame's geometry, render it in one pass over a cleared target, then download for
// readback / the A/B against aurora. The whole sequence — acquire, copy pass, render pass, copy
// pass, fence, map — is the shape every later milestone keeps; only what goes INSIDE the render
// pass grows (per-material pipelines, textures, TEV).
void sbr_render_end() {
    if (!g_ok) return;
    g_lastVerts = (int)g_verts.size();
    g_lastBatches = (int)g_batches.size();

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (cmd == nullptr) {
        lucent::error("nrender", "AcquireGPUCommandBuffer failed: {}", SDL_GetError());
        return;
    }

    const size_t vbytes = g_verts.size() * sizeof(SbrVertex);
    if (vbytes > 0) {
        ensure_vbuf(vbytes);
        if (void* m = SDL_MapGPUTransferBuffer(g_dev, g_vup, false)) {
            std::memcpy(m, g_verts.data(), vbytes);
            SDL_UnmapGPUTransferBuffer(g_dev, g_vup);
        }
        SDL_GPUCopyPass* up = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation src{}; src.transfer_buffer = g_vup; src.offset = 0;
        SDL_GPUBufferRegion dst{}; dst.buffer = g_vbuf; dst.offset = 0; dst.size = (Uint32)vbytes;
        SDL_UploadToGPUBuffer(up, &src, &dst, false);
        SDL_EndGPUCopyPass(up);
    }

    // Decode and upload any NEW textures BEFORE the render pass opens. texture_for acquires and
    // submits its own command buffer, which is illegal inside an active pass — doing it there cost
    // a VK_ERROR_DEVICE_LOST that took the whole process down.
    if (textures_enabled())
        for (const Batch& b : g_batches) texture_for(b.texKey, g_pendingTex[b.texKey]);

    SDL_GPUColorTargetInfo cti{};
    cti.texture = g_color;
    cti.clear_color = g_clear;
    cti.load_op = SDL_GPU_LOADOP_CLEAR;
    cti.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPUDepthStencilTargetInfo dsi{};
    dsi.texture = g_depth;
    dsi.clear_depth = 1.0f;
    dsi.load_op = SDL_GPU_LOADOP_CLEAR;
    dsi.store_op = SDL_GPU_STOREOP_DONT_CARE;

    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &cti, 1, &dsi);
    if (vbytes > 0) {
        SDL_GPUBufferBinding vbTmp{};
        (void)vbTmp;
        SDL_GPUBufferBinding vb{}; vb.buffer = g_vbuf; vb.offset = 0;
        SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
        for (const Batch& b : g_batches) {
            SDL_BindGPUGraphicsPipeline(rp, pipeline_for(b.st));
            SDL_GPUTextureSamplerBinding tsb{};
            const auto texIt = g_texs.find(b.texKey);
            tsb.texture = (texIt != g_texs.end()) ? texIt->second.tex : g_white;
            tsb.sampler = g_sampler;
            SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
            SDL_DrawGPUPrimitives(rp, b.count, 1, b.first, 0);
        }
    }
    SDL_EndGPURenderPass(rp);

    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion reg{};
    reg.texture = g_color; reg.w = (Uint32)g_w; reg.h = (Uint32)g_h; reg.d = 1;
    SDL_GPUTextureTransferInfo tti{};
    tti.transfer_buffer = g_dl; tti.pixels_per_row = (Uint32)g_w; tti.rows_per_layer = (Uint32)g_h;
    SDL_DownloadFromGPUTexture(cp, &reg, &tti);
    SDL_EndGPUCopyPass(cp);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(g_dev, true, &fence, 1);
        SDL_ReleaseGPUFence(g_dev, fence);
    }
    if (void* mapped = SDL_MapGPUTransferBuffer(g_dev, g_dl, false)) {
        std::memcpy(g_cpu.data(), mapped, g_cpu.size());
        SDL_UnmapGPUTransferBuffer(g_dev, g_dl);
    }
}

int sbr_render_last_vertex_count() { return g_lastVerts; }
int sbr_render_last_batch_count() { return g_lastBatches; }
int sbr_render_texture_count() { return (int)g_texs.size(); }

// SBR_RENDER_DUMP=/path.rgba — write the native frame out so it can be diffed against aurora's
// SB_DUMP_FRAME of the same moment (tools/render/compare_native.py). This is the parity harness the
// whole native-render arc is verified through; without it "looks about right" is all we would have.
// Written top-left origin RGBA8, same convention as aurora's dump, so the comparison is apples to
// apples rather than a flip away from nonsense.
bool sbr_render_dump(const char* path) {
    if (!g_ok || path == nullptr || path[0] == '\0') return false;
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        lucent::error("nrender", "cannot open {} for write", path);
        return false;
    }
    const size_t n = std::fwrite(g_cpu.data(), 1, g_cpu.size(), f);
    std::fclose(f);
    lucent::info("nrender", "dumped {}x{} native frame to {} ({} bytes)", g_w, g_h, path, n);
    return n == g_cpu.size();
}

bool sbr_render_readback(uint8_t* rgba, int w, int h) {
    if (!g_ok || w != g_w || h != g_h || rgba == nullptr) return false;
    std::memcpy(rgba, g_cpu.data(), (size_t)w * h * 4);
    return true;
}
